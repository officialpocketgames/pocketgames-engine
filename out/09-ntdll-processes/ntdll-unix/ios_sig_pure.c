// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11: pure parts of exceptions and signals on iOS (see ios_sig_pure.h). Everything here
// may run in a signal handler or on the Mach exception server: no locks, no allocation, no
// stdio. The A64 encodings are from Arm's A64 instruction set documentation ("Loads and
// stores" encoding groups).

#include <string.h>

#include "ios_sig_pure.h"

/* ---------- Part A5: register states ---------- */

void ios_ctx_from_darwin( struct ios_win_ctx *ctx, const struct ios_darwin_ss *ss,
                          const struct ios_darwin_ns *ns, uint64_t teb )
{
    int i;

    for (i = 0; i < 29; i++) ctx->X[i] = ss->x[i];
    ctx->X[18] = teb;
    ctx->Fp   = ss->fp;
    ctx->Lr   = ss->lr;
    ctx->Sp   = ss->sp;
    ctx->Pc   = ss->pc;
    ctx->Cpsr = ss->cpsr;
    if (!ns) return;
    memcpy( ctx->V, ns->v, sizeof(ctx->V) );
    ctx->Fpcr = ns->fpcr;
    ctx->Fpsr = ns->fpsr;
}

void ios_ctx_to_darwin( const struct ios_win_ctx *ctx, struct ios_darwin_ss *ss, struct ios_darwin_ns *ns )
{
    int i;

    for (i = 0; i < 29; i++) if (i != 18) ss->x[i] = ctx->X[i];
    ss->fp   = ctx->Fp;
    ss->lr   = ctx->Lr;
    ss->sp   = ctx->Sp;
    ss->pc   = ctx->Pc;
    ss->cpsr = ctx->Cpsr;
    if (!ns) return;
    memcpy( ns->v, ctx->V, sizeof(ns->v) );
    ns->fpcr = ctx->Fpcr;
    ns->fpsr = ctx->Fpsr;
}

/* ---------- Part B2 ---------- */

void ios_sig_exception( int kind, int sub, uint64_t esr, uint64_t addr, uint64_t pc,
                        uint32_t insn, int insn_ok, uint64_t x0, struct ios_exc *e )
{
    unsigned int ec = (unsigned int)(esr >> 26) & 0x3f;

    memset( e, 0, sizeof(*e) );
    e->action = IOS_EXC_RAISE;
    e->emul_reg = -1;

    switch (kind)
    {
    case IOS_SIGK_SEGV:
    case IOS_SIGK_BUS:
        e->code = IOS_STATUS_ACCESS_VIOLATION;
        e->nparams = 2;
        if (addr == pc || ec == 0x20 || ec == 0x21) e->info[0] = 8;           /* execute */
        else if ((ec == 0x24 || ec == 0x25) && (esr & 0x40)) e->info[0] = 1;  /* write (WnR) */
        else e->info[0] = 0;                                                  /* read */
        e->info[1] = addr;
        break;

    case IOS_SIGK_ILL:
        if (insn_ok && (insn & ~0x1fu) == 0xd5384240)  /* mrs xN, CurrentEL: EL0 */
        {
            e->action = IOS_EXC_EMULATED;
            e->emul_reg = insn & 0x1f;
            e->pc_adjust = 4;
            break;
        }
        e->code = IOS_STATUS_ILLEGAL_INSTRUCTION;
        break;

    case IOS_SIGK_TRAP:
        /* a single step stops in front of the next instruction: a brk there hasn't run yet */
        if (sub == IOS_TRAP_TRACE && ec != 0x3c)
        {
            e->code = IOS_STATUS_SINGLE_STEP;
            break;
        }
        if (insn_ok && (insn & 0xffe0001fu) == 0xd4200000)  /* brk #imm */
        {
            switch ((insn >> 5) & 0xffff)
            {
            case 0xf000:  /* __debugbreak */
                e->code = IOS_STATUS_BREAKPOINT;
                e->nparams = 1;
                break;
            case 0xf001:
                e->code = IOS_STATUS_ASSERTION_FAILURE;
                break;
            case 0xf003:  /* __fastfail(x0) */
                e->action = IOS_EXC_FASTFAIL;
                e->code = IOS_STATUS_STACK_BUFFER_OVERRUN;
                e->flags = IOS_EXCEPTION_NONCONTINUABLE;
                e->nparams = 1;
                e->info[0] = x0;
                break;
            case 0xf004:  /* the compiler's divide-by-zero check */
                e->code = IOS_STATUS_INT_DIVIDE_BY_ZERO;
                break;
            default:
                e->code = IOS_STATUS_BREAKPOINT;
                e->nparams = 1;
                break;
            }
            break;
        }
        e->code = IOS_STATUS_BREAKPOINT;
        e->nparams = 1;
        break;

    case IOS_SIGK_FPE:
        switch (sub)
        {
        case IOS_FPE_FLTSUB: e->code = IOS_STATUS_ARRAY_BOUNDS_EXCEEDED; break;
        case IOS_FPE_INTDIV: e->code = IOS_STATUS_INT_DIVIDE_BY_ZERO; break;
        case IOS_FPE_INTOVF: e->code = IOS_STATUS_INT_OVERFLOW; break;
        case IOS_FPE_FLTDIV: e->code = IOS_STATUS_FLT_DIVIDE_BY_ZERO; break;
        case IOS_FPE_FLTOVF: e->code = IOS_STATUS_FLT_OVERFLOW; break;
        case IOS_FPE_FLTUND: e->code = IOS_STATUS_FLT_UNDERFLOW; break;
        case IOS_FPE_FLTRES: e->code = IOS_STATUS_FLT_INEXACT_RESULT; break;
        default:             e->code = IOS_STATUS_FLT_INVALID_OPERATION; break;
        }
        break;
    }
}

