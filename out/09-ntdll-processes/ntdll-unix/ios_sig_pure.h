// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11: the pure parts of exceptions and signals on iOS. Only the C library is used, so
// tests/test_ios_sig_pure.c runs them on any host. signal_arm64_ios.c uses them with the real
// Darwin and Windows structures, whose layouts it checks against the mirrors below.

#ifndef MYIOSDECK_IOS_SIG_PURE_H
#define MYIOSDECK_IOS_SIG_PURE_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* ---------- Part A5: register states ---------- */

/* Darwin's arm64 thread state (__darwin_arm_thread_state64, ARM_THREAD_STATE64), arm64 (not
 * arm64e) layout */
struct ios_darwin_ss
{
    uint64_t x[29];
    uint64_t fp, lr, sp, pc;
    uint32_t cpsr, pad;
};

/* Darwin's arm64 NEON state (__darwin_arm_neon_state64, ARM_NEON_STATE64) */
struct ios_darwin_ns
{
    uint8_t  v[32][16];
    uint32_t fpsr, fpcr;
    uint8_t  pad[8];
};

/* Windows' ARM64 CONTEXT (ARM64_NT_CONTEXT), as Microsoft documents it */
struct ios_win_ctx
{
    uint32_t ContextFlags, Cpsr;
    uint64_t X[29];            /* X0-X28 */
    uint64_t Fp, Lr, Sp, Pc;
    uint8_t  V[32][16];
    uint32_t Fpcr, Fpsr;
    uint32_t Bcr[8];
    uint64_t Bvr[8];
    uint32_t Wcr[2];
    uint64_t Wvr[2];
};

/* Fills the integer, control and floating-point parts of ctx from a Darwin state (ns may be
 * NULL). X18 becomes teb: Windows code sees its TEB there, whatever Darwin left in x18. */
void ios_ctx_from_darwin( struct ios_win_ctx *ctx, const struct ios_darwin_ss *ss,
                          const struct ios_darwin_ns *ns, uint64_t teb );

/* Writes ctx's integer, control and floating-point parts into a Darwin state (ns may be NULL).
 * x18 is never written: the platform owns it. */
void ios_ctx_to_darwin( const struct ios_win_ctx *ctx, struct ios_darwin_ss *ss, struct ios_darwin_ns *ns );

/* ---------- Part B2: signals to exceptions ---------- */

#define IOS_STATUS_ACCESS_VIOLATION        0xc0000005u
#define IOS_STATUS_ILLEGAL_INSTRUCTION     0xc000001du
#define IOS_STATUS_BREAKPOINT              0x80000003u
#define IOS_STATUS_SINGLE_STEP             0x80000004u
#define IOS_STATUS_STACK_BUFFER_OVERRUN    0xc0000409u
#define IOS_STATUS_ASSERTION_FAILURE       0xc0000420u
#define IOS_STATUS_ARRAY_BOUNDS_EXCEEDED   0xc000008cu
#define IOS_STATUS_FLT_DIVIDE_BY_ZERO      0xc000008eu
#define IOS_STATUS_FLT_INEXACT_RESULT      0xc000008fu
#define IOS_STATUS_FLT_INVALID_OPERATION   0xc0000090u
#define IOS_STATUS_FLT_OVERFLOW            0xc0000091u
#define IOS_STATUS_FLT_UNDERFLOW           0xc0000093u
#define IOS_STATUS_INT_DIVIDE_BY_ZERO      0xc0000094u
#define IOS_STATUS_INT_OVERFLOW            0xc0000095u
#define IOS_EXCEPTION_NONCONTINUABLE       1u

enum ios_sig_kind { IOS_SIGK_SEGV, IOS_SIGK_BUS, IOS_SIGK_ILL, IOS_SIGK_TRAP, IOS_SIGK_FPE };

/* the signal's si_code, as the caller classified it */
enum ios_sig_sub
{
    IOS_SUB_NONE,
    IOS_TRAP_BRK, IOS_TRAP_TRACE,
    IOS_FPE_INTDIV, IOS_FPE_INTOVF, IOS_FPE_FLTDIV, IOS_FPE_FLTOVF, IOS_FPE_FLTUND,
    IOS_FPE_FLTRES, IOS_FPE_FLTINV, IOS_FPE_FLTSUB,
};

enum ios_exc_action
{
    IOS_EXC_RAISE,      /* deliver code/flags/info as a Windows exception */
    IOS_EXC_EMULATED,   /* done: write 0 to emul_reg (unless 31) and add pc_adjust to pc */
    IOS_EXC_FASTFAIL,   /* end the process: Windows' __fastfail (code in info[0]) */
};

