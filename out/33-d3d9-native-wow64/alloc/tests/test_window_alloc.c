// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 33 Part J4: host tests of the guest-window allocator. The guest window is a 4 GB-aligned
// range of a large PROT_NONE reservation; fakes of Wine's NtAllocateVirtualMemory /
// NtFreeVirtualMemory map and unmap blocks inside it, ios_wow_base() returns its base (or 0 on a
// thread told to have no window), and the clock is a variable.

#define _GNU_SOURCE
#include "ios_wow_window_alloc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Wine's unix-side calling convention is the platform's own (WINAPI is empty for WINE_UNIX_LIB) */
#define IOS_WOW_WINAPI

static int failures, checks;
#define CHECK(cond)                                                                           \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(cond)) {                                                                        \
            failures++;                                                                       \
            fprintf( stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond );        \
        }                                                                                     \
    } while (0)

/* ---- fakes ---- */

static uintptr_t window;                 /* B */
static uintptr_t next_free;              /* bump pointer for new blocks */
static atomic_int frees;                 /* NtFreeVirtualMemory calls that released */
static atomic_int allocs;                /* NtAllocateVirtualMemory calls */
static int place_outside;                /* next allocation lands outside the window */
static int fail_alloc;                   /* next allocation fails */
static _Thread_local int no_window;
static uint64_t fake_now = 1000;
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t ios_wow_window_fake_now_ns( void ) { return __atomic_load_n( &fake_now, __ATOMIC_RELAXED ); }
static void advance_ms( uint64_t ms ) { __atomic_add_fetch( &fake_now, ms * 1000000ull, __ATOMIC_RELAXED ); }

unsigned long ios_wow_base( void ) { return no_window ? 0 : (unsigned long)window; }

/* The fake remembers each allocation's size, as the system does (fake_lock held). */
static struct { void *addr; size_t size; } sizes[1024];
static void remember_size( void *addr, size_t size )
{
    for (int i = 0; i < 1024; i++)
        if (!sizes[i].addr)
        {
            sizes[i].addr = addr;
            sizes[i].size = size;
            return;
        }
    abort();
}
static size_t forget_size( void *addr )
{
    for (int i = 0; i < 1024; i++)
        if (sizes[i].addr == addr)
        {
            size_t size = sizes[i].size;
            sizes[i].addr = NULL;
            return size;
        }
    return 0;
}

