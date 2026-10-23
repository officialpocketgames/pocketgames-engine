// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/* Host tests: spec 18 Part H5/H6 (name rules), H7 (QPC arithmetic), I1/I2 (heap policies). */

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "../pe/dlls/ntdll/ntdll_names.h"
#include "../pe/dlls/ntdll/qpc_arith.h"
#include "../pe/dlls/ntdll/heap_policy.h"

static size_t to_w( unsigned short *out, const char *s )
{
    size_t i;
    for (i = 0; s[i]; i++) out[i] = (unsigned char)s[i];
    out[i] = 0;
    return i;
}

static int xinput( const char *s )
{
    unsigned short w[300];
    size_t len = to_w( w, s );
    return names_is_builtin_xinput( w, len );
}

static int sysx64( const char *s, const char *expect )
{
    unsigned short w[600], out[300], e[300];
    size_t len = to_w( w, s ), elen, n = names_sysx64_path( w, len, out, 300 );

    if (!expect) return n == 0;
    elen = to_w( e, expect );
    return n == elen && !memcmp( out, e, (elen + 1) * sizeof(unsigned short) );
}

static int always_log( const char *s )
{
    unsigned short w[300];
    size_t len = to_w( w, s );
    return names_always_log_failure( w, len );
}

static void test_names(void)
{
    char longname[300];

    CHECK( always_log( "xinput1_3.dll" ) );
    CHECK( always_log( "C:\\game\\XInput9_1_0.dll" ) );
    CHECK( always_log( "steam_api64.dll" ) );
    CHECK( always_log( "d3d11.dll" ) );
    CHECK( always_log( "DXGI.dll" ) );
    CHECK( always_log( "winemetal.dll" ) );
    CHECK( always_log( "d3d10_1.dll" ) );
    CHECK( always_log( "opengl32.dll" ) );
    CHECK( always_log( "d3dcompiler_47.dll" ) );
    CHECK( always_log( "winemac.drv" ) );
    CHECK( always_log( "C:/windows/system32/WINEIOS.DRV" ) );
    CHECK( !always_log( "d3d9.dll" ) );
    CHECK( !always_log( "mydxgi.dll" ) );
    CHECK( !always_log( "C:\\xinput\\foo.dll" ) );  /* only the base name counts */
    CHECK( !always_log( "foo.dr" ) );
    CHECK( !always_log( "" ) );

    CHECK( xinput( "xinput1_1.dll" ) );
    CHECK( xinput( "xinput1_2.dll" ) );
    CHECK( xinput( "XInput1_3.DLL" ) );
    CHECK( xinput( "xinput1_4.dll" ) );
    CHECK( xinput( "XINPUT9_1_0.dll" ) );
    CHECK( xinput( "xinputuap.dll" ) );
    CHECK( !xinput( "xinput1_5.dll" ) );
    CHECK( !xinput( "xinput1_3" ) );
    CHECK( !xinput( "xinput1_3.dll.bak" ) );
    CHECK( !xinput( "xinput1_3.dl" ) );
    CHECK( !xinput( "C:\\game\\xinput1_3.dll" ) );
    CHECK( !xinput( ".\\xinput1_3.dll" ) );
    CHECK( !xinput( "" ) );
    CHECK( !names_is_builtin_xinput( NULL, 0 ) );

    CHECK( sysx64( "\\??\\C:\\windows\\system32\\kernelbase.dll", "\\??\\C:\\windows\\sysx64\\kernelbase.dll" ) );
    CHECK( sysx64( "C:/windows/system32/msvcrt.dll", "\\??\\C:\\windows\\sysx64\\msvcrt.dll" ) );
    CHECK( sysx64( "user32", "\\??\\C:\\windows\\sysx64\\user32.dll" ) );
    CHECK( sysx64( "dir\\comctl32.ocx", "\\??\\C:\\windows\\sysx64\\comctl32.ocx" ) );
    CHECK( sysx64( "dir\\", NULL ) );
    CHECK( sysx64( "", NULL ) );

    /* too long for MAX_PATH: prefix (22) + name + ".dll" must leave room for the terminator */
    memset( longname, 'a', sizeof(longname) );
    longname[260 - 1 - 22 - 4] = 0;  /* exactly fits */
    CHECK( sysx64( longname, NULL ) == 0 );
    longname[260 - 1 - 22 - 4] = 'a';
    longname[260 - 22 - 4] = 0;      /* one too many */
    CHECK( sysx64( longname, NULL ) );
    {
        unsigned short w[300], out[300];
        size_t len;
        memset( longname, 'b', sizeof(longname) );
        longname[260 - 1 - 22 - 4] = 0;
        len = to_w( w, longname );
        CHECK_EQ( names_sysx64_path( w, len, out, 300 ), 259 );
        CHECK_EQ( names_sysx64_path( w, len, out, 259 ), 0 );  /* the output buffer is too small */
    }
}

