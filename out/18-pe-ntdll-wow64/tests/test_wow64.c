// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/* Host tests: spec 18 Part J (wow64 guest window) and Part K (wow64win tables). */

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "../pe/dlls/wow64/wow64_window.h"
#include "wow64win_shim.h"
#include "../pe/dlls/wow64win/wow64win_window.h"

static const uint64_t bases[] = { 0, 0x100000000ull, 0x7200000000ull };

static void test_conversions(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(bases) / sizeof(bases[0]); i++)
    {
        uint64_t b = bases[i];

        /* wow64 */
        CHECK_EQ( wow_guest_to_host( b, 0 ), 0 );
        CHECK_EQ( wow_host_to_guest( b, 0 ), 0 );
        CHECK_EQ( wow_guest_to_host( b, 0x401000 ), b + 0x401000 );
        CHECK_EQ( wow_host_to_guest( b, b + 0x401000 ), 0x401000 );
        CHECK_EQ( wow_guest_to_host( b, 0xffffffff ), b + 0xffffffff );  /* the window's last byte */
        CHECK_EQ( wow_host_to_guest( b, b + 0xffffffff ), 0xffffffff );
        CHECK_EQ( wow_guest_to_host( b, 0x8002 ), b + 0x8002 );          /* below 0x10000: converted */
        CHECK_EQ( wow_host_to_guest( b, b + 0x1234 ), 0x1234 );
        /* host -> guest of a host address is its low 32 bits */
        CHECK_EQ( wow_host_to_guest( b, b + 0x12345678 ), (uint32_t)(b + 0x12345678) );

        /* wow64win: atoms and resource IDs unchanged */
        CHECK_EQ( w64w_guest_to_host( b, 0 ), 0 );
        CHECK_EQ( w64w_host_to_guest( b, 0 ), 0 );
        CHECK_EQ( w64w_guest_to_host( b, 0x8002 ), 0x8002 );
        CHECK_EQ( w64w_host_to_guest( b, 0x8002 ), 0x8002 );
        CHECK_EQ( w64w_guest_to_host( b, 0xffff ), 0xffff );
        CHECK_EQ( w64w_guest_to_host( b, 0x10000 ), b + 0x10000 );
        CHECK_EQ( w64w_host_to_guest( b, b + 0x10000 ), 0x10000 );
        CHECK_EQ( w64w_guest_to_host( b, 0xffffffff ), b + 0xffffffff );
        CHECK_EQ( w64w_host_to_guest( b, b + 0xffffffff ), 0xffffffff );
        CHECK_EQ( w64w_guest_to_host( b, 0xffff0012 ), b + 0xffff0012 );  /* a window-procedure handle */
        CHECK_EQ( w64w_host_to_guest( b, b + 0x00450000 ), 0x00450000 );
    }

    /* B = 0 is the identity everywhere */
    CHECK_EQ( wow_guest_to_host( 0, 0x12345678 ), 0x12345678 );
    CHECK_EQ( wow_host_to_guest( 0, 0x12345678 ), 0x12345678 );
    CHECK_EQ( w64w_guest_to_host( 0, 0x12345678 ), 0x12345678 );
    CHECK_EQ( w64w_host_to_guest( 0, 0x12345678 ), 0x12345678 );

    /* gates */
    CHECK( wow_gate_ok( 0, 0x7ffe1234ull + 0x7200000000ull ) );  /* no window: no check */
    CHECK( wow_gate_ok( 0x7200000000ull, 0x7ffe1000 ) );
    CHECK( !wow_gate_ok( 0x7200000000ull, 0 ) );
    CHECK( !wow_gate_ok( 0x7200000000ull, 0x7200001000ull ) );   /* converted twice would be this */
}

/* J3 */

struct fake_procs
{
    int pid_queries, base_queries;
    uint64_t handle_pid[64];
    uint64_t pid_base[64];
};

