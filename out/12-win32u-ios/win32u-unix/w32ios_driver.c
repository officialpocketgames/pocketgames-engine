// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part E: the iOS user driver, installed at win32u's init (__wine_set_user_driver). Its
// entries forward to the app's weak hooks (interfaces/wine_unix_contract.h Section B) when they
// are linked; pUpdateDisplayDevices describes the virtual monitor of Part C. Two modes: desktop
// mode (PG_DESKTOP=1, a Windows desktop the app composites) and game mode (otherwise).
//
// Also the app's way in (Section A: pgios_drv_post_mouse / pgios_drv_post_key) and the message
// wait helpers patched into message.c (E2).

#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"
#include "w32ios.h"
#include "w32ios_app.h"

WINE_DEFAULT_DEBUG_CHANNEL(pgios);

/* ---------------------------------------------------------------- switches */

static int env_switch( int *cache, const char *name, BOOL def )
{
    if (*cache < 0) *cache = w32ios_env_bool( name, def );
    return *cache;
}

BOOL w32ios_desktop_mode( void )
{
    static int value = -1;
    return env_switch( &value, "PG_DESKTOP", FALSE );
}

static BOOL game_windows( void )        /* E4 in game mode */
{
    static int value = -1;
    return env_switch( &value, "PG_GAME_WINDOWS", TRUE );
}

static BOOL nav_keys_e0( void )         /* E1 */
{
    static int value = -1;
    return env_switch( &value, "PG_NAV_KEYS_E0", TRUE );
}

static BOOL game_input_wake( void )     /* E5 */
{
    static int value = -1;
    return env_switch( &value, "PG_GAME_INPUT_WAKE", TRUE );
}

static BOOL foreground_fix_enabled( void )   /* E6 */
{
    static int value = -1;
    return env_switch( &value, "PG_FOREGROUND_FIX", TRUE );
}

static BOOL windows_enabled( void )
{
    return w32ios_desktop_mode() || game_windows();
}

static BOOL direct_cursor( void )
{
    return !w32ios_desktop_mode() && pgios_direct_cursor_wanted && pgios_direct_cursor_wanted();
}

/* ---------------------------------------------------------------- per-thread state */

/* keyed by TEB: a pthread can outlive a Windows thread and later carry another one */
struct thread_state
{
    TEB  *teb;
    BOOL  slices;           /* E5: this thread's message waits slice */
    DWORD last_mask_time;   /* E2: its last set_queue_mask request of a pure poll */
    HWND  pending_hwnd;     /* E4: the window of the last pWindowPosChanging */
    RECT  pending_visible;  /*     its visible rectangle (desktop pixels) */
    UINT  pending_swp;
};
static __thread struct thread_state thread_state;

static struct thread_state *get_thread_state( void )
{
    TEB *teb = NtCurrentTeb();

    if (thread_state.teb != teb)
    {
        memset( &thread_state, 0, sizeof(thread_state) );
        thread_state.teb = teb;
    }
    return &thread_state;
}

/* ---------------------------------------------------------------- E1: input in */

static LONG mouse_events, key_events, input_failures;

/* E3: the server's cursor position, after it moved and clipped the cursor */
static void report_cursor_pos( void )
{
    struct object_lock lock = OBJECT_LOCK_INIT;
    const desktop_shm_t *desktop_shm;
    NTSTATUS status;
    int x = 0, y = 0;

    if (!direct_cursor() || !pgios_direct_cursor_pos) return;
    while ((status = get_shared_desktop( &lock, &desktop_shm )) == STATUS_PENDING)
    {
        x = desktop_shm->cursor.x;
        y = desktop_shm->cursor.y;
    }
    if (!status) pgios_direct_cursor_pos( x, y );
}

static void count_failure( const char *what, NTSTATUS status )
{
    LONG n = InterlockedIncrement( &input_failures );
    if (w32ios_log_event( n - 1 )) w32ios_log( "%s failed (%#x), %d failures", what, (unsigned int)status, (int)n );
}