static int farm( const char *f, const char *s, const char *expect )
{
    unsigned short w[600], out[300], e[300];
    size_t len = to_w( w, s ), elen, n = names_farm_path( f, w, len, out, 300 );

    if (!expect) return n == 0;
    elen = to_w( e, expect );
    return n == elen && !memcmp( out, e, (elen + 1) * sizeof(unsigned short) );
}

/* clarification 1: a model of find_dll_file's order; the normal search (system32 and the load
 * path) first, the farm only after its two statuses */
struct search_model
{
    unsigned int normal_status;  /* what the normal search ends in */
    int farm_has_it;
    int farm_tried;
};

static unsigned int model_search( struct search_model *m )
{
    unsigned int status = m->normal_status;

    if (!names_farm_fallback_due( status )) return status;
    m->farm_tried = 1;
    return m->farm_has_it ? 0 : status;
}

static void test_farm(void)
{
    static struct names_logged logged;
    unsigned short w[300];
    struct search_model m;
    char longname[300];
    size_t len;
    unsigned int i;

    /* the name rule, sysaa64 as sysx64 */
    CHECK( farm( "sysaa64", "\\??\\C:\\windows\\system32\\kernel32.dll", "\\??\\C:\\windows\\sysaa64\\kernel32.dll" ) );
    CHECK( farm( "sysaa64", "C:/windows/system32/ucrtbase.dll", "\\??\\C:\\windows\\sysaa64\\ucrtbase.dll" ) );
    CHECK( farm( "sysaa64", "kernelbase", "\\??\\C:\\windows\\sysaa64\\kernelbase.dll" ) );
    CHECK( farm( "sysaa64", "dir\\winemetal.drv", "\\??\\C:\\windows\\sysaa64\\winemetal.drv" ) );
    CHECK( farm( "sysaa64", "dir\\", NULL ) );
    CHECK( farm( "sysaa64", "", NULL ) );
    CHECK( farm( "sysx64", "user32", "\\??\\C:\\windows\\sysx64\\user32.dll" ) );
    /* MAX_PATH: "\??\C:\windows\sysaa64\" is 23 characters, one more than sysx64's */
    memset( longname, 'a', sizeof(longname) );
    longname[260 - 1 - 23 - 4] = 0;   /* exactly fits */
    len = to_w( w, longname );
    {
        unsigned short out[300];
        CHECK_EQ( names_farm_path( "sysaa64", w, len, out, 300 ), 259 );
    }
    longname[260 - 1 - 23 - 4] = 'a';
    longname[260 - 23 - 4] = 0;       /* one too many */
    CHECK( farm( "sysaa64", longname, NULL ) );

    /* the order: system32 first; the farm only after the two statuses */
    CHECK( names_farm_fallback_due( 0xc000007b ) );   /* STATUS_INVALID_IMAGE_FORMAT */
    CHECK( names_farm_fallback_due( 0xc0000135 ) );   /* STATUS_DLL_NOT_FOUND */
    CHECK( !names_farm_fallback_due( 0 ) );
    CHECK( !names_farm_fallback_due( 0xc0000022 ) );  /* STATUS_ACCESS_DENIED */
    CHECK( !names_farm_fallback_due( 0xc000003a ) );  /* STATUS_OBJECT_PATH_NOT_FOUND */
    CHECK( !names_farm_fallback_due( 0xc0000034 ) );  /* STATUS_OBJECT_NAME_NOT_FOUND */

    m = (struct search_model){ 0, 1, 0 };             /* found in system32: farm never tried */
    CHECK_EQ( model_search( &m ), 0 );
    CHECK( !m.farm_tried );
    m = (struct search_model){ 0xc000007b, 1, 0 };    /* ARM64EC kernel32 in system32 */
    CHECK_EQ( model_search( &m ), 0 );
    CHECK( m.farm_tried );
    m = (struct search_model){ 0xc0000135, 0, 0 };    /* nowhere: the original status */
    CHECK_EQ( model_search( &m ), 0xc0000135 );
    CHECK( m.farm_tried );
    m = (struct search_model){ 0xc0000022, 1, 0 };    /* another error: no farm */
    CHECK_EQ( model_search( &m ), 0xc0000022 );
    CHECK( !m.farm_tried );

    /* logged once per DLL name, whatever the case or the path */
    memset( &logged, 0, sizeof(logged) );
    len = to_w( w, "kernel32.dll" );
    CHECK( names_log_once( &logged, w, len ) );
    CHECK( !names_log_once( &logged, w, len ) );
    len = to_w( w, "C:\\windows\\system32\\KERNEL32.DLL" );
    CHECK( !names_log_once( &logged, w, len ) );
    len = to_w( w, "user32.dll" );
    CHECK( names_log_once( &logged, w, len ) );
    for (i = 0; i < NAMES_LOGGED_MAX + 10; i++)
    {
        char name[32];
        snprintf( name, sizeof(name), "dll%u.dll", i );
        len = to_w( w, name );
        names_log_once( &logged, w, len );
    }
    CHECK_EQ( logged.count, NAMES_LOGGED_MAX );
    len = to_w( w, "brandnew.dll" );
    CHECK( !names_log_once( &logged, w, len ) );      /* a full table logs nothing more */
}

