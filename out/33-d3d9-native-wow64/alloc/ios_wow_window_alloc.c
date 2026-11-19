// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 33 Part C: the guest-window allocator (see ios_wow_window_alloc.h).
//
// Blocks are reserved and committed in the calling process's guest window with Wine's
// NtAllocateVirtualMemory (spec 10 Part E4: a zero_bits limit below 4 GB is a guest number and
// is placed in [B, B + limit]). Each block keeps a sorted list of free ranges; allocations are
// first fit with alignment and coalesce on free. All bookkeeping lives in the native heap, never
// in guest memory, so the program cannot corrupt it. One mutex guards everything; allocations
// happen at resource creation, not per call.

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#endif

#include "ios_wow_window_alloc.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Wine's unix side, linked into the same binary ---- */

/* Spec 10 (Wine 11.4's dlls/ntdll/unix/virtual.c): the system calls on the current process.
   On the unix side WINAPI is the platform's own calling convention (Wine's minwindef.h defines
   __stdcall empty for WINE_UNIX_LIB). */
extern int32_t NtAllocateVirtualMemory( void *process, void **ret, uintptr_t zero_bits,
                                        size_t *size_ptr, uint32_t type, uint32_t protect );
extern int32_t NtFreeVirtualMemory( void *process, void **addr_ptr, size_t *size_ptr, uint32_t type );
/* Spec 06: B of the calling thread's program; 0 if it has none. */
extern unsigned long ios_wow_base( void );
/* Wine's unix-side debug output (dlls/ntdll/unix/debug.c); weak so host tests link without it. */
extern int __wine_dbg_output( const char *str ) __attribute__((weak));

#define CURRENT_PROCESS   ((void *)~(uintptr_t)0)
#define MEM_COMMIT        0x00001000u
#define MEM_RESERVE       0x00002000u
#define MEM_RELEASE       0x00008000u
#define PAGE_READWRITE    0x04u
#define WINDOW_ZERO_BITS  0x7fffffffu      /* C1: Windows' "below 2 GB" mask form */
#define GRANULARITY       0x10000u         /* Windows allocation granularity */
#define WINDOW_SIZE       0x100000000ull   /* 4 GB */
#define WINDOW_LOW        0x10000ull       /* Windows never maps the first 64 KB */
#define MIN_UNIT          16u

/* ---- clock (a fake one in host tests) ---- */

#ifdef IOS_WOW_WINDOW_FAKE_CLOCK
extern uint64_t ios_wow_window_fake_now_ns( void );
static uint64_t now_ns( void ) { return ios_wow_window_fake_now_ns(); }
#else
static uint64_t now_ns( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

/* ---- log ---- */

static void say( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));
static void say( const char *fmt, ... )
{
    char buf[256];
    va_list args;
    va_start( args, fmt );
    vsnprintf( buf, sizeof(buf), fmt, args );
    va_end( args );
    if (__wine_dbg_output) __wine_dbg_output( buf );
    else fputs( buf, stderr );
}

/* first, then every 256th */
static int rarely( atomic_uint *counter )
{
    return (atomic_fetch_add_explicit( counter, 1, memory_order_relaxed ) & 255u) == 0;
}

/* ---- state ---- */

struct range
{
    size_t off, len;
    struct range *next;
};

struct block
{
    struct block *next;
    uint8_t *host;
    size_t size;
    unsigned long base;        /* the window base B the block was allocated in */
    struct range *free;        /* sorted by offset, never adjacent */
    size_t used;
    uint32_t live;
    uint64_t empty_since;      /* 0 while in use */
};

