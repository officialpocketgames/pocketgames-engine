// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 10: pure parts of memory on iOS (see ios_vm_pure.h).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ios_vm_pure.h"

static uint64_t round_up( uint64_t v, uint64_t align )
{
    return (v + align - 1) & ~(align - 1);
}

/* ---------- Part A2 ---------- */

uint64_t ios_vm_user_limit( uint64_t task_max_address, uint64_t windows_limit )
{
    uint64_t limit = task_max_address & ~(IOS_GRANULARITY - 1);

    if (!limit || limit > windows_limit) limit = windows_limit;
    return limit;
}

/* ---------- Part E4/E5 ---------- */

uint64_t ios_wow_ceiling( int large_address_aware, const char *env_laa )
{
    if (!large_address_aware && env_laa && !strcmp( env_laa, "0" )) return IOS_2G;
    return IOS_4G;
}

int ios_wow_lowfirst( int large_address_aware, uint64_t ceiling, const char *env_lowfirst )
{
    if (large_address_aware || ceiling <= IOS_2G) return 0;
    return !(env_lowfirst && !strcmp( env_lowfirst, "0" ));
}

int ios_vm_translate_limits( uint64_t wow_base, uint64_t ceiling, uint64_t *low, uint64_t *high )
{
    if (!*high || *high >= IOS_4G) return 0;  /* no limit, or not a guest number */

    if (!wow_base)
    {
        /* a 64-bit process can't get memory below 4 GB */
        return (*high < IOS_LOWEST_ADDRESS) ? -1 : 0;
    }
    if (*low >= IOS_4G) return -1;  /* a guest high limit with a host low limit */
    if (*high > ceiling - 1) *high = ceiling - 1;
    if (*low < IOS_GRANULARITY) *low = IOS_GRANULARITY;
    if (*low > *high) return -1;
    *low += wow_base;
    *high += wow_base;
    return 0;
}

/* ---------- Part E2 ---------- */

static void win_log( const struct ios_win_ops *ops, const char *what, const struct ios_win_slot *slot )
{
    if (ops && ops->log) ops->log( ops->ctx, what, slot );
}

int ios_win_take( struct ios_win_table *t, const struct ios_win_ops *ops, const void *owner )
{
    int i;

    /* a free reservation */
    for (i = 0; i < IOS_WIN_SLOTS; i++)
    {
        struct ios_win_slot *slot = &t->slot[i];
        if (slot->state != IOS_WIN_FREE) continue;
        slot->state = IOS_WIN_RESERVED;
        slot->owner = owner;
        slot->peb = NULL;
        return i;
    }

    /* a released window: torn down only with no thread of its old process left */
    for (i = 0; i < IOS_WIN_SLOTS; i++)
    {
        struct ios_win_slot *slot = &t->slot[i];
        if (slot->state != IOS_WIN_RELEASED) continue;
        if (ops->threads_left( ops->ctx, slot ) && ops->alert_and_wait( ops->ctx, slot ))
        {
            slot->state = IOS_WIN_ABANDONED;
            win_log( ops, "abandoned: a thread of its old process is still there", slot );
            continue;
        }
        if (ops->teardown( ops->ctx, slot ))
        {
            slot->state = IOS_WIN_ABANDONED;
            win_log( ops, "abandoned: teardown failed", slot );
            continue;
        }
        win_log( ops, "torn down", slot );
        slot->state = IOS_WIN_RESERVED;
        slot->owner = owner;
        slot->peb = NULL;
        return i;
    }

    /* a new reservation */
    for (i = 0; i < IOS_WIN_SLOTS; i++)
    {
        struct ios_win_slot *slot = &t->slot[i];
        if (slot->state != IOS_WIN_EMPTY) continue;
        if (ops->reserve( ops->ctx, slot ))
        {
            win_log( ops, "no address space for another window", slot );
            return -1;
        }
        win_log( ops, "reserved", slot );
        slot->state = IOS_WIN_RESERVED;
        slot->owner = owner;
        slot->peb = NULL;
        return i;
    }
    win_log( ops, "every window slot is in use", NULL );
    return -1;
}