/* ---------- Part B4 ---------- */

unsigned int ios_storm_note( struct ios_storm *s, int kind, uint64_t pc, uint64_t addr )
{
    if (s->count && s->kind == kind && s->pc == pc && s->addr == addr) return ++s->count;
    s->kind = kind;
    s->pc = pc;
    s->addr = addr;
    return s->count = 1;
}

void ios_storm_reset( struct ios_storm *s )
{
    s->count = 0;
}

/* ---------- Part D: the thread registry ----------
 * A slot is claimed by compare-and-swap of its port from 0 to REG_BUSY; its fields change
 * between two increments of seq (odd while they change), so a reader that sees the same even
 * seq before and after its reads got one consistent {port, id, TEB}. A slot whose port is 0 or
 * REG_BUSY reads as empty. */

#define REG_BUSY 0xffffffffu

struct reg_slot
{
    _Atomic unsigned int seq;
    _Atomic uint32_t     mach;
    _Atomic uint64_t     id;
    _Atomic uintptr_t    teb;
};

struct reg_entry
{
    uint32_t  mach;
    uint64_t  id;
    uintptr_t teb;
};

static struct reg_slot reg[IOS_THREADREG_SLOTS];
static _Atomic int reg_used;

static void reg_begin( struct reg_slot *s )
{
    atomic_fetch_add_explicit( &s->seq, 1, memory_order_relaxed );
    atomic_thread_fence( memory_order_release );
}

static void reg_end( struct reg_slot *s )
{
    atomic_fetch_add_explicit( &s->seq, 1, memory_order_release );
}

/* the slot's port is REG_BUSY (claimed by the caller) until the final store */
static void reg_write( struct reg_slot *s, uint32_t mach, uint64_t id, uintptr_t teb )
{
    reg_begin( s );
    atomic_store_explicit( &s->id, id, memory_order_relaxed );
    atomic_store_explicit( &s->teb, teb, memory_order_relaxed );
    atomic_store_explicit( &s->mach, mach, memory_order_relaxed );
    reg_end( s );
}

/* A writer stays inside begin/end for a few stores only. A reader that still can't get a stable
 * read after many tries (the writer was preempted) reports an empty slot: for the Mach server
 * that means "not a Windows thread", and the fault goes on as a signal. */
#define REG_READ_TRIES 100000