struct rec
{
    uintptr_t host;            /* 0 empty, 1 removed */
    size_t len;
    struct block *block;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct block *blocks;
static struct rec *recs;
static size_t rec_cap, rec_count, rec_used;   /* used counts removed slots too */
static atomic_uint empty_blocks;
static _Thread_local int native_thread;

static atomic_uint log_native, log_nowindow, log_nomem, log_badfree, log_return;

/* ---- the record table: host pointer -> allocation ---- */

static size_t rec_hash( uintptr_t p )
{
    uint64_t x = (uint64_t)p >> 4;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (size_t)x;
}

static int rec_grow( void )
{
    /* Mostly removed slots: rebuild at the same size. */
    size_t cap = !rec_cap ? 256 : (rec_count + 1) * 4 <= rec_cap ? rec_cap : rec_cap * 2, i;
    struct rec *table = calloc( cap, sizeof(*table) );
    if (!table) return 0;
    for (i = 0; i < rec_cap; i++)
    {
        size_t j;
        if (recs[i].host <= 1) continue;
        for (j = rec_hash( recs[i].host ) & (cap - 1); table[j].host; j = (j + 1) & (cap - 1)) {}
        table[j] = recs[i];
    }
    free( recs );
    recs = table;
    rec_cap = cap;
    rec_used = rec_count;
    return 1;
}

static int rec_insert( uintptr_t host, size_t len, struct block *block )
{
    size_t j;
    if ((rec_used + 1) * 10 >= rec_cap * 7 && !rec_grow()) return 0;
    for (j = rec_hash( host ) & (rec_cap - 1); recs[j].host > 1; j = (j + 1) & (rec_cap - 1)) {}
    if (!recs[j].host) rec_used++;
    recs[j].host = host;
    recs[j].len = len;
    recs[j].block = block;
    rec_count++;
    return 1;
}

static struct rec *rec_find( uintptr_t host )
{
    size_t j;
    if (!rec_cap) return NULL;
    for (j = rec_hash( host ) & (rec_cap - 1); recs[j].host; j = (j + 1) & (rec_cap - 1))
        if (recs[j].host == host) return &recs[j];
    return NULL;
}

/* ---- ranges ---- */

static size_t align_up( size_t v, size_t a ) { return (v + a - 1) & ~(a - 1); }

/* Takes [off, off + len) from the block's free ranges; returns the offset or (size_t)-1. */
static size_t carve( struct block *b, size_t len, size_t align )
{
    struct range **link;
    for (link = &b->free; *link; link = &(*link)->next)
    {
        struct range *r = *link;
        uintptr_t start = align_up( (uintptr_t)b->host + r->off, align );
        size_t off = (size_t)(start - (uintptr_t)b->host), front, back;
        if (off + len > r->off + r->len || off + len < off) continue;
        front = off - r->off;
        back = r->off + r->len - (off + len);
        if (front && back)
        {
            struct range *tail = malloc( sizeof(*tail) );
            if (!tail) return (size_t)-1;
            tail->off = off + len;
            tail->len = back;
            tail->next = r->next;
            r->len = front;
            r->next = tail;
        }
        else if (front) r->len = front;
        else if (back)
        {
            r->off = off + len;
            r->len = back;
        }
        else
        {
            *link = r->next;
            free( r );
        }
        return off;
    }
    return (size_t)-1;
}

/* Puts [off, off + len) back, merging with its neighbours. */
static void give_back( struct block *b, size_t off, size_t len )
{
    struct range **link = &b->free, *prev = NULL, *r;
    while (*link && (*link)->off < off)
    {
        prev = *link;
        link = &(*link)->next;
    }
    if (prev && prev->off + prev->len == off)
    {
        prev->len += len;
        if (prev->next && prev->off + prev->len == prev->next->off)
        {
            struct range *n = prev->next;
            prev->len += n->len;
            prev->next = n->next;
            free( n );
        }
        return;
    }
    if (*link && off + len == (*link)->off)
    {
        (*link)->off = off;
        (*link)->len += len;
        return;
    }
    r = malloc( sizeof(*r) );
    if (!r)
    {
        /* Out of native heap: the range stays lost to this block until it is given back whole. */
        if (rarely( &log_nomem )) say( "ios_wow_window: no native memory to record a free range\n" );
        return;
    }
    r->off = off;
    r->len = len;
    r->next = *link;
    *link = r;
}

static void free_ranges( struct range *r )
{
    while (r)
    {
        struct range *n = r->next;
        free( r );
        r = n;
    }
}

/* ---- blocks ---- */

static struct block *new_block( unsigned long base, size_t len, size_t align )
{
    void *addr = NULL;
    size_t size = len;
    struct block *b;
    int32_t status;
    uintptr_t host;