int ios_win_bind( struct ios_win_table *t, const void *owner, const void *peb )
{
    int i;

    for (i = 0; i < IOS_WIN_SLOTS; i++)
    {
        struct ios_win_slot *slot = &t->slot[i];
        if (slot->state != IOS_WIN_RESERVED || slot->owner != owner) continue;
        slot->peb = peb;
        slot->state = IOS_WIN_BOUND;
        return i;
    }
    return -1;
}

int ios_win_release( struct ios_win_table *t, const void *owner )
{
    int i;

    for (i = 0; i < IOS_WIN_SLOTS; i++)
    {
        struct ios_win_slot *slot = &t->slot[i];
        if (slot->owner != owner) continue;
        if (slot->state != IOS_WIN_BOUND && slot->state != IOS_WIN_RESERVED) continue;
        slot->state = IOS_WIN_RELEASED;
        slot->peb = NULL;
        return i;
    }
    return -1;
}

int ios_win_abandon_owner( struct ios_win_table *t, const void *owner )
{
    int i;

    for (i = 0; i < IOS_WIN_SLOTS; i++)
    {
        struct ios_win_slot *slot = &t->slot[i];
        if (slot->owner != owner || slot->state == IOS_WIN_EMPTY || slot->state == IOS_WIN_FREE) continue;
        slot->state = IOS_WIN_ABANDONED;
        slot->peb = NULL;
        return i;
    }
    return -1;
}

uint64_t ios_win_base_for_peb( const struct ios_win_table *t, const void *peb )
{
    int i;

    if (!peb) return 0;
    for (i = 0; i < IOS_WIN_SLOTS; i++)
        if (t->slot[i].state == IOS_WIN_BOUND && t->slot[i].peb == peb) return t->slot[i].base;
    return 0;
}

/* ---------- Part D ---------- */

/* hex digits only (no "0x", sign or spaces, as the app writes them), at most 16; returns the
 * character after them, or NULL */
static const char *parse_hex( const char *p, uint64_t *value )
{
    uint64_t v = 0;
    int n = 0;

    for (;; p++, n++)
    {
        unsigned d;

        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else break;
        if (n == 16) return NULL;
        v = (v << 4) | d;
    }
    if (!n) return NULL;
    *value = v;
    return p;
}

int ios_exewin_parse( const char *env, struct ios_exewin *w )
{
    uint64_t base, size;
    const char *p;

    memset( w, 0, sizeof(*w) );
    if (!env) return -1;
    if (!(p = parse_hex( env, &base )) || *p != ':') return -1;
    if (!(p = parse_hex( p + 1, &size )) || *p) return -1;
    if (!size || base + size < base) return -1;
    w->base = base;
    w->size = size;
    return 0;
}

int ios_exewin_fits( const struct ios_exewin *w, uint64_t base, uint64_t size )
{
    if (!w->size || !size) return 0;
    return base >= w->base && base - w->base <= w->size && size <= w->size - (base - w->base);
}

int ios_exewin_claim( struct ios_exewin *w, const void *owner, uint64_t base, uint64_t size,
                      uint64_t host_page )
{
    uint64_t rounded = round_up( size, host_page );

    if (w->owner || !ios_exewin_fits( w, base, rounded ) || (base & (host_page - 1))) return -1;
    w->owner = owner;
    w->claim_base = base;
    w->claim_size = rounded;
    return 0;
}

int ios_exewin_confirm( const struct ios_exewin *w, const void *owner, uint64_t base, uint64_t size,
                        uint64_t host_page )
{
    return w->owner && w->owner == owner && w->claim_base == base &&
           w->claim_size == round_up( size, host_page );
}

int ios_exewin_retire( struct ios_exewin *w, const void *owner, uint64_t *base, uint64_t *size )
{
    if (!w->owner || w->owner != owner) return 0;
    *base = w->claim_base;
    *size = w->claim_size;
    w->owner = NULL;
    w->claim_base = w->claim_size = 0;
    return 1;
}

/* ---------- Part C ---------- */