static void reg_read( struct reg_slot *s, struct reg_entry *e )
{
    int tries;

    memset( e, 0, sizeof(*e) );
    for (tries = 0; tries < REG_READ_TRIES; tries++)
    {
        unsigned int seq = atomic_load_explicit( &s->seq, memory_order_acquire );
        struct reg_entry r;

        if (seq & 1) continue;
        r.mach = atomic_load_explicit( &s->mach, memory_order_relaxed );
        r.id   = atomic_load_explicit( &s->id, memory_order_relaxed );
        r.teb  = atomic_load_explicit( &s->teb, memory_order_relaxed );
        atomic_thread_fence( memory_order_acquire );
        if (atomic_load_explicit( &s->seq, memory_order_relaxed ) != seq) continue;
        if (r.mach && r.mach != REG_BUSY) *e = r;
        break;
    }
}

int ios_threadreg_add( uint32_t mach, uint64_t thread_id, uintptr_t teb )
{
    int i, used = atomic_load( &reg_used );

    if (!mach || mach == REG_BUSY) return -1;
    for (i = 0; i < used; i++)
    {
        uint32_t expected = mach;

        /* the same port again (a stale entry of a thread that died without leaving) */
        if (!atomic_compare_exchange_strong( &reg[i].mach, &expected, REG_BUSY )) continue;
        reg_write( &reg[i], mach, thread_id, teb );
        return 0;
    }
    for (i = 0; i < IOS_THREADREG_SLOTS; i++)
    {
        uint32_t expected = 0;

        if (!atomic_compare_exchange_strong( &reg[i].mach, &expected, REG_BUSY )) continue;
        reg_write( &reg[i], mach, thread_id, teb );
        used = atomic_load( &reg_used );
        while (used < i + 1 && !atomic_compare_exchange_weak( &reg_used, &used, i + 1 )) {}
        return 0;
    }
    return -1;
}

void ios_threadreg_remove( uint32_t mach )
{
    int i, used = atomic_load( &reg_used );

    if (!mach || mach == REG_BUSY) return;
    for (i = 0; i < used; i++)
    {
        uint32_t expected = mach;

        if (!atomic_compare_exchange_strong( &reg[i].mach, &expected, REG_BUSY )) continue;
        reg_write( &reg[i], 0, 0, 0 );
    }
}

uintptr_t ios_threadreg_teb_of( uint32_t mach )
{
    int i, used = atomic_load_explicit( &reg_used, memory_order_acquire );
    struct reg_entry e;

    if (!mach || mach == REG_BUSY) return 0;
    for (i = 0; i < used; i++)
    {
        reg_read( &reg[i], &e );
        if (e.mach == mach) return e.teb;
    }
    return 0;
}

uintptr_t ios_threadreg_teb_of_id( uint64_t thread_id )
{
    int i, used = atomic_load_explicit( &reg_used, memory_order_acquire );
    struct reg_entry e;

    if (!thread_id) return 0;
    for (i = 0; i < used; i++)
    {
        reg_read( &reg[i], &e );
        if (e.mach && e.id == thread_id) return e.teb;
    }
    return 0;
}

int ios_threadreg_count(void)
{
    return atomic_load_explicit( &reg_used, memory_order_acquire );
}

uintptr_t ios_threadreg_teb( int i )
{
    struct reg_entry e;

    if (i < 0 || i >= IOS_THREADREG_SLOTS) return 0;
    reg_read( &reg[i], &e );
    return e.teb;
}

uint32_t ios_threadreg_mach( int i )
{
    struct reg_entry e;

    if (i < 0 || i >= IOS_THREADREG_SLOTS) return 0;
    reg_read( &reg[i], &e );
    return e.mach;
}

int ios_threadreg_range_busy( uintptr_t base, uintptr_t size )
{
    int i, used = atomic_load_explicit( &reg_used, memory_order_acquire );
    struct reg_entry e;

    for (i = 0; i < used; i++)
    {
        reg_read( &reg[i], &e );
        if (e.mach && e.teb >= base && e.teb - base < size) return 1;
    }
    return 0;
}

void ios_threadreg_reset(void)
{
    int i;

    for (i = 0; i < IOS_THREADREG_SLOTS; i++) reg_write( &reg[i], 0, 0, 0 );
    atomic_store( &reg_used, 0 );
}

/* ---------- clarification 1: loads and stores ---------- */

enum { K_LDST, K_PAIR, K_RMW, K_CAS };
enum { M_OFF, M_PRE, M_POST, M_REG };
enum { OP_ADD, OP_CLR, OP_EOR, OP_SET, OP_SMAX, OP_SMIN, OP_UMAX, OP_UMIN, OP_SWP };

