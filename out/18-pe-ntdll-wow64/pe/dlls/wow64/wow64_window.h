// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/*
 * Spec 18 Part J: the 32-bit guest window, pure parts shared by wow64.dll and the host tests.
 *
 * A 32-bit process's address a is host address B + a; B is 4 GB aligned and above 4 GB, or 0
 * for a process without a window. With B = 0 every function here is the identity, i.e.
 * upstream's behaviour.
 *   J2   guest <-> host conversions;
 *   J3   the 8-entry cache of other processes' bases;
 *   J5   check of the system-call gate values;
 *   J7   which exception parameters are addresses, and their conversion both ways;
 *   J9   whether to fall back to the default CPU module;
 *   J10  NtContinueEx's second argument;
 *   J12  NtFreeVirtualMemory below 0x10000.
 */

#ifndef MYIOSDECK_WOW64_WINDOW_H
#define MYIOSDECK_WOW64_WINDOW_H

#include <stddef.h>
#include <stdint.h>

/* J2 */

static inline uint64_t wow_guest_to_host( uint64_t base, uint32_t value )
{
    return value ? base + value : 0;
}

static inline uint32_t wow_host_to_guest( uint64_t base, uint64_t ptr )
{
    return ptr ? (uint32_t)(ptr - base) : 0;
}

/* J3: another process's window */

#define WOW_BASE_CACHE_SIZE 8

struct wow_base_cache_entry
{
    uint64_t handle;  /* 0: empty; published last with release order */
    uint64_t pid;
    uint64_t base;
};

struct wow_base_cache
{
    struct wow_base_cache_entry entries[WOW_BASE_CACHE_SIZE];
    uint32_t                    next;
};

/* 1 and the value on success */
typedef int (*wow_query_pid_fn)( void *ctx, uint64_t handle, uint64_t *pid );
typedef int (*wow_query_base_fn)( void *ctx, uint64_t handle, uint64_t *base );

/* own_base: the caller's B; is_own: the handle names the own process */
static inline uint64_t wow_base_for_process( struct wow_base_cache *cache, uint64_t own_base,
                                             uint64_t handle, int is_own,
                                             wow_query_pid_fn query_pid, wow_query_base_fn query_base,
                                             void *ctx )
{
    struct wow_base_cache_entry *entry;
    uint64_t pid = 0, base = 0;
    unsigned int i;

    if (!own_base) return 0;
    if (!handle || is_own) return own_base;

    for (i = 0; i < WOW_BASE_CACHE_SIZE; i++)
    {
        entry = &cache->entries[i];
        if (__atomic_load_n( &entry->handle, __ATOMIC_ACQUIRE ) != handle) continue;
        base = __atomic_load_n( &entry->base, __ATOMIC_RELAXED );
        __atomic_thread_fence( __ATOMIC_ACQUIRE );
        if (__atomic_load_n( &entry->handle, __ATOMIC_RELAXED ) == handle) return base;
    }

    if (!query_pid( ctx, handle, &pid )) pid = 0;
    if (!query_base( ctx, handle, &base )) base = 0;
    /* a 0 base is not stored: a child's window is reserved on its own boot thread, and a handle
     * cached before that would keep the wrong base for its lifetime */
    if (!base) return 0;

    entry =&cache->entries[__atomic_fetch_add( &cache->next, 1, __ATOMIC_RELAXED ) % WOW_BASE_CACHE_SIZE];
    __atomic_store_n( &entry->handle, 0, __ATOMIC_RELEASE );
    __atomic_store_n( &entry->base, base, __ATOMIC_RELAXED );
    __atomic_store_n( &entry->pid, pid, __ATOMIC_RELAXED );
    __atomic_store_n( &entry->handle, handle, __ATOMIC_RELEASE );
    return base;
}

/* J5: with a window a gate is a guest address: non-zero and below 4 GB */
static inline int wow_gate_ok( uint64_t base, uint64_t gate )
{
    return !base || (gate && gate < 0x100000000ull);
}

/* J7: exception records */

#define WOW_EXC_MAX_PARAMS              15
#define WOW_EXC_ACCESS_VIOLATION        0xc0000005u
#define WOW_EXC_IN_PAGE_ERROR           0xc0000006u
#define WOW_EXC_WINE_NAME_THREAD        0x406d1388u
#define WOW_EXC_WINE_STUB               0x80000100u
#define WOW_EXC_DBG_PRINTEXCEPTION_C    0x40010006u
#define WOW_EXC_DBG_PRINTEXCEPTION_WIDE 0x4001000au

static inline uint32_t wow_exc_clamp_params( uint32_t count )
{
    return count > WOW_EXC_MAX_PARAMS ? WOW_EXC_MAX_PARAMS : count;
}

/* bit i set: parameter i is an address; decided from the parameters themselves, so the answer is
 * the same in the guest and the host namespace */
static inline uint32_t wow_exc_address_mask( uint32_t code, uint32_t count, uint64_t param0, uint64_t param1 )
{
    uint32_t mask = 0;

    count = wow_exc_clamp_params( count );
    switch (code)
    {
    case WOW_EXC_ACCESS_VIOLATION:
    case WOW_EXC_IN_PAGE_ERROR:
    case WOW_EXC_DBG_PRINTEXCEPTION_C:
    case WOW_EXC_DBG_PRINTEXCEPTION_WIDE:
        mask = 1u << 1;
        break;
    case WOW_EXC_WINE_NAME_THREAD:
        if (count >= 1 && param0 == 0x1000) mask = 1u << 1;
        break;
    case WOW_EXC_WINE_STUB:
        mask = 1u << 0;
        if (count >= 2 && (param1 >> 16)) mask |= 1u << 1;
        break;
    }
    return mask & ((1u << count) - 1);
}

static inline uint32_t wow_exc_params_32to64( uint64_t base, uint32_t code, uint32_t count,
                                              const uint32_t *in, uint64_t *out )
{
    uint32_t i, mask;

    count = wow_exc_clamp_params( count );
    mask = wow_exc_address_mask( code, count, count > 0 ? in[0] : 0, count > 1 ? in[1] : 0 );
    for (i = 0; i < count; i++)
        out[i] = (mask >> i) & 1 ? wow_guest_to_host( base, in[i] ) : in[i];
    return count;
}

static inline uint32_t wow_exc_params_64to32( uint64_t base, uint32_t code, uint32_t count,
                                              const uint64_t *in, uint32_t *out )
{
    uint32_t i, mask;

    count = wow_exc_clamp_params( count );
    mask = wow_exc_address_mask( code, count, count > 0 ? in[0] : 0, count > 1 ? in[1] : 0 );
    for (i = 0; i < count; i++)
        out[i] = (mask >> i) & 1 ? wow_host_to_guest( base, in[i] ) : (uint32_t)in[i];
    return count;
}

/* J9: is the default CPU module a different name than the configured one (ASCII, no case)? */
static inline int wow_cpu_fallback_differs( const unsigned short *configured, const unsigned short *def )
{
    for (;; configured++, def++)
    {
        unsigned short a = *configured, b = *def;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 1;
        if (!a) return 0;
    }
}

/* J10: NtContinueEx's second argument is the alertable BOOLEAN when its raw value is <= 0xff */
static inline int wow_continue_arg_is_boolean( uint32_t raw )
{
    return raw <= 0xff;
}

/* J12: with a window, a free below 0x10000 goes through unconverted and without notifications */
static inline int wow_free_passthrough( uint64_t base, uint32_t addr )
{
    return base && addr < 0x10000;
}

#endif /* MYIOSDECK_WOW64_WINDOW_H */