const char *ios_dylib_dir( unsigned short machine, const char *pe_dir, const char *pe_dir_a64 )
{
    const char *dir = NULL;

    if (machine == 0x8664) dir = pe_dir;          /* IMAGE_FILE_MACHINE_AMD64: ARM64EC images */
    else if (machine == 0xaa64) dir = pe_dir_a64; /* IMAGE_FILE_MACHINE_ARM64 */
    return (dir && *dir) ? dir : NULL;
}

int ios_dylib_path( const char *dir, const char *dll_path, char *buf, size_t size )
{
    const char *p, *name = dll_path;
    size_t len, i, pos;
    int n;

    if (!dir || !dll_path) return -1;
    for (p = dll_path; *p; p++) if (*p == '\\' || *p == '/') name = p + 1;
    if (!(len = strlen( name ))) return -1;
    n = snprintf( buf, size, "%s/lib", dir );
    if (n < 0 || (size_t)n >= size) return -1;
    pos = n;
    if (pos + len + sizeof(".dylib") > size) return -1;
    for (i = 0; i < len; i++)
    {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        buf[pos++] = c;
    }
    memcpy( buf + pos, ".dylib", sizeof(".dylib") );
    return 0;
}

/* clarification 4 (spec 18 Part B1): a named export of a mapped PE image */

static uint32_t pe_read32( const unsigned char *p )
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int pe_range_ok( size_t size, uint64_t off, uint64_t len )
{
    return off <= size && len <= size - off;
}

void *ios_pe_find_export( const void *image, size_t size, const char *name )
{
    const unsigned char *base = image, *nt, *exports;
    uint32_t nt_off, magic_off, dir_off, exp_rva, exp_size, count, names, funcs, ords, i;
    size_t name_len;

    if (!base || !name || !pe_range_ok( size, 0, 0x40 )) return NULL;
    if (base[0] != 'M' || base[1] != 'Z') return NULL;
    nt_off = pe_read32( base + 0x3c );
    if (!pe_range_ok( size, nt_off, 24 + 2 )) return NULL;
    nt = base + nt_off;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] || nt[3]) return NULL;
    magic_off = nt_off + 24;
    switch (base[magic_off] | (base[magic_off + 1] << 8))
    {
    case 0x20b: dir_off = magic_off + 112; break;  /* PE32+: data directories at +112 */
    case 0x10b: dir_off = magic_off + 96; break;   /* PE32 */
    default: return NULL;
    }
    if (!pe_range_ok( size, dir_off, 8 )) return NULL;
    exp_rva = pe_read32( base + dir_off );
    exp_size = pe_read32( base + dir_off + 4 );
    if (!exp_rva || exp_size < 40 || !pe_range_ok( size, exp_rva, exp_size )) return NULL;
    exports = base + exp_rva;
    count = pe_read32( exports + 24 );  /* NumberOfNames */
    funcs = pe_read32( exports + 28 );
    names = pe_read32( exports + 32 );
    ords = pe_read32( exports + 36 );
    if (!pe_range_ok( size, names, (uint64_t)count * 4 ) || !pe_range_ok( size, ords, (uint64_t)count * 2 ))
        return NULL;
    name_len = strlen( name );

    for (i = 0; i < count; i++)
    {
        uint32_t name_rva = pe_read32( base + names + 4 * (size_t)i ), func_rva;
        unsigned int ordinal;

        if (!pe_range_ok( size, name_rva, name_len + 1 )) continue;
        if (memcmp( base + name_rva, name, name_len + 1 )) continue;
        ordinal = base[ords + 2 * (size_t)i] | (base[ords + 2 * (size_t)i + 1] << 8);
        if (!pe_range_ok( size, funcs, ((uint64_t)ordinal + 1) * 4 )) return NULL;
        func_rva = pe_read32( base + funcs + 4 * (size_t)ordinal );
        if (!func_rva || func_rva >= size) return NULL;
        if (func_rva >= exp_rva && func_rva < exp_rva + exp_size) return NULL;  /* forwarder */
        return (void *)(base + func_rva);
    }
    return NULL;
}

/* ---------- Part B ---------- */