static int fake_pid( void *ctx, uint64_t handle, uint64_t *pid )
{
    struct fake_procs *p = ctx;
    p->pid_queries++;
    if (handle >= 64 || !p->handle_pid[handle]) return 0;
    *pid = p->handle_pid[handle];
    return 1;
}

static int fake_base( void *ctx, uint64_t handle, uint64_t *base )
{
    struct fake_procs *p = ctx;
    p->base_queries++;
    if (handle >= 64 || !p->handle_pid[handle]) return 0;
    *base = p->pid_base[p->handle_pid[handle]];
    return 1;
}

static void test_cache(void)
{
    struct wow_base_cache cache;
    struct fake_procs procs;
    const uint64_t own = 0x100000000ull;
    unsigned int i;

    memset( &cache, 0, sizeof(cache) );
    memset( &procs, 0, sizeof(procs) );
    for (i = 1; i < 20; i++) procs.handle_pid[i] = i;         /* handle i -> pid i */
    for (i = 1; i < 20; i++) procs.pid_base[i] = (uint64_t)(i + 1) << 32;

    /* B = 0: no query at all */
    CHECK_EQ( wow_base_for_process( &cache, 0, 5, 0, fake_pid, fake_base, &procs ), 0 );
    CHECK_EQ( procs.pid_queries + procs.base_queries, 0 );

    /* own process and NULL handle */
    CHECK_EQ( wow_base_for_process( &cache, own, (uint64_t)-1, 1, fake_pid, fake_base, &procs ), own );
    CHECK_EQ( wow_base_for_process( &cache, own, 0, 0, fake_pid, fake_base, &procs ), own );
    CHECK_EQ( procs.pid_queries + procs.base_queries, 0 );

    /* miss, then hit */
    CHECK_EQ( wow_base_for_process( &cache, own, 5, 0, fake_pid, fake_base, &procs ), 6ull << 32 );
    CHECK_EQ( procs.pid_queries, 1 );
    CHECK_EQ( procs.base_queries, 1 );
    CHECK_EQ( wow_base_for_process( &cache, own, 5, 0, fake_pid, fake_base, &procs ), 6ull << 32 );
    CHECK_EQ( procs.base_queries, 1 );
    CHECK_EQ( cache.entries[0].pid, 5 );

    /* a failing query or a process without a window gives 0 and isn't cached */
    CHECK_EQ( wow_base_for_process( &cache, own, 40, 0, fake_pid, fake_base, &procs ), 0 );
    CHECK_EQ( cache.entries[1].handle, 0 );
    procs.pid_base[19] = 0;
    procs.handle_pid[19] = 19;  /* pid 19 has no window yet */
    CHECK_EQ( wow_base_for_process( &cache, own, 19, 0, fake_pid, fake_base, &procs ), 0 );
    CHECK_EQ( cache.entries[1].handle, 0 );
    procs.pid_base[19] = 20ull << 32;  /* its window appears */
    CHECK_EQ( wow_base_for_process( &cache, own, 19, 0, fake_pid, fake_base, &procs ), 20ull << 32 );
    CHECK_EQ( cache.entries[1].handle, 19 );
    CHECK_EQ( cache.entries[1].pid, 19 );

    /* 9 different handles: round robin evicts the oldest */
    memset( &cache, 0, sizeof(cache) );
    procs.pid_queries = procs.base_queries = 0;
    for (i = 1; i <= 9; i++)
        CHECK_EQ( wow_base_for_process( &cache, own, i, 0, fake_pid, fake_base, &procs ), (uint64_t)(i + 1) << 32 );
    CHECK_EQ( procs.base_queries, 9 );
    CHECK_EQ( cache.entries[0].handle, 9 );  /* handle 1 was evicted */
    for (i = 2; i <= 9; i++) wow_base_for_process( &cache, own, i, 0, fake_pid, fake_base, &procs );
    CHECK_EQ( procs.base_queries, 9 );       /* all hits */
    wow_base_for_process( &cache, own, 1, 0, fake_pid, fake_base, &procs );
    CHECK_EQ( procs.base_queries, 10 );      /* handle 1 queried again */

    /* a handle value reused for another process (after its entry was evicted) gets the new base */
    memset( &cache, 0, sizeof(cache) );
    CHECK_EQ( wow_base_for_process( &cache, own, 7, 0, fake_pid, fake_base, &procs ), 8ull << 32 );
    for (i = 10; i < 18; i++) wow_base_for_process( &cache, own, i, 0, fake_pid, fake_base, &procs );
    procs.handle_pid[7] = 3;                 /* handle 7 now names pid 3 */
    CHECK_EQ( wow_base_for_process( &cache, own, 7, 0, fake_pid, fake_base, &procs ), 4ull << 32 );
    for (i = 0; i < WOW_BASE_CACHE_SIZE; i++)
        if (cache.entries[i].handle == 7) CHECK_EQ( cache.entries[i].pid, 3 );
}