int32_t IOS_WOW_WINAPI NtAllocateVirtualMemory( void *process, void **ret, uintptr_t zero_bits,
                                                size_t *size_ptr, uint32_t type, uint32_t protect )
{
    void *p;
    (void)type; (void)protect;
    if (process != (void *)~(uintptr_t)0 || *ret || zero_bits != 0x7fffffffu) return (int32_t)0xc000000d;
    atomic_fetch_add( &allocs, 1 );
    pthread_mutex_lock( &fake_lock );
    if (fail_alloc)
    {
        fail_alloc = 0;
        pthread_mutex_unlock( &fake_lock );
        return (int32_t)0xc0000017; /* STATUS_NO_MEMORY */
    }
    if (place_outside)
    {
        place_outside = 0;
        p = mmap( NULL, *size_ptr, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    }
    else
    {
        if (next_free + *size_ptr > window + 0x80000000ull)
        {
            pthread_mutex_unlock( &fake_lock );
            return (int32_t)0xc0000017;
        }
        p = mmap( (void *)next_free, *size_ptr, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0 );
        next_free += *size_ptr;
    }
    if (p == MAP_FAILED)
    {
        pthread_mutex_unlock( &fake_lock );
        return (int32_t)0xc0000017;
    }
    remember_size( p, *size_ptr );
    pthread_mutex_unlock( &fake_lock );
    *ret = p;
    return 0;
}

int32_t IOS_WOW_WINAPI NtFreeVirtualMemory( void *process, void **addr_ptr, size_t *size_ptr, uint32_t type )
{
    uintptr_t a = (uintptr_t)*addr_ptr;
    size_t size;
    if (process != (void *)~(uintptr_t)0 || *size_ptr != 0 || type != 0x8000) return (int32_t)0xc000000d;
    pthread_mutex_lock( &fake_lock );
    size = forget_size( (void *)a );
    pthread_mutex_unlock( &fake_lock );
    if (!size) return (int32_t)0xc000000d; /* not an allocation base */
    if (a >= window && a < window + 0x100000000ull)
    {
        /* back to PROT_NONE inside the reservation */
        mmap( (void *)a, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0 );
    }
    else munmap( (void *)a, size );
    atomic_fetch_add( &frees, 1 );
    return 0;
}

/* ---- helpers ---- */

static struct ios_wow_window_stats stats( void )
{
    struct ios_wow_window_stats s;
    ios_wow_window_get_stats( &s );
    return s;
}

static int in_window( void *host, uint32_t guest )
{
    return host && (uintptr_t)host == window + guest && guest >= 0x10000u;
}

/* ---- tests ---- */

static void test_basic( void )
{
    uint32_t guest = 123;
    char *p = ios_wow_window_alloc( 100, 0, &guest );
    CHECK( in_window( p, guest ) );
    CHECK( ((uintptr_t)p & 15) == 0 );
    if (p)
    {
        memset( p, 0x5a, 100 );
        CHECK( p[99] == 0x5a );
    }
    CHECK( ios_wow_window_owns( p ) );
    CHECK( ios_wow_window_owns( p + 50 ) );
    CHECK( ios_wow_window_guest( p + 50 ) == guest + 50 );
    CHECK( !ios_wow_window_owns( &guest ) );
    CHECK( ios_wow_window_guest( &guest ) == 0 );
    CHECK( stats().allocations == 1 );
    ios_wow_window_free( p );
    CHECK( stats().allocations == 0 );
    CHECK( stats().used_bytes == 0 );
    /* a NULL guest pointer is allowed */
    p = ios_wow_window_alloc( 10, 0, NULL );
    CHECK( p != NULL );
    ios_wow_window_free( p );
    ios_wow_window_free( NULL );
}

static void test_alignment( void )
{
    static const size_t aligns[] = { 1, 16, 64, 4096, 16384, 65536, 1u << 20, 4u << 20 };
    void *ptrs[8];
    for (int i = 0; i < 8; i++)
    {
        uint32_t guest = 0;
        ptrs[i] = ios_wow_window_alloc( 3000 + i, aligns[i], &guest );
        CHECK( in_window( ptrs[i], guest ) );
        CHECK( ((uintptr_t)ptrs[i] & (aligns[i] - 1)) == 0 );
        CHECK( (guest & (aligns[i] - 1)) == 0 );   /* B is 4 GB aligned: guest alignment too */
    }
    for (int i = 0; i < 8; i++) ios_wow_window_free( ptrs[i] );
    /* not a power of two */
    {
        uint32_t guest = 7;
        CHECK( ios_wow_window_alloc( 64, 3, &guest ) == NULL );
        CHECK( guest == 0 );
        CHECK( ios_wow_window_alloc( 64, 48, NULL ) == NULL );
    }
    /* the Metal no-copy rule the frontend uses */
    {
        uint32_t guest;
        void *p = ios_wow_window_alloc( 20000, IOS_WOW_WINDOW_METAL_ALIGN, &guest );
        CHECK( p && ((uintptr_t)p % 16384) == 0 && guest % 16384 == 0 );
        ios_wow_window_free( p );
    }
}

static void test_free_and_reuse( void )
{
    uint32_t g1, g2, g3, g4;
    struct ios_wow_window_stats before = stats();
    char *a = ios_wow_window_alloc( 1 << 20, 16, &g1 );
    char *b = ios_wow_window_alloc( 1 << 20, 16, &g2 );
    char *c = ios_wow_window_alloc( 1 << 20, 16, &g3 );
    CHECK( a && b && c );
    CHECK( stats().used_bytes == before.used_bytes + 3 * (1u << 20) );
    /* free and reuse the same place */
    ios_wow_window_free( b );
    {
        char *b2 = ios_wow_window_alloc( 1 << 20, 16, &g4 );
        CHECK( b2 == b && g4 == g2 );
        ios_wow_window_free( b2 );
    }
    /* coalescing: three neighbours merge into one range big enough for all three */
    ios_wow_window_free( a );
    ios_wow_window_free( c );
    {
        char *big = ios_wow_window_alloc( 3 << 20, 16, &g4 );
        CHECK( big != NULL );
        CHECK( big == (a < c ? a : c) || stats().blocks == before.blocks );
        ios_wow_window_free( big );
    }
    CHECK( stats().used_bytes == before.used_bytes );
    CHECK( stats().blocks == before.blocks || before.blocks == 0 );
}

static void test_large_block( void )
{
    uint32_t guest;
    struct ios_wow_window_stats before = stats();
    size_t size = (20u << 20) + 5;
    char *p = ios_wow_window_alloc( size, 16384, &guest );
    CHECK( in_window( p, guest ) );
    CHECK( stats().blocks == before.blocks + 1 );
    CHECK( stats().block_bytes - before.block_bytes >= size );
    CHECK( (stats().block_bytes - before.block_bytes) % 0x10000 == 0 );
    if (p)
    {
        p[0] = 1;
        p[size - 1] = 2;
    }
    ios_wow_window_free( p );
    /* small allocations get blocks of at least 8 MB */
    CHECK( IOS_WOW_WINDOW_BLOCK_MIN == 8u << 20 );
}

/* Drops every empty block, so the next test starts from none. */
static void drain( void )
{
    advance_ms( 2500 );
    ios_wow_window_collect();
}

static void test_two_second_return( void )
{
    uint32_t guest;
    void *p;
    int before;

    drain();
    CHECK( stats().blocks == 0 );
    p = ios_wow_window_alloc( 1000, 16, &guest );
    CHECK( stats().blocks == 1 );
    ios_wow_window_free( p );
    CHECK( stats().empty_blocks == 1 );

    before = atomic_load( &frees );
    advance_ms( 1999 );
    ios_wow_window_collect();
    CHECK( atomic_load( &frees ) == before );
    CHECK( stats().blocks == 1 );

    /* used again before its 2 s: the timer restarts */
    p = ios_wow_window_alloc( 1000, 16, &guest );
    CHECK( stats().empty_blocks == 0 );
    advance_ms( 5000 );
    ios_wow_window_collect();
    CHECK( stats().blocks == 1 );
    ios_wow_window_free( p );
    advance_ms( 1999 );
    ios_wow_window_collect();
    CHECK( stats().blocks == 1 );
    advance_ms( 1 );
    ios_wow_window_collect();
    CHECK( atomic_load( &frees ) == before + 1 );
    CHECK( stats().blocks == 0 );
    CHECK( stats().empty_blocks == 0 );

    /* an allocation after the 2 s gives the empty block back too */
    p = ios_wow_window_alloc( 1000, 16, &guest );
    ios_wow_window_free( p );
    advance_ms( 2000 );
    p = ios_wow_window_alloc( 1000, 16, &guest );
    CHECK( atomic_load( &frees ) == before + 2 );
    CHECK( stats().blocks == 1 );
    ios_wow_window_free( p );
    drain();
}

struct thread_arg
{
    int native, no_window;
    void *to_free;
    void *result;
    uint32_t guest;
    int is_native;
};

static void *thread_main( void *ctx )
{
    struct thread_arg *arg = ctx;
    if (arg->native) ios_wow_window_mark_native_thread();
    no_window = arg->no_window;
    arg->is_native = ios_wow_window_is_native_thread();
    if (arg->to_free) ios_wow_window_free( arg->to_free );
    arg->guest = 99;
    arg->result = ios_wow_window_alloc( 4096, 16, &arg->guest );
    /* collect never gives blocks back off a game thread */
    advance_ms( 3000 );
    ios_wow_window_collect();
    return NULL;
}

static void run_thread( struct thread_arg *arg )
{
    pthread_t t;
    pthread_create( &t, NULL, thread_main, arg );
    pthread_join( t, NULL );
}

static void test_threads( void )
{
    uint32_t guest;
    struct thread_arg arg;
    int before;

    drain();
    /* a native thread: no allocation, but free works there */
    memset( &arg, 0, sizeof(arg) );
    arg.native = 1;
    arg.to_free = ios_wow_window_alloc( 5000, 16, &guest );
    CHECK( arg.to_free != NULL );
    before = atomic_load( &frees );
    run_thread( &arg );
    CHECK( arg.is_native );
    CHECK( arg.result == NULL && arg.guest == 0 );
    CHECK( stats().allocations == 0 );
    CHECK( stats().empty_blocks == 1 );
    CHECK( atomic_load( &frees ) == before );   /* the native thread's collect did nothing */
    CHECK( !ios_wow_window_is_native_thread() );

    /* a thread without a guest window */
    memset( &arg, 0, sizeof(arg) );
    arg.no_window = 1;
    run_thread( &arg );
    CHECK( arg.result == NULL && arg.guest == 0 );
    CHECK( atomic_load( &frees ) == before );

    /* another game thread */
    memset( &arg, 0, sizeof(arg) );
    run_thread( &arg );
    CHECK( in_window( arg.result, arg.guest ) );
    ios_wow_window_free( arg.result );
    drain();
}

static void test_bad_inputs( void )
{
    uint32_t guest = 5;
    int x;
    /* a pointer it did not hand out is ignored */
    ios_wow_window_free( &x );
    /* too large */
    CHECK( ios_wow_window_alloc( (size_t)5 << 30, 16, &guest ) == NULL && guest == 0 );
    /* a block the system put outside the window is given back and never handed out */
    {
        int before;
        drain();
        CHECK( stats().blocks == 0 );
        before = atomic_load( &frees );
        place_outside = 1;
        CHECK( ios_wow_window_alloc( 100, 16, &guest ) == NULL );
        CHECK( atomic_load( &frees ) == before + 1 );
        CHECK( stats().blocks == 0 );
    }
    /* the system is out of memory */
    fail_alloc = 1;
    CHECK( ios_wow_window_alloc( 100, 16, &guest ) == NULL );
    /* size 0 still gives a unique pointer */
    {
        void *a = ios_wow_window_alloc( 0, 0, &guest ), *b = ios_wow_window_alloc( 0, 0, NULL );
        CHECK( a && b && a != b );
        ios_wow_window_free( a );
        ios_wow_window_free( b );
    }
    drain();
}

static void test_many( void )
{
    enum { N = 6000 };
    static void *p[N];
    uint32_t guest;
    int ok = 1;
    for (int i = 0; i < N; i++)
    {
        p[i] = ios_wow_window_alloc( 16 + (i % 7) * 48, 16, &guest );
        if (!in_window( p[i], guest )) ok = 0;
    }
    CHECK( ok );
    CHECK( stats().allocations == N );
    for (int i = 0; i < N; i += 2) ios_wow_window_free( p[i] );
    for (int i = 1; i < N; i += 2) ios_wow_window_free( p[i] );
    CHECK( stats().allocations == 0 );
    CHECK( stats().used_bytes == 0 );
    drain();
    CHECK( stats().blocks == 0 );
}

/* game threads allocate and free while native threads free what they are handed */
#define WORKERS 4
#define ROUNDS 2000
static void *game_worker( void *ctx )
{
    unsigned seed = (unsigned)(uintptr_t)ctx;
    void *keep[16] = { 0 };
    for (int i = 0; i < ROUNDS; i++)
    {
        int slot = (int)((seed = seed * 1103515245u + 12345u) >> 16) % 16;
        uint32_t guest;
        if (keep[slot]) ios_wow_window_free( keep[slot] );
        keep[slot] = ios_wow_window_alloc( 64 + (seed % 9000), (seed & 1) ? 16384 : 16, &guest );
        if (keep[slot] && (uintptr_t)keep[slot] != window + guest) abort();
    }
    for (int i = 0; i < 16; i++) ios_wow_window_free( keep[i] );
    return NULL;
}

static void test_concurrent( void )
{
    pthread_t t[WORKERS];
    for (int i = 0; i < WORKERS; i++) pthread_create( &t[i], NULL, game_worker, (void *)(uintptr_t)(i + 1) );
    for (int i = 0; i < WORKERS; i++) pthread_join( t[i], NULL );
    CHECK( stats().allocations == 0 );
    CHECK( stats().used_bytes == 0 );
    drain();
    CHECK( stats().blocks == 0 );
}

int main( void )
{
    /* an 8 GB PROT_NONE reservation holds a 4 GB-aligned 4 GB window */
    size_t reserve = (size_t)8 << 30;
    void *r = mmap( NULL, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0 );
    if (r == MAP_FAILED)
    {
        perror( "mmap" );
        return 1;
    }
    window = ((uintptr_t)r + 0xffffffffull) & ~(uintptr_t)0xffffffffull;
    next_free = window + 0x10000;

    test_basic();
    test_alignment();
    test_free_and_reuse();
    test_large_block();
    test_two_second_return();
    test_threads();
    test_bad_inputs();
    test_many();
    test_concurrent();

    printf( "test_window_alloc: %d checks, %d failures\n", checks, failures );
    return failures != 0;
}