struct a64
{
    int      kind, load, simd;
    unsigned size;           /* bytes per register */
    int      sext;           /* 0, or sign-extend the loaded value to 32 or 64 bits */
    int      rt, rt2, rn, rm, rs;
    int      mode, option, shift, op;
    int64_t  imm;
};

static int64_t sext_bits( uint64_t v, unsigned bits )
{
    uint64_t m = (uint64_t)1 << (bits - 1);
    v &= (bits == 64) ? ~(uint64_t)0 : (((uint64_t)1 << bits) - 1);
    return (int64_t)((v ^ m) - m);
}

static unsigned log2u( unsigned v )
{
    unsigned r = 0;
    while (v > 1) { v >>= 1; r++; }
    return r;
}

/* the single-register forms with bits 29:27 = 111 (Arm: "Load/store register ...") */
static int decode_ldst_reg( uint32_t insn, struct a64 *d )
{
    unsigned sizecode = insn >> 30, opc = (insn >> 22) & 3;
    int v = (insn >> 26) & 1;

    d->kind = K_LDST;
    d->simd = v;
    if (v)
    {
        if (opc & 2)
        {
            if (sizecode) return -1;
            d->size = 16;
        }
        else d->size = 1u << sizecode;
        d->load = opc & 1;
        return 0;
    }
    d->size = 1u << sizecode;
    switch (opc)
    {
    case 0: d->load = 0; break;
    case 1: d->load = 1; break;
    case 2: if (sizecode == 3) return -1; d->load = 1; d->sext = 64; break;  /* PRFM; LDRS* to X */
    case 3: if (sizecode >= 2) return -1; d->load = 1; d->sext = 32; break;  /* LDRS* to W */
    }
    return 0;
}