/* J7 */

static void roundtrip( uint64_t b, uint32_t code, uint32_t n, const uint32_t *params )
{
    uint64_t host[15];
    uint32_t back[15], count, i;

    count = wow_exc_params_32to64( b, code, n, params, host );
    CHECK_EQ( count, n > 15 ? 15 : n );
    CHECK_EQ( wow_exc_params_64to32( b, code, count, host, back ), count );
    for (i = 0; i < count; i++) CHECK_EQ( back[i], params[i] );
}

static void test_exceptions(void)
{
    static const uint32_t av[] = { 1, 0x00401234 };
    static const uint32_t av_null[] = { 0, 0 };
    static const uint32_t av_unknown[] = { 0, 0xffffffff };
    static const uint32_t name_thread[] = { 0x1000, 0x00402000, 0xffffffff, 0 };
    static const uint32_t name_other[] = { 0x1001, 0x00402000, 0, 0 };
    static const uint32_t stub_name[] = { 0x00403000, 0x00403100 };
    static const uint32_t stub_ordinal[] = { 0x00403000, 0x0042 };
    static const uint32_t print[] = { 12, 0x00404000 };
    static const uint32_t many[20] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
    uint64_t host[15];
    uint32_t back[15];
    unsigned int i;

    for (i = 0; i < sizeof(bases) / sizeof(bases[0]); i++)
    {
        uint64_t b = bases[i];

        roundtrip( b, WOW_EXC_ACCESS_VIOLATION, 2, av );
        roundtrip( b, WOW_EXC_ACCESS_VIOLATION, 2, av_null );
        roundtrip( b, WOW_EXC_ACCESS_VIOLATION, 2, av_unknown );
        roundtrip( b, WOW_EXC_IN_PAGE_ERROR, 2, av );
        roundtrip( b, WOW_EXC_WINE_NAME_THREAD, 4, name_thread );
        roundtrip( b, WOW_EXC_WINE_NAME_THREAD, 4, name_other );
        roundtrip( b, WOW_EXC_WINE_STUB, 2, stub_name );
        roundtrip( b, WOW_EXC_WINE_STUB, 2, stub_ordinal );
        roundtrip( b, WOW_EXC_DBG_PRINTEXCEPTION_C, 2, print );
        roundtrip( b, WOW_EXC_DBG_PRINTEXCEPTION_WIDE, 2, print );
        roundtrip( b, 0xe06d7363, 4, name_thread );  /* C++ exception: all scalars */
        roundtrip( b, WOW_EXC_ACCESS_VIOLATION, 20, many );

        /* what is converted */
        wow_exc_params_32to64( b, WOW_EXC_ACCESS_VIOLATION, 2, av, host );
        CHECK_EQ( host[0], 1 );
        CHECK_EQ( host[1], b + 0x00401234 );
        wow_exc_params_32to64( b, WOW_EXC_ACCESS_VIOLATION, 2, av_null, host );
        CHECK_EQ( host[1], 0 );
        wow_exc_params_32to64( b, WOW_EXC_WINE_NAME_THREAD, 4, name_thread, host );
        CHECK_EQ( host[1], b + 0x00402000 );
        CHECK_EQ( host[2], 0xffffffff );
        wow_exc_params_32to64( b, WOW_EXC_WINE_NAME_THREAD, 4, name_other, host );
        CHECK_EQ( host[1], 0x00402000 );
        wow_exc_params_32to64( b, WOW_EXC_WINE_STUB, 2, stub_name, host );
        CHECK_EQ( host[0], b + 0x00403000 );
        CHECK_EQ( host[1], b + 0x00403100 );
        wow_exc_params_32to64( b, WOW_EXC_WINE_STUB, 2, stub_ordinal, host );
        CHECK_EQ( host[0], b + 0x00403000 );
        CHECK_EQ( host[1], 0x42 );                   /* ordinals stay */
        wow_exc_params_32to64( b, WOW_EXC_DBG_PRINTEXCEPTION_C, 2, print, host );
        CHECK_EQ( host[0], 12 );
        CHECK_EQ( host[1], b + 0x00404000 );
        /* only parameters below NumberParameters */
        wow_exc_params_32to64( b, WOW_EXC_ACCESS_VIOLATION, 1, av, host );
        CHECK_EQ( host[0], 1 );

        /* Wow64RaiseException's sentinel for an unknown address comes back as 0xffffffff */
        host[0] = 0;
        host[1] = wow_guest_to_host( b, 0xffffffff );
        CHECK_EQ( host[1], b + 0xffffffff );
        wow_exc_params_64to32( b, WOW_EXC_ACCESS_VIOLATION, 2, host, back );
        CHECK_EQ( back[1], 0xffffffff );
    }

    /* more than 15 parameters are clamped both ways */
    CHECK_EQ( wow_exc_clamp_params( 16 ), 15 );
    CHECK_EQ( wow_exc_clamp_params( 0xffffffff ), 15 );
    CHECK_EQ( wow_exc_clamp_params( 3 ), 3 );
    memset( back, 0xcc, sizeof(back) );
    {
        uint64_t many64[15];
        for (i = 0; i < 15; i++) many64[i] = i + 1;
        CHECK_EQ( wow_exc_params_64to32( 0x100000000ull, 0x12345678, 99, many64, back ), 15 );
        CHECK_EQ( back[14], 15 );
    }
    /* the mask never names a parameter at or beyond the count */
    CHECK_EQ( wow_exc_address_mask( WOW_EXC_ACCESS_VIOLATION, 1, 0, 0x1234 ), 0 );
    CHECK_EQ( wow_exc_address_mask( WOW_EXC_WINE_STUB, 1, 0x1234, 0x123456 ), 1 );
    CHECK_EQ( wow_exc_address_mask( WOW_EXC_WINE_STUB, 2, 0x1234, 0x123456 ), 3 );
    CHECK_EQ( wow_exc_address_mask( WOW_EXC_WINE_NAME_THREAD, 2, 0x1000, 0x123456 ), 2 );
    CHECK_EQ( wow_exc_address_mask( WOW_EXC_WINE_NAME_THREAD, 0, 0x1000, 0x123456 ), 0 );
}

