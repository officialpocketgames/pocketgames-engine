/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 E1, E3, H, clarifications 7 and 10: the native side of the DLL's exports and of the
 * thunk's own gate calls (kind 15 in gen/d3d9w_ids.h). Each reads its arguments from esp + 4
 * like a generated method; there is no This. */

#include "d3d9w_dispatch.h"

_Static_assert(D3D9W_GUEST_FOCUS_MINIMIZE == D3D9W_FOCUS_MINIMIZE &&
               D3D9W_GUEST_FOCUS_MOVE == D3D9W_FOCUS_MOVE, "the thunk's focus bits are the seam's");

#define ARGS(n)                                       \
    uint32_t a[n];                                      \
    if (!d3d9w_read_args(a, esp, sizeof(a)))            \
    {                                                   \
        ret->eax = 0;                                   \
        return;                                         \
    }

static void put_status(uint32_t g, uint32_t status)
{
    if (g)
        d3d9w_write(g, &status, sizeof(status));
}

/* The creates hold d3d9w_create_lock (clarification 13): no process's end falls in the middle. */
static void create9(uint32_t esp, struct ios_wow_native_ret *ret);
static void create9ex(uint32_t esp, struct ios_wow_native_ret *ret);

D3D9W_EXTRA(Direct3DCreate9)
{
    d3d9w_create_lock();
    create9(esp, ret);
    d3d9w_create_unlock();
}

D3D9W_EXTRA(Direct3DCreate9Ex)
{
    d3d9w_create_lock();
    create9ex(esp, ret);
    d3d9w_create_unlock();
}

/* Direct3DCreate9(sdk, uint32_t *status) -> IDirect3D9 proxy. */
static void create9(uint32_t esp, struct ios_wow_native_ret *ret)
{
    uint32_t status;
    IDirect3D9 *d3d;
    ARGS(2)

    if ((status = d3d9w_owner_status()) != D3D9W_CREATE_OK)
    {
        put_status(a[1], status);
        return;
    }
    if (!(d3d = d3d9w_native_create9(a[0])))
    {
        put_status(a[1], D3D9W_CREATE_FAILED);
        return;
    }
    if (!(ret->eax = d3d9w_proxy_for(d3d, D3D9W_KB(D3D9W_KIND_D3D9))))
    {
        IDirect3D9_Release(d3d);
        status = D3D9W_CREATE_FAILED;
    }
    put_status(a[1], status);
}

/* Direct3DCreate9Ex(sdk, IDirect3D9Ex **out, uint32_t *status) -> HRESULT. The out pointer is
 * written only on success, as the frontend does. */