static int decode( uint32_t insn, struct a64 *d )
{
    memset( d, 0, sizeof(*d) );
    d->rt = insn & 0x1f;
    d->rn = (insn >> 5) & 0x1f;
    d->rt2 = d->rm = d->rs = -1;

    if ((insn & 0x3ffffc00) == 0x38bfc000)  /* LDAPR */
    {
        d->kind = K_LDST;
        d->load = 1;
        d->size = 1u << (insn >> 30);
        d->mode = M_OFF;
    }
    else if ((insn & 0x3f200c00) == 0x38200000)  /* LSE atomic memory operations */
    {
        unsigned o3 = (insn >> 15) & 1, opc = (insn >> 12) & 7;

        if (!o3) d->op = opc;
        else if (!opc) d->op = OP_SWP;
        else return -1;
        d->kind = K_RMW;
        d->size = 1u << (insn >> 30);
        d->rs = (insn >> 16) & 0x1f;
        d->mode = M_OFF;
    }
    else if ((insn & 0x3fa07c00) == 0x08a07c00)  /* CAS, CASA, CASL, CASAL (B, H, W, X) */
    {
        d->kind = K_CAS;
        d->size = 1u << (insn >> 30);
        d->rs = (insn >> 16) & 0x1f;
        d->mode = M_OFF;
    }
    else if ((insn & 0x3fbf7c00) == 0x089f7c00)  /* STLR, STLLR, LDAR, LDLAR */
    {
        d->kind = K_LDST;
        d->load = (insn >> 22) & 1;
        d->size = 1u << (insn >> 30);
        d->mode = M_OFF;
    }
    else if ((insn & 0x3f200c00) == 0x19000000)  /* STLUR, LDAPUR, LDAPURS* */
    {
        unsigned sizecode = insn >> 30, opc = (insn >> 22) & 3;

        d->kind = K_LDST;
        d->size = 1u << sizecode;
        switch (opc)
        {
        case 0: d->load = 0; break;
        case 1: d->load = 1; break;
        case 2: if (sizecode == 3) return -1; d->load = 1; d->sext = 64; break;
        case 3: if (sizecode >= 2) return -1; d->load = 1; d->sext = 32; break;
        }
        d->mode = M_OFF;
        d->imm = sext_bits( (insn >> 12) & 0x1ff, 9 );
    }
    else if ((insn & 0x3b000000) == 0x39000000)  /* unsigned immediate offset */
    {
        if (decode_ldst_reg( insn, d )) return -1;
        d->mode = M_OFF;
        d->imm = (int64_t)(((insn >> 10) & 0xfff) << log2u( d->size ));
    }
    else if ((insn & 0x3b000000) == 0x38000000)  /* unscaled, pre/post-index, register offset */
    {
        unsigned idx = (insn >> 10) & 3;

        if (decode_ldst_reg( insn, d )) return -1;
        if (!((insn >> 21) & 1))
        {
            d->imm = sext_bits( (insn >> 12) & 0x1ff, 9 );
            switch (idx)
            {
            case 0: d->mode = M_OFF; break;
            case 1: d->mode = M_POST; break;
            case 2: if (d->simd) return -1; d->mode = M_OFF; break;  /* LDTR/STTR: as at EL0 */
            case 3: d->mode = M_PRE; break;
            }
        }
        else
        {
            if (idx != 2) return -1;
            d->option = (insn >> 13) & 7;
            if (d->option != 2 && d->option != 3 && d->option != 6 && d->option != 7) return -1;
            d->rm = (insn >> 16) & 0x1f;
            d->shift = ((insn >> 12) & 1) ? (int)log2u( d->size ) : 0;
            d->mode = M_REG;
        }
    }
    else if ((insn & 0x3a000000) == 0x28000000)  /* LDP, STP, LDNP, STNP, LDPSW */
    {
        unsigned opc = insn >> 30, mode = (insn >> 23) & 3;

        d->kind = K_PAIR;
        d->simd = (insn >> 26) & 1;
        d->load = (insn >> 22) & 1;
        d->rt2 = (insn >> 10) & 0x1f;
        if (d->simd)
        {
            if (opc == 3) return -1;
            d->size = 4u << opc;
        }
        else
        {
            switch (opc)
            {
            case 0: d->size = 4; break;
            case 1:  /* LDPSW */
                if (!d->load || !mode) return -1;
                d->size = 4;
                d->sext = 64;
                break;
            case 2: d->size = 8; break;
            default: return -1;
            }
        }
        switch (mode)
        {
        case 0: case 2: d->mode = M_OFF; break;
        case 1: d->mode = M_POST; break;
        case 3: d->mode = M_PRE; break;
        }
        d->imm = sext_bits( (insn >> 15) & 0x7f, 7 ) * (int64_t)d->size;
        if (d->load && d->rt == d->rt2) return -1;
    }
    else return -1;

    /* x18 belongs to the platform: no instruction that names it as a general register */
    if (d->rn == 18 || d->rm == 18 || d->rs == 18) return -1;
    if (!d->simd && (d->rt == 18 || d->rt2 == 18)) return -1;
    /* a written-back base that is also loaded or stored is unpredictable */
    if ((d->mode == M_PRE || d->mode == M_POST) && d->rn != 31 && !d->simd &&
        (d->rn == d->rt || d->rn == d->rt2))
        return -1;
    return 0;
}

int ios_a64_base_reg( uint32_t insn )
{
    struct a64 d;
    return decode( insn, &d ) ? -1 : d.rn;
}

int ios_a64_is_simd( uint32_t insn )
{
    struct a64 d;
    return !decode( insn, &d ) && d.simd;
}

static uint64_t xreg( const struct ios_a64_regs *r, int n )
{
    return (n == 31) ? 0 : r->x[n];
}

static uint64_t width_mask( unsigned size )
{
    return (size >= 8) ? ~(uint64_t)0 : (((uint64_t)1 << (size * 8)) - 1);
}

static uint64_t rmw_apply( int op, unsigned size, uint64_t old, uint64_t val )
{
    unsigned bits = size * 8;
    uint64_t m = width_mask( size );

    switch (op)
    {
    case OP_ADD:  return (old + val) & m;
    case OP_CLR:  return old & ~val & m;
    case OP_EOR:  return (old ^ val) & m;
    case OP_SET:  return (old | val) & m;
    case OP_SMAX: return (sext_bits( old, bits ) >= sext_bits( val, bits ) ? old : val) & m;
    case OP_SMIN: return (sext_bits( old, bits ) <= sext_bits( val, bits ) ? old : val) & m;
    case OP_UMAX: return ((old & m) >= (val & m) ? old : val) & m;
    case OP_UMIN: return ((old & m) <= (val & m) ? old : val) & m;
    default:      return val & m;  /* SWP */
    }
}