static void test_qpc(void)
{
    const uint64_t f24 = 24000000, f1g = 1000000000;
    int64_t off;
    uint64_t t, prev, last;
    int have, i;

    /* conversion */
    CHECK_EQ( qpc_ticks_to_100ns( 0, f24 ), 0 );
    CHECK_EQ( qpc_ticks_to_100ns( f24, f24 ), 10000000 );
    CHECK_EQ( qpc_ticks_to_100ns( 12, f24 ), 5 );
    CHECK_EQ( qpc_ticks_to_100ns( f1g + 100, f1g ), 10000001 );
    CHECK_EQ( qpc_ticks_to_100ns( 99, f1g ), 0 );
    /* near 2^64: whole seconds plus scaled remainder, no overflow */
    t = UINT64_MAX;
    CHECK_EQ( qpc_ticks_to_100ns( t, f24 ), (t / f24) * 10000000 + (t % f24) * 10000000 / f24 );
    CHECK( qpc_ticks_to_100ns( t, f24 ) > qpc_ticks_to_100ns( t - f24, f24 ) );
    CHECK_EQ( qpc_ticks_to_100ns( t, f1g ), (t / f1g) * 10000000 + (t % f1g) * 10000000 / f1g );
    CHECK_EQ( qpc_ticks_to_100ns( t, f1g ), 184467440737095516ull );
    CHECK_EQ( qpc_ticks_to_100ns( t, f24 ), 7686143364045646ull * 1000 + 506ull );

    /* measured offset uses the midpoint */
    CHECK_EQ( qpc_measured_offset( 0, 1000, 2 * f24, f24 ), 1000 - 10000000 );

    /* offset rule */
    CHECK( qpc_adopt_offset( 0, 0, -5 ) );           /* first */
    CHECK( !qpc_adopt_offset( 1, 1000, 999 ) );      /* smaller */
    CHECK( !qpc_adopt_offset( 1, 1000, 1000 + 9999 ) );
    CHECK( !qpc_adopt_offset( 1, 1000, 1000 + 10000 ) );  /* larger by exactly 1 ms */
    CHECK( qpc_adopt_offset( 1, 1000, 1000 + 10001 ) );

    /* measurement due */
    CHECK( qpc_measure_due( 0, 0, 0, f24 ) );
    CHECK( !qpc_measure_due( 1, 100, 100 + f24 - 1, f24 ) );
    CHECK( qpc_measure_due( 1, 100, 100 + f24, f24 ) );

    /* monotonic over a fake sleep: the counter stops for 30 s, the system counter doesn't */
    {
        uint64_t counter = 5 * f24;  /* device counter */
        int64_t sys_origin = 123456789;  /* system counter = origin + awake + asleep time */
        uint64_t asleep = 0;

        have = 0; off = 0; last = 0; prev = 0;
        for (i = 0; i < 400; i++)
        {
            uint64_t result;
            counter += f24 / 10;  /* 100 ms per iteration */
            if (i == 200) asleep += 30 * 10000000ull;
            if (qpc_measure_due( have, last, counter, f24 ))
            {
                int64_t sys = sys_origin + (int64_t)qpc_ticks_to_100ns( counter, f24 ) + (int64_t)asleep;
                int64_t cand = qpc_measured_offset( counter, sys, counter, f24 );
                if (qpc_adopt_offset( have, off, cand )) off = cand;
                have = 1;
                last = counter;
            }
            result = qpc_ticks_to_100ns( counter, f24 ) + (uint64_t)off;
            CHECK( result > prev );
            prev = result;
        }
        /* after the sleep the result caught up with the system counter */
        CHECK_EQ( off, sys_origin + (int64_t)asleep );
    }
}

