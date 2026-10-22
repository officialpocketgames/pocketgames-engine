// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11 clarification 1: servicing faults at low windows (see ios_lowwin_fault.h).
//
// A 64-bit program's absolute addresses below 4 GB (the shared data page at 0x7ffe0000, images
// based low but mapped high) fault, because the low 4 GB of an iOS process are never mapped.
// spec 10 keeps a table of low windows (ios_vm_pure.c, lock-free) that says where each such
// range really is. Here a fault in a window is turned into the access the program meant:
//   - shared data page, fast path: the faulting instruction's base register points into the
//     page; it is moved to the same offset in the real page and the instruction retried, so
//     later accesses through that register don't fault;
//   - otherwise the A64 load or store is decoded (ios_sig_pure.c) and done on the real memory,
//     after checking that the whole span lies in the window and that the memory allows the
//     access (vm_region_64), then pc moves past it.
// Runs on the Mach exception server and in signal handlers: no locks, no allocation, write(2)
// logging, the instruction read with vm_read_overwrite. (The vm_* calls, not mach_vm_*: the
// iPhoneOS SDK's <mach/mach_vm.h> is an "unsupported" stub; on arm64 vm_address_t and
// vm_size_t are 64-bit.)

#if 0
#pragma makedep unix
#endif

#if defined(__aarch64__) && defined(WINE_IOS)

#include <mach/mach.h>
#include <mach/vm_map.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "ios_sig_pure.h"
#include "ios_vm_pure.h"
#include "ios_lowwin_fault.h"

extern void ios_log_write( const char *buf, int len );   /* debug.c: write(2) to the log fd */

#define LOW_LIMIT      ((uint64_t)1 << 32)
#define USD_LOW        0x7ffe0000ull   /* Windows' shared data page (KUSER_SHARED_DATA) */
#define USD_SIZE       0x1000ull
#define LOG_EVERY      4096

static void lw_log( const char *fmt, ... )
{
    char buffer[256];
    va_list args;
    size_t len;

    len = ios_sfmt( buffer, sizeof(buffer), "ntdll-ios: low windows: " );
    va_start( args, fmt );
    len += ios_vsfmt( buffer + len, sizeof(buffer) - len, fmt, args );
    va_end( args );
    ios_log_write( buffer, (int)len );
}

/* register n (31: sp) of a thread state */
static uint64_t get_reg( const struct ios_darwin_ss *ss, int n )
{
    if (n < 29) return ss->x[n];
    if (n == 29) return ss->fp;
    if (n == 30) return ss->lr;
    return ss->sp;
}

static void set_reg( struct ios_darwin_ss *ss, int n, uint64_t value )
{
    if (n == 18) return;  /* the platform's */
    if (n < 29) ss->x[n] = value;
    else if (n == 29) ss->fp = value;
    else if (n == 30) ss->lr = value;
    else ss->sp = value;
}

/* whether every byte of [start, start + len) is mapped with the access allowed */
static int region_allows( uint64_t start, uint64_t len, int access )
{
    vm_address_t addr = (vm_address_t)start, end = (vm_address_t)(start + len);

    while (addr < end)
    {
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        vm_address_t region = addr;
        vm_size_t size = 0;
        mach_port_t object = MACH_PORT_NULL;

        if (vm_region_64( mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                          (vm_region_info_t)&info, &count, &object ))
            return 0;
        if (region > addr || !size) return 0;   /* a hole */
        if ((access & IOS_A64_READ) && !(info.protection & VM_PROT_READ)) return 0;
        if ((access & IOS_A64_WRITE) && !(info.protection & VM_PROT_WRITE)) return 0;
        addr = region + size;
    }
    return 1;
}

struct map_ctx
{
    const void *peb;
    int         slot;
};

/* ios_a64_mem.map: the real address of a span, only when the whole span is in one window and
 * the real memory allows the access */
