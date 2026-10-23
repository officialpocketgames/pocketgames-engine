// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 19 Part C: host tests of Part A's pure code, the headers that pe/ shares with its DLLs:
// the tick conversion (A1), the 2 GB clamp rule (A3), the minidump header (A6), nsi's decisions
// and pending-request state machine (A9) and the services autostart rule (A10).

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "tick_convert.h"
#include "memstatus_clamp.h"
#include "minidump_header.h"
#include "autostart.h"
#include "compile_failure_log.h"

/* nsi_fallback.h's pending requests work on a fake OVERLAPPED and fake events */
struct fake_event
{
    int signaled;
    int resets;
    int sets;
};

struct fake_overlapped
{
    unsigned long long Internal;
    unsigned long long InternalHigh;
    void *hEvent;
};

static void fake_reset( struct fake_event *e ) { e->signaled = 0; e->resets++; }
static void fake_set( struct fake_event *e ) { e->signaled = 1; e->sets++; }

#define NSI_OVL struct fake_overlapped
#define NSI_HANDLE struct fake_event *
#define NSI_RESET_EVENT(h) fake_reset( h )
#define NSI_SET_EVENT(h) fake_set( h )
#include "nsi_fallback.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf( stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while (0)
#define CHECK_EQ_U(a, b) do { unsigned long long a_ = (a), b_ = (b); if (a_ != b_) { \
    fprintf( stderr, "%s:%d: %s = %llu (%#llx), expected %llu (%#llx)\n", __FILE__, __LINE__, #a, \
             a_, a_, b_, b_ ); failures++; } } while (0)

/* ---- A1: the tick conversion against a 128-bit reference ---- */

static void test_tick(void)
{
    static const unsigned long long freqs[] =
    {
        10000000ull,                 /* Wine's frequency: the counter as is */
        0,                           /* no frequency: the counter as is */
        24000000ull,                 /* Apple's generic timer */
        1000000000ull,               /* 1 GHz */
        1000003ull,                  /* a prime */
        2305843009213693951ull,      /* 2^61 - 1, a prime above 2^64 / 10^7: the slow path */
        0xffffffffffffffffull,       /* the largest possible frequency */
    };
    static const unsigned long long counters[] =
    {
        0, 1, 9999, 10000, 123456789ull, 24000000ull * 3600,
        0x7fffffffffffffffull, 0x8000000000000000ull, 0x8000000000003039ull,
        0xfffffffffffffffeull, 0xffffffffffffffffull,
    };
    size_t i, j;

    for (i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++)
    {
        for (j = 0; j < sizeof(counters) / sizeof(counters[0]); j++)
        {
            unsigned long long f = freqs[i], c = counters[j], ref;
            unsigned long long t = tick_counter_to_100ns( c, f );

            if (f == 10000000ull || !f) ref = c;
            else ref = (unsigned long long)((unsigned __int128)c * 10000000u / f);
            if (t != ref)
                fprintf( stderr, "freq %llu counter %llu: ", f, c );
            CHECK_EQ_U( t, ref );
            CHECK_EQ_U( tick_100ns_to_ms( t ), ref / 10000 );
            CHECK_EQ_U( tick_100ns_to_ms32( t ), (ref / 10000) & 0xffffffffu );
        }
    }
    /* the slow path agrees with the fast one where both apply */
    for (j = 0; j < sizeof(counters) / sizeof(counters[0]); j++)
    {
        unsigned long long f = 1000000007ull, rem = counters[j] % f;

        CHECK_EQ_U( tick_scale_remainder( rem, f ), rem * 10000000ull / f );
    }
    /* one second at 24 MHz is 10^7 units, 1000 ms */
    CHECK_EQ_U( tick_counter_to_100ns( 24000000ull, 24000000ull ), 10000000ull );
    CHECK_EQ_U( tick_100ns_to_ms( 10000000ull ), 1000 );
    /* GetTickCount wraps after 2^32 ms */
    CHECK_EQ_U( tick_100ns_to_ms32( (0x100000000ull + 5) * 10000 ), 5 );
}

