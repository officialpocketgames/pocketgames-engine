/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 E3 and clarification 7: what the PE build's device does with windows and the cursor,
 * done here in guest code because it sends messages and calls into the program (native code must
 * not). The behaviour (calls, order, return values) follows the PE build of the frontend,
 * out/05-dxmt-ios/dxmt/src/d3d9/d3d9_device.cpp: its constructor, Reset, destructor,
 * enterFullscreenWindow / leaveFullscreenWindow, the focus-window hook and procedure, and
 * SetCursorProperties / SetCursorPosition / ShowCursor. The device's own state (lost, not
 * reset, occluded) stays native: d3d9w_native_focus_activation. The window facts come from
 * d3d9w_native_window_state (the WindowState call).
 *
 * The window changes run after the native call returns, where the PE build makes them inside
 * it: after the device is constructed, after Reset rebuilt the swap chain (nothing after that
 * point in Reset can fail or touches windows), and after the final Release. */

#include "d3d9_thunk.h"
#include "d3d9_thunk_pure.h"

/* Window properties on a hooked focus window: its original procedure and the device record. */
static const WCHAR prop_proc[] = L"D3D9WFocusProc";
static const WCHAR prop_dev[] = L"D3D9WFocusDevice";

/* The guest-side state of one device: the PE device's window and cursor members. */
struct dev_rec
{
    struct dev_rec *next;
    IDirect3DDevice9 *device;   /* the proxy */
    LONG refs;                  /* the device's, plus one per focus message being handled */
    CRITICAL_SECTION cs;        /* cursor state */
    BOOL ex;
    DWORD behavior;
    HWND focus_param;           /* the focus window CreateDevice was given */
    /* fullscreen window */
    HWND fullscreen_window;
    LONG saved_style, saved_exstyle;
    RECT saved_rect;
    HMONITOR fullscreen_monitor;
    DEVMODEW saved_mode;
    BOOL saved_mode_valid;
    /* focus window */
    HWND focus_window;
    BOOL focus_hooked;
    BOOL focus_unicode;
    volatile BOOL filtered;     /* the window traffic is the device's own: keep it from the program */
    /* cursor */
    HCURSOR hw_cursor;
    BOOL cursor_visible;
    BOOL cursor_image_set;
};

static CRITICAL_SECTION recs_cs;
static struct dev_rec *recs;

void d3d9t_device_init(void)
{
    InitializeCriticalSection(&recs_cs);
}

/* A counted reference to a live record, by device or by record pointer (focus procedure). */
static struct dev_rec *rec_get(const void *device, const struct dev_rec *want)
{
    struct dev_rec *r;

    EnterCriticalSection(&recs_cs);
    for (r = recs; r; r = r->next)
        if (want ? r == want : r->device == device)
            break;
    if (r)
        r->refs++;
    LeaveCriticalSection(&recs_cs);
    return r;
}

static void rec_put(struct dev_rec *r)
{
    LONG left;

    EnterCriticalSection(&recs_cs);
    left = --r->refs;
    LeaveCriticalSection(&recs_cs);
    if (left)
        return;
    DeleteCriticalSection(&r->cs);
    HeapFree(GetProcessHeap(), 0, r);
}

static BOOL window_state(IDirect3DDevice9 *device, struct d3d9w_guest_window_state *st)
{
    memset(st, 0, sizeof(*st));
    return SUCCEEDED(d3d9w_s_Exports_WindowState(device, st));
}

/* The window the implicit swap chain presents to (hDeviceWindow, or the focus window). */
static HWND state_window(const struct d3d9w_guest_window_state *st)
{
    return (HWND)(ULONG_PTR)(st->device_window ? st->device_window : st->focus_window);
}

/* ------------------------------------------------------------------ fullscreen window */

/* Resize and restyle the device window to a borderless fullscreen rectangle; on the app's
 * virtual monitor first switch the display mode to the back buffer size (spec 07 C8). The
 * first entry saves the windowed style and rectangle; a later one only moves the window. */
