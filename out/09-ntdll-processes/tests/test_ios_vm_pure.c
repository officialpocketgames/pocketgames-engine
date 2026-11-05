// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 10 Part G: host tests of the pure parts of memory on iOS (ntdll-unix/ios_vm_pure.c), with
// fake bases: the highest user address, limit translation for guest ceilings, the
// large-address-aware rules, the window slot state machine, the exe-window claim/confirm/retire
// with host-page rounding, the exe-window check over several kernel regions (clarification 12),
// the dylib folder choice and path, the executable-request rules and the low windows
// (clarification 1, also under concurrent use).
//
//   cc -std=c11 -Wall -pthread -I../ntdll-unix ../ntdll-unix/ios_vm_pure.c test_ios_vm_pure.c -o t && ./t

#define _DEFAULT_SOURCE   /* MAP_ANONYMOUS with -std=c11 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "ios_vm_pure.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while (0)

#define CHECK_STR(a, b) do { checks++; if (strcmp( (a), (b) )) { failures++; \
    printf( "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b) ); } } while (0)

#define WIN_LIMIT  0x7fffffff0000ull
#define B          0x500000000ull     /* a fake guest window base, 4 GB aligned */
#define HOST_PAGE  0x4000ull

/* ---------- Part A2 ---------- */

static void test_user_limit(void)
{
    /* rounded down to 64 KB */
    CHECK( ios_vm_user_limit( 0xfc0001234ull, WIN_LIMIT ) == 0xfc0000000ull );
    CHECK( ios_vm_user_limit( 0x1000000000ull, WIN_LIMIT ) == 0x1000000000ull );
    /* never more than the Windows limit; unknown gives the Windows limit */
    CHECK( ios_vm_user_limit( 0x800000000000ull, WIN_LIMIT ) == WIN_LIMIT );
    CHECK( ios_vm_user_limit( 0, WIN_LIMIT ) == WIN_LIMIT );
}

/* ---------- Part E5 ---------- */

static void test_laa(void)
{
    /* 4 GB for everybody, unless the image lacks the flag and PG_LAA=0 */
    CHECK( ios_wow_ceiling( 1, NULL ) == IOS_4G );
    CHECK( ios_wow_ceiling( 0, NULL ) == IOS_4G );
    CHECK( ios_wow_ceiling( 0, "1" ) == IOS_4G );
    CHECK( ios_wow_ceiling( 0, "0" ) == IOS_2G );
    CHECK( ios_wow_ceiling( 1, "0" ) == IOS_4G );

    /* the low 2 GB first only for an image without the flag that was given 4 GB */
    CHECK( ios_wow_lowfirst( 0, IOS_4G, NULL ) );
    CHECK( ios_wow_lowfirst( 0, IOS_4G, "1" ) );
    CHECK( !ios_wow_lowfirst( 0, IOS_4G, "0" ) );
    CHECK( !ios_wow_lowfirst( 1, IOS_4G, NULL ) );
    CHECK( !ios_wow_lowfirst( 0, IOS_2G, NULL ) );
}

/* ---------- Parts A1 and E4 ---------- */

static void test_translate(void)
{
    uint64_t low, high;

    /* 32-bit process: a guest limit goes into the window, at least 64 KB, capped by the ceiling */
    low = 0; high = 0x7fffffff;
    CHECK( !ios_vm_translate_limits( B, IOS_4G, &low, &high ) );
    CHECK( low == B + 0x10000 && high == B + 0x7fffffff );

    low = 0x20000; high = 0xffffffff;
    CHECK( !ios_vm_translate_limits( B, IOS_2G, &low, &high ) );
    CHECK( low == B + 0x20000 && high == B + 0x7fffffff );

    low = 0; high = 0xfffeffff;
    CHECK( !ios_vm_translate_limits( B, IOS_4G, &low, &high ) );
    CHECK( high == B + 0xfffeffff );

    /* a low limit above the capped high one can't be met */
    low = 0x90000000; high = 0xffffffff;
    CHECK( ios_vm_translate_limits( B, IOS_2G, &low, &high ) == -1 );

    /* no limit, or a host limit: unchanged */
    low = 0; high = 0;
    CHECK( !ios_vm_translate_limits( B, IOS_4G, &low, &high ) && low == 0 && high == 0 );
    low = 0; high = 0x7fffffffffffull;
    CHECK( !ios_vm_translate_limits( B, IOS_4G, &low, &high ) && high == 0x7fffffffffffull );

    /* 64-bit process: memory below 4 GB can't be had */
    low = 0; high = 0x7fffffff;
    CHECK( ios_vm_translate_limits( 0, IOS_4G, &low, &high ) == -1 );
    low = 0; high = 0xffffffff;
    CHECK( ios_vm_translate_limits( 0, IOS_4G, &low, &high ) == -1 );
    low = 0; high = 0x7fffffffffffull;
    CHECK( !ios_vm_translate_limits( 0, IOS_4G, &low, &high ) && high == 0x7fffffffffffull );
}

/* ---------- Part E2: the slot state machine ---------- */

struct fake
{
    int      threads[IOS_WIN_SLOTS];   /* threads left per slot */
    int      leave_on_alert;           /* alerting makes them leave */
    int      teardowns, reserves, alerts;
    int      reserve_fails;
    uint64_t next_base;
};

static struct ios_win_table table;

static int fake_threads_left( void *ctx, struct ios_win_slot *slot )
{
    struct fake *f = ctx;
    return f->threads[slot - table.slot] > 0;
}

static int fake_alert_and_wait( void *ctx, struct ios_win_slot *slot )
{
    struct fake *f = ctx;
    f->alerts++;
    if (f->leave_on_alert) f->threads[slot - table.slot] = 0;
    return f->threads[slot - table.slot] > 0;
}

static int fake_teardown( void *ctx, struct ios_win_slot *slot )
{
    struct fake *f = ctx;
    f->teardowns++;
    return 0;
}

static int fake_reserve( void *ctx, struct ios_win_slot *slot )
{
    struct fake *f = ctx;
    if (f->reserve_fails) return -1;
    f->reserves++;
    slot->base = f->next_base;
    f->next_base += IOS_4G * 2;
    return 0;
}

static void fake_log( void *ctx, const char *what, const struct ios_win_slot *slot )
{
}

static void test_slots(void)
{
    struct fake f = { .next_base = B };
    struct ios_win_ops ops = { &f, fake_threads_left, fake_alert_and_wait, fake_teardown, fake_reserve, fake_log };
    /* only their addresses are used: owners and PEBs */
    static int p1, p2, p3, p4, peb1, peb2, peb3, peb4;
    int i;

    memset( &table, 0, sizeof(table) );

    /* reserve: a new reservation, RESERVED for its owner until bound */
    CHECK( (i = ios_win_take( &table, &ops, &p1 )) == 0 );
    CHECK( table.slot[0].state == IOS_WIN_RESERVED && table.slot[0].base == B && f.reserves == 1 );
    CHECK( ios_win_base_for_peb( &table, &peb1 ) == 0 );           /* not bound yet */

    /* bind */
    CHECK( ios_win_bind( &table, &p1, &peb1 ) == 0 );
    CHECK( table.slot[0].state == IOS_WIN_BOUND );
    CHECK( ios_win_base_for_peb( &table, &peb1 ) == B );
    CHECK( ios_win_bind( &table, &p1, &peb1 ) == -1 );             /* nothing left to bind */

    /* a second process gets a second window */
    CHECK( ios_win_take( &table, &ops, &p2 ) == 1 );
    CHECK( ios_win_bind( &table, &p2, &peb2 ) == 1 );
    CHECK( ios_win_base_for_peb( &table, &peb2 ) == B + 2 * IOS_4G );

    /* release: stops resolving at once, kept until a slot is needed */
    CHECK( ios_win_release( &table, &p1 ) == 0 );
    CHECK( table.slot[0].state == IOS_WIN_RELEASED );
    CHECK( ios_win_base_for_peb( &table, &peb1 ) == 0 );
    CHECK( f.teardowns == 0 );
    CHECK( ios_win_release( &table, &p1 ) == -1 );

    /* teardown only with no threads left: a released window without threads is reused */
    CHECK( ios_win_take( &table, &ops, &p3 ) == 0 );
    CHECK( f.teardowns == 1 && f.alerts == 0 && f.reserves == 2 );
    CHECK( table.slot[0].owner == &p3 && table.slot[0].state == IOS_WIN_RESERVED );
    CHECK( ios_win_bind( &table, &p3, &peb3 ) == 0 );

    /* threads left that leave when alerted: torn down after the alert */
    CHECK( ios_win_release( &table, &p3 ) == 0 );
    f.threads[0] = 1;
    f.leave_on_alert = 1;
    CHECK( ios_win_take( &table, &ops, &p4 ) == 0 );
    CHECK( f.alerts == 1 && f.teardowns == 2 );
    CHECK( ios_win_bind( &table, &p4, &peb4 ) == 0 );

    /* threads that stay: abandoned, another slot is used */
    CHECK( ios_win_release( &table, &p4 ) == 0 );
    f.threads[0] = 1;
    f.leave_on_alert = 0;
    CHECK( ios_win_take( &table, &ops, &p1 ) == 2 );
    CHECK( table.slot[0].state == IOS_WIN_ABANDONED );
    CHECK( f.alerts == 2 && f.teardowns == 2 && f.reserves == 3 );
    /* an abandoned window is never reused */
    CHECK( ios_win_bind( &table, &p1, &peb1 ) == 2 );
    CHECK( ios_win_release( &table, &p1 ) == 2 );
    CHECK( ios_win_take( &table, &ops, &p3 ) == 2 );               /* slot 2, not slot 0 */
    CHECK( table.slot[0].state == IOS_WIN_ABANDONED );

    /* a process that stops being 32-bit abandons its window (Part E8) */
    CHECK( ios_win_abandon_owner( &table, &p3 ) == 2 );
    CHECK( table.slot[2].state == IOS_WIN_ABANDONED );
    CHECK( ios_win_take( &table, &ops, &p4 ) == 3 );

    /* a FREE pre-reservation (the 32-bit main program, Part E1) is taken first */
    memset( &table, 0, sizeof(table) );
    table.slot[0].state = IOS_WIN_FREE;
    table.slot[0].base = B;
    f.reserves = 0;
    CHECK( ios_win_take( &table, &ops, &p1 ) == 0 && f.reserves == 0 );

    /* no address space: -1 */
    f.reserve_fails = 1;
    CHECK( ios_win_take( &table, &ops, &p2 ) == -1 );

    /* every slot in use: -1 */
    memset( &table, 0, sizeof(table) );
    f.reserve_fails = 0;
    for (i = 0; i < IOS_WIN_SLOTS; i++) CHECK( ios_win_take( &table, &ops, &table.slot[i] ) == i );
    CHECK( ios_win_take( &table, &ops, &p1 ) == -1 );
}

/* ---------- Part D: the exe window ---------- */

static void test_exewin(void)
{
    struct ios_exewin w;
    uint64_t base, size;
    static int a, b;   /* owners: only their addresses are used */

    CHECK( !ios_exewin_parse( "140000000:8000000", &w ) );
    CHECK( w.base == 0x140000000ull && w.size == 0x8000000ull && !w.owner );
    CHECK( !ios_exewin_parse( "14000C000:800A000", &w ) && w.base == 0x14000c000ull && w.size == 0x800a000ull );
    /* hex digits only, as the app writes them: no 0x, sign or spaces */
    CHECK( ios_exewin_parse( "0x140000000:8000000", &w ) == -1 );
    CHECK( ios_exewin_parse( "140000000:0x8000000", &w ) == -1 );
    CHECK( ios_exewin_parse( " 140000000:8000000", &w ) == -1 );
    CHECK( ios_exewin_parse( "140000000: 8000000", &w ) == -1 );
    CHECK( ios_exewin_parse( "+140000000:8000000", &w ) == -1 );
    CHECK( ios_exewin_parse( "140000000:8000000 ", &w ) == -1 );
    CHECK( ios_exewin_parse( "10000000000000000:1000", &w ) == -1 );   /* 17 digits */
    CHECK( ios_exewin_parse( "ffffffffffff0000:20000", &w ) == -1 );   /* wraps */
    CHECK( ios_exewin_parse( "140000000", &w ) == -1 );
    CHECK( ios_exewin_parse( "140000000:", &w ) == -1 );
    CHECK( ios_exewin_parse( "140000000:0", &w ) == -1 );
    CHECK( ios_exewin_parse( "", &w ) == -1 && w.size == 0 );
    CHECK( ios_exewin_parse( NULL, &w ) == -1 );

    CHECK( !ios_exewin_parse( "140000000:8000000", &w ) );
    CHECK( ios_exewin_fits( &w, 0x140000000ull, 0x8000000ull ) );
    CHECK( !ios_exewin_fits( &w, 0x140000000ull, 0x8000001ull ) );
    CHECK( !ios_exewin_fits( &w, 0x13fff0000ull, 0x1000ull ) );
    CHECK( !ios_exewin_fits( &w, 0x148000000ull, 0x1000ull ) );

    /* the claim is in host pages: 0x1234567 bytes become 0x1238000 */
    CHECK( !ios_exewin_claim( &w, &a, 0x140000000ull, 0x1234567ull, HOST_PAGE ) );
    CHECK( w.owner == &a && w.claim_size == 0x1238000ull );
    CHECK( ios_exewin_confirm( &w, &a, 0x140000000ull, 0x1234567ull, HOST_PAGE ) );
    CHECK( ios_exewin_confirm( &w, &a, 0x140000000ull, 0x1238000ull, HOST_PAGE ) );
    CHECK( !ios_exewin_confirm( &w, &a, 0x140000000ull, 0x1238001ull, HOST_PAGE ) );
    CHECK( !ios_exewin_confirm( &w, &b, 0x140000000ull, 0x1234567ull, HOST_PAGE ) );

    /* one occupant at a time */
    CHECK( ios_exewin_claim( &w, &b, 0x140000000ull, 0x1000ull, HOST_PAGE ) == -1 );
    CHECK( !ios_exewin_retire( &w, &b, &base, &size ) );
    CHECK( ios_exewin_retire( &w, &a, &base, &size ) == 1 );
    CHECK( base == 0x140000000ull && size == 0x1238000ull && !w.owner );
    CHECK( !ios_exewin_retire( &w, &a, &base, &size ) );

    /* the next program can claim it at once */
    CHECK( !ios_exewin_claim( &w, &b, 0x140000000ull, 0x1000ull, HOST_PAGE ) );
    CHECK( w.claim_size == HOST_PAGE );
    CHECK( ios_exewin_retire( &w, &b, &base, &size ) == 1 );

    /* a base that isn't host-page aligned, or a rounded size that doesn't fit, isn't claimed */
    CHECK( ios_exewin_claim( &w, &a, 0x140001000ull, 0x1000ull, HOST_PAGE ) == -1 );
    CHECK( ios_exewin_claim( &w, &a, 0x147ffc000ull, 0x4001ull, HOST_PAGE ) == -1 );
    CHECK( !ios_exewin_claim( &w, &a, 0x147ffc000ull, 0x4000ull, HOST_PAGE ) );

    /* no window: nothing claimed */
    memset( &w, 0, sizeof(w) );
    CHECK( ios_exewin_claim( &w, &a, 0x140000000ull, 0x1000ull, HOST_PAGE ) == -1 );
}

/* ---------- clarification 12: the exe window as several kernel regions ---------- */

#define MB128  0x8000000ull

struct fake_region { uint64_t start, size; int protection; };

struct fake_regions
{
    const struct fake_region *list;   /* sorted, not overlapping */
    unsigned int count, queries;
};

/* vm_region_64's rule: the region holding *addr, else the next one above it, else
 * KERN_INVALID_ADDRESS (1) */
static int fake_region_query( void *ctx, uint64_t *addr, uint64_t *size, int *protection )
{
    struct fake_regions *f = ctx;
    unsigned int i;

    f->queries++;
    for (i = 0; i < f->count; i++)
    {
        if (f->list[i].start + f->list[i].size <= *addr) continue;
        *addr = f->list[i].start;
        *size = f->list[i].size;
        *protection = f->list[i].protection;
        return 0;
    }
    return 1;
}

static int walk_regions( const struct fake_region *list, unsigned int count, uint64_t base,
                         uint64_t size, struct ios_exewin_walk *walk, char *line, size_t line_size )
{
    struct fake_regions f = { list, count, 0 };
    int ret = ios_exewin_walk( base, size, fake_region_query, &f, walk );

    ios_exewin_walk_describe( walk, line, line_size );
    return ret;
}

static void test_exewin_regions(void)
{
    static struct fake_region many[IOS_EXEWIN_MAX_REGIONS + 1];
    struct fake_region list[4];
    struct ios_exewin_walk walk;
    char line[256];
    unsigned int i;

    /* four adjacent 128 MB PROT_NONE regions: the 512 MB window of the device run */
    for (i = 0; i < 4; i++) list[i] = (struct fake_region){ 0x140000000ull + i * MB128, MB128, 0 };
    CHECK( !walk_regions( list, 4, 0x140000000ull, 4 * MB128, &walk, line, sizeof(line) ) );
    CHECK( walk.result == IOS_EXEWIN_WALK_OK && walk.regions == 4 );
    CHECK( walk.start == 0x140000000ull && walk.end == 0x160000000ull );
    CHECK_STR( line, "exe window check: kr 0, 4 regions 0x140000000-0x160000000, protection 0" );
    /* a smaller window inside them stops once it is covered */
    CHECK( !walk_regions( list, 4, 0x140000000ull, MB128 + 1, &walk, line, sizeof(line) ) );
    CHECK( walk.regions == 2 && walk.end == 0x150000000ull );

    /* one region: the 128 MB window of before, and a window starting inside a larger region */
    CHECK( !walk_regions( list, 1, 0x140000000ull, MB128, &walk, line, sizeof(line) ) );
    CHECK_STR( line, "exe window check: kr 0, 1 regions 0x140000000-0x148000000, protection 0" );
    CHECK( !walk_regions( list, 1, 0x144000000ull, 0x4000000ull, &walk, line, sizeof(line) ) );
    CHECK( walk.regions == 1 && walk.start == 0x140000000ull );

    /* a gap between two regions */
    list[1].start += 0x4000;
    CHECK( walk_regions( list, 4, 0x140000000ull, 4 * MB128, &walk, line, sizeof(line) ) == -1 );
    CHECK( walk.result == IOS_EXEWIN_WALK_GAP && walk.regions == 1 && walk.end == 0x148000000ull );
    CHECK_STR( line, "exe window check: kr 0, 1 regions 0x140000000-0x148000000, protection 0; "
               "refused: a gap at 0x148000000 (next region 0x148004000-0x150004000)" );
    list[1].start -= 0x4000;
    /* a first region starting above the base is a gap too */
    CHECK( walk_regions( list, 4, 0x13fffc000ull, 4 * MB128, &walk, line, sizeof(line) ) == -1 );
    CHECK( walk.result == IOS_EXEWIN_WALK_GAP && walk.regions == 0 );

    /* one region with read access (VM_PROT_READ is 1) */
    list[0].protection = 1;
    CHECK( walk_regions( list, 1, 0x140000000ull, MB128, &walk, line, sizeof(line) ) == -1 );
    CHECK( walk.result == IOS_EXEWIN_WALK_PROTECTION && walk.regions == 0 && walk.protection == 1 );
    CHECK_STR( line, "exe window check: kr 0, 0 regions 0x140000000-0x140000000, protection 1; "
               "refused: region 0x140000000-0x148000000 has protection 1" );
    /* and a later one */
    list[0].protection = 0;
    list[2].protection = 3;
    CHECK( walk_regions( list, 4, 0x140000000ull, 4 * MB128, &walk, line, sizeof(line) ) == -1 );
    CHECK( walk.result == IOS_EXEWIN_WALK_PROTECTION && walk.regions == 2 && walk.protection == 3 );
    list[2].protection = 0;

    /* the list ends short of base + size: the next query finds nothing */
    CHECK( walk_regions( list, 3, 0x140000000ull, 4 * MB128, &walk, line, sizeof(line) ) == -1 );
    CHECK( walk.result == IOS_EXEWIN_WALK_QUERY_FAILED && walk.kr == 1 && walk.regions == 3 );
    CHECK_STR( line, "exe window check: kr 1, 3 regions 0x140000000-0x158000000, protection -1; "
               "refused: vm_region_64 failed at 0x158000000" );
    /* no region at all */
    CHECK( walk_regions( list, 0, 0x140000000ull, MB128, &walk, line, sizeof(line) ) == -1 );
    CHECK( walk.result == IOS_EXEWIN_WALK_QUERY_FAILED && walk.regions == 0 );

    /* 65 regions: refused; 64 are still accepted */
    for (i = 0; i <= IOS_EXEWIN_MAX_REGIONS; i++)
        many[i] = (struct fake_region){ 0x140000000ull + i * HOST_PAGE, HOST_PAGE, 0 };
    CHECK( walk_regions( many, IOS_EXEWIN_MAX_REGIONS + 1, 0x140000000ull,
                         (IOS_EXEWIN_MAX_REGIONS + 1) * HOST_PAGE, &walk, line, sizeof(line) ) == -1 );
    CHECK( walk.result == IOS_EXEWIN_WALK_TOO_MANY && walk.regions == IOS_EXEWIN_MAX_REGIONS );
    CHECK_STR( line, "exe window check: kr 0, 64 regions 0x140000000-0x140100000, protection 0; "
               "refused: more than 64 regions" );
    CHECK( !walk_regions( many, IOS_EXEWIN_MAX_REGIONS + 1, 0x140000000ull,
                          IOS_EXEWIN_MAX_REGIONS * HOST_PAGE, &walk, line, sizeof(line) ) );
    CHECK( walk.regions == IOS_EXEWIN_MAX_REGIONS );

    /* the line fits a small buffer without overflowing */
    walk_regions( list, 4, 0x140000000ull, 4 * MB128, &walk, line, 20 );
    CHECK( strlen( line ) == 19 );
}

/* ---------- Part C ---------- */

static void test_dylib(void)
{
    char buf[256];

    CHECK_STR( ios_dylib_dir( 0x8664, "/app/pe", "/app/pe64" ), "/app/pe" );
    CHECK_STR( ios_dylib_dir( 0xaa64, "/app/pe", "/app/pe64" ), "/app/pe64" );
    CHECK( ios_dylib_dir( 0x014c, "/app/pe", "/app/pe64" ) == NULL );   /* i386: never */
    CHECK( ios_dylib_dir( 0x8664, NULL, "/app/pe64" ) == NULL );
    CHECK( ios_dylib_dir( 0x8664, "", "/app/pe64" ) == NULL );
    CHECK( ios_dylib_dir( 0xaa64, "/app/pe", NULL ) == NULL );

    CHECK( !ios_dylib_path( "/app/pe", "\\??\\C:\\windows\\system32\\NTDLL.dll", buf, sizeof(buf) ) );
    CHECK_STR( buf, "/app/pe/libntdll.dll.dylib" );
    CHECK( !ios_dylib_path( "/app/pe", "/x/y/XtaJit64.DLL", buf, sizeof(buf) ) );
    CHECK_STR( buf, "/app/pe/libxtajit64.dll.dylib" );
    CHECK( ios_dylib_path( "/app/pe", "C:\\windows\\", buf, sizeof(buf) ) == -1 );
    CHECK( ios_dylib_path( "/app/pe", "kernel32.dll", buf, 20 ) == -1 );
    CHECK( ios_dylib_path( NULL, "kernel32.dll", buf, sizeof(buf) ) == -1 );
}

/* ---------- Part B ---------- */

static void test_exec(void)
{
    int strip;

    /* signed code: a successful no-op */
    CHECK( ios_vm_exec_prot( IOS_PLACE_IMAGE_CODE, 5, &strip ) == -1 && !strip );
    CHECK( ios_vm_exec_prot( IOS_PLACE_IMAGE_CODE, 3, &strip ) == -1 );
    /* signed data: without PROT_EXEC */
    CHECK( ios_vm_exec_prot( IOS_PLACE_IMAGE_DATA, 7, &strip ) == 3 && strip );
    CHECK( ios_vm_exec_prot( IOS_PLACE_IMAGE_DATA, 1, &strip ) == 1 && !strip );
    /* guest window: x86 code is data; execute alone becomes readable */
    CHECK( ios_vm_exec_prot( IOS_PLACE_GUEST, 5, &strip ) == 1 );
    CHECK( ios_vm_exec_prot( IOS_PLACE_GUEST, 4, &strip ) == 1 );
    CHECK( ios_vm_exec_prot( IOS_PLACE_GUEST, 7, &strip ) == 3 );
    CHECK( ios_vm_exec_prot( IOS_PLACE_GUEST, 0, &strip ) == 0 );
    /* elsewhere: without PROT_EXEC, and reported for the log */
    CHECK( ios_vm_exec_prot( IOS_PLACE_OTHER, 5, &strip ) == 1 && strip );
    CHECK( ios_vm_exec_prot( IOS_PLACE_OTHER, 3, &strip ) == 3 && !strip );
}

/* ---------- clarification 1: low windows ---------- */

static void test_lowwin(void)
{
    static int peb1, peb2;   /* PEBs: only their addresses are used */

    ios_lowwin_reset();
    CHECK( !ios_lowwin_add( 0x7ffe0000, 0x1000, 0x280000000ull, NULL ) );          /* shared data */
    CHECK( !ios_lowwin_add( 0x400000, 0x200000, 0x300000000ull, &peb1 ) );         /* two programs */
    CHECK( !ios_lowwin_add( 0x400000, 0x100000, 0x310000000ull, &peb2 ) );         /* at 0x400000 */

    /* the shared data page for everybody */
    CHECK( ios_lowwin_lookup( 0x7ffe0014, &peb1 ) == 0x280000014ull );
    CHECK( ios_lowwin_lookup( 0x7ffe0014, &peb2 ) == 0x280000014ull );
    CHECK( ios_lowwin_lookup( 0x7ffe0014, NULL ) == 0x280000014ull );
    CHECK( ios_lowwin_lookup( 0x7ffe1000, &peb1 ) == 0 );

    /* each process its own image */
    CHECK( ios_lowwin_lookup( 0x401000, &peb1 ) == 0x300001000ull );
    CHECK( ios_lowwin_lookup( 0x401000, &peb2 ) == 0x310001000ull );
    CHECK( ios_lowwin_lookup( 0x550000, &peb1 ) == 0x300150000ull );
    CHECK( ios_lowwin_lookup( 0x550000, &peb2 ) == 0 );               /* beyond peb2's image */
    CHECK( ios_lowwin_lookup( 0x401000, (void *)&checks ) == 0 );     /* another process: none */
    CHECK( ios_lowwin_lookup( 0x401000, NULL ) != 0 );                /* no PEB known: any */

    /* a process's windows go when it ends; the shared one stays */
    ios_lowwin_remove_owner( &peb1 );
    CHECK( ios_lowwin_lookup( 0x401000, &peb1 ) == 0 );
    CHECK( ios_lowwin_lookup( 0x401000, &peb2 ) == 0x310001000ull );
    CHECK( ios_lowwin_lookup( 0x7ffe0000, &peb1 ) == 0x280000000ull );
    ios_lowwin_remove_owner( NULL );                                  /* never the shared ones */
    CHECK( ios_lowwin_lookup( 0x7ffe0000, NULL ) == 0x280000000ull );

    /* full table */
    ios_lowwin_reset();
    for (int i = 0; i < IOS_LOWWIN_SLOTS; i++) CHECK( !ios_lowwin_add( 0x10000ull * (i + 1), 0x1000, 0x400000000ull, &peb1 ) );
    CHECK( ios_lowwin_add( 0x7ffe0000, 0x1000, 0x280000000ull, NULL ) == -1 );
    ios_lowwin_reset();
}

/* readers never see a half-written window while a writer keeps reusing the same slot for two
 * different windows: {0x400000, 4 KB, 0x500000000} and {0x400000, 8 KB, 0x600000000}. At
 * 0x401800 only the second covers the address, so a torn read (8 KB with the first's real
 * address) gives 0x500001800. */
#define ROUNDS 20000

static _Atomic int stop_readers;
static _Atomic int bad_reads;

static void *reader( void *arg )
{
    while (!atomic_load( &stop_readers ))
    {
        uint64_t real = ios_lowwin_lookup( 0x401800, arg );
        if (real && real != 0x600001800ull) atomic_fetch_add( &bad_reads, 1 );
        real = ios_lowwin_lookup( 0x400010, arg );
        if (real && real != 0x500000010ull && real != 0x600000010ull) atomic_fetch_add( &bad_reads, 1 );
        real = ios_lowwin_lookup( 0x7ffe0008, arg );
        if (real != 0x280000008ull) atomic_fetch_add( &bad_reads, 1 );
    }
    return NULL;
}

static void test_lowwin_threads(void)
{
    pthread_t threads[4];
    static int peb;   /* a PEB: only its address is used */
    int i;

    ios_lowwin_reset();
    ios_lowwin_add( 0x7ffe0000, 0x1000, 0x280000000ull, NULL );
    for (i = 0; i < 4; i++) pthread_create( &threads[i], NULL, reader, &peb );
    for (i = 0; i < ROUNDS; i++)
    {
        if (i & 1) ios_lowwin_add( 0x400000, 0x2000, 0x600000000ull, &peb );
        else ios_lowwin_add( 0x400000, 0x1000, 0x500000000ull, &peb );
        ios_lowwin_remove_owner( &peb );
    }
    atomic_store( &stop_readers, 1 );
    for (i = 0; i < 4; i++) pthread_join( threads[i], NULL );
    CHECK( atomic_load( &bad_reads ) == 0 );
    ios_lowwin_reset();
}

/* clarification 4 (spec 18 Part B1): finding ntdll's ios_teb_tsd_offset in a synthetic image */
static void pe_put32( unsigned char *p, uint32_t v )
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void build_pe( unsigned char *img, int pe32plus )
{
    unsigned int dir = 0x80 + 24 + (pe32plus ? 112 : 96);

    memset( img, 0, 0x1000 );
    img[0] = 'M'; img[1] = 'Z';
    pe_put32( img + 0x3c, 0x80 );
    img[0x80] = 'P'; img[0x81] = 'E';
    img[0x80 + 24] = 0x0b; img[0x80 + 25] = pe32plus ? 0x02 : 0x01;
    pe_put32( img + dir, 0x400 );       /* export directory */
    pe_put32( img + dir + 4, 0x100 );
    pe_put32( img + 0x400 + 24, 2 );    /* NumberOfNames */
    pe_put32( img + 0x400 + 28, 0x500 );/* AddressOfFunctions */
    pe_put32( img + 0x400 + 32, 0x520 );/* AddressOfNames */
    pe_put32( img + 0x400 + 36, 0x540 );/* AddressOfNameOrdinals */
    pe_put32( img + 0x500, 0x800 );     /* function 0 */
    pe_put32( img + 0x504, 0x900 );     /* function 1 */
    pe_put32( img + 0x520, 0x600 );     /* name 0 */
    pe_put32( img + 0x524, 0x620 );     /* name 1 */
    img[0x540] = 1; img[0x542] = 0;     /* name 0 -> ordinal 1, name 1 -> ordinal 0 */
    strcpy( (char *)img + 0x600, "ios_teb_tsd_offset" );
    strcpy( (char *)img + 0x620, "RtlWow64SuspendThread" );
}

static void test_pe_export(void)
{
    static unsigned char img[0x1000];

    build_pe( img, 1 );
    CHECK( ios_pe_find_export( img, sizeof(img), "ios_teb_tsd_offset" ) == img + 0x900 );
    CHECK( ios_pe_find_export( img, sizeof(img), "RtlWow64SuspendThread" ) == img + 0x800 );
    CHECK( !ios_pe_find_export( img, sizeof(img), "ios_teb_tsd" ) );
    CHECK( !ios_pe_find_export( img, sizeof(img), "missing" ) );
    build_pe( img, 0 );
    CHECK( ios_pe_find_export( img, sizeof(img), "ios_teb_tsd_offset" ) == img + 0x900 );

    /* bounds and format failures */
    build_pe( img, 1 );
    CHECK( !ios_pe_find_export( img, 0x480, "ios_teb_tsd_offset" ) );    /* exports past the end */
    CHECK( !ios_pe_find_export( NULL, sizeof(img), "ios_teb_tsd_offset" ) );
    pe_put32( img + 0x504, 0x2000 );                                     /* function outside */
    CHECK( !ios_pe_find_export( img, sizeof(img), "ios_teb_tsd_offset" ) );
    pe_put32( img + 0x504, 0x410 );                                      /* forwarder */
    CHECK( !ios_pe_find_export( img, sizeof(img), "ios_teb_tsd_offset" ) );
    build_pe( img, 1 );
    img[0] = 'X';
    CHECK( !ios_pe_find_export( img, sizeof(img), "ios_teb_tsd_offset" ) );
    build_pe( img, 1 );
    pe_put32( img + 0x3c, 0xff0 );
    CHECK( !ios_pe_find_export( img, sizeof(img), "ios_teb_tsd_offset" ) );
    build_pe( img, 1 );
    pe_put32( img + 0x400 + 24, 0x40000000 );                            /* absurd name count */
    CHECK( !ios_pe_find_export( img, sizeof(img), "ios_teb_tsd_offset" ) );
}

/* ---------- clarification 9: only our builtins take their dylib (public-18 device run) ----------
 * The four cases of the clarification. "marked": the server found Wine's builtin signature;
 * "as builtin": the unix loader loads it from the bundle's farm; the path is where the PE
 * loader found the file. Only the file decides, never a size. */
static void test_native_copy(void)
{
    /* clarification 9, images without native ARM code (plain x64: 0x8664, no ARM64EC metadata) */
    /* a builtin-marked image that has a dylib */
    CHECK( ios_image_takes_dylib( 1, 0, 0x8664, 0 ) );
    /* an unmarked same-named file outside the system directories (Microsoft's
     * D3DCompiler_47.dll in the game's folder): mapped from its file */
    CHECK( !ios_image_takes_dylib( 0, 0, 0x8664, 0 ) );
    /* a marked file in the game folder (a copied builtin): the dylib */
    CHECK( ios_image_takes_dylib( 1, 0, 0x8664, 0 ) );
    /* an unmarked file in system32 (a user replaced a builtin): its file */
    CHECK( !ios_image_takes_dylib( 0, 0, 0x8664, 0 ) );
    /* the unix loader's own builtins from the bundle's farms (ntdll, apisets): the dylib */
    CHECK( ios_image_takes_dylib( 0, 1, 0x8664, 0 ) );
    CHECK( ios_image_takes_dylib( 1, 1, 0x8664, 0 ) );

    /* clarification 9a (public-20 device run: xtajit.dll and xtajit64.dll, our CPU modules
     * copied into system32 without a Wine marker, were mapped from their files and the app
     * died): native ARM code takes its dylib whatever its marker or path */
    /* an unmarked ARM64EC image (x64 machine with ARM64EC metadata) in system32 */
    CHECK( ios_image_takes_dylib( 0, 0, 0x8664, 1 ) );
    /* an unmarked ARM64 image (xtajit.dll), and an ARM64X one */
    CHECK( ios_image_takes_dylib( 0, 0, 0xaa64, 0 ) );
    CHECK( ios_image_takes_dylib( 0, 0, 0xaa64, 1 ) );
    /* an unmarked plain-x64 same-named file in the game folder: still its file */
    CHECK( !ios_image_takes_dylib( 0, 0, 0x8664, 0 ) );
    /* i386 code is data for the CPU module: by the marker rule (the dylib lookup has none) */
    CHECK( !ios_image_takes_dylib( 0, 0, 0x014c, 0 ) );
}

/* ---------- clarification 10: no second instance in the same process ---------- */

/* ---------- clarification 13: allocation lines ---------- */

static void test_alloc_log(void)
{
    const unsigned int commit = 0x1000, reserve = 0x2000, top_down = 0x100000;

    /* a failed call: logged, whatever it asked for */
    CHECK( ios_alloc_log_kind( commit, 1, 0xc0000018 /* conflicting addresses */ ) == IOS_ALLOC_LOG_FAILED );
    CHECK( ios_alloc_log_kind( reserve | commit, 0, 0xc0000017 /* no memory */ ) == IOS_ALLOC_LOG_FAILED );
    CHECK( ios_alloc_log_kind( commit, 0, 0xc000000d ) == IOS_ALLOC_LOG_FAILED );
    /* MEM_RESERVE at a given base: logged on success and on failure */
    CHECK( ios_alloc_log_kind( reserve, 1, 0 ) == IOS_ALLOC_LOG_FIXED );
    CHECK( ios_alloc_log_kind( reserve | commit | top_down, 1, 0 ) == IOS_ALLOC_LOG_FIXED );
    CHECK( ios_alloc_log_kind( reserve, 1, 0xc0000018 ) == IOS_ALLOC_LOG_FAILED );
    /* MEM_COMMIT inside an existing reservation, succeeding: not logged */
    CHECK( ios_alloc_log_kind( commit, 1, 0 ) == IOS_ALLOC_LOG_NONE );
    /* MEM_RESERVE with a base of 0, succeeding: not logged */
    CHECK( ios_alloc_log_kind( reserve, 0, 0 ) == IOS_ALLOC_LOG_NONE );
    CHECK( ios_alloc_log_kind( reserve | commit, 0, 0 ) == IOS_ALLOC_LOG_NONE );

}

/* ---------- clarification 14 B: the log folds runs ---------- */

static char alloc_lines[600][256];
static unsigned int alloc_line_count;

static void collect_line( void *ctx, const char *line )
{
    (void)ctx;
    if (alloc_line_count < sizeof(alloc_lines) / sizeof(alloc_lines[0]))
        snprintf( alloc_lines[alloc_line_count], sizeof(alloc_lines[0]), "%s", line );
    alloc_line_count++;
}

static struct ios_alloc_event failure( uint64_t base, uint32_t status )
{
    struct ios_alloc_event ev;

    memset( &ev, 0, sizeof(ev) );
    ev.base = base;
    ev.size = 0x10000;
    ev.type = 0x3000;   /* MEM_COMMIT | MEM_RESERVE */
    ev.prot = 0x40;     /* PAGE_EXECUTE_READWRITE */
    ev.status = status;
    ev.got = base;
    ev.got_size = 0x10000;
    return ev;
}

static void log_event( struct ios_alloc_log *log, const struct ios_alloc_event *ev )
{
    ios_alloc_log_event( log, ios_alloc_log_kind( ev->type, ev->base != 0, ev->status ), ev,
                         collect_line, NULL );
}

static void test_alloc_fold(void)
{
    struct ios_alloc_log log;
    struct ios_alloc_event ev;
    unsigned int i;

    /* the device run: 300 failures walking down from 0x123b10000 in 64 KB steps: one line,
     * written when the run ends (here at the next call, a successful commit) */
    memset( &log, 0, sizeof(log) );
    alloc_line_count = 0;
    for (i = 0; i < 300; i++)
    {
        ev = failure( 0x123b10000ull - (uint64_t)i * 0x10000, 0xc0000018 );
        log_event( &log, &ev );
    }
    CHECK( alloc_line_count == 0 );   /* the run isn't over yet */
    ev = failure( 0x200000000ull, 0 );
    ev.type = 0x1000;
    log_event( &log, &ev );
    CHECK( alloc_line_count == 1 );
    CHECK_STR( alloc_lines[0], "alloc failed x300: bases 0x123b10000..0x122860000 step -0x10000 "
                               "size 0x10000 type 0x3000 prot 0x40 status 0xc0000018" );

    /* a failure with another status breaks the run; a run of one is clarification 13's line */
    memset( &log, 0, sizeof(log) );
    alloc_line_count = 0;
    for (i = 0; i < 3; i++)
    {
        ev = failure( 0x150000000ull + (uint64_t)i * 0x20000, 0xc0000018 );
        log_event( &log, &ev );
    }
    ev = failure( 0x150060000ull, 0xc0000017 );
    log_event( &log, &ev );
    CHECK( alloc_line_count == 1 );
    CHECK_STR( alloc_lines[0], "alloc failed x3: bases 0x150000000..0x150040000 step 0x20000 "
                               "size 0x10000 type 0x3000 prot 0x40 status 0xc0000018" );
    ios_alloc_log_flush( &log, collect_line, NULL );
    CHECK( alloc_line_count == 2 );
    CHECK_STR( alloc_lines[1], "alloc failed: base 0x150060000 size 0x10000 type 0x3000 prot 0x40 "
                               "zero_bits 0 status 0xc0000017" );
    ios_alloc_log_flush( &log, collect_line, NULL );   /* nothing left */
    CHECK( alloc_line_count == 2 );

    /* a different step breaks the run too */
    memset( &log, 0, sizeof(log) );
    alloc_line_count = 0;
    ev = failure( 0x160000000ull, 0xc0000018 ); log_event( &log, &ev );
    ev = failure( 0x160010000ull, 0xc0000018 ); log_event( &log, &ev );
    ev = failure( 0x160030000ull, 0xc0000018 ); log_event( &log, &ev );
    CHECK( alloc_line_count == 1 );
    CHECK( !strncmp( alloc_lines[0], "alloc failed x2: bases 0x160000000..0x160010000 step 0x10000 ", 61 ) );

    /* after a folded run, a reservation at a given base still gets its line */
    memset( &log, 0, sizeof(log) );
    alloc_line_count = 0;
    for (i = 0; i < 10; i++)
    {
        ev = failure( 0x170000000ull - (uint64_t)i * 0x10000, 0xc0000018 );
        log_event( &log, &ev );
    }
    ev = failure( 0x180000000ull, 0 );
    ev.type = 0x2000;
    log_event( &log, &ev );
    CHECK( alloc_line_count == 2 );
    CHECK( !strncmp( alloc_lines[0], "alloc failed x10: ", 18 ) );
    CHECK_STR( alloc_lines[1], "alloc at 0x180000000 size 0x10000 type 0x2000 prot 0x40: status 0, "
                               "got 0x180000000 size 0x10000" );

    /* a run is written at 4096 events */
    memset( &log, 0, sizeof(log) );
    alloc_line_count = 0;
    for (i = 0; i < 4097; i++)
    {
        ev = failure( 0x400000000ull + (uint64_t)i * 0x10000, 0xc0000018 );
        log_event( &log, &ev );
    }
    CHECK( alloc_line_count == 1 );
    CHECK( !strncmp( alloc_lines[0], "alloc failed x4096: ", 20 ) );
    CHECK( log.count == 1 );   /* the 4097th starts a new run */

    /* the cap counts written lines: 512, then the 513th is the suppression line, then nothing */
    memset( &log, 0, sizeof(log) );
    alloc_line_count = 0;
    for (i = 0; i < 600; i++)
    {
        ev = failure( 0x190000000ull + (uint64_t)i * 0x10000, 0 );
        ev.type = 0x2000;
        log_event( &log, &ev );
    }
    CHECK( alloc_line_count == 513 );
    CHECK( !strncmp( alloc_lines[511], "alloc at ", 9 ) );
    CHECK_STR( alloc_lines[512], "alloc: further allocation lines suppressed (512 so far)" );
}

/* ---------- clarification 14 A: memory outside Wine's views ---------- */

static void test_free_region(void)
{
    uint64_t ab, end;

    /* an address inside a host region: reserved, with 64 KB bounds, clipped to the next view */
    CHECK( ios_free_region_answer( 0x123b10000ull, 0x140000000ull, 0, 1, 0x123a04000ull, 0x123b24000ull,
                                   &ab, &end ) == IOS_FREE_RESERVED );
    CHECK( ab == 0x123a00000ull && end == 0x123b30000ull );
    CHECK( ios_free_region_answer( 0x123b10000ull, 0x123b20000ull, 0, 1, 0x123a04000ull, 0x123b24000ull,
                                   &ab, &end ) == IOS_FREE_RESERVED );
    CHECK( end == 0x123b20000ull );
    /* an address below a host region: free up to the region's start rounded down */
    CHECK( ios_free_region_answer( 0x120000000ull, 0x140000000ull, 0, 1, 0x120234000ull, 0x120300000ull,
                                   &ab, &end ) == IOS_FREE_FREE );
    CHECK( end == 0x120230000ull );
    /* the next view comes first: free up to it */
    CHECK( ios_free_region_answer( 0x120000000ull, 0x120100000ull, 0, 1, 0x120234000ull, 0x120300000ull,
                                   &ab, &end ) == IOS_FREE_FREE );
    CHECK( end == 0x120100000ull );
    /* a free gap shorter than 64 KB: the block is in use, reserved */
    CHECK( ios_free_region_answer( 0x120004000ull, 0x140000000ull, 0, 1, 0x12000c000ull, 0x120020000ull,
                                   &ab, &end ) == IOS_FREE_RESERVED );
    CHECK( ab == 0x120000000ull && end == 0x120020000ull );
    CHECK( ios_free_region_answer( 0x120004000ull, 0x120008000ull, 0, 0, 0, 0, &ab, &end ) == IOS_FREE_RESERVED );
    CHECK( ab == 0x120000000ull && end == 0x120008000ull );
    /* an address inside the exe window (or another range Wine keeps): upstream's answer */
    CHECK( ios_free_region_answer( 0x150000000ull, 0x160000000ull, 1, 1, 0x140000000ull, 0x160000000ull,
                                   &ab, &end ) == IOS_FREE_UPSTREAM );
    CHECK( ab == 0 && end == 0x160000000ull );
    /* no host region at all: free up to the next view */
    CHECK( ios_free_region_answer( 0x300000000ull, 0x380000000ull, 0, 0, 0, 0, &ab, &end ) == IOS_FREE_FREE );
    CHECK( end == 0x380000000ull );
}

/* ---------- clarification 15: large memory events ---------- */

static void test_mem_events(void)
{
    unsigned int lines = 0, i, written = 0, suppressed = 0, none = 0;

    /* 1 MB or more: logged; 1 MB minus 64 KB: not */
    CHECK( ios_mem_event_wanted( IOS_MEM_ALLOC, 0x100000 ) );
    CHECK( ios_mem_event_wanted( IOS_MEM_FREE, 0x100000 ) );
    CHECK( ios_mem_event_wanted( IOS_MEM_MAP, 0x1904000 ) );
    CHECK( ios_mem_event_wanted( IOS_MEM_UNMAP, 0x100000 ) );
    CHECK( !ios_mem_event_wanted( IOS_MEM_ALLOC, 0xf0000 ) );
    CHECK( !ios_mem_event_wanted( IOS_MEM_FREE, 0xf0000 ) );
    CHECK( !ios_mem_event_wanted( IOS_MEM_MAP, 0xf0000 ) );
    CHECK( !ios_mem_event_wanted( IOS_MEM_UNMAP, 0xf0000 ) );
    /* an image of 64 KB: logged */
    CHECK( ios_mem_event_wanted( IOS_MEM_IMAGE, 0x10000 ) );
    CHECK( !ios_mem_event_wanted( 0, 0x10000000 ) );
    /* 256 lines, then the suppression line once, then nothing */
    for (i = 0; i < 300; i++)
    {
        switch (ios_mem_event_take( &lines ))
        {
        case IOS_MEM_LINE_WRITE:    CHECK( !suppressed ); written++; break;
        case IOS_MEM_LINE_SUPPRESS: suppressed++; break;
        default:                    CHECK( suppressed == 1 ); none++; break;
        }
    }
    CHECK( written == 256 && suppressed == 1 && none == 43 );
}

/* clarification 16: the address an image view's caller asked for */
static void test_image_asked(void)
{
    char buf[96], tiny[12];

    CHECK( ios_image_asked_base( 0 ) == 0 );
    CHECK( ios_image_asked_base( 0x7000012345ull ) == 0x7000010000ull );
    CHECK( ios_image_asked_base( 0x150000000ull ) == 0x150000000ull );
    CHECK( ios_image_asked_base( 0xffffull ) == 0 );
    /* at its preferred base: nothing added */
    CHECK_STR( ios_image_asked_note( buf, sizeof(buf), 0x150000000ull, 0, 0x140000000ull, 0x140000000ull ), "" );
    CHECK_STR( ios_image_asked_note( buf, sizeof(buf), 0, 0, 0x7096400000ull, 0x140000000ull ), ", asked any" );
    CHECK_STR( ios_image_asked_note( buf, sizeof(buf), 0x150000000ull, 0, 0x7096400000ull, 0x140000000ull ),
               ", asked 0x150000000" );
    CHECK_STR( ios_image_asked_note( buf, sizeof(buf), 0, 0x7fffffffull, 0x7096400000ull, 0x140000000ull ),
               ", asked any, limit 0x7fffffff" );
    CHECK_STR( ios_image_asked_note( buf, sizeof(buf), 0x60000000ull, 0xffffffffull, 0x7096400000ull, 0x140000000ull ),
               ", asked 0x60000000, limit 0xffffffff" );
    /* a short buffer stays terminated */
    ios_image_asked_note( tiny, sizeof(tiny), 0x150000000ull, 0x7fffffffull, 0x7096400000ull, 0x140000000ull );
    CHECK( strlen( tiny ) == sizeof(tiny) - 1 );
    CHECK( ios_image_asked_note( buf, 0, 0, 0, 1, 2 ) == buf );
}

static void test_loaded_again(void)
{
    static const char *const advapi_names[] =
    {
        "advapi32.dll",                                   /* LoadLibraryW by base name */
        "ADVAPI32.DLL",
        "\\??\\C:\\windows\\system32\\advapi32.dll",      /* full path, LOAD_LIBRARY_SEARCH_SYSTEM32 */
        "C:\\Windows\\System32\\AdvApi32.dll",
        "\\??\\C:\\windows\\sysx64\\advapi32.dll",        /* the farm the loader fell back to */
        "C:/windows/system32/advapi32.dll",
    };
    char first[256], other[256];
    int owner_a = 0, owner_b = 0;
    unsigned int i;

    /* every way of asking for the DLL gives the dylib the loaded copy came from (a Godot/.NET 9
     * game: CoreCLR asked again for advapi32, kernel32 and ntdll and got second instances) */
    CHECK( !ios_dylib_path( "/app/PE/wine", advapi_names[0], first, sizeof(first) ) );
    CHECK_STR( first, "/app/PE/wine/libadvapi32.dll.dylib" );
    for (i = 1; i < sizeof(advapi_names) / sizeof(advapi_names[0]); i++)
    {
        CHECK( !ios_dylib_path( "/app/PE/wine", advapi_names[i], other, sizeof(other) ) );
        CHECK_STR( other, first );
    }
    /* another DLL, or the same name for the other machine's directory, is another dylib */
    CHECK( !ios_dylib_path( "/app/PE/wine", "\\??\\C:\\windows\\system32\\advapi32res.dll", other, sizeof(other) ) );
    CHECK( strcmp( other, first ) );
    CHECK( !ios_dylib_path( "/app/PE/wine", "kernel32.dll", other, sizeof(other) ) );
    CHECK( strcmp( other, first ) );
    CHECK( !ios_dylib_path( "/app/PE/aarch64", "advapi32.dll", other, sizeof(other) ) );
    CHECK( strcmp( other, first ) );

    /* the calling process's own view at the instance's start is handed back */
    CHECK( ios_dylib_view_reusable( 1, 1, &owner_a, &owner_a ) );
    /* another pseudo-process's instance: a new instance for this one (Part C4) */
    CHECK( !ios_dylib_view_reusable( 1, 1, &owner_b, &owner_a ) );
    /* a view that starts elsewhere (the instance's view is gone, another lies over its address),
     * or that isn't a dylib image view: not the loaded copy */
    CHECK( !ios_dylib_view_reusable( 0, 1, &owner_a, &owner_a ) );
    CHECK( !ios_dylib_view_reusable( 1, 0, &owner_a, &owner_a ) );
    /* the app's own views (no owner) never count */
    CHECK( !ios_dylib_view_reusable( 1, 1, NULL, NULL ) );
}

/* ---------- clarification 8: a DLL's 64-bit machine against its process ---------- */

static void test_image_machine(void)
{
    /* the device run: an ARM64 program got system32's ARM64EC kernel32 (0x8664): refused */
    CHECK( !ios_image_fits_process( 0xaa64, 0x8664, 0 ) );
    CHECK( !ios_image_fits_process( 0xaa64, 0x8664, 1 ) );
    /* an ARM64 program takes ARM64 images, hybrid or not */
    CHECK( ios_image_fits_process( 0xaa64, 0xaa64, 0 ) );
    CHECK( ios_image_fits_process( 0xaa64, 0xaa64, 1 ) );
    /* an x64 program takes the ARM64EC builds and ARM64X images, not plain ARM64 ones */
    CHECK( ios_image_fits_process( 0x8664, 0x8664, 0 ) );
    CHECK( ios_image_fits_process( 0x8664, 0xaa64, 1 ) );
    CHECK( !ios_image_fits_process( 0x8664, 0xaa64, 0 ) );
    /* clarification 11 (a kept session's 32-bit game mapped the ARM64EC win32u.dll beside its
     * ARM64 side): a WoW64 process refuses an x64-machine image with ARM64EC metadata */
    CHECK( !ios_image_fits_process( 0x014c, 0x8664, 1 ) );
    /* and keeps upstream's rules for plain x64, ARM64 (hybrid or not) and 32-bit images */
    CHECK( ios_image_fits_process( 0x014c, 0x8664, 0 ) );
    CHECK( ios_image_fits_process( 0x014c, 0xaa64, 0 ) );
    CHECK( ios_image_fits_process( 0x014c, 0xaa64, 1 ) );
    CHECK( ios_image_fits_process( 0x014c, 0x014c, 0 ) );
    /* a main image not known yet accepts every image */
    CHECK( ios_image_fits_process( 0, 0x8664, 0 ) );
    CHECK( ios_image_fits_process( 0, 0x8664, 1 ) );
    CHECK( ios_image_fits_process( 0, 0xaa64, 0 ) );
    CHECK( ios_image_fits_process( 0, 0xaa64, 1 ) );
    CHECK( ios_image_fits_process( 0, 0x014c, 0 ) );
    /* images that aren't 64-bit are left alone */
    CHECK( ios_image_fits_process( 0xaa64, 0x014c, 0 ) );
    CHECK( ios_image_fits_process( 0x8664, 0x014c, 0 ) );
}

/* ---------- Part E4: the window holds only guest requests (public-4 device run) ---------- */

/* where map_reserved_area puts a general allocation, as on iOS: the first reserved area the
 * request may use. Areas: the process's own guest window, then an ordinary reservation above
 * it. Returns the address, or 0 when it would go to the free address space instead. */
static uint64_t mock_place( uint64_t window, uint64_t other, uint64_t other_size,
                            uint64_t range_low, uint64_t range_high, uint64_t size )
{
    /* the own window: only for a request the rule allows */
    if (range_low < window + IOS_4G && range_high > window &&
        ios_win_request_in_window( window, range_low, range_high ))
        return range_low > window ? range_low : window;
    if (range_low < other + other_size && range_high > other && other_size >= size)
        return range_low > other ? range_low : other;
    return 0;
}

static void test_window_requests(void)
{
    const uint64_t b = 0x7000000000ull, other = 0x9000000000ull;
    const uint64_t user_low = IOS_LOWEST_ADDRESS, user_high = 0x7fffffff0000ull;
    uint64_t usd;

    /* the rule */
    CHECK( ios_win_request_in_window( b, b + 0x10000, b + 0x80000000ull ) );     /* guest, 2 GB */
    CHECK( ios_win_request_in_window( b, b + 0x10000, b + IOS_4G ) );            /* guest, 4 GB */
    CHECK( !ios_win_request_in_window( b, user_low, user_high ) );               /* no limit */
    CHECK( !ios_win_request_in_window( b, b, b + IOS_4G + 0x1000 ) );            /* past it */
    CHECK( !ios_win_request_in_window( 0, b, b + 0x1000 ) );
    CHECK( !ios_win_request_in_window( b, b + 0x2000, b + 0x2000 ) );

    /* the shared data page (a unix-side request with no limit) lands outside [B, B + 4 GB):
     * in the device run it went to B, the 32-bit program's NULL page */
    usd = mock_place( b, other, 0x100000, user_low, user_high, 0x1000 );
    CHECK( usd && (usd < b || usd >= b + IOS_4G) );
    CHECK( usd == other );

    /* a guest request (translated: [B + 64 KB, B + 2 GB)) still lands in the window */
    CHECK( mock_place( b, other, 0x100000, b + 0x10000, b + 0x80000000ull, 0x1000 ) == b + 0x10000 );

    /* with real memory: a window and another area, the page placed by the same rule, then the
     * window's low 64 KB made inaccessible as for the 32-bit program's NULL region. The page
     * is still readable (TickCount's offset); placed in the window it would fault here. */
    {
        unsigned char *win = mmap( NULL, 0x20000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
        unsigned char *area = mmap( NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
        unsigned char *page;

        CHECK( win != MAP_FAILED && area != MAP_FAILED );
        if (win == MAP_FAILED || area == MAP_FAILED) return;
        page = (unsigned char *)(uintptr_t)mock_place( (uintptr_t)win, (uintptr_t)area, 0x4000,
                                                       0x10000, ~(uint64_t)0 >> 1, 0x1000 );
        CHECK( page == area );
        if (page != area) return;
        memset( page, 0x5a, 0x1000 );
        CHECK( !mprotect( win, 0x10000, PROT_NONE ) );
        CHECK( page[0x320] == 0x5a );
        munmap( win, 0x20000 );
        munmap( area, 0x4000 );
    }
}

/* ---------- Part E4: builtins named by guest address (fifth device run) ---------- */

/* the lookup get_builtin_module does on iOS: the first registered builtin that matches */
static int mock_find_builtin( const uint64_t *registered, int count, uint64_t query, uint64_t wow_base )
{
    int i;

    for (i = 0; i < count; i++) if (ios_module_match( registered[i], query, wow_base )) return i;
    return -1;
}

static void test_module_match(void)
{
    const uint64_t b1 = 0x700000000ull, b2 = 0x900000000ull;   /* two 32-bit processes' windows */
    const uint64_t registered[] =
    {
        b1 + 0xfead0000ull,          /* the 32-bit winemetal.dll of process 1 */
        b2 + 0xfead0000ull,          /* the same DLL at the same guest base in process 2 */
        0x180000000ull,              /* a 64-bit builtin (ntdll of a 64-bit process) */
    };

    /* WoW64 asks with the guest address: found, in the caller's own window */
    CHECK( mock_find_builtin( registered, 3, 0xfead0000ull, b1 ) == 0 );
    CHECK( mock_find_builtin( registered, 3, 0xfead0000ull, b2 ) == 1 );
    /* and with the host address */
    CHECK( mock_find_builtin( registered, 3, b1 + 0xfead0000ull, b1 ) == 0 );
    CHECK( mock_find_builtin( registered, 3, b2 + 0xfead0000ull, b1 ) == 1 );
    /* a 64-bit process: host addresses only; a guest number means nothing there */
    CHECK( mock_find_builtin( registered, 3, 0x180000000ull, 0 ) == 2 );
    CHECK( mock_find_builtin( registered, 3, 0xfead0000ull, 0 ) == -1 );
    /* a guest address that isn't a module of this process, and NULL */
    CHECK( mock_find_builtin( registered, 3, 0xfeac0000ull, b1 ) == -1 );
    CHECK( mock_find_builtin( registered, 3, 0, b1 ) == -1 );
    /* a host address above 4 GB is never shifted by the window */
    CHECK( !ios_module_match( b1 + 0x180000000ull, 0x180000000ull, b1 ) );
}

/* ---------- clarification 7: shared image sections ---------- */

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define SCN_SHARED  0x10000000u
#define SCN_EXECUTE 0x20000000u
#define SCN_READ    0x40000000u
#define SCN_WRITE   0x80000000u
#define PROT_R 1
#define PROT_W 2   /* write-copy, as the loader gives every writable section */
#define PROT_X 4

struct mock_section { uint32_t rva, vsize, raw_ptr, raw_size, chars; };

/* the GameMaker layout of the fourth device run: 4 KB alignment, a writable shared .mydata
 * between two ordinary sections, 16 KB host pages */
static const struct mock_section mock_sections[] =
{
    { 0x1000, 0x1000, 0x400, 0x200, SCN_READ | SCN_EXECUTE },              /* .text   */
    { 0x2000, 0x1000, 0x600, 0x100, SCN_READ | SCN_WRITE | SCN_SHARED },   /* .mydata */
    { 0x3000, 0x1000, 0x800, 0x200, SCN_READ | SCN_WRITE },                /* .data   */
};
#define MOCK_IMAGE 0x4000
#define MOCK_HOST_PAGE 0x4000

static unsigned char mock_file[0xa00];

/* maps the mock image the way map_image_into_view does on iOS: a writable shared section
 * keeps a shared mapping only when ios_shared_section_shareable allows it (not here, so the
 * test fails if it did); every other section is copied from the file and zero-filled, then all
 * get their protections from their characteristics. Returns how many sections went shared. */
static int mock_load( unsigned char *image, unsigned char *prot_per_4k )
{
    unsigned int i, shared = 0;

    memset( image, 0xcc, MOCK_IMAGE );
    for (i = 0; i < ARRAY_SIZE(mock_sections); i++)
    {
        const struct mock_section *s = &mock_sections[i];

        if ((s->chars & SCN_SHARED) && (s->chars & SCN_WRITE) &&
            ios_shared_section_shareable( s->rva, s->vsize, MOCK_HOST_PAGE ))
        {
            shared++;
            continue;
        }
        memcpy( image + s->rva, mock_file + s->raw_ptr, s->raw_size );
        memset( image + s->rva + s->raw_size, 0, s->vsize - s->raw_size );
    }
    for (i = 0; i < ARRAY_SIZE(mock_sections); i++)
    {
        const struct mock_section *s = &mock_sections[i];
        unsigned char p = 0;

        if (s->chars & SCN_READ) p |= PROT_R;
        if (s->chars & SCN_WRITE) p |= PROT_W;
        if (s->chars & SCN_EXECUTE) p |= PROT_X;
        prot_per_4k[s->rva / 0x1000] = p;
    }
    return shared;
}

static void test_shared_section(void)
{
    static unsigned char a[MOCK_IMAGE], b[MOCK_IMAGE];
    unsigned char prot_a[MOCK_IMAGE / 0x1000] = { 0 }, prot_b[MOCK_IMAGE / 0x1000] = { 0 };
    unsigned int i;

    /* the rule */
    CHECK( ios_shared_section_shareable( 0x4000, 0x4000, 0x4000 ) );
    CHECK( ios_shared_section_shareable( 0x8000, 0x8000, 0x4000 ) );
    CHECK( !ios_shared_section_shareable( 0x2000, 0x1000, 0x4000 ) );   /* GameMaker's .mydata */
    CHECK( !ios_shared_section_shareable( 0x4000, 0x1000, 0x4000 ) );   /* shares its host page */
    CHECK( !ios_shared_section_shareable( 0x1000, 0x4000, 0x4000 ) );   /* starts inside one */
    CHECK( !ios_shared_section_shareable( 0x4000, 0, 0x4000 ) );
    CHECK( !ios_shared_section_shareable( 0x4000, 0x4000, 0 ) );
    CHECK( ios_shared_section_shareable( 0x1000, 0x1000, 0x1000 ) );    /* 4 KB host pages: upstream */

    for (i = 0; i < sizeof(mock_file); i++) mock_file[i] = (unsigned char)(i * 7 + 3);

    /* two processes load the image: it loads, nothing is shared */
    CHECK( mock_load( a, prot_a ) == 0 );
    CHECK( mock_load( b, prot_b ) == 0 );

    /* the section holds the file's data, zero past its raw size */
    CHECK( !memcmp( a + 0x2000, mock_file + 0x600, 0x100 ) );
    for (i = 0x2100; i < 0x3000; i++) if (a[i]) break;
    CHECK( i == 0x3000 );

    /* a write lands in that process's copy only */
    a[0x2010] ^= 0xff;
    CHECK( a[0x2010] != b[0x2010] );
    CHECK( b[0x2010] == mock_file[0x610] );

    /* the neighbours keep their contents and protections */
    CHECK( !memcmp( a + 0x1000, mock_file + 0x400, 0x200 ) && !memcmp( a + 0x3000, mock_file + 0x800, 0x200 ) );
    CHECK( prot_a[1] == (PROT_R | PROT_X) );
    CHECK( prot_a[2] == (PROT_R | PROT_W) );
    CHECK( prot_a[3] == (PROT_R | PROT_W) );
    CHECK( !memcmp( prot_a, prot_b, sizeof(prot_a) ) );
}

int main(void)
{
    test_native_copy();
    test_loaded_again();
    test_alloc_log();
    test_alloc_fold();
    test_free_region();
    test_mem_events();
    test_image_asked();
    test_image_machine();
    test_window_requests();
    test_module_match();
    test_shared_section();
    test_user_limit();
    test_laa();
    test_translate();
    test_slots();
    test_exewin();
    test_exewin_regions();
    test_dylib();
    test_exec();
    test_lowwin();
    test_lowwin_threads();
    test_pe_export();
    printf( "%d checks, %d failed\n", checks, failures );
    return failures ? 1 : 0;
}