#define RMW_LOOP(type) \
    do { \
        type *q = p, cur = __atomic_load_n( q, __ATOMIC_SEQ_CST ); \
        while (!__atomic_compare_exchange_n( q, &cur, (type)rmw_apply( op, size, cur, val ), 0, \
                                             __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST )) {} \
        return cur; \
    } while (0)

static uint64_t atomic_rmw( void *p, unsigned size, int op, uint64_t val )
{
    switch (size)
    {
    case 1: RMW_LOOP(uint8_t);
    case 2: RMW_LOOP(uint16_t);
    case 4: RMW_LOOP(uint32_t);
    default: RMW_LOOP(uint64_t);
    }
}

#define CAS_ONE(type) \
    do { \
        type cur = (type)expected; \
        __atomic_compare_exchange_n( (type *)p, &cur, (type)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ); \
        return cur; \
    } while (0)

static uint64_t atomic_cas( void *p, unsigned size, uint64_t expected, uint64_t desired )
{
    switch (size)
    {
    case 1: CAS_ONE(uint8_t);
    case 2: CAS_ONE(uint16_t);
    case 4: CAS_ONE(uint32_t);
    default: CAS_ONE(uint64_t);
    }
}

static uint64_t load_value( const void *p, unsigned size, int sext )
{
    uint64_t v = 0;

    memcpy( &v, p, size );  /* little-endian host */
    if (sext == 64) v = (uint64_t)sext_bits( v, size * 8 );
    else if (sext == 32) v = (uint32_t)sext_bits( v, size * 8 );
    return v;
}

int ios_a64_emulate( uint32_t insn, struct ios_a64_regs *r, const struct ios_a64_mem *mem )
{
    struct a64 d;
    uint64_t base, addr, wb = 0, len;
    int access;
    char *p;

    if (decode( insn, &d )) return IOS_A64_UNSUPPORTED;

    base = (d.rn == 31) ? r->sp : r->x[d.rn];
    switch (d.mode)
    {
    case M_OFF:  addr = base + (uint64_t)d.imm; break;
    case M_PRE:  addr = wb = base + (uint64_t)d.imm; break;
    case M_POST: addr = base; wb = base + (uint64_t)d.imm; break;
    default:
    {
        uint64_t off = xreg( r, d.rm );
        switch (d.option)
        {
        case 2: off = (uint32_t)off; break;
        case 6: off = (uint64_t)(int64_t)(int32_t)off; break;
        default: break;
        }
        addr = base + (off << d.shift);
        break;
    }
    }

    len = d.size * (d.kind == K_PAIR ? 2 : 1);
    if (d.kind == K_RMW || d.kind == K_CAS)
    {
        if (addr & (d.size - 1)) return IOS_A64_NO_ACCESS;  /* atomics need natural alignment */
        access = IOS_A64_READ | IOS_A64_WRITE;
    }
    else access = d.load ? IOS_A64_READ : IOS_A64_WRITE;
    if (addr + len < addr) return IOS_A64_NO_ACCESS;
    if (!(p = mem->map( mem->ctx, addr, len, access ))) return IOS_A64_NO_ACCESS;
    if ((d.kind == K_RMW || d.kind == K_CAS) && ((uintptr_t)p & (d.size - 1))) return IOS_A64_NO_ACCESS;

    switch (d.kind)
    {
    case K_LDST:
    case K_PAIR:
    {
        int regs[2] = { d.rt, d.rt2 }, i, n = (d.kind == K_PAIR) ? 2 : 1;

        for (i = 0; i < n; i++)
        {
            char *q = p + i * d.size;

            if (d.simd)
            {
                if (d.load)
                {
                    memset( r->v[regs[i]], 0, 16 );
                    memcpy( r->v[regs[i]], q, d.size );
                }
                else memcpy( q, r->v[regs[i]], d.size );
            }
            else if (d.load)
            {
                uint64_t v = load_value( q, d.size, d.sext );
                if (regs[i] != 31) r->x[regs[i]] = v;
            }
            else
            {
                uint64_t v = xreg( r, regs[i] );
                memcpy( q, &v, d.size );
            }
        }
        break;
    }
    case K_RMW:
    {
        uint64_t old = atomic_rmw( p, d.size, d.op, xreg( r, d.rs ) & width_mask( d.size ));
        if (d.rt != 31) r->x[d.rt] = old & width_mask( d.size );
        break;
    }
    case K_CAS:
    {
        uint64_t old = atomic_cas( p, d.size, xreg( r, d.rs ) & width_mask( d.size ),
                                   xreg( r, d.rt ) & width_mask( d.size ));
        if (d.rs != 31) r->x[d.rs] = old & width_mask( d.size );
        break;
    }
    }

    if (d.mode == M_PRE || d.mode == M_POST)
    {
        if (d.rn == 31) r->sp = wb;
        else r->x[d.rn] = wb;
    }
    r->pc += 4;
    return IOS_A64_DONE;
}