static void create9ex(uint32_t esp, struct ios_wow_native_ret *ret)
{
    IDirect3D9Ex *d3d = NULL;
    uint32_t status, g;
    HRESULT hr;
    ARGS(3)

    if ((status = d3d9w_owner_status()) != D3D9W_CREATE_OK)
    {
        put_status(a[2], status);
        ret->eax = (uint32_t)D3DERR_NOTAVAILABLE;
        return;
    }
    if (a[1] && !d3d9w_guest_ok(a[1], 4))
    {
        put_status(a[2], D3D9W_CREATE_FAILED);
        ret->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    hr = d3d9w_native_create9ex(a[0], a[1] ? &d3d : NULL);
    if (SUCCEEDED(hr) && d3d)
    {
        if ((g = d3d9w_proxy_for(d3d, D3D9W_KB(D3D9W_KIND_D3D9))))
            d3d9w_write(a[1], &g, sizeof(g));
        else
        {
            IDirect3D9Ex_Release(d3d);
            hr = E_OUTOFMEMORY;
        }
    }
    put_status(a[2], SUCCEEDED(hr) ? D3D9W_CREATE_OK : D3D9W_CREATE_FAILED);
    ret->eax = (uint32_t)hr;
}

/* The D3DPERF and Debug exports do what the frontend's own exports do (out/05-dxmt-ios/dxmt
 * src/d3d9/d3d9.cpp): a PIX event nesting level, and constant answers. The name strings are
 * never read. */
static int perf_level;

D3D9W_EXTRA(D3DPERF_BeginEvent)
{
    (void)esp;
    ret->eax = (uint32_t)__atomic_fetch_add(&perf_level, 1, __ATOMIC_RELAXED);
}

D3D9W_EXTRA(D3DPERF_EndEvent)
{
    (void)esp;
    ret->eax = (uint32_t)(__atomic_fetch_sub(&perf_level, 1, __ATOMIC_RELAXED) - 1);
}

D3D9W_EXTRA(D3DPERF_GetStatus)
{
    (void)esp;
    ret->eax = 0;
}

D3D9W_EXTRA(D3DPERF_QueryRepeatFrame)
{
    (void)esp;
    ret->eax = FALSE;
}

D3D9W_EXTRA(D3DPERF_SetMarker)
{
    (void)esp;
    (void)ret;
}

D3D9W_EXTRA(D3DPERF_SetOptions)
{
    (void)esp;
    (void)ret;
}

D3D9W_EXTRA(D3DPERF_SetRegion)
{
    (void)esp;
    (void)ret;
}

D3D9W_EXTRA(DebugSetLevel)
{
    (void)esp;
    (void)ret;
}

D3D9W_EXTRA(DebugSetMute)
{
    (void)esp;
    ret->eax = TRUE;
}

/* Init(version, const uint32_t *vtables, count, uint32_t *shared) -> D3D9W_INIT_*. */
D3D9W_EXTRA(Init)
{
    uint32_t vt[D3D9W_NUM_KINDS], shared_guest = 0;
    ARGS(4)

    if (a[2] != D3D9W_NUM_KINDS || !d3d9w_read(vt, a[1], sizeof(vt)))
    {
        ret->eax = D3D9W_INIT_VERSION;
        return;
    }
    ret->eax = d3d9w_init(a[0], vt, a[2], &shared_guest);
    if (ret->eax == D3D9W_INIT_OK)
        d3d9w_write(a[3], &shared_guest, sizeof(shared_guest));
}

/* WindowState(IDirect3DDevice9 *, struct d3d9w_guest_window_state *) -> HRESULT
 * (interfaces/d3d9w_native.h, d3d9w_native_window_state). */
D3D9W_EXTRA(WindowState)
{
    struct d3d9w_window_state ws;
    struct d3d9w_guest_window_state gs;
    IDirect3DDevice9 *device;
    HRESULT hr;
    ARGS(2)

    if (!(device = d3d9w_this(a[0], D3D9W_KIND_Device)) || !d3d9w_guest_ok(a[1], sizeof(gs)))
    {
        ret->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    memset(&ws, 0, sizeof(ws));
    hr = d3d9w_native_window_state(device, &ws);
    if (SUCCEEDED(hr))
    {
        gs.focus_window = (uint32_t)(uintptr_t)ws.focus_window;
        gs.device_window = (uint32_t)(uintptr_t)ws.device_window;
        gs.windowed = (uint32_t)ws.windowed;
        gs.ex = (uint32_t)ws.ex;
        gs.backbuffer_width = ws.backbuffer_width;
        gs.backbuffer_height = ws.backbuffer_height;
        gs.behavior_flags = ws.behavior_flags;
        gs.virtual_mode = (uint32_t)ws.virtual_mode;
        gs.mode_width = ws.mode.Width;
        gs.mode_height = ws.mode.Height;
        gs.mode_refresh_rate = ws.mode.RefreshRate;
        gs.mode_format = (uint32_t)ws.mode.Format;
        d3d9w_write(a[1], &gs, sizeof(gs));
    }
    ret->eax = (uint32_t)hr;
}

/* FocusActivation(IDirect3DDevice9 *, BOOL active, RECT *pos) -> D3D9W_FOCUS_* bits
 * (d3d9w_native_focus_activation). */
D3D9W_EXTRA(FocusActivation)
{
    IDirect3DDevice9 *device;
    RECT pos;
    ARGS(3)

    if (!(device = d3d9w_this(a[0], D3D9W_KIND_Device)) || !d3d9w_read(&pos, a[2], sizeof(pos)))
    {
        ret->eax = 0;
        return;
    }
    ret->eax = d3d9w_native_focus_activation(device, (BOOL)a[1], &pos);
    d3d9w_write(a[2], &pos, sizeof(pos));
}

/* Unpin(proxy) -> HRESULT. */
D3D9W_EXTRA(Unpin)
{
    ARGS(1)

    ret->eax = (uint32_t)d3d9w_unpin(a[0]);
}

/* Pin(proxy) -> HRESULT: the thunk keeps guest-side state on a live proxy (E3). Done here, under
 * the table lock, rather than by a guest store, so it never races d3d9w_object_gone. */
D3D9W_EXTRA(Pin)
{
    ARGS(1)

    ret->eax = (uint32_t)d3d9w_pin(a[0]);
}