void pgios_drv_post_mouse( int x, int y, unsigned int flags, unsigned int mouse_data, HWND hwnd )
{
    INPUT input = { .type = INPUT_MOUSE };
    LONG n = InterlockedIncrement( &mouse_events );
    NTSTATUS status;

    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.mouseData = mouse_data;
    input.mi.dwFlags = flags;
    input.mi.time = NtGetTickCount();
    if (w32ios_log_event( n - 1 ))
        w32ios_log_verbose( "mouse %d: %d,%d flags %#x data %#x", (int)n, x, y, flags, mouse_data );

    /* no window: the server hit-tests */
    if ((status = send_hardware_message( 0, 0, &input, 0 ))) count_failure( "mouse input", status );
    if (flags & (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE)) report_cursor_pos();
}

void pgios_drv_post_key( unsigned short vk, unsigned int flags )
{
    INPUT input = { .type = INPUT_KEYBOARD };
    LONG n = InterlockedIncrement( &key_events );
    HKL layout = NtUserGetKeyboardLayout( 0 );
    UINT scan = NtUserMapVirtualKeyEx( vk, MAPVK_VK_TO_VSC_EX, layout );
    NTSTATUS status;

    input.ki.wVk = vk;
    input.ki.wScan = scan & 0xff;
    input.ki.dwFlags = flags;
    if (w32ios_key_extended( vk, scan, nav_keys_e0() )) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    input.ki.time = NtGetTickCount();
    if (w32ios_log_event( n - 1 ))
        w32ios_log_verbose( "key %d: vk %#x scan %#x flags %#x", (int)n, vk, scan, (unsigned int)input.ki.dwFlags );

    if ((status = send_hardware_message( 0, 0, &input, 0 ))) count_failure( "key input", status );
}

/* ---------------------------------------------------------------- E2: draining the app's queue */

BOOL w32ios_skip_queue_mask( void )
{
    struct thread_state *state = get_thread_state();
    DWORD now = NtGetTickCount();

    if (w32ios_poll_skip_request( 1, now, state->last_mask_time )) return TRUE;
    state->last_mask_time = now;   /* this one goes to the server */
    return FALSE;
}

static BOOL thread_slices( void )
{
    if (w32ios_desktop_mode()) return TRUE;
    return get_thread_state()->slices;
}

NTSTATUS w32ios_wait_objects( DWORD count, const HANDLE *handles, WAIT_TYPE type, BOOLEAN alertable,
                              LARGE_INTEGER *abs )
{
    LARGE_INTEGER now, end;
    NTSTATUS ret;
    int final;

    if (type != WaitAny || !count || !thread_slices())
        return NtWaitForMultipleObjects( count, handles, type, alertable, abs );

    for (;;)
    {
        NtQuerySystemTime( &now );
        end.QuadPart = w32ios_slice_deadline( now.QuadPart, abs != NULL, abs ? abs->QuadPart : 0,
                                              W32IOS_SLICE, &final );
        ret = NtWaitForMultipleObjects( count, handles, type, alertable, &end );
        if (ret != WAIT_TIMEOUT || final) return ret;
        /* the queue handle is the last one: a delivery counts as the queue being signalled */
        if (user_driver->pProcessEvents( QS_ALLINPUT )) return count - 1;
    }
}

/* ---------------------------------------------------------------- E3: cursor */

static HCURSOR last_sent;   /* the cursor whose image went out last */

