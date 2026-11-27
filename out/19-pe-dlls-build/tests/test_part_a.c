// SPDX-License-Identifier: LGPL-2.1-or-later
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
#include "compile_failure_dump.h"
#include "load_drawable_guard.h"
#include "format_check_log.h"

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

/* ---- d3dcompiler: the failed-shader dump (A17) ---- */

static void test_compile_failure_dump(void)
{
    static const char messages[] = "s.hlsl:4:12: E5005: Field \"pixel\" is not defined.\r\n\n"
                                   "s.hlsl:4:5: E5002: Can't implicitly convert from <error type>.\n";
    static const char expected[] =
        "// d3dcompiler: a failed compile, dumped for diagnosis (PG_D3DCOMPILER_DUMP_FAILED=1)\n"
        "// source: (memory)\n"
        "// entry: VSMain\n"
        "// target: vs_4_0\n"
        "// flags: 0x1000\n"
        "// result: 0x80004005\n"
        "// compiler version: -\n"              /* A25's lines, with nothing known */
        "// called through: unknown (no call stack)\n"
        "// call stack: -\n"
        "// macros: none (NULL)\n"
        "// includes: no include handler\n"
        "// text: after preprocessing\n"
        "// messages:\n"
        "//   s.hlsl:4:12: E5005: Field \"pixel\" is not defined.\n"
        "//   s.hlsl:4:5: E5002: Can't implicitly convert from <error type>.\n"
        "\n"
        "float4 VSMain() : SV_Position { return 0; }\n";
    static const char text[] = "float4 VSMain() : SV_Position { return 0; }\0";  /* a blob's NUL */
    struct compile_dump_info info, other;
    char out[1024], small[40], name[COMPILE_DUMP_NAME_SIZE], line[80];
    size_t len;
    int i;

    /* on only with exactly "1" */
    CHECK( compile_dump_enabled( "1" ) );
    CHECK( !compile_dump_enabled( NULL ) );
    CHECK( !compile_dump_enabled( "" ) );
    CHECK( !compile_dump_enabled( "0" ) );
    CHECK( !compile_dump_enabled( "11" ) );
    CHECK( !compile_dump_enabled( "1 " ) );
    CHECK( !strcmp( COMPILE_DUMP_ENV, "PG_D3DCOMPILER_DUMP_FAILED" ) );
    CHECK( !strcmp( COMPILE_DUMP_DIR, "C:\\users\\Public\\pg-dumps\\shaders" ) );
    CHECK_EQ_U( COMPILE_DUMP_MAX_FILES, 64 );

    memset( &info, 0, sizeof(info) );
    info.text = text;
    info.text_size = compile_dump_text_size( text, sizeof(text) );
    CHECK_EQ_U( info.text_size, strlen( text ) );
    info.preprocessed = 1;
    info.entry = "VSMain";
    info.target = "vs_4_0";
    info.flags = 0x1000;
    info.result = 0x80004005u;
    info.messages = messages;
    info.messages_size = sizeof(messages);

    /* the file: header, every message line, then the text with a final newline */
    len = compile_dump_format( NULL, 0, &info );
    CHECK_EQ_U( len, strlen( expected ) );
    memset( out, 0, sizeof(out) );
    CHECK_EQ_U( compile_dump_format( out, sizeof(out), &info ), len );
    CHECK( !strcmp( out, expected ) );
    /* a short buffer gets its first bytes, nothing past them */
    memset( small, '#', sizeof(small) );
    CHECK_EQ_U( compile_dump_format( small, 20, &info ), len );
    CHECK( !memcmp( small, expected, 20 ) && small[20] == '#' );

    /* the source as given when preprocessing failed; flags 0; a name with a newline stays on its line */
    other = info;
    other.preprocessed = 0;
    other.flags = 0;
    other.source_name = "a.fx\nb";
    other.messages = NULL;
    other.messages_size = 0;
    compile_dump_format( out, sizeof(out), &other );
    out[compile_dump_format( NULL, 0, &other )] = 0;
    CHECK( strstr( out, "// source: a.fx\n// entry" ) != NULL );
    CHECK( strstr( out, "// flags: 0x0\n" ) != NULL );
    CHECK( strstr( out, "// text: as given (preprocessing failed)\n// messages:\n\nfloat4" ) != NULL );

    /* the name: 8 hex digits and .hlsl; the same shader the same name, a different one another */
    compile_dump_name( name, 0x0a1b2c3du );
    CHECK( !strcmp( name, "0a1b2c3d.hlsl" ) );
    compile_dump_name( name, compile_dump_hash( &info ) );
    CHECK_EQ_U( strlen( name ), 13 );
    for (i = 0; i < 8; i++) CHECK( strchr( "0123456789abcdef", name[i] ) != NULL );
    CHECK_EQ_U( compile_dump_hash( &info ), compile_dump_hash( &info ) );
    other = info;
    other.messages = NULL;              /* the messages and the result don't name the file */
    other.result = 0x80070057u;
    CHECK_EQ_U( compile_dump_hash( &other ), compile_dump_hash( &info ) );
    other.target = "vs_5_0";
    CHECK( compile_dump_hash( &other ) != compile_dump_hash( &info ) );
    other = info;
    other.flags = 0;
    CHECK( compile_dump_hash( &other ) != compile_dump_hash( &info ) );
    other = info;
    other.text_size--;
    CHECK( compile_dump_hash( &other ) != compile_dump_hash( &info ) );
    /* FNV-1a's published value for "a" */
    CHECK_EQ_U( compile_dump_hash_bytes( 2166136261u, "a", 1 ), 0xe40c292cu );

    /* the log line */
    snprintf( line, sizeof(line), COMPILE_DUMP_LOG_FORMAT, "0a1b2c3d.hlsl" );
    CHECK( !strcmp( line, "[d3dcompiler] failed shader dumped: 0a1b2c3d.hlsl\n" ) );
}