static void test_small_rules(void)
{
    static const unsigned short xtajit[] = { 'x','t','a','j','i','t','.','d','l','l',0 };
    static const unsigned short xtajit_upper[] = { 'X','T','A','J','I','T','.','D','L','L',0 };
    static const unsigned short other[] = { 'w','o','w','a','r','m','h','w','.','d','l','l',0 };
    static const unsigned short prefix[] = { 'x','t','a','j','i','t',0 };

    /* J10 */
    CHECK( wow_continue_arg_is_boolean( 0 ) );
    CHECK( wow_continue_arg_is_boolean( 1 ) );
    CHECK( wow_continue_arg_is_boolean( 0xff ) );
    CHECK( !wow_continue_arg_is_boolean( 0x100 ) );
    CHECK( !wow_continue_arg_is_boolean( 0x0012f000 ) );

    /* J12 */
    CHECK( wow_free_passthrough( 0x100000000ull, 1 ) );
    CHECK( wow_free_passthrough( 0x100000000ull, 0 ) );
    CHECK( wow_free_passthrough( 0x100000000ull, 0xffff ) );
    CHECK( !wow_free_passthrough( 0x100000000ull, 0x10000 ) );
    CHECK( !wow_free_passthrough( 0, 1 ) );  /* no window: upstream's path */

    /* J9 */
    CHECK( !wow_cpu_fallback_differs( xtajit, xtajit ) );
    CHECK( !wow_cpu_fallback_differs( xtajit_upper, xtajit ) );
    CHECK( wow_cpu_fallback_differs( other, xtajit ) );
    CHECK( wow_cpu_fallback_differs( prefix, xtajit ) );
}