/* ---------- logging ---------- */

static size_t put_num( char *buf, size_t size, size_t pos, uint64_t v, unsigned base, int width, int neg )
{
    char tmp[24];
    int n = 0;

    do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    while (n < width - !!neg && n < (int)sizeof(tmp) - 1) tmp[n++] = '0';
    if (neg) tmp[n++] = '-';
    while (n && pos + 1 < size) buf[pos++] = tmp[--n];
    return pos;
}

size_t ios_vsfmt( char *buf, size_t size, const char *fmt, va_list args )
{
    size_t pos = 0;

    if (!size) return 0;
    while (*fmt && pos + 1 < size)
    {
        int width = 0, longs = 0;

        if (*fmt != '%') { buf[pos++] = *fmt++; continue; }
        fmt++;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') { longs++; fmt++; }
        switch (*fmt)
        {
        case 's':
        {
            const char *s = va_arg( args, const char * );
            if (!s) s = "(null)";
            while (*s && pos + 1 < size) buf[pos++] = *s++;
            break;
        }
        case 'd':
        {
            int64_t v = longs ? va_arg( args, long long ) : va_arg( args, int );
            pos = put_num( buf, size, pos, v < 0 ? (uint64_t)-v : (uint64_t)v, 10, width, v < 0 );
            break;
        }
        case 'u':
            pos = put_num( buf, size, pos, longs ? va_arg( args, unsigned long long ) : va_arg( args, unsigned int ),
                           10, width, 0 );
            break;
        case 'x':
            pos = put_num( buf, size, pos, longs ? va_arg( args, unsigned long long ) : va_arg( args, unsigned int ),
                           16, width, 0 );
            break;
        case 'p':
            if (pos + 3 < size) { buf[pos++] = '0'; buf[pos++] = 'x'; }
            pos = put_num( buf, size, pos, (uintptr_t)va_arg( args, void * ), 16, width, 0 );
            break;
        case '%':
            buf[pos++] = '%';
            break;
        default:
            if (!*fmt) { buf[pos] = 0; return pos; }
            buf[pos++] = '?';
            break;
        }
        fmt++;
    }
    buf[pos] = 0;
    return pos;
}

size_t ios_sfmt( char *buf, size_t size, const char *fmt, ... )
{
    va_list args;
    size_t len;

    va_start( args, fmt );
    len = ios_vsfmt( buf, size, fmt, args );
    va_end( args );
    return len;
}

#define ONCE_SLOTS 256

static struct
{
    _Atomic int state;   /* 0 free, 1 being written, 2 set */
    uint64_t    pc, page;
} once_pairs[ONCE_SLOTS];

int ios_once_pair( uint64_t pc, uint64_t page )
{
    int i;

    for (i = 0; i < ONCE_SLOTS; i++)
    {
        int state = atomic_load_explicit( &once_pairs[i].state, memory_order_acquire );

        if (state == 2 && once_pairs[i].pc == pc && once_pairs[i].page == page) return 0;
        if (state == 0)
        {
            int expected = 0;
            if (!atomic_compare_exchange_strong( &once_pairs[i].state, &expected, 1 )) continue;
            once_pairs[i].pc = pc;
            once_pairs[i].page = page;
            atomic_store_explicit( &once_pairs[i].state, 2, memory_order_release );
            return 1;
        }
    }
    return 0;
}

void ios_once_reset(void)
{
    int i;
    for (i = 0; i < ONCE_SLOTS; i++) atomic_store( &once_pairs[i].state, 0 );
}