static void test_heap(void)
{
    static const unsigned short one[] = { '1' }, zero[] = { '0' }, eleven[] = { '1', '1' };
    const size_t align = 0x10000;

    /* parser */
    CHECK( heap_policy_value_on( one, 1 ) );
    CHECK( !heap_policy_value_on( zero, 1 ) );
    CHECK( !heap_policy_value_on( eleven, 2 ) );
    CHECK( !heap_policy_value_on( one, 0 ) );    /* empty */
    CHECK( !heap_policy_value_on( NULL, 0 ) );

    /* initial grow and growth */
    CHECK_EQ( heap_policy_initial_grow( 0x10000, 1 ), 0x10000 );
    CHECK_EQ( heap_policy_initial_grow( 0x1000000, 1 ), 0x200000 );
    CHECK_EQ( heap_policy_initial_grow( 0x1000000, 0 ), 0x1000000 );
    CHECK_EQ( heap_policy_grow_after_success( 0x100000, 0x4000000, 1 ), 0x200000 );
    CHECK_EQ( heap_policy_grow_after_success( 0x200000, 0x4000000, 1 ), 0x200000 );
    CHECK_EQ( heap_policy_grow_after_success( 0x200000, 0x4000000, 0 ), 0x400000 );
    CHECK_EQ( heap_policy_grow_after_success( 0x4000000, 0x4000000, 0 ), 0x4000000 );

    /* upstream retry: halve while above the need and above 4 MB */
    CHECK_EQ( heap_policy_retry_grow( 0x2000000, 0x10000, align, 0 ), 0x1000000 );
    CHECK_EQ( heap_policy_retry_grow( 0x800000, 0x10000, align, 0 ), 0x400000 );
    CHECK_EQ( heap_policy_retry_grow( 0x400000, 0x10000, align, 0 ), 0 );
    CHECK_EQ( heap_policy_retry_grow( 0x2000000, 0x2000000, align, 0 ), 0 );

    /* COMPACT retry: halve down to the need rounded up, then fail */
    CHECK_EQ( heap_policy_retry_grow( 0x200000, 0x18000, align, 1 ), 0x100000 );
    CHECK_EQ( heap_policy_retry_grow( 0x100000, 0x18000, align, 1 ), 0x80000 );
    CHECK_EQ( heap_policy_retry_grow( 0x40000, 0x18000, align, 1 ), 0x20000 );
    CHECK_EQ( heap_policy_retry_grow( 0x30000, 0x18000, align, 1 ), 0x20000 );  /* not below the rounded need */
    CHECK_EQ( heap_policy_retry_grow( 0x20000, 0x18000, align, 1 ), 0 );
    CHECK_EQ( heap_policy_retry_grow( 0x10000, 0x18000, align, 1 ), 0 );
    {
        /* the sequence always terminates */
        size_t grow = 0x200000, steps = 0;
        while ((grow = heap_policy_retry_grow( grow, 0x12345, align, 1 ))) steps++;
        CHECK( steps > 0 && steps < 8 );
    }

    /* census: 40 sites, the first 32 tracked, top 6 by bytes */
    {
        static struct heap_census census;
        uint32_t top[HEAP_CENSUS_TOP], n, i;
        uint64_t rest = 0;

        memset( &census, 0, sizeof(census) );
        for (i = 0; i < 40; i++) heap_census_add( &census, 0x1000 + i, 0x10000 * (i % 10 + 1) );
        heap_census_add( &census, 0x1003, 0x500000 );  /* tracked site grows */
        heap_census_add( &census, 0, 0x20000 );        /* heap's own block, past the first 32 */
        CHECK_EQ( census.count, 32 );
        CHECK_EQ( census.untracked, 0x10000 * (3 + 4 + 5 + 6 + 7 + 8 + 9 + 10) + 0x20000 );
        CHECK_EQ( census.sites[3].blocks, 2 );
        n = heap_census_top( &census, top, &rest );
        CHECK_EQ( n, 6 );
        CHECK_EQ( census.sites[top[0]].site, 0x1003 );
        for (i = 1; i < n; i++) CHECK( census.sites[top[i - 1]].bytes >= census.sites[top[i]].bytes );
        {
            uint64_t total = 0, topsum = 0;
            for (i = 0; i < census.count; i++) total += census.sites[i].bytes;
            for (i = 0; i < n; i++) topsum += census.sites[top[i]].bytes;
            CHECK_EQ( rest, total - topsum );
        }

        /* fewer than 6 sites */
        memset( &census, 0, sizeof(census) );
        rest = 0;
        heap_census_add( &census, 0x10, 5 );
        heap_census_add( &census, 0x20, 7 );
        n = heap_census_top( &census, top, &rest );
        CHECK_EQ( n, 2 );
        CHECK_EQ( census.sites[top[0]].site, 0x20 );
        CHECK_EQ( rest, 0 );
    }
}

int main(void)
{
    test_names();
    test_farm();
    test_qpc();
    test_heap();
    return check_report( "misc" );
}