static void test_tables(void)
{
    static const unsigned int lparam_msgs[] =
    {
        WM_NCCREATE, WM_CREATE, WM_GETTEXT, WM_ASKCBFORMATNAME, WM_WININICHANGE, WM_SETTEXT,
        WM_DEVMODECHANGE, CB_DIR, LB_DIR, LB_ADDFILE, EM_REPLACESEL, WM_GETMINMAXINFO, WM_DRAWITEM,
        WM_MEASUREITEM, WM_DELETEITEM, WM_COMPAREITEM, WM_WINE_SETWINDOWPOS, WM_WINDOWPOSCHANGING,
        WM_WINDOWPOSCHANGED, WM_COPYDATA, WM_NOTIFY, WM_HELP, WM_STYLECHANGING, WM_STYLECHANGED,
        WM_NCCALCSIZE, WM_GETDLGCODE, SBM_SETSCROLLINFO, SBM_GETSCROLLINFO, SBM_GETSCROLLBARINFO,
        EM_GETSEL, SBM_GETRANGE, CB_GETEDITSEL, EM_GETRECT, LB_GETITEMRECT, CB_GETDROPPEDCONTROLRECT,
        EM_SETRECT, EM_SETRECTNP, EM_GETLINE, EM_SETTABSTOPS, LB_SETTABSTOPS, CB_ADDSTRING,
        CB_INSERTSTRING, CB_FINDSTRING, CB_FINDSTRINGEXACT, CB_SELECTSTRING, CB_GETLBTEXT,
        CB_GETCOMBOBOXINFO, LB_ADDSTRING, LB_INSERTSTRING, LB_FINDSTRING, LB_FINDSTRINGEXACT,
        LB_SELECTSTRING, LB_GETTEXT, LB_GETSELITEMS, WM_NEXTMENU, WM_SIZING, WM_MOVING, WM_MDICREATE,
        WM_MDIGETACTIVE, WM_DEVICECHANGE, WM_WINE_KEYBOARD_LL_HOOK, WM_WINE_MOUSE_LL_HOOK,
        WM_PAINTCLIPBOARD, WM_DROPOBJECT, WM_QUERYDROPOBJECT, WM_DRAGLOOP, WM_DRAGSELECT, WM_DRAGMOVE,
    };
    static const unsigned int value_msgs[] =
    {
        WM_PAINT, WM_MOUSEMOVE, WM_KEYDOWN, WM_TIMER, WM_SETFONT, LB_GETCOUNT, WM_COMMAND, WM_USER,
        WM_WINE_OTHER_SHIM, 0,
    };
    unsigned int i;

    CHECK_EQ( sizeof(lparam_msgs) / sizeof(lparam_msgs[0]), 68 );
    for (i = 0; i < sizeof(lparam_msgs) / sizeof(lparam_msgs[0]); i++)
        CHECK( w64w_msg_lparam_is_pointer( lparam_msgs[i] ) );
    for (i = 0; i < sizeof(value_msgs) / sizeof(value_msgs[0]); i++)
    {
        CHECK( !w64w_msg_lparam_is_pointer( value_msgs[i] ) );
        CHECK( !w64w_msg_wparam_is_pointer( value_msgs[i] ) );
    }
    CHECK( w64w_msg_wparam_is_pointer( EM_GETSEL ) );
    CHECK( w64w_msg_wparam_is_pointer( SBM_GETRANGE ) );
    CHECK( w64w_msg_wparam_is_pointer( CB_GETEDITSEL ) );
    CHECK( !w64w_msg_wparam_is_pointer( WM_SETTEXT ) );
    CHECK( !w64w_msg_wparam_is_pointer( WM_COPYDATA ) );

    /* NtUserCallOneParam */
    CHECK( w64w_one_param_converts( NtUserCallOneParam_GetPrimaryMonitorRect ) );
    CHECK( w64w_one_param_converts( NtUserCallOneParam_GetAsyncKeyboardState ) );
    CHECK( w64w_one_param_converts( NtUserCallOneParam_D3DKMTOpenAdapterFromGdiDisplayName ) );
    CHECK( w64w_one_param_converts( NtUserGetDeskPattern ) );
    CHECK( !w64w_one_param_converts( NtUserCallOneParam_EnableThunkLock ) );
    CHECK( !w64w_one_param_converts( NtUserCallOneParam_OtherShim ) );

    /* NtUserCallTwoParam */
    CHECK_EQ( w64w_two_param_mask( NtUserCallTwoParam_GetMonitorInfo ), W64W_ARG2 );
    CHECK_EQ( w64w_two_param_mask( NtUserCallTwoParam_SetIMECompositionRect ), W64W_ARG2 );
    CHECK_EQ( w64w_two_param_mask( NtUserCallTwoParam_SetIconParam ), W64W_ARG2 );
    CHECK_EQ( w64w_two_param_mask( NtUserCallTwoParam_GetGamepadState ), W64W_ARG2 );
    CHECK_EQ( w64w_two_param_mask( NtUserCallTwoParam_GetMenuInfo ), W64W_ARG2 );
    CHECK_EQ( w64w_two_param_mask( NtUserCallTwoParam_MonitorFromRect ), W64W_ARG1 );
    CHECK_EQ( w64w_two_param_mask( NtUserCallTwoParam_GetVirtualScreenRect ), W64W_ARG1 );
    CHECK_EQ( w64w_two_param_mask( NtUserCallTwoParam_AdjustWindowRect ), W64W_ARG1 | W64W_ARG2 );
    CHECK_EQ( w64w_two_param_mask( NtUserCallTwoParam_GetDialogProc ), 0 );
    CHECK_EQ( w64w_two_param_mask( NtUserAllocWinProc ), 0 );

    /* NtUserCallHwndParam */
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_ClientToScreen ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_ScreenToClient ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_GetChildRect ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_GetWindowInfo ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_GetWindowThread ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_ExposeWindowSurface ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_SetRawWindowPos ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_GetScrollInfo ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_GetWindowRect ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_GetClientRect ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_GetPresentRect ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_MapWindowPoints ) );
    CHECK( w64w_hwnd_param_converts( NtUserCallHwndParam_SendHardwareInput ) );
    CHECK( !w64w_hwnd_param_converts( NtUserCallHwndParam_SetDialogInfo ) );
    CHECK( !w64w_hwnd_param_converts( NtUserCallHwndParam_SetMDIClientInfo ) );
    CHECK( !w64w_hwnd_param_converts( NtUserCallHwndParam_OtherShim ) );
}

int main(void)
{
    test_conversions();
    test_cache();
    test_exceptions();
    test_small_rules();
    test_tables();
    return check_report( "wow64" );
}