static void enter_fullscreen(struct dev_rec *r, const struct d3d9w_guest_window_state *st, HWND window)
{
    UINT width = st->backbuffer_width, height = st->backbuffer_height;
    MONITORINFO mi;
    HMONITOR monitor;
    BOOL saved;
    LONG x = 0, y = 0;

    if (!window)
        return;
    if (st->virtual_mode && width && height)
    {
        DEVMODEW current;

        memset(&current, 0, sizeof(current));
        current.dmSize = sizeof(current);
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &current))
        {
            if (!r->saved_mode_valid)
            {
                r->saved_mode = current;
                r->saved_mode_valid = TRUE;
            }
            if (current.dmPelsWidth != width || current.dmPelsHeight != height)
            {
                DEVMODEW mode = current;

                mode.dmPelsWidth = width;
                mode.dmPelsHeight = height;
                mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
                if (ChangeDisplaySettingsW(&mode, CDS_FULLSCREEN) != DISP_CHANGE_SUCCESSFUL)
                    d3d9t_log("d3d9: virtual display mode %ux%u refused", width, height);
            }
        }
    }
    if (r->behavior & D3DCREATE_NOWINDOWCHANGES)
        return;
    saved = r->filtered;
    r->filtered = TRUE;
    monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoW(monitor, &mi))
    {
        x = mi.rcMonitor.left;
        y = mi.rcMonitor.top;
    }
    r->fullscreen_monitor = monitor;
    if (r->fullscreen_window != window)
    {
        r->fullscreen_window = window;
        r->saved_style = GetWindowLongW(window, GWL_STYLE);
        r->saved_exstyle = GetWindowLongW(window, GWL_EXSTYLE);
        GetWindowRect(window, &r->saved_rect);
        SetWindowLongW(window, GWL_STYLE, (r->saved_style | WS_POPUP | WS_SYSMENU) & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowLongW(window, GWL_EXSTYLE, r->saved_exstyle & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE));
    }
    SetWindowPos(window, HWND_TOPMOST, x, y, (int)width, (int)height,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    r->filtered = saved;
}

/* Back to the remembered display mode; the saved style (keeping the window's current
 * visibility and topmost bits); a 9Ex device also gets its saved rectangle back. */