    if (align > GRANULARITY) size += align;            /* a block starts 64 KB aligned */
    if (size < len) return NULL;
    size = align_up( size, GRANULARITY );
    if (size < IOS_WOW_WINDOW_BLOCK_MIN) size = IOS_WOW_WINDOW_BLOCK_MIN;
    if (size >= WINDOW_SIZE) return NULL;

    if (!(b = calloc( 1, sizeof(*b) ))) return NULL;
    if (!(b->free = malloc( sizeof(*b->free) )))
    {
        free( b );
        return NULL;
    }
    status = NtAllocateVirtualMemory( CURRENT_PROCESS, &addr, WINDOW_ZERO_BITS, &size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    host = (uintptr_t)addr;
    if (status || !addr)
    {
        if (rarely( &log_nomem ))
            say( "ios_wow_window: NtAllocateVirtualMemory of %zu bytes failed: status %#x\n", size, (unsigned)status );
        free( b->free );
        free( b );
        return NULL;
    }
    if (host < base + WINDOW_LOW || host - base >= WINDOW_SIZE || size > WINDOW_SIZE - (host - base))
    {
        /* Not in the program's window: never hand it out (C3). */
        void *release = addr;
        size_t zero = 0;
        say( "ios_wow_window: block %p+%#zx is outside the guest window at %#lx; given back\n", addr, size, base );
        NtFreeVirtualMemory( CURRENT_PROCESS, &release, &zero, MEM_RELEASE );
        free( b->free );
        free( b );
        return NULL;
    }
    b->host = addr;
    b->size = size;
    b->base = base;
    b->free->off = 0;
    b->free->len = size;
    b->free->next = NULL;
    b->next = blocks;
    blocks = b;
    return b;
}

/* Gives back this window's blocks that have been empty for 2 s. Lock held, Wine thread. */
static void collect_locked( unsigned long base, uint64_t now )
{
    struct block **link = &blocks;
    while (*link)
    {
        struct block *b = *link;
        if (b->live || b->base != base || now - b->empty_since < IOS_WOW_WINDOW_RETURN_NS)
        {
            link = &b->next;
            continue;
        }
        {
            void *addr = b->host;
            size_t zero = 0;
            int32_t status = NtFreeVirtualMemory( CURRENT_PROCESS, &addr, &zero, MEM_RELEASE );
            if (status)
            {
                /* Keep it: it is still committed, so it can serve the next allocation. */
                if (rarely( &log_return ))
                    say( "ios_wow_window: giving back block %p failed: status %#x\n", b->host, (unsigned)status );
                b->empty_since = now ? now : 1;
                link = &b->next;
                continue;
            }
        }
        *link = b->next;
        free_ranges( b->free );
        free( b );
        atomic_fetch_sub_explicit( &empty_blocks, 1, memory_order_relaxed );
    }
}

static struct block *block_of( uintptr_t host )
{
    struct block *b;
    for (b = blocks; b; b = b->next)
        if (host >= (uintptr_t)b->host && host - (uintptr_t)b->host < b->size) return b;
    return NULL;
}

/* ---- API ---- */

void *ios_wow_window_alloc( size_t size, size_t align, uint32_t *guest )
{
    unsigned long base;
    struct block *b;
    size_t off = (size_t)-1, len;
    uint8_t *host;

    if (guest) *guest = 0;
    if (native_thread)
    {
        if (rarely( &log_native )) say( "ios_wow_window: refused on a native thread (spec 33 C2)\n" );
        return NULL;
    }
    if (!align) align = MIN_UNIT;
    if (align & (align - 1)) return NULL;
    if (align < MIN_UNIT) align = MIN_UNIT;
    if (size >= WINDOW_SIZE || align >= WINDOW_SIZE) return NULL;
    len = align_up( size ? size : 1, MIN_UNIT );
    if (!(base = ios_wow_base()))
    {
        if (rarely( &log_nowindow )) say( "ios_wow_window: refused on a thread without a guest window\n" );
        return NULL;
    }

    pthread_mutex_lock( &lock );
    if (atomic_load_explicit( &empty_blocks, memory_order_relaxed )) collect_locked( base, now_ns() );
    for (b = blocks; b; b = b->next)
    {
        if (b->base != base) continue;
        if ((off = carve( b, len, align )) != (size_t)-1) break;
    }
    if (!b && (b = new_block( base, len, align ))) off = carve( b, len, align );
    if (!b || off == (size_t)-1)
    {
        pthread_mutex_unlock( &lock );
        if (rarely( &log_nomem )) say( "ios_wow_window: no room for %zu bytes (align %zu)\n", size, align );
        return NULL;
    }
    host = b->host + off;
    if (!rec_insert( (uintptr_t)host, len, b ))
    {
        give_back( b, off, len );
        pthread_mutex_unlock( &lock );
        if (rarely( &log_nomem )) say( "ios_wow_window: no native memory for the record of %zu bytes\n", size );
        return NULL;
    }
    b->used += len;
    if (!b->live++ && b->empty_since)
    {
        b->empty_since = 0;
        atomic_fetch_sub_explicit( &empty_blocks, 1, memory_order_relaxed );
    }
    pthread_mutex_unlock( &lock );

    if (guest) *guest = (uint32_t)((uintptr_t)host - base);
    return host;
}

void ios_wow_window_free( void *host )
{
    struct rec *r;
    struct block *b;

    if (!host) return;
    pthread_mutex_lock( &lock );
    if (!(r = rec_find( (uintptr_t)host )))
    {
        pthread_mutex_unlock( &lock );
        if (rarely( &log_badfree )) say( "ios_wow_window: free of %p, which it did not hand out\n", host );
        return;
    }
    b = r->block;
    give_back( b, (size_t)((uint8_t *)host - b->host), r->len );
    b->used -= r->len;
    r->host = 1;
    r->block = NULL;
    rec_count--;
    if (!--b->live)
    {
        uint64_t now = now_ns();
        b->empty_since = now ? now : 1;
        atomic_fetch_add_explicit( &empty_blocks, 1, memory_order_relaxed );
    }
    pthread_mutex_unlock( &lock );
}

int ios_wow_window_owns( const void *host )
{
    int ret;
    pthread_mutex_lock( &lock );
    ret = block_of( (uintptr_t)host ) != NULL;
    pthread_mutex_unlock( &lock );
    return ret;
}

uint32_t ios_wow_window_guest( const void *host )
{
    struct block *b;
    uint32_t ret = 0;
    pthread_mutex_lock( &lock );
    if ((b = block_of( (uintptr_t)host ))) ret = (uint32_t)((uintptr_t)host - b->base);
    pthread_mutex_unlock( &lock );
    return ret;
}

void ios_wow_window_collect( void )
{
    unsigned long base;
    if (native_thread || !atomic_load_explicit( &empty_blocks, memory_order_relaxed )) return;
    if (!(base = ios_wow_base())) return;
    pthread_mutex_lock( &lock );
    collect_locked( base, now_ns() );
    pthread_mutex_unlock( &lock );
}

void ios_wow_window_mark_native_thread( void )
{
    native_thread = 1;
}

int ios_wow_window_is_native_thread( void )
{
    return native_thread;
}

void ios_wow_window_get_stats( struct ios_wow_window_stats *out )
{
    struct block *b;
    memset( out, 0, sizeof(*out) );
    pthread_mutex_lock( &lock );
    for (b = blocks; b; b = b->next)
    {
        out->blocks++;
        if (!b->live) out->empty_blocks++;
        out->block_bytes += b->size;
        out->used_bytes += b->used;
    }
    out->allocations = (uint32_t)rec_count;
    pthread_mutex_unlock( &lock );
}