int ios_vm_exec_prot( int place, int unix_prot, int *strip )
{
    *strip = 0;
    switch (place)
    {
    case IOS_PLACE_IMAGE_CODE:
        return -1;  /* already executable, never changes */
    case IOS_PLACE_IMAGE_DATA:
        if (unix_prot & 4) *strip = 1;
        return unix_prot & ~4;
    case IOS_PLACE_GUEST:
        /* x86 code is data for the interpreter; PAGE_EXECUTE alone becomes readable */
        if (!(unix_prot & 4)) return unix_prot;
        unix_prot &= ~4;
        return unix_prot ? unix_prot : 1;
    default:
        if (unix_prot & 4) *strip = 1;
        return unix_prot & ~4;
    }
}

/* ---------- clarification 9: our builtins only ---------- */

int ios_image_takes_dylib( int wine_builtin_marker, int loaded_as_builtin, unsigned short machine,
                           int is_hybrid )
{
    if (machine == 0xaa64 || is_hybrid) return 1;   /* 9a: native ARM code */
    return wine_builtin_marker || loaded_as_builtin;
}

/* ---------- clarification 8: image machine against process ---------- */

int ios_image_fits_process( unsigned short main_machine, unsigned short image_machine, int is_hybrid )
{
    if (image_machine != 0x8664 && image_machine != 0xaa64) return 1;   /* not a 64-bit image */
    switch (main_machine)
    {
    case 0xaa64: return image_machine == 0xaa64;                         /* plain ARM64 */
    case 0x8664: return image_machine == 0x8664 || is_hybrid;            /* x64 (ARM64EC) */
    default:     return 1;                                               /* WoW64, not known */
    }
}

/* ---------- clarification 10: the calling process's loaded copy ---------- */

int ios_dylib_view_reusable( int starts_at_image, int is_dylib_view, const void *view_owner,
                             const void *owner )
{
    return starts_at_image && is_dylib_view && owner && view_owner == owner;
}

/* ---------- Part E4: what a window holds ---------- */

int ios_win_request_in_window( uint64_t base, uint64_t range_low, uint64_t range_high )
{
    if (!base) return 0;
    return range_low >= base && range_high <= base + IOS_4G && range_low < range_high;
}

/* ---------- Part E4: modules named by guest address ---------- */

int ios_module_match( uint64_t registered, uint64_t query, uint64_t wow_base )
{
    if (!query) return 0;
    if (registered == query) return 1;
    return wow_base && query < IOS_4G && registered == wow_base + query;
}

/* ---------- clarification 7: shared image sections ---------- */

int ios_shared_section_shareable( uint64_t rva, uint64_t map_size, uint64_t host_page )
{
    if (!host_page || !map_size) return 0;
    return !(rva % host_page) && !(map_size % host_page);
}

/* ---------- clarification 1: low windows ----------
 * A slot is claimed by compare-and-swap of its state (FREE -> BUSY -> LIVE, LIVE -> BUSY ->
 * FREE). Each change of its fields happens between two increments of seq (odd while they
 * change), so a reader that sees the same even seq before and after reading them read one
 * consistent window, even if the slot was reused meanwhile. */

enum { LW_FREE, LW_BUSY, LW_LIVE };

struct lowwin
{
    _Atomic unsigned int  seq;
    _Atomic int           state;
    _Atomic uint64_t      low, size, real;
    _Atomic(const void *) owner;
    _Atomic unsigned int  serviced;   /* accesses serviced through it (spec 11 cl. 1) */
};

static struct lowwin lowwins[IOS_LOWWIN_SLOTS];

static void lw_begin( struct lowwin *w )
{
    atomic_fetch_add_explicit( &w->seq, 1, memory_order_relaxed );
    atomic_thread_fence( memory_order_release );
}

static void lw_end( struct lowwin *w )
{
    atomic_fetch_add_explicit( &w->seq, 1, memory_order_release );
}

int ios_lowwin_add( uint64_t low, uint64_t size, uint64_t real, const void *owner )
{
    int i;

    for (i = 0; i < IOS_LOWWIN_SLOTS; i++)
    {
        struct lowwin *w = &lowwins[i];
        int expected = LW_FREE;

        if (!atomic_compare_exchange_strong( &w->state, &expected, LW_BUSY )) continue;
        lw_begin( w );
        atomic_store_explicit( &w->low, low, memory_order_relaxed );
        atomic_store_explicit( &w->size, size, memory_order_relaxed );
        atomic_store_explicit( &w->real, real, memory_order_relaxed );
        atomic_store_explicit( &w->owner, owner, memory_order_relaxed );
        atomic_store_explicit( &w->serviced, 0, memory_order_relaxed );
        atomic_store_explicit( &w->state, LW_LIVE, memory_order_relaxed );
        lw_end( w );
        return 0;
    }
    return -1;
}