static void leave_fullscreen(struct dev_rec *r)
{
    HWND window = r->fullscreen_window;
    LONG style, exstyle;
    BOOL saved;

    if (r->saved_mode_valid)
    {
        DEVMODEW mode = r->saved_mode;

        mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        if (ChangeDisplaySettingsW(&mode, CDS_FULLSCREEN) != DISP_CHANGE_SUCCESSFUL)
            d3d9t_log("d3d9: restoring the virtual display mode failed");
        r->saved_mode_valid = FALSE;
    }
    if (!window)
        return;
    r->fullscreen_window = NULL;
    saved = r->filtered;
    r->filtered = TRUE;
    style = (r->saved_style & ~WS_VISIBLE) | (GetWindowLongW(window, GWL_STYLE) & WS_VISIBLE);
    exstyle = (r->saved_exstyle & ~WS_EX_TOPMOST) | (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOPMOST);
    SetWindowLongW(window, GWL_STYLE, style);
    SetWindowLongW(window, GWL_EXSTYLE, exstyle);
    if (r->ex)
        SetWindowPos(window, HWND_NOTOPMOST, r->saved_rect.left, r->saved_rect.top,
                     r->saved_rect.right - r->saved_rect.left, r->saved_rect.bottom - r->saved_rect.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    else
        SetWindowPos(window, NULL, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    r->saved_style = r->saved_exstyle = 0;
    memset(&r->saved_rect, 0, sizeof(r->saved_rect));
    r->fullscreen_monitor = NULL;
    r->filtered = saved;
}

/* ------------------------------------------------------------------ focus window */

/* WM_ACTIVATEAPP of a fullscreen device: native code changes the device's state and says which
 * window actions to take (minimize on losing focus, move back on regaining it). */
static void focus_activation(struct dev_rec *r, BOOL active)
{
    struct d3d9w_guest_window_state st;
    BOOL saved = r->filtered;
    RECT pos;
    DWORD actions;

    r->filtered = TRUE;
    memset(&pos, 0, sizeof(pos));
    actions = d3d9w_s_Exports_FocusActivation(r->device, active, &pos);
    if (actions && window_state(r->device, &st) && !(st.behavior_flags & D3DCREATE_NOWINDOWCHANGES))
    {
        HWND window = (HWND)(ULONG_PTR)st.device_window;

        if ((actions & D3D9W_GUEST_FOCUS_MINIMIZE) && window && IsWindowVisible(window))
            ShowWindow(window, SW_MINIMIZE);
        if ((actions & D3D9W_GUEST_FOCUS_MOVE) && window)
            SetWindowPos(window, NULL, pos.left, pos.top, pos.right - pos.left, pos.bottom - pos.top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
    }
    r->filtered = saved;
}

static LRESULT call_orig(WNDPROC orig, BOOL unicode, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!orig)
        return unicode ? DefWindowProcW(hwnd, msg, wp, lp) : DefWindowProcA(hwnd, msg, wp, lp);
    return unicode ? CallWindowProcW(orig, hwnd, msg, wp, lp) : CallWindowProcA(orig, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK focus_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC orig = (WNDPROC)GetPropW(hwnd, prop_proc);
    struct dev_rec *r = (struct dev_rec *)GetPropW(hwnd, prop_dev);
    BOOL unicode = IsWindowUnicode(hwnd);
    LRESULT res;

    /* The window goes while still hooked: give it back its procedure first. */
    if (msg == WM_NCDESTROY && orig)
    {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)orig);
        RemovePropW(hwnd, prop_proc);
        RemovePropW(hwnd, prop_dev);
        return call_orig(orig, unicode, hwnd, msg, wp, lp);
    }
    if (r && (r = rec_get(NULL, r)))
    {
        /* The device's own window traffic never reaches the program (except a mode change). */
        if (r->filtered && msg != WM_DISPLAYCHANGE)
        {
            rec_put(r);
            return unicode ? DefWindowProcW(hwnd, msg, wp, lp) : DefWindowProcA(hwnd, msg, wp, lp);
        }
        if (msg == WM_ACTIVATEAPP)
            focus_activation(r, wp != FALSE);
        if (msg == WM_SYSCOMMAND && wp == SC_RESTORE)
        {
            if (unicode)
                DefWindowProcW(hwnd, msg, wp, lp);
            else
                DefWindowProcA(hwnd, msg, wp, lp);
        }
        rec_put(r);
    }
    res = call_orig(orig, unicode, hwnd, msg, wp, lp);
    return res;
}

/* Subclass the focus window (or the device window when there is none). Activates it first, as
 * the PE build does, so the process is the active application and later sees WM_ACTIVATEAPP. */
static void hook_focus(struct dev_rec *r, HWND fallback)
{
    HWND focus = r->focus_param ? r->focus_param : fallback;
    LONG_PTR orig;

    if (r->focus_hooked || !focus || !IsWindow(focus) || GetPropW(focus, prop_proc))
        return;
    SetWindowPos(focus, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    r->focus_unicode = IsWindowUnicode(focus);
    orig = r->focus_unicode ? SetWindowLongPtrW(focus, GWLP_WNDPROC, (LONG_PTR)focus_proc)
                            : SetWindowLongPtrA(focus, GWLP_WNDPROC, (LONG_PTR)focus_proc);
    SetPropW(focus, prop_proc, (HANDLE)orig);
    SetPropW(focus, prop_dev, (HANDLE)r);
    r->focus_window = focus;
    r->focus_hooked = TRUE;
}

static void unhook_focus(struct dev_rec *r)
{
    HWND focus = r->focus_window;
    WNDPROC orig, current;

    if (!r->focus_hooked)
        return;
    r->focus_hooked = FALSE;
    r->focus_window = NULL;
    if (!focus || !IsWindow(focus))
        return;
    RemovePropW(focus, prop_dev);
    if (!(orig = (WNDPROC)GetPropW(focus, prop_proc)))
        return;
    /* Only while ours is still the installed procedure: a program that subclassed on top of it
     * keeps its own (the property stays for the WM_NCDESTROY path). */
    current = (WNDPROC)(r->focus_unicode ? GetWindowLongPtrW(focus, GWLP_WNDPROC)
                                         : GetWindowLongPtrA(focus, GWLP_WNDPROC));
    if (current != focus_proc)
        return;
    if (r->focus_unicode)
        SetWindowLongPtrW(focus, GWLP_WNDPROC, (LONG_PTR)orig);
    else
        SetWindowLongPtrA(focus, GWLP_WNDPROC, (LONG_PTR)orig);
    RemovePropW(focus, prop_proc);
}

/* ------------------------------------------------------------------ device life */

void d3d9t_device_created(IDirect3DDevice9 *device)
{
    struct d3d9w_guest_window_state st;
    struct dev_rec *r;

    if (!window_state(device, &st))
    {
        d3d9t_log("d3d9: no window state for device %p", (void *)device);
        return;
    }
    if (!(r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r))))
        return;
    r->device = device;
    r->refs = 1;
    r->ex = st.ex != 0;
    r->behavior = st.behavior_flags;
    r->focus_param = (HWND)(ULONG_PTR)st.focus_window;
    InitializeCriticalSection(&r->cs);
    EnterCriticalSection(&recs_cs);
    r->next = recs;
    recs = r;
    LeaveCriticalSection(&recs_cs);

    /* A device created fullscreen takes its window fullscreen and hooks the focus window. */
    if (!st.windowed)
    {
        enter_fullscreen(r, &st, state_window(&st));
        hook_focus(r, state_window(&st));
    }
}