static void *map_span( void *arg, uint64_t addr, uint64_t len, int access )
{
    struct map_ctx *ctx = arg;
    uint64_t real, real_last;
    int slot;

    if (!len || addr >= LOW_LIMIT || len > LOW_LIMIT - addr) return NULL;
    if ((slot = ios_lowwin_find( addr, ctx->peb, &real )) < 0) return NULL;
    if (ios_lowwin_find( addr + len - 1, ctx->peb, &real_last ) != slot) return NULL;
    if (real_last - real != len - 1) return NULL;
    if (!region_allows( real, len, access )) return NULL;
    ctx->slot = slot;
    return (void *)(uintptr_t)real;
}

static void count_service( int slot, uint64_t addr )
{
    unsigned int count = ios_lowwin_note_service( slot );

    if (count && !(count % LOG_EVERY))
        lw_log( "window %d (address %p): %u accesses serviced\n", slot, (void *)addr, count );
}

static int decline( uint64_t pc, uint64_t addr, const char *why )
{
    if (ios_once_pair( pc, addr & ~0xfffull ))
        lw_log( "declined %s at pc %p, address %p\n", why, (void *)pc, (void *)addr );
    return IOS_LW_NO;
}

int ios_lowwin_service( struct ios_darwin_ss *ss, struct ios_darwin_ns *ns, uint64_t fault_addr,
                        const void *peb )
{
    struct ios_a64_regs regs;
    struct ios_a64_mem mem;
    struct map_ctx ctx;
    vm_size_t got = 0;
    uint64_t real;
    uint32_t insn;
    int slot, base, i, ret;

    if (fault_addr >= LOW_LIMIT) return IOS_LW_NO;
    if (fault_addr == ss->pc) return IOS_LW_NO;             /* an instruction fetch */
    if ((slot = ios_lowwin_find( fault_addr, peb, &real )) < 0) return IOS_LW_NO;  /* no window */

    if ((ss->pc & 3) ||
        vm_read_overwrite( mach_task_self(), (vm_address_t)ss->pc, sizeof(insn), (vm_address_t)&insn, &got ) ||
        got != sizeof(insn))
        return decline( ss->pc, fault_addr, "(instruction unreadable)" );

    if ((base = ios_a64_base_reg( insn )) < 0)
        return decline( ss->pc, fault_addr, "an unsupported instruction" );

    /* the shared data page: move the base register into the real page and retry */
    if (base != 31)
    {
        uint64_t value = get_reg( ss, base ), real_base;

        if (value - USD_LOW < USD_SIZE && ios_lowwin_find( value, peb, &real_base ) == slot)
        {
            set_reg( ss, base, real_base );
            count_service( slot, fault_addr );
            return IOS_LW_DONE;
        }
    }

    if (ios_a64_is_simd( insn ) && !ns) return IOS_LW_NEED_NS;

    /* otherwise emulate the access on the real memory */
    memset( &regs, 0, sizeof(regs) );
    for (i = 0; i < 29; i++) regs.x[i] = ss->x[i];
    regs.x[29] = ss->fp;
    regs.x[30] = ss->lr;
    regs.sp    = ss->sp;
    regs.pc    = ss->pc;
    if (ns) memcpy( regs.v, ns->v, sizeof(regs.v) );

    ctx.peb  = peb;
    ctx.slot = slot;
    mem.ctx  = &ctx;
    mem.map  = map_span;
    ret = ios_a64_emulate( insn, &regs, &mem );
    if (ret == IOS_A64_UNSUPPORTED) return decline( ss->pc, fault_addr, "an unsupported instruction" );
    if (ret != IOS_A64_DONE) return decline( ss->pc, fault_addr, "an access outside the window or refused by the memory" );

    for (i = 0; i < 29; i++) if (i != 18) ss->x[i] = regs.x[i];
    ss->fp = regs.x[29];
    ss->lr = regs.x[30];
    ss->sp = regs.sp;
    ss->pc = regs.pc;
    if (ns) memcpy( ns->v, regs.v, sizeof(ns->v) );
    count_service( ctx.slot, fault_addr );
    return IOS_LW_DONE;
}

#endif /* __aarch64__ && WINE_IOS */