static BOOL cursor_bits( HCURSOR cursor, ICONINFO *info, BITMAP *bm, BOOL color, void **bits, int *rows )
{
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[2] )];
    BITMAPINFO *bmi = (BITMAPINFO *)buffer;
    HBITMAP bitmap = color ? info->hbmColor : info->hbmMask;
    HDC hdc;
    int stride, ok;

    *rows = bm->bmHeight;
    stride = color ? bm->bmWidth * 4 : ((bm->bmWidth + 31) / 32) * 4;
    if (!(*bits = calloc( *rows, stride ))) return FALSE;

    memset( bmi, 0, sizeof(buffer) );
    bmi->bmiHeader.biSize = sizeof(bmi->bmiHeader);
    bmi->bmiHeader.biWidth = bm->bmWidth;
    bmi->bmiHeader.biHeight = -*rows;   /* top-down */
    bmi->bmiHeader.biPlanes = 1;
    bmi->bmiHeader.biBitCount = color ? 32 : 1;
    bmi->bmiHeader.biCompression = BI_RGB;
    if (!color)
    {
        bmi->bmiColors[1].rgbRed = bmi->bmiColors[1].rgbGreen = bmi->bmiColors[1].rgbBlue = 0xff;
        bmi->bmiHeader.biClrUsed = 2;
    }
    hdc = NtGdiCreateCompatibleDC( 0 );
    ok = NtGdiGetDIBitsInternal( hdc, bitmap, 0, *rows, *bits, bmi, DIB_RGB_COLORS, 0, 0 ) == *rows;
    NtGdiDeleteObjectApp( hdc );
    if (!ok)
    {
        free( *bits );
        *bits = NULL;
    }
    return ok;
}

/* straight-alpha BGRA of the cursor's first frame, at most 256 x 256 */
static BOOL convert_cursor( HCURSOR cursor, int *w, int *h, int *hot_x, int *hot_y, BYTE **bgra )
{
    ICONINFO info;
    BITMAP mask_bm, color_bm;
    void *mask = NULL, *color = NULL;
    int mask_rows, color_rows, full_w, full_h, x, y;
    BYTE *full = NULL;
    BOOL ret = FALSE;

    if (!NtUserGetIconInfo( cursor, &info, NULL, NULL, NULL, 0 )) return FALSE;
    if (!NtGdiExtGetObjectW( info.hbmMask, sizeof(mask_bm), &mask_bm )) goto done;
    full_w = mask_bm.bmWidth;
    if (info.hbmColor)
    {
        if (!NtGdiExtGetObjectW( info.hbmColor, sizeof(color_bm), &color_bm )) goto done;
        full_h = color_bm.bmHeight;
        if (!cursor_bits( cursor, &info, &color_bm, TRUE, &color, &color_rows )) goto done;
        if (!cursor_bits( cursor, &info, &mask_bm, FALSE, &mask, &mask_rows )) goto done;
        if (!(full = malloc( (size_t)full_w * full_h * 4 ))) goto done;
        w32ios_cursor_color( color, mask_rows >= full_h ? mask : NULL, full_w, full_h, full );
    }
    else   /* monochrome: the AND mask above the XOR mask in one bitmap */
    {
        full_h = mask_bm.bmHeight / 2;
        if (!cursor_bits( cursor, &info, &mask_bm, FALSE, &mask, &mask_rows )) goto done;
        if (!(full = malloc( (size_t)full_w * full_h * 4 ))) goto done;
        w32ios_cursor_mono( mask, full_w, full_h, full );
    }

    *w = min( full_w, W32IOS_CURSOR_MAX );
    *h = min( full_h, W32IOS_CURSOR_MAX );
    *hot_x = min( (int)info.xHotspot, *w - 1 );
    *hot_y = min( (int)info.yHotspot, *h - 1 );
    if (*w == full_w) *bgra = full;
    else if ((*bgra = malloc( (size_t)*w * *h * 4 )))
    {
        for (y = 0; y < *h; y++)
            for (x = 0; x < *w * 4; x++) (*bgra)[y * *w * 4 + x] = full[y * full_w * 4 + x];
        free( full );
    }
    full = NULL;
    ret = *bgra != NULL;

done:
    free( full );
    free( mask );
    free( color );
    if (info.hbmColor) NtGdiDeleteObjectApp( info.hbmColor );
    if (info.hbmMask) NtGdiDeleteObjectApp( info.hbmMask );
    return ret;
}