/* ---- A3: the 2 GB clamp ---- */

static void test_memclamp(void)
{
    static const struct { int laa; unsigned long long total; int clamp; } cases[] =
    {
        { 0, 0x7fff0000ull, 1 },    /* below 2 GB, no flag: Windows' clamp */
        { 0, 0x80000000ull, 1 },    /* at 2 GB */
        { 0, 0x80000001ull, 0 },    /* above: given more than 2 GB here, say so */
        { 0, 0xffff0000ull, 0 },    /* 4 GB */
        { 0, 0x100000000ull, 0 },
        { 1, 0x7fff0000ull, 0 },    /* large-address aware: never clamped */
        { 1, 0x80000000ull, 0 },
        { 1, 0xffff0000ull, 0 },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK_EQ_U( memstatus_clamp_to_2gb( cases[i].laa, cases[i].total ), cases[i].clamp );
}

/* ---- A6: the minidump header ---- */

static void test_minidump(void)
{
    static const unsigned char normal[32] =
    {
        'M', 'D', 'M', 'P',  0x93, 0xa7, 0, 0,  0, 0, 0, 0,  32, 0, 0, 0,
        0, 0, 0, 0,  0x78, 0x56, 0x34, 0x12,  0, 0, 0, 0,  0, 0, 0, 0,
    };
    static const unsigned char full[32] =
    {
        'M', 'D', 'M', 'P',  0x93, 0xa7, 0, 0,  0, 0, 0, 0,  32, 0, 0, 0,
        0, 0, 0, 0,  0xff, 0xff, 0xff, 0xff,  0x02, 0x10, 0, 0,  0, 0, 0, 0,
    };
    static const unsigned char wide[32] =
    {
        'M', 'D', 'M', 'P',  0x93, 0xa7, 0, 0,  0, 0, 0, 0,  32, 0, 0, 0,
        0, 0, 0, 0,  0, 0, 0, 0,  0xff, 0xff, 0x3f, 0x00,  0x01, 0, 0, 0,
    };
    unsigned char out[MINIDUMP_HEADER_ONLY_SIZE];

    /* MiniDumpNormal */
    memset( out, 0xcc, sizeof(out) );
    minidump_header_only( out, 0x12345678, 0 );
    CHECK( !memcmp( out, normal, 32 ) );
    /* MiniDumpWithFullMemory | MiniDumpWithThreadInfo (0x2 | 0x1000) */
    minidump_header_only( out, 0xffffffffu, 0x1002 );
    CHECK( !memcmp( out, full, 32 ) );
    /* many flags, and a bit above 32 (the field is 64 bits wide) */
    minidump_header_only( out, 0, 0x1003fffffull );
    CHECK( !memcmp( out, wide, 32 ) );
}

/* ---- A9: nsi ---- */

static void test_nsi_switch(void)
{
    CHECK( nsi_switch_on( NULL ) );     /* unset */
    CHECK( nsi_switch_on( "" ) );       /* empty */
    CHECK( !nsi_switch_on( "0" ) );
    CHECK( !nsi_switch_on( "00" ) );
    CHECK( !nsi_switch_on( "0off" ) );
    CHECK( nsi_switch_on( "1" ) );
    CHECK( nsi_switch_on( "10" ) );     /* a 0 after other text */
    CHECK( nsi_switch_on( " 0" ) );
    CHECK( nsi_switch_on( "off" ) );
}

static void test_nsi_status(void)
{
    /* item 4: enumerate */
    CHECK_EQ_U( nsi_map_enumerate_status( NSI_FB_STATUS_SUCCESS ), NSI_MAP_CONVERT );
    CHECK_EQ_U( nsi_map_enumerate_status( NSI_FB_STATUS_BUFFER_OVERFLOW ), NSI_MAP_MORE_DATA );
    CHECK_EQ_U( nsi_map_enumerate_status( NSI_FB_STATUS_NOT_SUPPORTED ), NSI_MAP_DEVICE_ERROR );
    CHECK_EQ_U( nsi_map_enumerate_status( 0xc000000du /* STATUS_INVALID_PARAMETER */ ), NSI_MAP_CONVERT );
    CHECK_EQ_U( nsi_map_enumerate_status( 0xc0000017u /* STATUS_NO_MEMORY */ ), NSI_MAP_CONVERT );
    /* item 5: one row / one field */
    CHECK_EQ_U( nsi_map_parameter_status( NSI_FB_STATUS_SUCCESS ), NSI_MAP_CONVERT );
    CHECK_EQ_U( nsi_map_parameter_status( NSI_FB_STATUS_NOT_SUPPORTED ), NSI_MAP_DEVICE_ERROR );
    CHECK_EQ_U( nsi_map_parameter_status( NSI_FB_STATUS_BUFFER_OVERFLOW ), NSI_MAP_CONVERT );
    CHECK_EQ_U( nsi_map_parameter_status( 0xc000000du ), NSI_MAP_CONVERT );
    /* item 2: the probe */
    CHECK( nsi_probe_allows_parameters( NSI_FB_STATUS_SUCCESS ) );
    CHECK( nsi_probe_allows_parameters( NSI_FB_STATUS_BUFFER_OVERFLOW ) );
    CHECK( nsi_probe_allows_parameters( 0xc000000du ) );
    CHECK( !nsi_probe_allows_parameters( NSI_FB_STATUS_NOT_SUPPORTED ) );
    /* item 3: what is remembered */
    CHECK( nsi_remember_missing_device( NSI_FB_ERROR_FILE_NOT_FOUND, 1, 1 ) );
    CHECK( !nsi_remember_missing_device( NSI_FB_ERROR_FILE_NOT_FOUND, 0, 1 ) );  /* no fallback */
    CHECK( !nsi_remember_missing_device( NSI_FB_ERROR_FILE_NOT_FOUND, 1, 0 ) );  /* switched off */
    CHECK( !nsi_remember_missing_device( 5 /* ERROR_ACCESS_DENIED */, 1, 1 ) );
    /* item 6: when requests stay pending */
    CHECK( nsi_notify_stays_pending( NSI_FB_ERROR_FILE_NOT_FOUND, 1, 1 ) );
    CHECK( !nsi_notify_stays_pending( NSI_FB_ERROR_FILE_NOT_FOUND, 0, 1 ) );
    CHECK( !nsi_notify_stays_pending( NSI_FB_ERROR_FILE_NOT_FOUND, 1, 0 ) );
    CHECK( !nsi_notify_stays_pending( 5, 1, 1 ) );
    /* the contract's entry numbers */
    CHECK_EQ_U( NSI_UNIX_ENUMERATE_ALL, 0 );
    CHECK_EQ_U( NSI_UNIX_GET_ALL_PARAMETERS, 1 );
    CHECK_EQ_U( NSI_UNIX_GET_PARAMETER, 2 );
}

static void *with_low_bit( struct fake_event *e )
{
    return (void *)((size_t)e | 1);
}

static void test_nsi_pending(void)
{
    struct fake_event ev = { 1, 0, 0 }, ev2 = { 1, 0, 0 };
    struct fake_overlapped ovr, never, nullev;

    /* a request: pending, no bytes, its event reset */
    memset( &ovr, 0, sizeof(ovr) );
    ovr.InternalHigh = 77;
    ovr.hEvent = &ev;
    CHECK_EQ_U( nsi_pending_request( &ovr ), NSI_FB_ERROR_IO_PENDING );
    CHECK_EQ_U( ovr.Internal, NSI_FB_STATUS_PENDING );
    CHECK_EQ_U( ovr.InternalHigh, 0 );
    CHECK( !ev.signaled && ev.resets == 1 && ev.sets == 0 );
    CHECK( ovr.hEvent == &ev );   /* the caller's handle stays as it was */

    /* cancel: cancelled, event set */
    CHECK_EQ_U( nsi_pending_cancel( &ovr ), NSI_FB_ERROR_SUCCESS );
    CHECK_EQ_U( ovr.Internal, NSI_FB_STATUS_CANCELLED );
    CHECK( ev.signaled && ev.sets == 1 );

    /* cancel twice: no pending request any more */
    CHECK_EQ_U( nsi_pending_cancel( &ovr ), NSI_FB_ERROR_NOT_FOUND );
    CHECK_EQ_U( ovr.Internal, NSI_FB_STATUS_CANCELLED );
    CHECK( ev.sets == 1 );

    /* cancel of a request never made */
    memset( &never, 0, sizeof(never) );
    never.hEvent = &ev2;
    CHECK_EQ_U( nsi_pending_cancel( &never ), NSI_FB_ERROR_NOT_FOUND );
    CHECK_EQ_U( never.Internal, 0 );
    CHECK( ev2.sets == 0 && ev2.resets == 0 );
    CHECK_EQ_U( nsi_pending_cancel( NULL ), NSI_FB_ERROR_NOT_FOUND );

    /* the event handle's low bit (no completion port) is masked off for the event calls */
    ev2.signaled = 1;
    never.hEvent = with_low_bit( &ev2 );
    CHECK( nsi_ovl_event( &never ) == &ev2 );
    CHECK_EQ_U( nsi_pending_request( &never ), NSI_FB_ERROR_IO_PENDING );
    CHECK( !ev2.signaled && ev2.resets == 1 );
    CHECK( never.hEvent == with_low_bit( &ev2 ) );
    CHECK_EQ_U( nsi_pending_cancel( &never ), NSI_FB_ERROR_SUCCESS );
    CHECK( ev2.signaled && ev2.sets == 1 );

    /* an OVERLAPPED without an event (or with only the low bit) */
    memset( &nullev, 0, sizeof(nullev) );
    CHECK_EQ_U( nsi_pending_request( &nullev ), NSI_FB_ERROR_IO_PENDING );
    CHECK_EQ_U( nsi_pending_cancel( &nullev ), NSI_FB_ERROR_SUCCESS );
    nullev.hEvent = (void *)1;
    CHECK( nsi_ovl_event( &nullev ) == NULL );
    CHECK_EQ_U( nsi_pending_request( &nullev ), NSI_FB_ERROR_IO_PENDING );
    CHECK_EQ_U( nsi_pending_cancel( &nullev ), NSI_FB_ERROR_SUCCESS );
}

/* ---- A10: services ---- */

static void test_services(void)
{
    unsigned int type;

    for (type = AUTOSTART_BOOT_START; type <= AUTOSTART_DISABLED; type++)
    {
        int expect_plain = type <= AUTOSTART_AUTO_START;
        int expect_pnp = type != AUTOSTART_DISABLED;

        CHECK_EQ_U( services_autostart( type, 0 ), expect_plain );
        CHECK_EQ_U( services_autostart( type, 1 ), expect_pnp );
    }
    /* the case the change is about: a disabled root PnP device service */
    CHECK( !services_autostart( AUTOSTART_DISABLED, 1 ) );
    CHECK( services_autostart( AUTOSTART_DEMAND_START, 1 ) );
    CHECK( !services_autostart( AUTOSTART_DEMAND_START, 0 ) );
    CHECK( !strcmp( services_start_type_name( AUTOSTART_DISABLED ), "disabled" ) );
    CHECK( !strcmp( services_start_type_name( 9 ), "unknown" ) );
}

/* ---- d3dcompiler: failed compiles in the log ---- */

/* collects what compile_fail_lines_next returns, as "line|line|..." */
static unsigned int collect_lines( const char *text, size_t size, char *out, size_t out_size, size_t *bytes )
{
    struct compile_fail_lines it;
    const char *line;
    unsigned int n = 0;
    int len;

    out[0] = 0;
    *bytes = 0;
    compile_fail_lines_init( &it, text, size );
    while (compile_fail_lines_next( &it, &line, &len ))
    {
        size_t used = strlen( out );
        snprintf( out + used, out_size - used, "%s%.*s", n ? "|" : "", len, line );
        *bytes += (size_t)len;
        n++;
    }
    return n;
}

static void test_compile_failure_log(void)
{
    static const char blob[] = "a.hlsl:3:1: E5000: syntax error\r\n\nb.hlsl:9:2: W1: unused\n";
    static char big[20000], out[25000];
    size_t bytes;
    long i;

    /* the rate limit: 32 reports, one notice, then nothing */
    CHECK_EQ_U( compile_fail_action( 1 ), COMPILE_FAIL_REPORT );
    CHECK_EQ_U( compile_fail_action( 32 ), COMPILE_FAIL_REPORT );
    CHECK_EQ_U( compile_fail_action( 33 ), COMPILE_FAIL_LAST_NOTICE );
    CHECK_EQ_U( compile_fail_action( 34 ), COMPILE_FAIL_SILENT );
    CHECK_EQ_U( compile_fail_action( 0x7fffffffL ), COMPILE_FAIL_SILENT );
    CHECK_EQ_U( compile_fail_action( 0 ), COMPILE_FAIL_SILENT );
    for (i = 1, bytes = 0; i <= 100; i++) bytes += compile_fail_action( i ) == COMPILE_FAIL_REPORT;
    CHECK_EQ_U( bytes, 32 );

    /* lines: CR dropped, empty lines skipped, a NUL ends the text (blob sizes count it) */
    CHECK_EQ_U( collect_lines( blob, sizeof(blob), out, sizeof(out), &bytes ), 2 );
    CHECK( !strcmp( out, "a.hlsl:3:1: E5000: syntax error|b.hlsl:9:2: W1: unused" ) );
    CHECK_EQ_U( collect_lines( blob, 10, out, sizeof(out), &bytes ), 1 );   /* not NUL-terminated */
    CHECK( !strcmp( out, "a.hlsl:3:1" ) );
    CHECK_EQ_U( collect_lines( NULL, 0, out, sizeof(out), &bytes ), 0 );
    CHECK_EQ_U( collect_lines( "\n\r\n\n", 4, out, sizeof(out), &bytes ), 0 );
    CHECK_EQ_U( collect_lines( "x\0y\n", 4, out, sizeof(out), &bytes ), 1 );
    CHECK( !strcmp( out, "x" ) );

    /* at most 12 lines */
    for (i = 0; i < 20; i++) memcpy( big + i * 4, "ab\n\n", 4 );
    CHECK_EQ_U( collect_lines( big, 80, out, sizeof(out), &bytes ), 12 );
    CHECK_EQ_U( bytes, 24 );

    /* at most 2 KB: long lines, the last one cut to fit */
    memset( big, 'e', sizeof(big) );
    for (i = 900; i < (long)sizeof(big); i += 901) big[i] = '\n';
    CHECK_EQ_U( collect_lines( big, sizeof(big), out, sizeof(out), &bytes ), 3 );
    CHECK_EQ_U( bytes, COMPILE_FAIL_MAX_BYTES );
    /* one line longer than the budget */
    CHECK_EQ_U( collect_lines( big, 900, out, sizeof(out), &bytes ), 1 );
    CHECK_EQ_U( bytes, 900 );
    memset( big, 'f', 5000 );
    CHECK_EQ_U( collect_lines( big, 5000, out, sizeof(out), &bytes ), 1 );
    CHECK_EQ_U( bytes, COMPILE_FAIL_MAX_BYTES );
}

int main(void)
{
    test_tick();
    test_memclamp();
    test_minidump();
    test_nsi_switch();
    test_nsi_status();
    test_nsi_pending();
    test_services();
    test_compile_failure_log();
    if (failures)
    {
        fprintf( stderr, "test_part_a: %d failure(s)\n", failures );
        return 1;
    }
    printf( "test_part_a: all passed\n" );
    return 0;
}