void ios_lowwin_remove_owner( const void *owner )
{
    int i;

    if (!owner) return;
    for (i = 0; i < IOS_LOWWIN_SLOTS; i++)
    {
        struct lowwin *w = &lowwins[i];
        int expected = LW_LIVE;

        if (atomic_load_explicit( &w->owner, memory_order_relaxed ) != owner) continue;
        if (!atomic_compare_exchange_strong( &w->state, &expected, LW_BUSY )) continue;
        if (atomic_load_explicit( &w->owner, memory_order_relaxed ) != owner)
        {
            atomic_store( &w->state, LW_LIVE );  /* reused by another owner meanwhile */
            continue;
        }
        lw_begin( w );
        atomic_store_explicit( &w->owner, NULL, memory_order_relaxed );
        atomic_store_explicit( &w->size, 0, memory_order_relaxed );
        lw_end( w );
        atomic_store_explicit( &w->state, LW_FREE, memory_order_release );
    }
}

/* one consistent read of a slot: 1 and the fields if it is a live window, 0 otherwise */
static int lw_read( struct lowwin *w, uint64_t *low, uint64_t *size, uint64_t *real, const void **owner )
{
    for (;;)
    {
        unsigned int seq = atomic_load_explicit( &w->seq, memory_order_acquire );
        int state;

        if (seq & 1) continue;  /* being written: a few stores, try again */
        state = atomic_load_explicit( &w->state, memory_order_relaxed );
        *low = atomic_load_explicit( &w->low, memory_order_relaxed );
        *size = atomic_load_explicit( &w->size, memory_order_relaxed );
        *real = atomic_load_explicit( &w->real, memory_order_relaxed );
        *owner = atomic_load_explicit( &w->owner, memory_order_relaxed );
        atomic_thread_fence( memory_order_acquire );
        if (atomic_load_explicit( &w->seq, memory_order_relaxed ) != seq) continue;
        return state == LW_LIVE && *size;
    }
}

int ios_lowwin_find( uint64_t addr, const void *peb, uint64_t *real_ret )
{
    uint64_t any = 0, low, size, real;
    const void *owner;
    int i, any_slot = -1;

    for (i = 0; i < IOS_LOWWIN_SLOTS; i++)
    {
        if (atomic_load_explicit( &lowwins[i].state, memory_order_relaxed ) == LW_FREE) continue;
        if (!lw_read( &lowwins[i], &low, &size, &real, &owner )) continue;
        if (addr < low || addr - low >= size) continue;
        real += addr - low;
        if (!peb || owner == peb)               /* no PEB known: any match; else its own wins */
        {
            *real_ret = real;
            return i;
        }
        if (!owner && any_slot == -1)           /* an owner-NULL one otherwise */
        {
            any = real;
            any_slot = i;
        }
    }
    *real_ret = any;
    return any_slot;
}

uint64_t ios_lowwin_lookup( uint64_t addr, const void *peb )
{
    uint64_t real;

    return (ios_lowwin_find( addr, peb, &real ) >= 0) ? real : 0;
}

unsigned int ios_lowwin_note_service( int slot )
{
    if (slot < 0 || slot >= IOS_LOWWIN_SLOTS) return 0;
    return atomic_fetch_add_explicit( &lowwins[slot].serviced, 1, memory_order_relaxed ) + 1;
}

void ios_lowwin_reset(void)
{
    int i;

    for (i = 0; i < IOS_LOWWIN_SLOTS; i++)
    {
        struct lowwin *w = &lowwins[i];

        lw_begin( w );
        atomic_store( &w->owner, NULL );
        atomic_store( &w->size, 0 );
        lw_end( w );
        atomic_store( &w->state, LW_FREE );
    }
}