static void ios_SetCursor( HWND hwnd, HCURSOR cursor )
{
    BOOL desktop = w32ios_desktop_mode(), direct = !desktop && direct_cursor();
    void (*set)( unsigned int, int, int, int, int, const void * );
    void (*show)( int );
    int w, h, hot_x, hot_y;
    BYTE *bgra;

    if (desktop) { set = pgios_cursor_set; show = pgios_cursor_show; }
    else if (direct) { set = pgios_direct_cursor_set; show = pgios_direct_cursor_show; }
    else
    {
        if (pgios_pSetCursor) pgios_pSetCursor( hwnd, cursor );
        return;
    }

    if (!cursor)
    {
        if (show) show( 0 );
        return;
    }
    if (InterlockedCompareExchangePointer( (void **)&last_sent, cursor, cursor ) == cursor)
    {
        if (show) show( 1 );
        return;
    }
    if (!set || !convert_cursor( cursor, &w, &h, &hot_x, &hot_y, &bgra ))
    {
        if (show) show( 1 );
        return;
    }
    set( HandleToULong( cursor ), w, h, hot_x, hot_y, bgra );
    free( bgra );
    InterlockedExchangePointer( (void **)&last_sent, cursor );
    if (show) show( 1 );
}

static BOOL ios_SetCursorPos( INT x, INT y )
{
    report_cursor_pos();
    return TRUE;
}

static void ios_DestroyCursorIcon( HCURSOR cursor )
{
    InterlockedCompareExchangePointer( (void **)&last_sent, NULL, cursor );   /* the handle may come back */
    if (pgios_pDestroyCursorIcon) pgios_pDestroyCursorIcon( cursor );
}

/* ---------------------------------------------------------------- E4/E5: window images */

struct ios_surface
{
    struct window_surface header;
};

static void ios_surface_set_clip( struct window_surface *surface, const RECT *rects, UINT count )
{
}

static BOOL ios_surface_flush( struct window_surface *surface, const RECT *rect, const RECT *dirty,
                               const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                               const BITMAPINFO *shape_info, const void *shape_bits )
{
    int w = color_info->bmiHeader.biWidth, h = abs( color_info->bmiHeader.biHeight );

    if (!pgios_surface_present) return TRUE;
    /* 0: the bits could not be copied, win32u keeps the damage and flushes again */
    return pgios_surface_present( surface->hwnd, dirty->left, dirty->top, dirty->right - dirty->left,
                                   dirty->bottom - dirty->top, w, h, w * 4, color_bits ) != 0;
}

static void ios_surface_destroy( struct window_surface *surface )
{
}

static const struct window_surface_funcs ios_surface_funcs =
{
    ios_surface_set_clip,
    ios_surface_flush,
    ios_surface_destroy
};

static struct window_surface *ios_surface_create( HWND hwnd, const RECT *rect )
{
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)buffer;

    memset( info, 0, sizeof(*info) );
    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth = rect->right;
    info->bmiHeader.biHeight = -rect->bottom;   /* top-down, as large as right and bottom */
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biSizeImage = rect->right * rect->bottom * 4;
    info->bmiHeader.biCompression = BI_RGB;
    return window_surface_create( sizeof(struct ios_surface), &ios_surface_funcs, hwnd, rect, info, 0 );
}

static BOOL is_toplevel( HWND hwnd )
{
    HWND parent = NtUserGetAncestor( hwnd, GA_PARENT );

    if (get_window_long( hwnd, GWL_STYLE ) & WS_CHILD) return FALSE;
    return !parent || parent == NtUserGetDesktopWindow();
}

static BOOL ios_WindowPosChanging( HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects )
{
    struct thread_state *state = get_thread_state();

    state->pending_hwnd = hwnd;
    state->pending_visible = rects->visible;
    state->pending_swp = swp_flags;
    return TRUE;
}

/* E5: the thread's own visible top-level dialog-sized window got a surface */
static void mark_dialog_thread( HWND hwnd, const RECT *visible )
{
    static LONG marked;
    struct thread_state *state = get_thread_state();
    struct w32ios_size screen = w32ios_display_current();
    LONG n;

    if (state->slices || !game_input_wake()) return;
    if (NtUserGetWindowThread( hwnd, NULL ) != GetCurrentThreadId()) return;
    if (!is_toplevel( hwnd )) return;
    if (!(get_window_long( hwnd, GWL_STYLE ) & WS_VISIBLE) && !(state->pending_swp & SWP_SHOWWINDOW)) return;
    if (!w32ios_dialog_candidate( visible->left, visible->top, visible->right, visible->bottom, screen.w, screen.h ))
        return;
    state->slices = TRUE;
    n = InterlockedIncrement( &marked );
    if (n <= 8)
        w32ios_log( "thread %04x: window %p (%dx%d) waits in slices", (unsigned int)GetCurrentThreadId(), hwnd,
                    (int)(visible->right - visible->left), (int)(visible->bottom - visible->top) );
}

