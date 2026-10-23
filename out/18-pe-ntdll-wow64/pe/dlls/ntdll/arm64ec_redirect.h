// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/*
 * Spec 18 Part D1: from an x64-callable export address of an ARM64EC module to the native code.
 *
 * Pure code shared by signal_arm64ec.c (arm64ec_redirect_ptr) and the host tests. The caller
 * describes a module by a view (base, SizeOfImage, redirection table) and supplies a lookup
 * that finds the loaded ARM64EC module containing an address.
 *
 * Resolution order for a pointer inside a module:
 *   1. redirection table hit whose destination RVA is inside SizeOfImage -> the destination;
 *      an out-of-image destination counts as "no entry";
 *   2. the documented 14-byte fast-forward sequence 48 8B C4 48 89 58 20 55 5D E9 <rel32>
 *      -> ptr + 14 + rel32;
 *   3. an indirect jump FF 25 <disp32> -> the 8-byte slot at ptr + 6 + disp32; a bound-looking
 *      value (non-zero, not ptr, >= 0x10000000) is redirected again with the module holding it,
 *      or returned as is when no ARM64EC module holds it;
 *   4. anything else -> ptr unchanged.
 * Bytes are read only inside the module's image, and when the module has a code-range map only
 * inside its x64 ranges (exports of ARM64EC modules are x64 thunks; this keeps the function
 * idempotent on native addresses).
 */

#ifndef MYIOSDECK_ARM64EC_REDIRECT_H
#define MYIOSDECK_ARM64EC_REDIRECT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* same layout as IMAGE_ARM64EC_REDIRECTION_ENTRY */
struct ec_redirect_entry
{
    uint32_t source;
    uint32_t destination;
};

/* same layout as IMAGE_CHPE_RANGE_ENTRY: start RVA with the type in its low 2 bits (2 = x64) */
struct ec_code_range
{
    uint32_t start;
    uint32_t length;
};

struct ec_module_view
{
    uintptr_t                       base;
    uint32_t                        size;          /* SizeOfImage */
    const struct ec_redirect_entry *redir;         /* sorted by source, may be NULL */
    uint32_t                        redir_count;
    const struct ec_code_range     *code_map;      /* may be NULL: then bytes are always decoded */
    uint32_t                        code_map_count;
};

/* fill *view and return 1 when addr lies in a loaded ARM64EC module, else 0 */
typedef int (*ec_find_module_fn)( void *ctx, uintptr_t addr, struct ec_module_view *view );

#define EC_REDIRECT_MAX_DEPTH  8
#define EC_BOUND_SLOT_MIN      0x10000000

static inline uint32_t ec_redirect_read32( const unsigned char *p )
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* binary search of the redirection table; 1 and *dest_rva on a usable hit */
static inline int ec_redirect_lookup( const struct ec_module_view *mod, uint32_t rva, uint32_t *dest_rva )
{
    uint32_t lo = 0, hi = mod->redir_count;

    if (!mod->redir) return 0;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t src = mod->redir[mid].source;

        if (src == rva)
        {
            if (mod->redir[mid].destination >= mod->size) return 0;  /* corrupt: no entry */
            *dest_rva = mod->redir[mid].destination;
            return 1;
        }
        if (src < rva) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

/* may the bytes at [rva, rva + len) be decoded as x64 code? Without a code map: yes. With one,
 * only inside an x64 range, so native code that happens to look like a thunk is never misread. */
static inline int ec_may_decode( const struct ec_module_view *mod, uint32_t rva, uint32_t len )
{
    uint32_t i;

    if (!mod->code_map || !mod->code_map_count) return 1;
    for (i = 0; i < mod->code_map_count; i++)
    {
        uint32_t start = mod->code_map[i].start;

        if ((start & 3) != 2) continue;
        start &= ~3u;
        if (rva >= start && (uint64_t)rva + len <= (uint64_t)start + mod->code_map[i].length) return 1;
    }
    return 0;
}

static inline int ec_is_fast_forward( const unsigned char *code )
{
    static const unsigned char prefix[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x5d, 0xe9 };
    return !memcmp( code, prefix, sizeof(prefix) );
}

static inline uintptr_t ec_redirect_resolve( const struct ec_module_view *module, uintptr_t ptr,
                                             ec_find_module_fn find, void *ctx )
{
    struct ec_module_view cur, next;
    unsigned int depth;

    if (!ptr) return 0;
    if (module) cur = *module;
    else if (!find || !find( ctx, ptr, &cur )) return ptr;

    for (depth = 0; depth < EC_REDIRECT_MAX_DEPTH; depth++)
    {
        const unsigned char *code;
        uintptr_t rva, avail, slot;
        uint64_t value;
        uint32_t dest;

        if (ptr < cur.base || ptr - cur.base >= cur.size)
        {
            /* not in this module: use the module that holds it, if it is ARM64EC */
            if (!find || !find( ctx, ptr, &next )) return ptr;
            cur = next;
            if (ptr < cur.base || ptr - cur.base >= cur.size) return ptr;
        }
        rva = ptr - cur.base;
        if (ec_redirect_lookup( &cur, (uint32_t)rva, &dest )) return cur.base + dest;

        code = (const unsigned char *)ptr;
        avail = cur.size - rva;
        if (avail >= 14 && ec_may_decode( &cur, (uint32_t)rva, 14 ) && ec_is_fast_forward( code ))
            return ptr + 14 + (intptr_t)(int32_t)ec_redirect_read32( code + 10 );

        if (avail < 6 || !ec_may_decode( &cur, (uint32_t)rva, 6 )) return ptr;
        if (code[0] != 0xff || code[1] != 0x25) return ptr;
        slot = ptr + 6 + (intptr_t)(int32_t)ec_redirect_read32( code + 2 );
        if (cur.size < 8 || slot < cur.base || slot - cur.base > (uintptr_t)cur.size - 8) return ptr;
        memcpy( &value, (const void *)slot, sizeof(value) );
        if (!value || value == (uint64_t)ptr || value < EC_BOUND_SLOT_MIN) return ptr;  /* unbound */
        if (!find || !find( ctx, (uintptr_t)value, &next )) return (uintptr_t)value;
        cur = next;
        ptr = (uintptr_t)value;
    }
    return ptr;
}

#endif /* MYIOSDECK_ARM64EC_REDIRECT_H */
