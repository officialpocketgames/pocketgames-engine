// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Host tests of the pure parts of the iOS win32u (spec 12 Part G). Build and run with
// run-host-tests.sh (any C11 compiler; no Wine, no iOS SDK needed).

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "w32ios_pure.h"
#include "w32ios_kmt_pure.h"

typedef void *HWND;
typedef void *HCURSOR;
typedef unsigned int UINT;
typedef int INT;
typedef uint32_t DWORD;
typedef int BOOL;
typedef struct { int32_t left, top, right, bottom; } RECT;
struct window_rects;
struct window_surface;
#include "wine_unix_contract.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf( "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond ); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; \
    if (_a != _b) { failures++; printf( "%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b ); } } while (0)

static int same( struct w32ios_size s, int w, int h ) { return s.w == w && s.h == h; }

/* ---------------------------------------------------------------- Part C: modes */

static void test_modes( void )
{
    struct w32ios_size def = { 1024, 768 }, list[W32IOS_STD_MODE_COUNT + 1];
    static const int expect_default[][2] =
    {
        { 1024, 768 }, { 640, 360 }, { 640, 400 }, { 640, 480 }, { 720, 480 }, { 720, 576 },
        { 800, 480 }, { 800, 600 }, { 848, 480 }, { 854, 480 }, { 960, 540 }, { 960, 600 },
        { 960, 720 }, { 1024, 576 }, { 1024, 600 }, { 1024, 640 }, { 1152, 648 }, { 1176, 664 },
    };
    int i, j, count;

    /* order, cap, current first */
    count = w32ios_build_modes( def, def, 0, list, W32IOS_STD_MODE_COUNT + 1 );
    CHECK_EQ( count, 18 );
    for (i = 0; i < count && i < 18; i++) CHECK( same( list[i], expect_default[i][0], expect_default[i][1] ) );

    /* a smaller current mode: first, then the same standard list (the list doesn't shrink) */
    {
        struct w32ios_size cur = { 640, 480 };
        count = w32ios_build_modes( cur, def, 0, list, W32IOS_STD_MODE_COUNT + 1 );
        CHECK_EQ( count, 18 );
        CHECK( same( list[0], 640, 480 ) );
        CHECK( same( list[1], 640, 360 ) );
        CHECK( same( list[2], 640, 400 ) );
        CHECK( same( list[3], 720, 480 ) );   /* 640x480 not twice */
        CHECK( same( list[15], 1024, 768 ) );
    }

    /* a current mode above the default (1280x720 through the four-times rule) */
    {
        struct w32ios_size cur = { 1280, 720 };
        count = w32ios_build_modes( cur, def, 0, list, W32IOS_STD_MODE_COUNT + 1 );
        CHECK_EQ( count, 19 );
        CHECK( same( list[0], 1280, 720 ) );
        CHECK( same( list[16], 1024, 768 ) );
    }

    /* PG_EXTENDED_MODES=1: four times the default's pixels (2048x1536 is exactly that) */
    count = w32ios_build_modes( def, def, 1, list, W32IOS_STD_MODE_COUNT + 1 );
    CHECK_EQ( count, 36 );
    CHECK( same( list[count - 1], 2048, 1536 ) );

    /* no duplicates, ever */
    for (i = 0; i < count; i++)
        for (j = i + 1; j < count; j++) CHECK( !same( list[i], list[j].w, list[j].h ) );

    /* max respected */
    CHECK_EQ( w32ios_build_modes( def, def, 0, list, 3 ), 3 );
    CHECK_EQ( w32ios_build_modes( def, def, 0, list, 0 ), 0 );

    /* the standard table itself */
    CHECK( same( w32ios_std_modes[0], 640, 360 ) );
    CHECK( same( w32ios_std_modes[W32IOS_STD_MODE_COUNT - 1], 2560, 1440 ) );
}

static void test_mode_decision( void )
{
    struct w32ios_size def = { 1024, 768 }, cur = { 1024, 768 }, out;

    /* restore */
    CHECK_EQ( w32ios_mode_decision( 0, 0, 0, 0, 0, cur, def, 0, &out ), W32IOS_CDS_RESTORE );
    CHECK( same( out, 1024, 768 ) );
    {
        struct w32ios_size cur2 = { 800, 600 };
        CHECK_EQ( w32ios_mode_decision( 1, 0, 0, 0, 0, cur2, def, 0, &out ), W32IOS_CDS_RESTORE );
        CHECK( same( out, 1024, 768 ) );
    }
    /* in the list */
    CHECK_EQ( w32ios_mode_decision( 1, 1, 1, 800, 600, cur, def, 0, &out ), W32IOS_CDS_ACCEPT );
    CHECK( same( out, 800, 600 ) );
    /* the four-times rule: standard modes up to four times the default's pixels */
    CHECK_EQ( w32ios_mode_decision( 1, 1, 1, 1280, 720, cur, def, 0, &out ), W32IOS_CDS_ACCEPT );
    CHECK_EQ( w32ios_mode_decision( 1, 1, 1, 1920, 1080, cur, def, 0, &out ), W32IOS_CDS_ACCEPT );
    CHECK_EQ( w32ios_mode_decision( 1, 1, 1, 2048, 1536, cur, def, 0, &out ), W32IOS_CDS_ACCEPT );
    CHECK_EQ( w32ios_mode_decision( 1, 1, 1, 2560, 1440, cur, def, 0, &out ), W32IOS_CDS_BADMODE );
    /* not a standard mode */
    CHECK_EQ( w32ios_mode_decision( 1, 1, 1, 1000, 700, cur, def, 0, &out ), W32IOS_CDS_BADMODE );
    CHECK_EQ( w32ios_mode_decision( 1, 1, 1, 0, 0, cur, def, 0, &out ), W32IOS_CDS_BADMODE );
    /* the session default even when it isn't standard */
    {
        struct w32ios_size odd = { 1000, 700 };
        CHECK_EQ( w32ios_mode_decision( 1, 1, 1, 1000, 700, cur, odd, 0, &out ), W32IOS_CDS_ACCEPT );
    }
    /* one field: the other keeps the current value */
    CHECK_EQ( w32ios_mode_decision( 1, 1, 0, 800, 0, cur, def, 0, &out ), W32IOS_CDS_BADMODE );
    CHECK( same( out, 800, 768 ) );
    {
        struct w32ios_size cur2 = { 1024, 600 };
        CHECK_EQ( w32ios_mode_decision( 1, 0, 1, 0, 576, cur2, def, 0, &out ), W32IOS_CDS_ACCEPT );
        CHECK( same( out, 1024, 576 ) );
    }
}

static size_t wide( uint16_t *dst, const char *src )
{
    size_t i;
    for (i = 0; src[i]; i++) dst[i] = (unsigned char)src[i];
    dst[i] = 0;
    return i;
}

static void test_device_names( void )
{
    uint16_t name[64];
    size_t len;

    CHECK_EQ( w32ios_match_device( NULL, 0 ), W32IOS_DEV_ADAPTER );
    len = wide( name, "" );
    CHECK_EQ( w32ios_match_device( name, len ), W32IOS_DEV_ADAPTER );
    len = wide( name, "\\\\.\\DISPLAY1" );
    CHECK_EQ( w32ios_match_device( name, len ), W32IOS_DEV_ADAPTER );
    len = wide( name, "\\\\.\\display1" );
    CHECK_EQ( w32ios_match_device( name, len ), W32IOS_DEV_ADAPTER );
    CHECK_EQ( w32ios_match_device( name, len + 1 ), W32IOS_DEV_ADAPTER );   /* a counted NUL */
    len = wide( name, "\\\\.\\DISPLAY1\\Monitor0" );
    CHECK_EQ( w32ios_match_device( name, len ), W32IOS_DEV_MONITOR );
    len = wide( name, "\\\\.\\Display1\\MONITOR0" );
    CHECK_EQ( w32ios_match_device( name, len ), W32IOS_DEV_MONITOR );
    len = wide( name, "\\\\.\\DISPLAY2" );
    CHECK_EQ( w32ios_match_device( name, len ), W32IOS_DEV_NONE );
    len = wide( name, "\\\\.\\DISPLAY10" );
    CHECK_EQ( w32ios_match_device( name, len ), W32IOS_DEV_NONE );
    len = wide( name, "WinDisc" );
    CHECK_EQ( w32ios_match_device( name, len ), W32IOS_DEV_NONE );
    len = wide( name, "\\\\.\\DISPLAY1\\Monitor1" );
    CHECK_EQ( w32ios_match_device( name, len ), W32IOS_DEV_NONE );
}

static void test_driver_version( void )
{
    CHECK( !strcmp( w32ios_driver_version( 0x10de ), "32.0.15.8157" ) );
    CHECK( !strcmp( w32ios_driver_version( 0x1002 ), "35.0.21025.1024" ) );
    CHECK( !strcmp( w32ios_driver_version( 0x8086 ), "35.0.101.6314" ) );
    CHECK( !strcmp( w32ios_driver_version( 0x106b ), "35.0.10.1000" ) );
    CHECK_EQ( w32ios_pack_version( "32.0.15.8157" ), 0x00200000000f1fddll );
    CHECK_EQ( w32ios_pack_version( "35.0.10.1000" ), 0x00230000000a03e8ll );
    CHECK_EQ( w32ios_pack_version( "35.0.21025.1024" ), (35ll << 48) | (21025ll << 16) | 1024 );
    CHECK_EQ( w32ios_pack_version( "1.2.3.4" ), 0x0001000200030004ll );
    CHECK_EQ( w32ios_pack_version( NULL ), 0 );
}

static void test_session_default( void )
{
    struct w32ios_size s;

    s = w32ios_session_default( "1366", "1024" );
    CHECK( same( s, 1366, 1024 ) );
    s = w32ios_session_default( NULL, "768" );
    CHECK( same( s, 1024, 768 ) );
    s = w32ios_session_default( "0", "768" );
    CHECK( same( s, 1024, 768 ) );
    s = w32ios_session_default( "-5", "768" );
    CHECK( same( s, 1024, 768 ) );
    s = w32ios_session_default( "abc", "1" );
    CHECK( same( s, 1024, 768 ) );
    s = w32ios_session_default( " 800 ", "600" );
    CHECK( same( s, 800, 600 ) );

    /* clarification 6: only a process with no parent starts a session */
    CHECK( w32ios_is_session_root( 1, 0 ) );
    CHECK( !w32ios_is_session_root( 1, 0x20 ) );   /* wineboot, a launcher's game, services */
    CHECK( !w32ios_is_session_root( 0, 0 ) );      /* the query failed */
}

/* clarification 10: the display state as w32ios_display_session_start changes it */
struct launch_state { int set; struct w32ios_size def, cur; };

static enum w32ios_launch_size launch( struct launch_state *st, const char *w, const char *h, int others )
{
    struct w32ios_size want = w32ios_session_default( w, h );
    enum w32ios_launch_size what = w32ios_launch_size( !st->set, st->set ? st->cur : want, want, 0 );

    if (what == W32IOS_LAUNCH_CHANGE && others) what = W32IOS_LAUNCH_STAYS;
    switch (what)
    {
    case W32IOS_LAUNCH_FIRST:
    case W32IOS_LAUNCH_CHANGE: st->def = st->cur = want; st->set = 1; break;
    case W32IOS_LAUNCH_SAME:   st->def = want; break;
    case W32IOS_LAUNCH_STAYS:  break;
    }
    return what;
}

static void test_launch_size( void )
{
    static const struct w32ios_size s1280 = { 1280, 720 }, s960 = { 960, 720 }, s1024 = { 1024, 768 };
    struct w32ios_size modes[W32IOS_STD_MODE_COUNT + 1];
    struct launch_state st = { 0 };
    int n, i, has_1280;

    /* the decision alone */
    CHECK_EQ( w32ios_launch_size( 1, s1024, s1280, 1 ), W32IOS_LAUNCH_FIRST );
    CHECK_EQ( w32ios_launch_size( 0, s1280, s1280, 1 ), W32IOS_LAUNCH_SAME );
    CHECK_EQ( w32ios_launch_size( 0, s1280, s960, 0 ), W32IOS_LAUNCH_CHANGE );
    CHECK_EQ( w32ios_launch_size( 0, s1280, s960, 1 ), W32IOS_LAUNCH_STAYS );

    /* two top-level programs in one session: 1280x720, then 960x720 with nothing else running */
    CHECK_EQ( launch( &st, "1280", "720", 0 ), W32IOS_LAUNCH_FIRST );
    CHECK( same( st.cur, 1280, 720 ) );
    CHECK_EQ( launch( &st, "960", "720", 0 ), W32IOS_LAUNCH_CHANGE );
    CHECK( same( st.cur, 960, 720 ) && same( st.def, 960, 720 ) );
    /* what its first EnumDisplaySettings sees: current first, no mode above its own pixel count */
    n = w32ios_build_modes( st.cur, st.def, 0, modes, W32IOS_STD_MODE_COUNT + 1 );
    CHECK( n > 0 && same( modes[0], 960, 720 ) );
    for (i = 0, has_1280 = 0; i < n; i++) if (same( modes[i], 1280, 720 )) has_1280 = 1;
    CHECK( !has_1280 );

    /* refused: a third program asks for 1280x720 while another user program runs */
    CHECK_EQ( launch( &st, "1280", "720", 1 ), W32IOS_LAUNCH_STAYS );
    CHECK( same( st.cur, 960, 720 ) && same( st.def, 960, 720 ) );

    /* the same size again: nothing to change, whoever runs */
    CHECK_EQ( launch( &st, "960", "720", 1 ), W32IOS_LAUNCH_SAME );
    /* after a ChangeDisplaySettings to 800x600, a program asking for 960x720 gets it back */
    st.cur.w = 800; st.cur.h = 600;
    CHECK_EQ( launch( &st, "960", "720", 0 ), W32IOS_LAUNCH_CHANGE );
    CHECK( same( st.cur, 960, 720 ) );
}

static size_t to16( uint16_t *out, size_t cap, const char *s )
{
    size_t i;
    for (i = 0; s[i] && i < cap - 1; i++) out[i] = (uint16_t)(unsigned char)s[i];
    out[i] = 0;
    return i;
}

static int user_program( const char *path, const char *root, const char *name )
{
    uint16_t p[128], r[64], n[64];
    size_t pl = path ? to16( p, 128, path ) : 0, rl = to16( r, 64, root ), nl = to16( n, 64, name );
    return w32ios_is_user_program( path ? p : NULL, pl, r, rl, n, nl );
}

static void test_user_program( void )
{
    CHECK( !user_program( "C:\\windows\\system32\\services.exe", "C:\\windows", "services.exe" ) );
    CHECK( !user_program( "C:\\windows\\explorer.exe", "C:\\windows", "explorer.exe" ) );
    CHECK( !user_program( "c:\\WINDOWS\\SysWOW64\\winedevice.exe", "C:\\windows", "winedevice.exe" ) );
    CHECK( !user_program( "C:\\windows\\system32\\conhost.exe", "C:\\windows\\", "conhost.exe" ) );
    CHECK( user_program( "C:\\Games\\Game\\Game.exe", "C:\\windows", "Game.exe" ) );
    CHECK( user_program( "C:\\windowsapps\\Game.exe", "C:\\windows", "Game.exe" ) );   /* not inside */
    CHECK( user_program( "C:\\windows", "C:\\windows", "windows" ) );
    CHECK( user_program( NULL, "C:\\windows", "Game.exe" ) );                         /* unreadable */
    CHECK( user_program( "C:\\Games\\Game.exe", "", "Game.exe" ) );                   /* no root */
    /* helpers don't count, wherever they are */
    CHECK( !user_program( "C:\\Games\\Game\\UnityCrashHandler64.exe", "C:\\windows", "UnityCrashHandler64.exe" ) );
    CHECK( !user_program( NULL, "C:\\windows", "CrashReporter.exe" ) );
    CHECK( !user_program( "C:\\Games\\x\\BugReport.exe", "C:\\windows", "BugReport.exe" ) );
    CHECK( !user_program( "C:\\Games\\x\\EpicHelper.exe", "C:\\windows", "EpicHelper.exe" ) );
}

/* ---------------------------------------------------------------- Part E */

static void test_cursor( void )
{
    uint8_t out[4 * 4];

    /* colour with alpha: as is (a transparent pixel's colour cleared) */
    {
        const uint8_t color[] = { 10, 20, 30, 128,  1, 2, 3, 0 };
        CHECK( w32ios_cursor_has_alpha( color, 2, 1 ) );
        w32ios_cursor_color( color, NULL, 2, 1, out );
        CHECK( out[0] == 10 && out[1] == 20 && out[2] == 30 && out[3] == 128 );
        CHECK( out[4] == 0 && out[5] == 0 && out[6] == 0 && out[7] == 0 );
    }
    /* colour without alpha: alpha from the AND mask, 0 = opaque */
    {
        const uint8_t color[] = { 10, 20, 30, 0,  40, 50, 60, 0 };
        const uint8_t mask[4] = { 0x40, 0, 0, 0 };   /* pixel 1 transparent */
        CHECK( !w32ios_cursor_has_alpha( color, 2, 1 ) );
        w32ios_cursor_color( color, mask, 2, 1, out );
        CHECK( out[0] == 10 && out[1] == 20 && out[2] == 30 && out[3] == 255 );
        CHECK( out[7] == 0 );
    }
    /* two rows: the mask's rows are padded to 32 bits */
    {
        const uint8_t color[] = { 1, 1, 1, 0,  2, 2, 2, 0 };
        const uint8_t mask[8] = { 0x00, 0, 0, 0,  0x80, 0, 0, 0 };   /* row 1 transparent */
        w32ios_cursor_color( color, mask, 1, 2, out );
        CHECK( out[3] == 255 && out[7] == 0 );
    }
    /* monochrome: AND above XOR. AND-only transparent, XOR white, neither black */
    {
        const uint8_t mask[8] = { 0x90, 0, 0, 0,  0x50, 0, 0, 0 };   /* AND 1001, XOR 0101 */
        w32ios_cursor_mono( mask, 4, 1, out );
        CHECK( out[0] == 0 && out[3] == 0 );                                      /* AND only */
        CHECK( out[4] == 255 && out[5] == 255 && out[6] == 255 && out[7] == 255 ); /* XOR */
        CHECK( out[8] == 0 && out[9] == 0 && out[10] == 0 && out[11] == 255 );    /* neither */
        CHECK( out[12] == 255 && out[15] == 255 );                                 /* both: XOR */
    }
}

static void test_extended_key( void )
{
    CHECK( !w32ios_key_extended( 0x41, 0x1e, 1 ) );       /* 'A' */
    CHECK( w32ios_key_extended( 0x25, 0xe04b, 1 ) );      /* left arrow, e0 prefix */
    CHECK( w32ios_key_extended( 0x25, 0xe04b, 0 ) );      /* the prefix counts even without the nav rule */
    CHECK( w32ios_key_extended( 0x13, 0xe11d, 1 ) );      /* e1 prefix */
    CHECK( w32ios_key_extended( 0x21, 0x49, 1 ) );        /* Page Up without prefix: nav rule */
    CHECK( !w32ios_key_extended( 0x21, 0x49, 0 ) );       /* PG_NAV_KEYS_E0=0 */
    CHECK( w32ios_key_extended( 0x2e, 0x53, 1 ) );        /* Delete */
    CHECK( w32ios_key_extended( 0x2d, 0x52, 1 ) );        /* Insert */
    CHECK( w32ios_key_extended( 0x24, 0x47, 1 ) );        /* Home */
    CHECK( w32ios_key_extended( 0x28, 0x50, 1 ) );        /* Down */
    CHECK( !w32ios_key_extended( 0x0d, 0x1c, 1 ) );       /* Return */
    CHECK( !w32ios_key_extended( 0x10, 0x2a, 1 ) );       /* Shift */
}

static void test_log_event( void )
{
    CHECK( w32ios_log_event( 0 ) && w32ios_log_event( 7 ) );
    CHECK( !w32ios_log_event( 8 ) && !w32ios_log_event( 300 ) );
    CHECK( w32ios_log_event( 256 ) && w32ios_log_event( 512 ) );
}

/* spec 15 clarification 9b: the raw input diagnostics' counts and slot devices */
static void test_diag( void )
{
    struct w32ios_diag d;

    memset( &d, 0, sizeof(d) );
    CHECK_EQ( w32ios_diag_due( &d, 100000 ), 0 );        /* nothing counted */
    CHECK_EQ( w32ios_diag_event( &d, 1000 ), 1 );
    CHECK_EQ( w32ios_diag_event( &d, 2000 ), 2 );
    CHECK_EQ( w32ios_diag_event( &d, 3000 ), 3 );
    CHECK_EQ( w32ios_diag_due( &d, 5999 ), 0 );          /* the window began at 1000 */
    CHECK_EQ( w32ios_diag_due( &d, 6000 ), 3 );
    CHECK_EQ( w32ios_diag_due( &d, 12000 ), 0 );         /* no events since */
    CHECK_EQ( w32ios_diag_event( &d, 12000 ), 4 );
    CHECK_EQ( w32ios_diag_due( &d, 12000 ), 1 );         /* that window began at 6000 */
    CHECK_EQ( w32ios_diag_event( &d, 13000 ), 5 );
    CHECK_EQ( w32ios_diag_due( &d, 16999 ), 0 );         /* the next began at 12000 */
    CHECK_EQ( w32ios_diag_due( &d, 17000 ), 1 );
    CHECK_EQ( d.total, 5 );
    CHECK_EQ( w32ios_diag_due( &d, 1000 ), 0 );          /* a clock before the window */

    CHECK_EQ( w32ios_rawinput_slot( 0x101, 0x045e, 0x028e ), 0 );
    CHECK_EQ( w32ios_rawinput_slot( 0x104, 0x045e, 0x028e ), 3 );
    CHECK_EQ( w32ios_rawinput_slot( 0x100, 0x054c, 0x0ce6 ), -1 );   /* the HID pad */
    CHECK_EQ( w32ios_rawinput_slot( 0x105, 0x045e, 0x028e ), -1 );
    CHECK_EQ( w32ios_rawinput_slot( 0x102, 0x045e, 0x02ea ), -1 );   /* another Microsoft pad */
}

static void test_slicing( void )
{
    int final;

    CHECK_EQ( w32ios_slice_deadline( 1000, 0, 0, W32IOS_SLICE, &final ), 1000 + 160000 );
    CHECK_EQ( final, 0 );
    CHECK_EQ( w32ios_slice_deadline( 1000, 1, 50000, W32IOS_SLICE, &final ), 50000 );
    CHECK_EQ( final, 1 );
    CHECK_EQ( w32ios_slice_deadline( 1000, 1, 161000, W32IOS_SLICE, &final ), 161000 );
    CHECK_EQ( final, 1 );
    CHECK_EQ( w32ios_slice_deadline( 1000, 1, 200000, W32IOS_SLICE, &final ), 161000 );
    CHECK_EQ( final, 0 );
    CHECK_EQ( w32ios_slice_deadline( 1000, 1, 500, W32IOS_SLICE, &final ), 500 );   /* already past */
    CHECK_EQ( final, 1 );

    /* a pure poll's set_queue_mask request at most once a second */
    CHECK( w32ios_poll_skip_request( 1, 1500, 1000 ) );
    CHECK( !w32ios_poll_skip_request( 1, 2000, 1000 ) );
    CHECK( !w32ios_poll_skip_request( 0, 1500, 1000 ) );
    CHECK( w32ios_poll_skip_request( 1, 100, 0xffffff00u ) );   /* tick wrap */
}

static void test_rects( void )
{
    CHECK( w32ios_covers_screen( 0, 0, 1024, 768, 1024, 768 ) );
    CHECK( w32ios_covers_screen( -8, -8, 1032, 776, 1024, 768 ) );
    CHECK( !w32ios_covers_screen( 0, 0, 1023, 768, 1024, 768 ) );
    CHECK( !w32ios_dialog_candidate( 0, 0, 31, 100, 1024, 768 ) );
    CHECK( !w32ios_dialog_candidate( 0, 0, 100, 31, 1024, 768 ) );
    CHECK( w32ios_dialog_candidate( 0, 0, 32, 32, 1024, 768 ) );
    CHECK( w32ios_dialog_candidate( 100, 100, 400, 300, 1024, 768 ) );
    CHECK( !w32ios_dialog_candidate( 0, 0, 1024, 768, 1024, 768 ) );
}

/* ---------------------------------------------------------------- Part B */

static int dead_slot = -1;
static void *slot_pebs[W32IOS_PP_SLOTS];

static int fake_is_dead( void *peb, uint32_t pid, void *ctx )
{
    return dead_slot >= 0 && peb == slot_pebs[dead_slot];
}

static void test_pp_table( void )
{
    static struct w32ios_pp_table table;
    char pebs[W32IOS_PP_SLOTS + 2];
    int i, index, reuse;

    memset( &table, 0, sizeof(table) );
    CHECK_EQ( w32ios_pp_find( &table, &pebs[0], 1 ), -1 );

    index = w32ios_pp_pick( &table, &pebs[0], 1, NULL, NULL, &reuse );
    CHECK_EQ( index, 0 );
    CHECK_EQ( reuse, 0 );
    w32ios_pp_begin( &table, index );
    CHECK( table.slot[index].seq & 1 );                       /* readers skip it */
    CHECK_EQ( w32ios_pp_find( &table, &pebs[0], 1 ), -1 );
    w32ios_pp_publish( &table, index, &pebs[0], 1 );
    CHECK( !(table.slot[index].seq & 1) );
    CHECK_EQ( w32ios_pp_find( &table, &pebs[0], 1 ), 0 );
    CHECK_EQ( w32ios_pp_find( &table, &pebs[0], 2 ), -1 );    /* keyed by PEB and process id */

    index = w32ios_pp_pick( &table, &pebs[0], 1, NULL, NULL, &reuse );
    CHECK_EQ( index, 0 );
    CHECK_EQ( reuse, 1 );

    index = w32ios_pp_pick( &table, &pebs[1], 2, NULL, NULL, &reuse );
    CHECK_EQ( index, 1 );
    w32ios_pp_publish( &table, index, &pebs[1], 2 );
    CHECK_EQ( w32ios_pp_find( &table, &pebs[1], 2 ), 1 );

    /* fill every slot */
    for (i = 2; i < W32IOS_PP_SLOTS; i++)
    {
        index = w32ios_pp_pick( &table, &pebs[i], 100 + i, NULL, NULL, &reuse );
        CHECK_EQ( index, i );
        w32ios_pp_publish( &table, index, &pebs[i], 100 + i );
    }
    for (i = 0; i < W32IOS_PP_SLOTS; i++) slot_pebs[i] = &pebs[i];

    /* full: a new process gets nothing while every process lives */
    dead_slot = -1;
    CHECK_EQ( w32ios_pp_pick( &table, &pebs[W32IOS_PP_SLOTS], 999, fake_is_dead, NULL, &reuse ), -1 );
    CHECK_EQ( w32ios_pp_pick( &table, &pebs[W32IOS_PP_SLOTS], 999, NULL, NULL, &reuse ), -1 );
    /* ... a slot whose process ended is recycled */
    dead_slot = 5;
    CHECK_EQ( w32ios_pp_pick( &table, &pebs[W32IOS_PP_SLOTS], 999, fake_is_dead, NULL, &reuse ), 5 );
    CHECK_EQ( reuse, 0 );
    /* ... and a new process at a dead one's PEB takes its slot first */
    CHECK_EQ( w32ios_pp_pick( &table, &pebs[7], 555, fake_is_dead, NULL, &reuse ), 7 );
    w32ios_pp_begin( &table, 7 );
    w32ios_pp_publish( &table, 7, &pebs[7], 555 );
    CHECK_EQ( w32ios_pp_find( &table, &pebs[7], 555 ), 7 );
    CHECK_EQ( w32ios_pp_find( &table, &pebs[7], 107 ), -1 );   /* the old one is gone */
}

static void test_client_procs( void )
{
    static w32ios_proc_pair global[17];
    static struct w32ios_client_procs own;
    int a, b, c;

    global[3][1] = &a;
    own.procs[3][0] = &b;
    own.procs[4][1] = &c;

    /* a process that didn't fill its table answers with the global one */
    CHECK( w32ios_procs_table( &own, global ) == global );
    CHECK( w32ios_procs_table( NULL, global ) == global );
    own.set = 1;
    CHECK( w32ios_procs_table( &own, global ) == own.procs );

    /* matching uses the caller's table, either flavour */
    CHECK_EQ( w32ios_procs_match( own.procs, 17, &b ), 3 );
    CHECK_EQ( w32ios_procs_match( own.procs, 17, &c ), 4 );
    CHECK_EQ( w32ios_procs_match( own.procs, 17, &a ), -1 );
    CHECK_EQ( w32ios_procs_match( global, 17, &a ), 3 );
    CHECK_EQ( w32ios_procs_match( own.procs, 17, NULL ), -1 );
    CHECK_EQ( w32ios_procs_match( own.procs, 4, &c ), -1 );   /* only the first count entries */

    /* handles from entries */
    CHECK_EQ( w32ios_procs_index( own.procs, 17, &own.procs[5] ), 5 );
    CHECK_EQ( w32ios_procs_index( own.procs, 17, &global[5] ), -1 );
    CHECK_EQ( w32ios_procs_index( own.procs, 17, &own.procs[17] ), -1 );
}

static void test_builtin_action( void )
{
    CHECK_EQ( w32ios_builtin_action( 1, 0, 7 ), W32IOS_BUILTIN_SKIP );
    CHECK_EQ( w32ios_builtin_action( 1, 9, 7 ), W32IOS_BUILTIN_SKIP );
    CHECK_EQ( w32ios_builtin_action( 0, 7, 7 ), W32IOS_BUILTIN_SKIP );     /* re-entry */
    CHECK_EQ( w32ios_builtin_action( 0, 9, 7 ), W32IOS_BUILTIN_WAIT );
    CHECK_EQ( w32ios_builtin_action( 0, 0, 7 ), W32IOS_BUILTIN_REGISTER );
}

static void test_ceiling( void )
{
    CHECK_EQ( w32ios_ceiling( 0, 0x7ffeffffull ), 0 );
    CHECK_EQ( w32ios_ceiling( 1, 0x7ffeffffull ), 0x7fffffffll );
    CHECK_EQ( w32ios_ceiling( 1, 0xfffeffffull ), 0xffffffffll );
    CHECK_EQ( w32ios_ceiling( 1, 0 ), 0x7fffffffll );
}

static void test_winproc_handle( void )
{
    uint64_t base = 0x300000000ull;

    CHECK_EQ( w32ios_winproc_handle( base + 0xffff0012ull, base, 1 ), 0xffff0012ll );
    CHECK_EQ( w32ios_winproc_handle( base + 0xffff0012ull, base, 0 ), (long long)(base + 0xffff0012ull) );
    CHECK_EQ( w32ios_winproc_handle( base + 0xffff0012ull, 0, 1 ), (long long)(base + 0xffff0012ull) );
    CHECK_EQ( w32ios_winproc_handle( 0xffff0012ull, base, 1 ), 0xffff0012ll );       /* already a handle */
    CHECK_EQ( w32ios_winproc_handle( base + 0x12345678ull, base, 1 ), (long long)(base + 0x12345678ull) );
    CHECK_EQ( w32ios_winproc_handle( base + 0x1ffff0012ull, base, 1 ), (long long)(base + 0x1ffff0012ull) );
    CHECK_EQ( w32ios_winproc_handle( 0x200ffff0012ull, base, 1 ), 0x200ffff0012ll );  /* far above */
    CHECK_EQ( w32ios_winproc_handle( base - 0x10000ull, base, 1 ), (long long)(base - 0x10000ull) );
}

/* ---------------------------------------------------------------- Part F */

static struct pgios_gamepad fake_pad;
static int fake_rumble, vib_index = -1;
static uint16_t vib_left, vib_right;

static int fake_get_state( int index, void *out )
{
    memcpy( out, &fake_pad, sizeof(fake_pad) );
    return fake_pad.connected;
}

static void fake_set_vibration( int index, uint16_t left, uint16_t right )
{
    vib_index = index;
    vib_left = left;
    vib_right = right;
}

static int fake_rumble_caps( void ) { return fake_rumble; }

static void test_xinput( void )
{
    const struct w32ios_pad_source src = { fake_get_state, fake_set_vibration, fake_rumble_caps };
    unsigned char buffer[32];
    struct w32ios_xinput_state state;
    struct w32ios_xinput_capabilities caps;
    struct w32ios_xinput_vibration vib = { 0x1111, 0x2222 };

    memset( &fake_pad, 0, sizeof(fake_pad) );
    fake_pad.packet = 7;
    fake_pad.buttons = 0x1234;
    fake_pad.left_trigger = 10;
    fake_pad.right_trigger = 20;
    fake_pad.lx = 1;
    fake_pad.ly = -2;
    fake_pad.rx = 3;
    fake_pad.ry = -4;
    fake_pad.connected = 1;

    /* State: exactly 16 bytes */
    memset( buffer, 0xcc, sizeof(buffer) );
    CHECK_EQ( w32ios_gamepad_answer( &src, 0 | (W32IOS_PAD_STATE << 8), buffer ), 1 );
    memcpy( &state, buffer, sizeof(state) );
    CHECK_EQ( state.dwPacketNumber, 7 );
    CHECK_EQ( state.Gamepad.wButtons, 0x1234 );
    CHECK_EQ( state.Gamepad.bLeftTrigger, 10 );
    CHECK_EQ( state.Gamepad.bRightTrigger, 20 );
    CHECK_EQ( state.Gamepad.sThumbLX, 1 );
    CHECK_EQ( state.Gamepad.sThumbLY, -2 );
    CHECK_EQ( state.Gamepad.sThumbRX, 3 );
    CHECK_EQ( state.Gamepad.sThumbRY, -4 );
    CHECK_EQ( buffer[16], 0xcc );
    /* the byte layout of XINPUT_STATE */
    CHECK_EQ( buffer[0], 7 );
    CHECK_EQ( buffer[4], 0x34 );
    CHECK_EQ( buffer[5], 0x12 );
    CHECK_EQ( buffer[6], 10 );
    CHECK_EQ( buffer[7], 20 );

    /* bad index, no buffer, disconnected */
    CHECK_EQ( w32ios_gamepad_answer( &src, 4, buffer ), 0 );
    CHECK_EQ( w32ios_gamepad_answer( &src, 0, NULL ), 0 );
    CHECK_EQ( w32ios_gamepad_answer( &src, 3 | (7 << 8), buffer ), 0 );   /* unknown operation */
    fake_pad.connected = 0;
    CHECK_EQ( w32ios_gamepad_answer( &src, 1, buffer ), 0 );
    CHECK_EQ( w32ios_gamepad_answer( &src, 1 | (W32IOS_PAD_CAPS << 8), buffer ), 0 );
    fake_pad.connected = 1;

    /* Caps: 20 bytes */
    memset( buffer, 0xcc, sizeof(buffer) );
    fake_rumble = 1;
    CHECK_EQ( w32ios_gamepad_answer( &src, 2 | (W32IOS_PAD_CAPS << 8), buffer ), 1 );
    memcpy( &caps, buffer, sizeof(caps) );
    CHECK_EQ( caps.Type, 1 );
    CHECK_EQ( caps.SubType, 1 );
    CHECK_EQ( caps.Flags, 0 );
    CHECK_EQ( caps.Gamepad.wButtons, 0xf3ff );
    CHECK_EQ( caps.Gamepad.bLeftTrigger, 0xff );
    CHECK_EQ( caps.Gamepad.bRightTrigger, 0xff );
    CHECK_EQ( (uint16_t)caps.Gamepad.sThumbLX, 0xffc0 );
    CHECK_EQ( (uint16_t)caps.Gamepad.sThumbRY, 0xffc0 );
    CHECK_EQ( caps.Vibration.wLeftMotorSpeed, 0xff );
    CHECK_EQ( caps.Vibration.wRightMotorSpeed, 0xff );
    CHECK_EQ( buffer[20], 0xcc );
    fake_rumble = 0;
    CHECK_EQ( w32ios_gamepad_answer( &src, 2 | (W32IOS_PAD_CAPS << 8), buffer ), 1 );
    memcpy( &caps, buffer, sizeof(caps) );
    CHECK_EQ( caps.Vibration.wLeftMotorSpeed, 0 );

    /* SetVibration */
    CHECK_EQ( w32ios_gamepad_answer( &src, 3 | (W32IOS_PAD_VIBRATION << 8), &vib ), 1 );
    CHECK_EQ( vib_index, 3 );
    CHECK_EQ( vib_left, 0x1111 );
    CHECK_EQ( vib_right, 0x2222 );

    CHECK_EQ( sizeof(struct pgios_gamepad), 20 );
}

/* ---------------------------------------------------------------- Part D */

static int wide_equals( const w32ios_wchar *w, const char *s )
{
    while (*s) if (*w++ != (unsigned char)*s++) return 0;
    return !*w;
}

static void test_kmt( void )
{
    const struct w32ios_kmt_adapter_desc desc = { 0x106b, 0x0001, 4096ull << 20, 0x00230000000a03e8ull, "Apple GPU" };
    static struct w32ios_kmt_adapter_registry_info reg;
    static struct w32ios_kmt_driver_description descr;
    uint32_t status, value, small = 0;

    CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_OTHER, &value, sizeof(value), &desc, &status ), 0 );

    CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_DRIVERVERSION, &value, sizeof(value), &desc, &status ), 1 );
    CHECK_EQ( status, W32IOS_STATUS_SUCCESS );
    CHECK_EQ( value, 3100 );
    CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_DRIVERVERSION, &small, 2, &desc, &status ), 1 );
    CHECK_EQ( status, W32IOS_STATUS_INVALID_PARAMETER );

    {
        struct w32ios_kmt_driver_version v;
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_UMD_DRIVER_VERSION, &v, sizeof(v), &desc, &status ), 1 );
        CHECK_EQ( v.DriverVersion, 0x00230000000a03e8ll );
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_KMD_DRIVER_VERSION, &v, sizeof(v), &desc, &status ), 1 );
        CHECK_EQ( v.DriverVersion, 0x00230000000a03e8ll );
    }
    {
        struct w32ios_kmt_adapter_type t;
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_ADAPTERTYPE, &t, sizeof(t), &desc, &status ), 1 );
        CHECK_EQ( t.Value, 0x3 );   /* render + display, not software */
    }
    {
        struct w32ios_kmt_wddm_2_0_caps c20;
        struct w32ios_kmt_wddm_2_7_caps c27;
        struct w32ios_kmt_wddm_2_9_caps c29;
        struct w32ios_kmt_wddm_3_0_caps c30;
        struct w32ios_kmt_wddm_3_1_caps c31;
        struct w32ios_kmt_wddm_1_2_caps c12;
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_WDDM_2_0_CAPS, &c20, sizeof(c20), &desc, &status ), 1 );
        CHECK_EQ( c20.Value, 0x3 );   /* 64-bit atomics, GPU MMU */
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_WDDM_2_7_CAPS, &c27, sizeof(c27), &desc, &status ), 1 );
        CHECK_EQ( c27.Value, 0x3 );   /* hardware scheduling supported and enabled */
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_WDDM_2_9_CAPS, &c29, sizeof(c29), &desc, &status ), 1 );
        CHECK_EQ( c29.Value, 0x6 );   /* support state stable, enabled */
        c30.Value = c31.Value = 0xffffffff;
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_WDDM_3_0_CAPS, &c30, sizeof(c30), &desc, &status ), 1 );
        CHECK_EQ( c30.Value, 0 );
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_WDDM_3_1_CAPS, &c31, sizeof(c31), &desc, &status ), 1 );
        CHECK_EQ( c31.Value, 0 );
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_WDDM_1_2_CAPS, &c12, sizeof(c12), &desc, &status ), 1 );
        CHECK_EQ( c12.GraphicsPreemptionGranularity, 200 );
        CHECK( c12.Value & 0x80 );   /* hardware cursor */
    }
    {
        struct w32ios_kmt_physical_adapter_count c;
        struct w32ios_kmt_query_device_ids ids = { 0 };
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_PHYSICALADAPTERCOUNT, &c, sizeof(c), &desc, &status ), 1 );
        CHECK_EQ( c.Count, 1 );
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_PHYSICALADAPTERDEVICEIDS, &ids, sizeof(ids), &desc, &status ), 1 );
        CHECK_EQ( status, W32IOS_STATUS_SUCCESS );
        CHECK_EQ( ids.DeviceIds.VendorID, 0x106b );
        CHECK_EQ( ids.DeviceIds.DeviceID, 0x0001 );
        CHECK_EQ( ids.DeviceIds.SubSystemID, 0 );
        ids.PhysicalAdapterIndex = 1;
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_PHYSICALADAPTERDEVICEIDS, &ids, sizeof(ids), &desc, &status ), 1 );
        CHECK_EQ( status, W32IOS_STATUS_INVALID_PARAMETER );
    }
    {
        struct w32ios_kmt_adapter_address a = { 9, 9, 9 };
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_ADAPTERADDRESS, &a, sizeof(a), &desc, &status ), 1 );
        CHECK( !a.BusNumber && !a.DeviceNumber && !a.FunctionNumber );
    }
    {
        struct w32ios_kmt_segment_size s;
        struct w32ios_kmt_segment_group_size g = { 0 };
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_GETSEGMENTSIZE, &s, sizeof(s), &desc, &status ), 1 );
        CHECK_EQ( s.DedicatedVideoMemorySize, 4096ll << 20 );
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_GETSEGMENTGROUPSIZE, &g, sizeof(g), &desc, &status ), 1 );
        CHECK_EQ( g.LocalMemory, 4096ll << 20 );
        CHECK_EQ( g.LegacyInfo.DedicatedVideoMemorySize, 4096ll << 20 );
        g.PhysicalAdapterIndex = 2;
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_GETSEGMENTGROUPSIZE, &g, sizeof(g), &desc, &status ), 1 );
        CHECK_EQ( status, W32IOS_STATUS_INVALID_PARAMETER );
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_GETSEGMENTSIZE, &s, sizeof(s) - 1, &desc, &status ), 1 );
        CHECK_EQ( status, W32IOS_STATUS_INVALID_PARAMETER );
    }
    CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_ADAPTERREGISTRYINFO, &reg, sizeof(reg), &desc, &status ), 1 );
    CHECK( wide_equals( reg.AdapterString, "Apple GPU" ) );
    CHECK( wide_equals( reg.BiosString, "Apple GPU" ) );
    CHECK( wide_equals( reg.ChipType, "Apple GPU" ) );
    CHECK( wide_equals( reg.DacType, "Integrated RAMDAC" ) );
    CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_DRIVER_DESCRIPTION, &descr, sizeof(descr), &desc, &status ), 1 );
    CHECK( wide_equals( descr.DriverDescription, "Apple GPU" ) );
    {
        struct w32ios_kmt_node_metadata n = { 0 };
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_NODEMETADATA, &n, sizeof(n), &desc, &status ), 1 );
        CHECK_EQ( status, W32IOS_STATUS_SUCCESS );
        CHECK_EQ( n.EngineType, W32IOS_ENGINE_TYPE_3D );
        CHECK( wide_equals( n.FriendlyName, "3D" ) );
        CHECK_EQ( n.GpuMmuSupported, 1 );
        n.NodeOrdinalAndAdapterIndex = 1;
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_NODEMETADATA, &n, sizeof(n), &desc, &status ), 1 );
        CHECK_EQ( status, W32IOS_STATUS_INVALID_PARAMETER );
        n.NodeOrdinalAndAdapterIndex = 0x10000;   /* adapter index in the high half: node 0 */
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_NODEMETADATA, &n, sizeof(n), &desc, &status ), 1 );
        CHECK_EQ( status, W32IOS_STATUS_SUCCESS );
    }
    {
        struct w32ios_kmt_adapter_perfdata p = { 0 };
        struct w32ios_kmt_adapter_perfdata_caps pc = { 0 };
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_ADAPTERPERFDATA, &p, sizeof(p), &desc, &status ), 1 );
        CHECK_EQ( p.Temperature, 380 );
        CHECK( p.MemoryFrequency <= p.MaxMemoryFrequency );
        CHECK_EQ( w32ios_kmt_answer( W32IOS_QAI_ADAPTERPERFDATA_CAPS, &pc, sizeof(pc), &desc, &status ), 1 );
        CHECK( pc.TemperatureWarning < pc.TemperatureMax );
        CHECK( p.MemoryBandwidth <= pc.MaxMemoryBandwidth );
    }
}