void d3d9t_device_reset(IDirect3DDevice9 *device)
{
    struct d3d9w_guest_window_state st;
    struct dev_rec *r;

    if (!(r = rec_get(device, NULL)))
        return;
    if (window_state(device, &st))
    {
        if (!st.windowed)
        {
            enter_fullscreen(r, &st, state_window(&st));
            hook_focus(r, state_window(&st));
        }
        else
        {
            leave_fullscreen(r);
            unhook_focus(r);
        }
    }
    /* A plain device's Reset keeps the hardware cursor; ShowCursor works again only behind it. */
    if (!r->ex)
    {
        EnterCriticalSection(&r->cs);
        r->cursor_image_set = r->hw_cursor != NULL;
        LeaveCriticalSection(&r->cs);
    }
    rec_put(r);
}

/* The device's final Release: what the PE destructor does with windows and the cursor. */
void d3d9t_device_released(IDirect3DDevice9 *device)
{
    struct dev_rec *r, **p;

    EnterCriticalSection(&recs_cs);
    for (p = &recs; (r = *p); p = &r->next)
        if (r->device == device)
        {
            *p = r->next;
            break;
        }
    LeaveCriticalSection(&recs_cs);
    if (!r)
        return;
    leave_fullscreen(r);
    unhook_focus(r);
    if (r->hw_cursor)
        DestroyCursor(r->hw_cursor);
    r->hw_cursor = NULL;
    rec_put(r);
}

/* ------------------------------------------------------------------ guest methods */

/* Clarification 12: the x87 control word of the thread that created the device, as the PE
 * build's device constructor sets it (first, before its window work). Native code runs on the
 * host's FPU, so this is the thunk's to do. */
static void setup_fpu(DWORD flags)
{
#if defined(__i386__)
    unsigned short cw;

    if (flags & D3DCREATE_FPU_PRESERVE)
        return;
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    cw = d3d9t_fpu_control(cw);
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
#else
    (void)flags;
#endif
}

HRESULT WINAPI d3d9t_D3D9_CreateDevice(IDirect3D9Ex *This, UINT adapter_idx, D3DDEVTYPE device_type,
                                       HWND focus_window, DWORD flags, D3DPRESENT_PARAMETERS *parameters,
                                       struct IDirect3DDevice9 **device)
{
    HRESULT hr = d3d9w_s_D3D9_CreateDevice(This, adapter_idx, device_type, focus_window, flags,
                                           parameters, device);

    if (SUCCEEDED(hr) && device && *device)
    {
        setup_fpu(flags);
        d3d9t_device_created(*device);
    }
    return hr;
}

HRESULT WINAPI d3d9t_D3D9_CreateDeviceEx(IDirect3D9Ex *This, UINT adapter_idx, D3DDEVTYPE device_type,
                                         HWND focus_window, DWORD flags, D3DPRESENT_PARAMETERS *parameters,
                                         D3DDISPLAYMODEEX *mode, struct IDirect3DDevice9Ex **device)
{
    HRESULT hr = d3d9w_s_D3D9_CreateDeviceEx(This, adapter_idx, device_type, focus_window, flags,
                                             parameters, mode, device);

    if (SUCCEEDED(hr) && device && *device)
    {
        setup_fpu(flags);
        d3d9t_device_created((IDirect3DDevice9 *)*device);
    }
    return hr;
}

HRESULT WINAPI d3d9t_Device_Reset(IDirect3DDevice9Ex *This, D3DPRESENT_PARAMETERS *parameters)
{
    HRESULT hr = d3d9w_s_Device_Reset(This, parameters);

    if (SUCCEEDED(hr))
        d3d9t_device_reset((IDirect3DDevice9 *)This);
    return hr;
}

HRESULT WINAPI d3d9t_Device_ResetEx(IDirect3DDevice9Ex *This, D3DPRESENT_PARAMETERS *parameters,
                                    D3DDISPLAYMODEEX *mode)
{
    HRESULT hr = d3d9w_s_Device_ResetEx(This, parameters, mode);

    if (SUCCEEDED(hr))
        d3d9t_device_reset((IDirect3DDevice9 *)This);
    return hr;
}

ULONG WINAPI d3d9t_Device_Release(IDirect3DDevice9Ex *This)
{
    ULONG refs = d3d9w_s_Device_Release(This);

    if (!refs)
        d3d9t_device_released((IDirect3DDevice9 *)This);
    d3d9t_sweep_if_needed();
    return refs;
}

/* The bitmap becomes a 32x32 hardware cursor (a larger one is cropped to its top-left 32x32), as
 * in the PE build; the bitmap is read through the surface's own LockRect. */