static BOOL ios_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface )
{
    struct thread_state *state = get_thread_state();
    struct w32ios_size screen = w32ios_display_current();
    struct window_surface *previous = *surface;
    BOOL known = state->pending_hwnd == hwnd;
    const RECT *visible = &state->pending_visible;

    if (!windows_enabled()) return FALSE;   /* upstream's null driver */

    /* the game's own window is drawn by Metal */
    if (!w32ios_desktop_mode() && known &&
        w32ios_covers_screen( visible->left, visible->top, visible->right, visible->bottom, screen.w, screen.h ))
    {
        if (previous) window_surface_release( previous );
        *surface = NULL;
        return TRUE;
    }

    /* keep a surface whose rectangle didn't change */
    if (previous && previous->funcs == &ios_surface_funcs && EqualRect( &previous->rect, surface_rect ))
        return TRUE;

    if (previous) window_surface_release( previous );
    *surface = ios_surface_create( hwnd, surface_rect );
    if (*surface && !w32ios_desktop_mode() && known) mark_dialog_thread( hwnd, visible );
    return TRUE;
}

static void ios_WindowPosChanged( HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                                  const struct window_rects *new_rects, struct window_surface *surface )
{
    const RECT *vis = &new_rects->visible, *client = &new_rects->client;
    BOOL visible = !(swp_flags & SWP_HIDEWINDOW) && !IsRectEmpty( vis );
    BOOL toplevel = is_toplevel( hwnd );

    if (pgios_window_frame && (w32ios_desktop_mode() || toplevel))
        pgios_window_frame( hwnd, vis->left, vis->top, vis->right - vis->left, vis->bottom - vis->top, visible,
                             client->left, client->top, client->right - client->left, client->bottom - client->top );

    /* game mode: a window that was just shown or reframed paints its first image */
    if (!w32ios_desktop_mode() && windows_enabled() && surface && surface != &dummy_surface && visible &&
        (swp_flags & (SWP_SHOWWINDOW | SWP_FRAMECHANGED)))
        NtUserRedrawWindow( hwnd, NULL, 0, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN );

    if (pgios_pWindowPosChanged)
        pgios_pWindowPosChanged( hwnd, insert_after, owner_hint, swp_flags, new_rects, surface );
}

/* ---------------------------------------------------------------- E6: foreground */

void w32ios_foreground_fix( void )
{
    struct w32ios_proc *proc;
    HWND foreground, best = 0, *list;
    DWORD pid, now = NtGetTickCount();
    LONGLONG best_area = 0;
    RECT rect;
    UINT i;

    if (!foreground_fix_enabled()) return;
    proc = w32ios_proc_current();
    if (proc->foreground_fix_done && now - proc->foreground_fix_time < 1000) return;   /* at most once a second */
    proc->foreground_fix_done = TRUE;
    proc->foreground_fix_time = now;

    if ((foreground = NtUserGetForegroundWindow()) && NtUserGetWindowThread( foreground, &pid ) &&
        pid == GetCurrentProcessId())
        return;

    if (!(list = list_window_children( get_desktop_window() ))) return;
    for (i = 0; list[i]; i++)
    {
        LONGLONG area;

        if (!NtUserGetWindowThread( list[i], &pid ) || pid != GetCurrentProcessId()) continue;
        if (!(get_window_long( list[i], GWL_STYLE ) & WS_VISIBLE)) continue;
        if (!get_window_rect( list[i], &rect, get_thread_dpi() )) continue;
        if (rect.right - rect.left < 320 || rect.bottom - rect.top < 200) continue;
        area = (LONGLONG)(rect.right - rect.left) * (rect.bottom - rect.top);
        if (area > best_area)
        {
            best_area = area;
            best = list[i];
        }
    }
    free( list );

    if (best)
    {
        w32ios_log_verbose( "process %04x reads a controller: window %p made foreground (was %p)",
                            (unsigned int)GetCurrentProcessId(), best, foreground );
        NtUserSetForegroundWindow( best );
    }
}