struct ios_exc
{
    int       action;
    uint32_t  code, flags, nparams;
    uint64_t  info[2];
    int       emul_reg;
    int64_t   pc_adjust;
};

/* The exception for a signal. esr: the fault's syndrome (0 if unknown); addr: the fault
 * address; insn: the instruction at pc when insn_ok. For an access fault, info[0] is 8 when the
 * address is the pc (execute), else 1 for a write (ESR WnR of a data abort), else 0. A brk
 * reports its own address. */
void ios_sig_exception( int kind, int sub, uint64_t esr, uint64_t addr, uint64_t pc,
                        uint32_t insn, int insn_ok, uint64_t x0, struct ios_exc *e );

/* ---------- Part B4: the signal storm ---------- */

#define IOS_STORM_LIMIT 1000

struct ios_storm
{
    uint64_t     pc, addr;
    int          kind;
    unsigned int count;
};

/* Notes one fault a Windows thread took (same kind, pc and address as the last one or not);
 * returns how many identical ones came in a row (1 for a new one). A fault that made progress
 * (an emulated access, pc advanced) resets the count with ios_storm_reset. */
unsigned int ios_storm_note( struct ios_storm *s, int kind, uint64_t pc, uint64_t addr );
void ios_storm_reset( struct ios_storm *s );

/* ---------- Part D: the thread registry ---------- */

#define IOS_THREADREG_SLOTS 1024

/* Registers {mach port, TEB} with the thread's unique 64-bit id (pthread_threadid_np,
 * THREAD_IDENTIFIER_INFO), so a thread can be recognised whichever of its port names an
 * exception message carries. A port already registered gets the new id and TEB. 0, or -1 when
 * full (or for port 0). */
int ios_threadreg_add( uint32_t mach, uint64_t thread_id, uintptr_t teb );
/* Forgets the thread with this port. */
void ios_threadreg_remove( uint32_t mach );
/* The TEB of exactly this port, or 0. */
uintptr_t ios_threadreg_teb_of( uint32_t mach );
/* The TEB of exactly this thread id, or 0 (id 0 is never found). */
uintptr_t ios_threadreg_teb_of_id( uint64_t thread_id );
/* Slots in use or ever used; slot i's values (0 for an empty one). */
int ios_threadreg_count(void);
uintptr_t ios_threadreg_teb( int i );
uint32_t ios_threadreg_mach( int i );
/* Whether a registered thread's TEB lies in [base, base + size). */
int ios_threadreg_range_busy( uintptr_t base, uintptr_t size );
void ios_threadreg_reset(void);   /* tests */

/* ---------- clarification 1: emulated loads and stores ---------- */

struct ios_a64_regs
{
    uint64_t x[31];          /* x0-x30 */
    uint64_t sp, pc;
    uint8_t  v[32][16];
};

/* map: the host address of [addr, addr + len) when the whole span lies in one low window and
 * the memory there allows the access (IOS_A64_READ and/or IOS_A64_WRITE), else NULL. Nothing
 * outside what map returns is ever touched. */
#define IOS_A64_READ   1
#define IOS_A64_WRITE  2

struct ios_a64_mem
{
    void  *ctx;
    void *(*map)( void *ctx, uint64_t addr, uint64_t len, int access );
};

enum ios_a64_result
{
    IOS_A64_DONE,         /* registers updated, pc advanced by 4 */
    IOS_A64_UNSUPPORTED,  /* not a supported encoding (or uses x18) */
    IOS_A64_NO_ACCESS,    /* the span isn't in one window, or the memory refuses it */
};

/* The base register (Rn, 31 = sp) of a supported load or store, else -1. */
int ios_a64_base_reg( uint32_t insn );

/* Whether the instruction is a supported load/store of FP/SIMD registers. */
int ios_a64_is_simd( uint32_t insn );

/* Emulates one supported load or store. The address is computed from r; registers are only
 * changed when the access was done. */
int ios_a64_emulate( uint32_t insn, struct ios_a64_regs *r, const struct ios_a64_mem *mem );

/* ---------- logging without locks or allocation ---------- */

/* A small printf for signal handlers and the Mach server: %s, %d, %u, %x, %lx, %llx, %p and
 * %%. Always terminates; returns the length written. */
size_t ios_sfmt( char *buf, size_t size, const char *fmt, ... );
size_t ios_vsfmt( char *buf, size_t size, const char *fmt, va_list args );

/* "Log once" for (pc, page) pairs: 1 the first time a pair is seen, 0 after (and once 256
 * pairs are known). */
int ios_once_pair( uint64_t pc, uint64_t page );
void ios_once_reset(void);   /* tests */

#endif /* MYIOSDECK_IOS_SIG_PURE_H */