/* ---- d3dcompiler: successful Shader Model 1-3 compiles in the dump (A19) ---- */

static void put16( unsigned char *p, unsigned int v ) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32( unsigned char *p, unsigned int v ) { put16( p, v & 0xffff ); put16( p + 2, v >> 16 ); }

/* vs_2_0 bytecode with a CTAB comment of two constants (as d3dx9shader.h lays it out), then END */
static size_t make_sm1_code( unsigned char *code, unsigned int constants )
{
    static const float def[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
    unsigned char *t = code + 12;   /* the CTAB data: after the version, comment and "CTAB" tokens */
    unsigned int i;

    memset( code, 0, 160 );
    put32( code, 0xfffe0200 );
    put32( code + 4, 0xfffe | (35u << 16) );   /* "CTAB" + 136 bytes */
    put32( code + 8, 0x42415443 );
    put32( t, 28 ); put32( t + 4, 133 ); put32( t + 8, 0xfffe0200 ); put32( t + 12, constants );
    put32( t + 16, 28 ); put32( t + 20, 0 ); put32( t + 24, 126 );
    /* Tint: FLOAT4 c4, 1 register, a float4 with a default; Diff: SAMPLER s5 */
    put32( t + 28, 116 ); put16( t + 32, 2 ); put16( t + 34, 4 ); put16( t + 36, 1 ); put32( t + 40, 68 ); put32( t + 44, 100 );
    put32( t + 48, 121 ); put16( t + 52, 3 ); put16( t + 54, 5 ); put16( t + 56, 1 ); put32( t + 60, 84 ); put32( t + 64, 0 );
    put16( t + 68, 1 ); put16( t + 70, 3 ); put16( t + 72, 1 ); put16( t + 74, 4 ); put16( t + 76, 1 );
    put16( t + 84, 4 ); put16( t + 86, 12 ); put16( t + 88, 1 ); put16( t + 90, 1 ); put16( t + 92, 1 );
    for (i = 0; i < 4; i++) memcpy( t + 100 + 4 * i, &def[i], 4 );
    memcpy( t + 116, "Tint", 5 );
    memcpy( t + 121, "Diff", 5 );
    memcpy( t + 126, "vs_2_0", 7 );
    memcpy( t + 133, "me", 3 );
    put32( t + 136, 0x0000ffff );   /* END */
    return 12 + 136 + 4;
}

static size_t ctab_text( const unsigned char *code, size_t size, char *out, size_t out_size )
{
    struct compile_dump_out o = { out, out_size - 1, 0 };

    compile_dump_ctab( &o, code, size );
    out[o.len < out_size - 1 ? o.len : out_size - 1] = 0;
    return o.len;
}

static void test_compile_dump_sm1(void)
{
    static const char expected_ctab[] =
        "// constant table: 2 constants, version 0xfffe0200, creator \"me\", flags 0, target \"vs_2_0\"\n"
        "//   Tint: FLOAT4 c4 count 1, VECTOR FLOAT 1x4, default {1, 0.5, 0.25, 1}\n"
        "//   Diff: SAMPLER s5 count 1, OBJECT SAMPLER2D 1x1\n";
    unsigned char code[160];
    struct compile_dump_info info;
    char out[4096];
    size_t size, len;

    /* the targets: Shader Model 1-3 vertex and pixel shaders only */
    CHECK( compile_dump_sm1_target( "vs_1_1" ) );
    CHECK( compile_dump_sm1_target( "ps_2_a" ) );
    CHECK( compile_dump_sm1_target( "ps_3_0" ) );
    CHECK( compile_dump_sm1_target( "vs_3_sw" ) );
    CHECK( !compile_dump_sm1_target( "vs_4_0" ) );
    CHECK( !compile_dump_sm1_target( "ps_5_0" ) );
    CHECK( !compile_dump_sm1_target( "fx_2_0" ) );
    CHECK( !compile_dump_sm1_target( "tx_1_0" ) );
    CHECK( !compile_dump_sm1_target( "ps_3" ) );
    CHECK( !compile_dump_sm1_target( NULL ) );
    CHECK( !strcmp( COMPILE_DUMP_ALL_ENV, "PG_D3DCOMPILER_DUMP_ALL" ) );
    CHECK_EQ_U( COMPILE_DUMP_ALL_MAX_FILES, 400 );

    /* a log line per 50 files */
    CHECK( compile_dump_all_log( 50 ) && compile_dump_all_log( 100 ) && compile_dump_all_log( 400 ) );
    CHECK( !compile_dump_all_log( 0 ) && !compile_dump_all_log( 1 ) && !compile_dump_all_log( 49 ) && !compile_dump_all_log( 51 ) );
    snprintf( out, sizeof(out), COMPILE_DUMP_ALL_LOG_FORMAT, 50L, "0a1b2c3d.hlsl" );
    CHECK( !strcmp( out, "[d3dcompiler] successful shaders dumped: 50, the last 0a1b2c3d.hlsl\n" ) );
    /* A22: the line at the first compile */
    snprintf( out, sizeof(out), COMPILE_DUMP_FIRST_LOG_FORMAT, "off", "on", 35u );
    CHECK( !strcmp( out, "[d3dcompiler] dump: failed off, all on (entry vkd3d_D3DCompile2VKD3D, compiler version 35)\n" ) );

    /* ps_1_x with the backwards-compatibility flag is compiled as ps_2_0, nothing else changes */
    CHECK( !strcmp( compile_effective_profile( "ps_1_1", 0x1000 ), "ps_2_0" ) );
    CHECK( !strcmp( compile_effective_profile( "ps_1_4", 0x1200 ), "ps_2_0" ) );
    CHECK( !strcmp( compile_effective_profile( "ps_1_1", 0x200 ), "ps_1_1" ) );
    CHECK( !strcmp( compile_effective_profile( "vs_1_1", 0x1000 ), "vs_1_1" ) );
    CHECK( !strcmp( compile_effective_profile( "ps_2_0", 0x1000 ), "ps_2_0" ) );
    CHECK( !strcmp( compile_effective_profile( "ps_4_0", 0x1000 ), "ps_4_0" ) );
    CHECK( compile_effective_profile( NULL, 0x1000 ) == NULL );

    /* the constant table as text */
    size = make_sm1_code( code, 2 );
    ctab_text( code, size, out, sizeof(out) );
    CHECK( !strcmp( out, expected_ctab ) );
    /* a count running past the table, no CTAB, nothing at all */
    make_sm1_code( code, 1000 );
    ctab_text( code, size, out, sizeof(out) );
    CHECK( strstr( out, "1000 constants" ) && strstr( out, "(the constants run past the table)\n" ) );
    put32( code + 8, 0x12345678 );  /* a comment that isn't the CTAB */
    ctab_text( code, size, out, sizeof(out) );
    CHECK( !strcmp( out, "// constant table: none\n" ) );
    ctab_text( NULL, 0, out, sizeof(out) );
    CHECK( !strcmp( out, "// constant table: none\n" ) );
    make_sm1_code( code, 2 );
    put32( code + 4, 0xfffe | (200u << 16) );   /* a comment longer than the code */
    ctab_text( code, size, out, sizeof(out) );
    CHECK( !strcmp( out, "// constant table: none\n" ) );

    /* the whole file of a successful compile: its title, the table, the bytecode, the source */
    size = make_sm1_code( code, 2 );
    memset( &info, 0, sizeof(info) );
    info.succeeded = 1;
    info.code = code;
    info.code_size = size;
    info.text = "float4 main() : POSITION { return 0; }";
    info.text_size = strlen( info.text );
    info.preprocessed = 1;
    info.entry = "main";
    info.target = "vs_2_0";
    info.flags = 0x1200;
    len = compile_dump_format( out, sizeof(out) - 1, &info );
    CHECK( len < sizeof(out) );
    out[len < sizeof(out) ? len : sizeof(out) - 1] = 0;
    {
        static const char title[] = "// d3dcompiler: a successful compile, dumped for diagnosis (PG_D3DCOMPILER_DUMP_ALL=1)\n";
        CHECK( !strncmp( out, title, strlen( title ) ) );
    }
    CHECK( strstr( out, "// flags: 0x1200\n// result: 0x0\n" ) != NULL );
    CHECK( strstr( out, expected_ctab ) != NULL );
    CHECK( strstr( out, "// bytecode: 152 bytes\n//   0000: fffe0200 0023fffe 42415443 0000001c 00000085 fffe0200 00000002 0000001c\n" ) != NULL );
    CHECK( strstr( out, " 0000ffff\n\nfloat4 main() : POSITION { return 0; }\n" ) != NULL );
    CHECK( compile_dump_hash( &info ) != 0 );
}

/* ---- A25: macros, the source as given, include files, the call stack ---- */

/* a PE image with three named exports: Alpha at 0x1000, Beta at 0x1100, Fwd a forwarder at 0x2a0 */
static void make_pe( unsigned char *img, size_t size, int pe32 )
{
    unsigned int opt = 0x98, dir = opt + (pe32 ? 96 : 112);

    memset( img, 0, size );
    img[0] = 'M';
    img[1] = 'Z';
    put32( img + 0x3c, 0x80 );
    memcpy( img + 0x80, "PE\0\0", 4 );
    img[opt] = 0x0b;                                        /* 0x10b PE32, 0x20b PE32+ */
    img[opt + 1] = pe32 ? 0x01 : 0x02;
    put32( img + opt + 56, (unsigned int)size );            /* SizeOfImage */
    put32( img + opt + (pe32 ? 92 : 108), 16 );             /* NumberOfRvaAndSizes */
    put32( img + dir, 0x200 );                              /* the export directory */
    put32( img + dir + 4, 0x100 );
    put32( img + 0x200 + 0x14, 3 );
    put32( img + 0x200 + 0x18, 3 );
    put32( img + 0x200 + 0x1c, 0x240 );
    put32( img + 0x200 + 0x20, 0x250 );
    put32( img + 0x200 + 0x24, 0x260 );
    put32( img + 0x240, 0x1000 );
    put32( img + 0x244, 0x1100 );
    put32( img + 0x248, 0x2a0 );
    put32( img + 0x250, 0x278 );    /* names in alphabetical order, functions not */
    put32( img + 0x254, 0x270 );
    put32( img + 0x258, 0x280 );
    img[0x260] = 1;                 /* Beta's ordinal (index 1); Alpha's 0, Fwd's 2 */
    img[0x262] = 0;
    img[0x264] = 2;
    memcpy( img + 0x270, "Alpha", 6 );
    memcpy( img + 0x278, "Beta", 5 );
    memcpy( img + 0x280, "Fwd", 4 );
    memcpy( img + 0x2a0, "X.Y", 4 );
}

static void test_compile_dump_a25(void)
{
    static const char expected[] =
        "// d3dcompiler: a failed compile, dumped for diagnosis (PG_D3DCOMPILER_DUMP_FAILED=1)\n"
        "// source: (memory)\n"
        "// entry: main\n"
        "// target: ps_4_0\n"
        "// flags: 0x0\n"
        "// result: 0x80004005\n"
        "// compiler version: 43\n"
        "// called through: d3dx10_43.dll!D3DX10CompileFromMemory+0x5c, from game.exe+0x1234\n"
        "// call stack: wined3d.dll!vkd3d_D3DCompile2VKD3D+0x40 < d3dcompiler_43.dll!D3DCompile2+0x88"
        " < D3DCompiler_43.dll!D3DCompile+0x30 < d3dx10_43.dll!D3DX10CompileFromMemory+0x5c < game.exe+0x1234"
        " < 0x7ffe0000\n"
        "// macros: 4\n"
        "//   \"LIGHTS\" = \"3\"\n"
        "//   \"NO_VALUE\" = NULL\n"
        "//   \"EMPTY\" = \"\"\n"
        "//   \"ODD\" = \"a \\\"b\\\"\\n\\\\c\\x01\"\n"
        "// includes: 3 asked, in this order; their text follows the source below\n"
        "//   1: \"common.h\", local, 22 bytes\n"
        "//   2: \"missing.h\", system, the handler failed, 0x80004005\n"
        "//   3: \"sys.h\", system, 9 bytes\n"
        "// text: after preprocessing\n"
        "// messages:\n"
        "//   s.hlsl:9:5: E5005: Field \"pixel\" is not defined.\n"
        "\n"
        "struct PS { float4 c : SV_Target; };\n"
        "\n"
        "//==== source as given: 51 bytes ====\n"
        "#include \"common.h\"\n"
        "#if HAS_PIXEL\n"
        "int pixel;\n"
        "#endif"
        "\n//==== end ====\n"
        "\n"
        "//==== include 1: \"common.h\", local, 22 bytes ====\n"
        "#define HAS_PIXEL 1\r\n\n"
        "//==== end ====\n"
        "\n"
        "//==== include 3: \"sys.h\", system, 9 bytes ====\n"
        "float x;\n"
        "//==== end ====\n";
    static const char source[] = "#include \"common.h\"\n#if HAS_PIXEL\nint pixel;\n#endif";
    static const char preprocessed[] = "struct PS { float4 c : SV_Target; };\n";
    static const char messages[] = "s.hlsl:9:5: E5005: Field \"pixel\" is not defined.\n";
    static const struct compile_dump_macro macros[] =
    {
        { "LIGHTS", "3" }, { "NO_VALUE", NULL }, { "EMPTY", "" }, { "ODD", "a \"b\"\n\\c\x01" }, { NULL, NULL },
    };
    static const struct compile_dump_frame frames[] =
    {
        { "wined3d.dll", "vkd3d_D3DCompile2VKD3D", 0x40, 1 },
        { "d3dcompiler_43.dll", "D3DCompile2", 0x88, 0 },
        { "D3DCompiler_43.dll", "D3DCompile", 0x30, 0 },
        { "d3dx10_43.dll", "D3DX10CompileFromMemory", 0x5c, 0 },
        { "game.exe", NULL, 0x1234, 0 },
        { NULL, NULL, 0x7ffe0000, 0 },
    };
    static struct compile_dump_includes inc, many;
    static unsigned char img[0x2000];
    struct compile_dump_info info;
    unsigned int export_rva = 0;
    char out[4096];
    size_t len, i;
    char *big;

    /* the include record: in the order asked, a failed request without text */
    memset( &inc, 0, sizeof(inc) );
    compile_dump_includes_add( &inc, "common.h", 1, 0, "#define HAS_PIXEL 1\r\n\n", 22 );
    compile_dump_includes_add( &inc, "missing.h", 0, 0x80004005u, NULL, 0 );
    compile_dump_includes_add( &inc, "sys.h", 0, 0, "float x;\n", 9 );
    CHECK_EQ_U( inc.count, 3 );
    CHECK_EQ_U( inc.kept, 31 );
    CHECK( inc.files[1].text == NULL && inc.files[1].size == 0 );
    /* the handler's text is copied, not referenced */
    CHECK( inc.files[2].text && !memcmp( inc.files[2].text, "float x;\n", 9 ) );

    memset( &info, 0, sizeof(info) );
    info.text = preprocessed;
    info.text_size = strlen( preprocessed );
    info.preprocessed = 1;
    info.entry = "main";
    info.target = "ps_4_0";
    info.result = 0x80004005u;
    info.messages = messages;
    info.messages_size = strlen( messages );
    info.compiler_version = 43;
    info.has_macros = 1;
    info.macros = macros;
    info.source = source;
    info.source_size = strlen( source );
    info.includes = &inc;
    info.frames = frames;
    info.frame_count = sizeof(frames) / sizeof(frames[0]);

    /* the source as given has no newline at its end: one comes before the end marker (the
     * marker line gives the exact size); the include's "\r\n\n" stays as it is */
    len = compile_dump_format( NULL, 0, &info );
    CHECK_EQ_U( len, strlen( expected ) );
    memset( out, 0, sizeof(out) );
    CHECK_EQ_U( compile_dump_format( out, sizeof(out) - 1, &info ), len );
    CHECK( !strcmp( out, expected ) );
    if (strcmp( out, expected )) fprintf( stderr, "got:\n%s\n", out );
    /* the macros, the source and the includes don't name the file: the preprocessed text does */
    {
        struct compile_dump_info plain = info;

        plain.macros = NULL;
        plain.has_macros = 0;
        plain.includes = NULL;
        plain.source = NULL;
        CHECK_EQ_U( compile_dump_hash( &plain ), compile_dump_hash( &info ) );
    }

    /* an empty macro list apart from none; no handler; a failed D3DPreprocess (text as given, so
     * no second copy of the source) */
    info.macros = macros + 4;
    info.includes = NULL;
    info.preprocess_call = 1;
    info.preprocessed = 0;
    info.entry = info.target = NULL;
    info.text = source;
    info.text_size = strlen( source );
    len = compile_dump_format( out, sizeof(out) - 1, &info );
    out[len < sizeof(out) ? len : sizeof(out) - 1] = 0;
    {
        static const char title[] = "// d3dcompiler: a failed D3DPreprocess, dumped for diagnosis (PG_D3DCOMPILER_DUMP_FAILED=1)\n";
        CHECK( !strncmp( out, title, strlen( title ) ) );
    }
    CHECK( strstr( out, "// entry: -\n// target: -\n" ) != NULL );
    CHECK( strstr( out, "// macros: 0\n// includes: no include handler\n// text: as given (preprocessing failed)\n" ) != NULL );
    CHECK( strstr( out, "//==== source" ) == NULL );

    /* the call stack: none; no compiler DLL in front; compiler DLLs to the end */
    info.frame_count = 0;
    len = compile_dump_format( out, sizeof(out) - 1, &info );
    out[len < sizeof(out) ? len : sizeof(out) - 1] = 0;
    CHECK( strstr( out, "// called through: unknown (no call stack)\n// call stack: -\n" ) != NULL );
    info.frames = frames + 4;
    info.frame_count = 2;
    len = compile_dump_format( out, sizeof(out) - 1, &info );
    out[len < sizeof(out) ? len : sizeof(out) - 1] = 0;
    CHECK( strstr( out, "// called through: no compiler DLL, called from game.exe+0x1234\n"
                        "// call stack: game.exe+0x1234 < 0x7ffe0000\n" ) != NULL );
    info.frames = frames;
    info.frame_count = 3;
    len = compile_dump_format( out, sizeof(out) - 1, &info );
    out[len < sizeof(out) ? len : sizeof(out) - 1] = 0;
    CHECK( strstr( out, "// called through: D3DCompiler_43.dll!D3DCompile+0x30, from -\n" ) != NULL );
    info.frame_count = 1;
    len = compile_dump_format( out, sizeof(out) - 1, &info );
    out[len < sizeof(out) ? len : sizeof(out) - 1] = 0;
    CHECK( strstr( out, "// called through: unknown (no call stack)\n" ) != NULL );

    /* the compiler DLLs, any case; others not */
    CHECK( compile_dump_front_module( "d3dcompiler_47.dll" ) );
    CHECK( compile_dump_front_module( "D3DX9_36.DLL" ) );
    CHECK( compile_dump_front_module( "d3dx10_43.dll" ) );
    CHECK( compile_dump_front_module( "d3dx11_43.dll" ) );
    CHECK( compile_dump_front_module( "d3d10.dll" ) );
    CHECK( compile_dump_front_module( "d3d10_1.dll" ) );
    CHECK( !compile_dump_front_module( "d3d10core.dll" ) );
    CHECK( !compile_dump_front_module( "d3d9.dll" ) );
    CHECK( !compile_dump_front_module( "wined3d.dll" ) );
    CHECK( !compile_dump_front_module( "d3dx9" ) );
    CHECK( !compile_dump_front_module( NULL ) );

    /* the limits: 64 files, then counted only; 4 MB of text, then recorded without it */
    memset( &many, 0, sizeof(many) );
    for (i = 0; i < COMPILE_DUMP_MAX_INCLUDES + 2; i++) compile_dump_includes_add( &many, "a.h", 1, 0, "x", 1 );
    CHECK_EQ_U( many.count, 64 );
    CHECK_EQ_U( many.dropped, 2 );
    compile_dump_includes_free( &many );
    CHECK_EQ_U( many.count, 0 );
    if ((big = calloc( 1, COMPILE_DUMP_MAX_INCLUDE_BYTES )))
    {
        compile_dump_includes_add( &many, "big.h", 0, 0, big, COMPILE_DUMP_MAX_INCLUDE_BYTES - 4 );
        compile_dump_includes_add( &many, "next.h", 0, 0, big, 5 );
        compile_dump_includes_add( &many, "fits.h", 0, 0, big, 4 );
        CHECK( many.files[0].text != NULL );
        CHECK( many.files[1].text == NULL && many.files[1].size == 5 );
        CHECK( many.files[2].text != NULL );
        info.includes = &many;
        len = compile_dump_format( NULL, 0, &info );
        CHECK( len > COMPILE_DUMP_MAX_INCLUDE_BYTES );
        compile_dump_includes_free( &many );
        free( big );
    }
    compile_dump_includes_free( &inc );
    {
        struct compile_dump_out o = { out, sizeof(out) - 1, 0 };
        struct compile_dump_include_file f = { NULL, 1, 0, 5, NULL };

        compile_dump_include_header( &o, 0, &f );
        out[o.len] = 0;
        CHECK( !strcmp( out, "1: NULL, local, 5 bytes, text not kept (the 4 MB limit)" ) );
    }

    /* the nearest export, PE32+ and PE32 */
    for (i = 0; i < 2; i++)
    {
        make_pe( img, sizeof(img), (int)i );
        CHECK_EQ_U( compile_dump_pe_image_size( img, 0x1000 ), sizeof(img) );
        CHECK( !strcmp( compile_dump_nearest_export( img, sizeof(img), 0x1050, &export_rva ), "Alpha" ) );
        CHECK_EQ_U( export_rva, 0x1000 );
        CHECK( !strcmp( compile_dump_nearest_export( img, sizeof(img), 0x1100, &export_rva ), "Beta" ) );
        CHECK_EQ_U( export_rva, 0x1100 );
        CHECK( !strcmp( compile_dump_nearest_export( img, sizeof(img), 0x1fff, &export_rva ), "Beta" ) );
        CHECK( compile_dump_nearest_export( img, sizeof(img), 0x2b0, &export_rva ) == NULL );  /* a forwarder only */
        CHECK( compile_dump_nearest_export( img, sizeof(img), 0xfff, NULL ) == NULL );
        put32( img + 0x200 + 0x18, 0x40000000 );     /* names past the image */
        CHECK( compile_dump_nearest_export( img, sizeof(img), 0x1050, &export_rva ) == NULL );
    }
    img[0] = 'X';
    CHECK_EQ_U( compile_dump_pe_image_size( img, 0x1000 ), 0 );
    CHECK( compile_dump_nearest_export( NULL, 0, 0x1050, &export_rva ) == NULL );
}

/* ---- wined3d: load_drawable's re-entry guard (A23) ---- */

static void test_load_drawable_guard(void)
{
    static struct load_drawable_guard guard;
    static int textures[LOAD_DRAWABLE_GUARD_MAX + 2];  /* only their addresses count */
    unsigned int i;

    /* the cycle: an outer call, then the same texture, sub-resource and thread again */
    memset( &guard, 0, sizeof(guard) );
    CHECK_EQ_U( load_drawable_guard_enter( &guard, &textures[0], 0, 7 ), LOAD_DRAWABLE_ENTERED );
    CHECK_EQ_U( load_drawable_guard_enter( &guard, &textures[0], 0, 7 ), LOAD_DRAWABLE_REENTERED );
    /* another sub-resource, texture or thread isn't a re-entry */
    CHECK_EQ_U( load_drawable_guard_enter( &guard, &textures[0], 1, 7 ), LOAD_DRAWABLE_ENTERED );
    CHECK_EQ_U( load_drawable_guard_enter( &guard, &textures[1], 0, 7 ), LOAD_DRAWABLE_ENTERED );
    CHECK_EQ_U( load_drawable_guard_enter( &guard, &textures[0], 0, 8 ), LOAD_DRAWABLE_ENTERED );
    CHECK_EQ_U( guard.count, 4 );
    /* leaving out of order (another thread's call ends first) keeps the others */
    load_drawable_guard_leave( &guard, &textures[0], 0, 7 );
    CHECK_EQ_U( guard.count, 3 );
    CHECK_EQ_U( load_drawable_guard_enter( &guard, &textures[0], 0, 8 ), LOAD_DRAWABLE_REENTERED );
    CHECK_EQ_U( load_drawable_guard_enter( &guard, &textures[0], 0, 7 ), LOAD_DRAWABLE_ENTERED );
    load_drawable_guard_leave( &guard, &textures[0], 0, 7 );
    load_drawable_guard_leave( &guard, &textures[0], 1, 7 );
    load_drawable_guard_leave( &guard, &textures[1], 0, 7 );
    load_drawable_guard_leave( &guard, &textures[0], 0, 8 );
    CHECK_EQ_U( guard.count, 0 );
    /* leaving what isn't there changes nothing */
    load_drawable_guard_leave( &guard, &textures[0], 0, 7 );
    CHECK_EQ_U( guard.count, 0 );

    /* a full table: a new call goes on untracked, a running one is still caught */
    for (i = 0; i < LOAD_DRAWABLE_GUARD_MAX; ++i)
        CHECK_EQ_U( load_drawable_guard_enter( &guard, &textures[i], 0, 1 ), LOAD_DRAWABLE_ENTERED );
    CHECK_EQ_U( load_drawable_guard_enter( &guard, &textures[LOAD_DRAWABLE_GUARD_MAX], 0, 1 ),
                LOAD_DRAWABLE_UNTRACKED );
    CHECK_EQ_U( load_drawable_guard_enter( &guard, &textures[3], 0, 1 ), LOAD_DRAWABLE_REENTERED );

    CHECK( !strcmp( LOAD_DRAWABLE_REENTERED_MESSAGE,
                    "[wined3d] load_drawable re-entered for the same sub-resource; frame not shown\n" ) );
}

/* ---- wined3d: formats not FBO attachable, logged once each (A23) ---- */

static void test_format_check_log(void)
{
    static struct format_check_log log;
    char line[256];
    unsigned int i, logged = 0;

    /* once per format, whatever the resource types and repeats */
    CHECK( format_check_log_first( &log, 21 ) );
    CHECK( !format_check_log_first( &log, 21 ) );
    CHECK( format_check_log_first( &log, 22 ) );
    CHECK( !format_check_log_first( &log, 21 ) );
    CHECK_EQ_U( log.count, 2 );
    /* the table full: nothing more is logged */
    for (i = 100; i < 100 + 2 * FORMAT_CHECK_LOG_MAX; ++i)
        logged += format_check_log_first( &log, i );
    CHECK_EQ_U( logged, FORMAT_CHECK_LOG_MAX - 2 );
    CHECK( !format_check_log_first( &log, 22 ) );

    snprintf( line, sizeof(line), FORMAT_CHECK_LOG_FORMAT, "WINED3DFMT_B5G6R5_UNORM", 0x8d62u, 1u,
              "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT", "GL_INVALID_OPERATION" );
    CHECK( !strcmp( line, "[wined3d] format WINED3DFMT_B5G6R5_UNORM (GL internal format 0x8d62) is not FBO "
                    "attachable, resource type 1: GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT, GL error GL_INVALID_OPERATION\n" ) );
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
    test_compile_failure_dump();
    test_compile_dump_sm1();
    test_compile_dump_a25();
    test_load_drawable_guard();
    test_format_check_log();
    if (failures)
    {
        fprintf( stderr, "test_part_a: %d failure(s)\n", failures );
        return 1;
    }
    printf( "test_part_a: all passed\n" );
    return 0;
}