/* ---------------------------------------------------------------- E7 and the rest */

static BOOL ios_CreateWindow( HWND hwnd )
{
    if (pgios_pCreateWindow) return pgios_pCreateWindow( hwnd );
    return TRUE;
}

static void ios_DestroyWindow( HWND hwnd )
{
    if (pgios_pDestroyWindow) pgios_pDestroyWindow( hwnd );
}

static UINT ios_ShowWindow( HWND hwnd, INT cmd, RECT *rect, UINT swp )
{
    if (pgios_pShowWindow) return pgios_pShowWindow( hwnd, cmd, rect, swp );
    return ~0u;   /* upstream's default */
}

static BOOL ios_ProcessEvents( DWORD mask )
{
    if (pgios_pProcessEvents) return pgios_pProcessEvents( mask );
    return FALSE;
}

/* the virtual monitor (Part C) for upstream's device manager */
static UINT ios_UpdateDisplayDevices( const struct gdi_device_manager *manager, void *param )
{
    struct w32ios_size modes[W32IOS_STD_MODE_COUNT + 1], cur = w32ios_display_current();
    DEVMODEW devmodes[W32IOS_STD_MODE_COUNT + 1], current = { .dmSize = sizeof(DEVMODEW) };
    struct gdi_monitor monitor = {0};
    struct pci_id pci = {0};
    const char *name;
    int i, count;

    w32ios_gpu_identity( &pci.vendor, &pci.device, &name );
    manager->add_gpu( name, &pci, NULL, param );
    manager->add_source( "Default", DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE |
                         DISPLAY_DEVICE_VGA_COMPATIBLE, 96, param );
    SetRect( &monitor.rc_monitor, 0, 0, cur.w, cur.h );
    monitor.rc_work = monitor.rc_monitor;
    manager->add_monitor( &monitor, param );

    count = w32ios_build_modes( cur, w32ios_display_default(), w32ios_display_extended_modes(), modes, ARRAY_SIZE(modes) );
    for (i = 0; i < count; i++)
    {
        memset( &devmodes[i], 0, sizeof(devmodes[i]) );
        devmodes[i].dmSize = sizeof(DEVMODEW);
        w32ios_display_fill_devmode( &devmodes[i], modes[i] );
    }
    w32ios_display_fill_devmode( &current, cur );
    manager->add_modes( &current, count, devmodes, param );
    return STATUS_SUCCESS;
}

static const struct user_driver_funcs ios_driver_funcs =
{
    .pSetCursor = ios_SetCursor,
    .pSetCursorPos = ios_SetCursorPos,
    .pDestroyCursorIcon = ios_DestroyCursorIcon,
    .pUpdateDisplayDevices = ios_UpdateDisplayDevices,
    .pCreateWindow = ios_CreateWindow,
    .pDestroyWindow = ios_DestroyWindow,
    .pProcessEvents = ios_ProcessEvents,
    .pShowWindow = ios_ShowWindow,
    .pWindowPosChanging = ios_WindowPosChanging,
    .pCreateWindowSurface = ios_CreateWindowSurface,
    .pWindowPosChanged = ios_WindowPosChanged,
};

void w32ios_driver_install( void )
{
    static LONG installed;

    if (InterlockedExchange( &installed, 1 )) return;
    __wine_set_user_driver( &ios_driver_funcs, WINE_GDI_DRIVER_VERSION );
    w32ios_log( "iOS user driver installed (%s mode)", w32ios_desktop_mode() ? "desktop" : "game" );
}

void w32ios_driver_session_start( void )
{
    InterlockedExchangePointer( (void **)&last_sent, NULL );
}