HRESULT WINAPI d3d9t_Device_SetCursorProperties(IDirect3DDevice9Ex *This, UINT hotspot_x, UINT hotspot_y,
                                                IDirect3DSurface9 *bitmap)
{
    D3DDEVICE_CREATION_PARAMETERS cp;
    D3DSURFACE_DESC desc;
    D3DDISPLAYMODE mode;
    D3DLOCKED_RECT locked;
    IDirect3D9 *d3d = NULL;
    uint32_t pixels[32 * 32];
    BYTE mask[32 * 32 / 8];
    ICONINFO info;
    HCURSOR cursor, old;
    struct dev_rec *r;
    UINT row, copy_w, copy_h;
    HRESULT hr;

    if (!bitmap)
        return D3DERR_INVALIDCALL;
    if (FAILED(hr = d3d9w_s_Surface_GetDesc(bitmap, &desc)))
        return hr;
    if (FAILED(hr = d3d9w_s_Device_GetCreationParameters(This, &cp)))
        return hr;
    if (FAILED(hr = d3d9w_s_Device_GetDirect3D(This, &d3d)))
        return hr;
    hr = d3d9w_s_D3D9_GetAdapterDisplayMode((IDirect3D9Ex *)d3d, cp.AdapterOrdinal, &mode);
    d3d9w_s_D3D9_Release((IDirect3D9Ex *)d3d);
    if (FAILED(hr))
        return hr;
    if (FAILED(hr = d3d9t_validate_cursor(desc.Format, desc.Width, desc.Height, hotspot_x, hotspot_y,
                                          mode.Width, mode.Height)))
        return hr;
    if (!(r = rec_get(This, NULL)))
        return D3DERR_INVALIDCALL;

    EnterCriticalSection(&r->cs);
    if (FAILED(d3d9w_s_Surface_LockRect(bitmap, &locked, NULL, D3DLOCK_READONLY)))
    {
        LeaveCriticalSection(&r->cs);
        rec_put(r);
        return D3DERR_INVALIDCALL;
    }
    memset(pixels, 0, sizeof(pixels));
    copy_w = (desc.Width < 32 ? desc.Width : 32) * 4;
    copy_h = desc.Height < 32 ? desc.Height : 32;
    for (row = 0; row < copy_h; row++)
        memcpy(&pixels[row * 32], (const BYTE *)locked.pBits + row * locked.Pitch, copy_w);
    d3d9w_s_Surface_UnlockRect(bitmap);
    /* An all-ones mask keeps a bitmap with no alpha fully transparent instead of falling back to
     * the monochrome mask. */
    memset(mask, 0xff, sizeof(mask));
    info.fIcon = FALSE;
    info.xHotspot = hotspot_x;
    info.yHotspot = hotspot_y;
    info.hbmMask = CreateBitmap(32, 32, 1, 1, mask);
    info.hbmColor = CreateBitmap(32, 32, 1, 32, pixels);
    cursor = CreateIconIndirect(&info);
    if (info.hbmMask)
        DeleteObject(info.hbmMask);
    if (info.hbmColor)
        DeleteObject(info.hbmColor);
    old = r->hw_cursor;
    r->hw_cursor = cursor;
    if (old)
        DestroyCursor(old);
    if (r->cursor_visible)
        SetCursor(r->hw_cursor);
    r->cursor_image_set = TRUE;
    LeaveCriticalSection(&r->cs);
    rec_put(r);
    return D3D_OK;
}

/* The OS pointer moves only behind a hardware cursor, and not when it is already there. */
void WINAPI d3d9t_Device_SetCursorPosition(IDirect3DDevice9Ex *This, int x, int y, DWORD flags)
{
    struct dev_rec *r;
    POINT pt;

    (void)flags;
    if (!(r = rec_get(This, NULL)))
        return;
    EnterCriticalSection(&r->cs);
    if (r->hw_cursor && !(GetCursorPos(&pt) && pt.x == x && pt.y == y))
        SetCursorPos(x, y);
    LeaveCriticalSection(&r->cs);
    rec_put(r);
}

/* Returns the previous visibility; it latches only once a cursor image was set. */
BOOL WINAPI d3d9t_Device_ShowCursor(IDirect3DDevice9Ex *This, BOOL show)
{
    struct dev_rec *r;
    BOOL prev;

    if (!(r = rec_get(This, NULL)))
        return FALSE;
    EnterCriticalSection(&r->cs);
    prev = r->cursor_visible;
    if (r->cursor_image_set)
        r->cursor_visible = show;
    if (r->hw_cursor)
        SetCursor(show ? r->hw_cursor : NULL);
    LeaveCriticalSection(&r->cs);
    rec_put(r);
    return prev;
}