static void test_memory_and_luid( void )
{
    CHECK_EQ( w32ios_dedicated_memory( "2048", 99 ), 2048ll << 20 );
    CHECK_EQ( w32ios_dedicated_memory( "256", 99 ), 256ll << 20 );
    CHECK_EQ( w32ios_dedicated_memory( "255", 99 ), 99 );
    CHECK_EQ( w32ios_dedicated_memory( "abc", 99 ), 99 );
    CHECK_EQ( w32ios_dedicated_memory( "", 99 ), 99 );
    CHECK_EQ( w32ios_dedicated_memory( NULL, 99 ), 99 );
    CHECK_EQ( w32ios_dedicated_memory( " 3072 ", 99 ), 3072ll << 20 );
    CHECK_EQ( w32ios_luid_from_registry_id( 0x0102030405060708ull ), 0x0807060504030201ll );
}

/* ---------------------------------------------------------------- helpers */

static void test_settings( void )
{
    const char text[] = "# comment\nvram-mb = 2048\n  other=1\nvram-mb=3072  \nnoequals\nlist = a=b\r\n";
    const char bom[] = "\xef\xbb\xbfkey = v\n";
    char value[32];

    CHECK( w32ios_cfg_find( text, strlen( text ), "vram-mb", value, sizeof(value) ) );
    CHECK( !strcmp( value, "3072" ) );   /* the last occurrence wins */
    CHECK( w32ios_cfg_find( text, strlen( text ), "other", value, sizeof(value) ) );
    CHECK( !strcmp( value, "1" ) );
    CHECK( w32ios_cfg_find( text, strlen( text ), "list", value, sizeof(value) ) );
    CHECK( !strcmp( value, "a=b" ) );    /* the value runs to the end of the line */
    CHECK( !w32ios_cfg_find( text, strlen( text ), "noequals", value, sizeof(value) ) );
    CHECK( !w32ios_cfg_find( text, strlen( text ), "comment", value, sizeof(value) ) );
    CHECK( !w32ios_cfg_find( text, strlen( text ), "vram", value, sizeof(value) ) );
    CHECK( w32ios_cfg_find( bom, strlen( bom ), "key", value, sizeof(value) ) );
    CHECK( !strcmp( value, "v" ) );

    CHECK( w32ios_parse_bool( "1", 0 ) && w32ios_parse_bool( "on", 0 ) && w32ios_parse_bool( "TRUE", 0 ) &&
           w32ios_parse_bool( "yes", 0 ) );
    CHECK( !w32ios_parse_bool( "0", 1 ) && !w32ios_parse_bool( "off", 1 ) && !w32ios_parse_bool( "False", 1 ) );
    CHECK( w32ios_parse_bool( "maybe", 1 ) && !w32ios_parse_bool( "maybe", 0 ) );
    CHECK( !w32ios_parse_bool( NULL, 0 ) && w32ios_parse_bool( "", 1 ) );
}

int main( void )
{
    test_modes();
    test_mode_decision();
    test_device_names();
    test_driver_version();
    test_session_default();
    test_launch_size();
    test_user_program();
    test_cursor();
    test_extended_key();
    test_log_event();
    test_diag();
    test_slicing();
    test_rects();
    test_pp_table();
    test_client_procs();
    test_builtin_action();
    test_ceiling();
    test_winproc_handle();
    test_xinput();
    test_kmt();
    test_memory_and_luid();
    test_settings();

    printf( "%d checks, %d failures\n", checks, failures );
    return failures ? 1 : 0;
}
