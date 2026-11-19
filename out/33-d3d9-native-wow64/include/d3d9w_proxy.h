/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33: what the i386 thunk (thunk/) and the native dispatch (native/) share besides the
 * generated ids: the proxy and arena layout in guest memory (E2, clarification 5), the
 * registration unix call's parameters (D2), and the arguments of the thunk's own gate calls.
 * Plain C with fixed-width fields only: the same bytes on i386 and on aarch64. */

#ifndef D3D9W_PROXY_H
#define D3D9W_PROXY_H

#include <stdint.h>

/* Bumped whenever anything in this file or gen/d3d9w_ids.h changes meaning. The thunk passes
 * it to the registration call and to Init; a different number is refused (a mismatched build). */
#define D3D9W_ABI_VERSION 1u

/* ---- Proxies (spec 33 E2, clarification 5) ----
 * The program's interface pointer for a native object is the guest address of its proxy, a
 * 16-byte slot of an arena the dispatch keeps in guest-window memory (Part C). Arena chunks are
 * 64 KB, 64 KB aligned in guest space, 4096 slots each. Slot 0 of the first chunk holds the
 * shared header below instead of a proxy. */
#define D3D9W_CHUNK_BYTES       0x10000u
#define D3D9W_SLOT_BYTES        16u
#define D3D9W_SLOTS_PER_CHUNK   (D3D9W_CHUNK_BYTES / D3D9W_SLOT_BYTES)
#define D3D9W_MAX_CHUNKS        256u          /* 1,048,576 proxies */

struct d3d9w_proxy
{
    uint32_t vtbl;      /* guest address of the thunk's vtable for this interface */
    uint32_t handle;    /* the native table's slot: index in bits 0-19, generation in 20-31 */
    uint32_t flags;     /* D3D9W_PROXY_* */
    uint32_t reserved;  /* 0 */
};

#define D3D9W_HANDLE_INDEX(h) ((h) & 0xfffffu)
#define D3D9W_HANDLE_GEN(h)   ((h) >> 20)

/* Set through the thunk's Pin call on a proxy it keeps guest-side state for: IUnknown private
 * data or an open GDI DC (spec 33 E3). While it is set, the dispatch does not reuse the slot when
 * the native object goes: it sets GONE, bumps the shared header's sweep counter, and keeps the
 * slot until the thunk has dropped its state and called Unpin. The thunk only reads proxies. */
#define D3D9W_PROXY_PINNED  0x1u
/* Set by the dispatch when the native object is gone (d3d9w_object_gone). */
#define D3D9W_PROXY_GONE    0x2u

struct d3d9w_shared
{
    uint32_t magic;     /* D3D9W_SHARED_MAGIC */
    uint32_t sweep;     /* bumped every time a pinned proxy becomes GONE */
    uint32_t live;      /* live proxies (informational) */
    uint32_t reserved;
};
#define D3D9W_SHARED_MAGIC 0x57394433u   /* "3D9W" */

/* ---- The registration unix call (spec 33 D2) ----
 * The thunk's own unix library, entry 0, WoW64 table only. All fields 32-bit, so the i386
 * struct is the struct the unix side reads (spec 06 C: Wine converts the outer pointer). */
struct d3d9w_register_params
{
    uint32_t version;   /* in: D3D9W_ABI_VERSION */
    uint32_t gate;      /* out: the gate's guest address, 0 when there is none */
    uint32_t reason;    /* out: D3D9W_NOGATE_* when gate is 0 */
    uint32_t reserved;
};
#define D3D9W_NOGATE_NONE         0u
#define D3D9W_NOGATE_NO_CPU_GATE  1u   /* the app's CPU has no Section A (an App Store build) */
#define D3D9W_NOGATE_REFUSED      2u   /* ios_wow_native_register returned 0 */
#define D3D9W_NOGATE_VERSION      3u   /* the thunk and the native library disagree */
#define D3D9W_NOGATE_NOT_WOW64    4u   /* called from a 64-bit program */

/* ---- The thunk's own gate calls (kind 15, gen/d3d9w_ids.h) ---- */

/* Init(version, const uint32_t *vtables, count, uint32_t *shared) -> D3D9W_INIT_* (EAX).
 * Hands the dispatch the guest addresses of the thunk's 15 vtables and makes this process the
 * native frontend's owner (spec 33 clarification 10). *shared receives the guest address of
 * struct d3d9w_shared. */
#define D3D9W_INIT_OK         0u
#define D3D9W_INIT_BUSY       1u   /* another process holds native objects */
#define D3D9W_INIT_VERSION    2u
#define D3D9W_INIT_NOMEM      3u

/* Direct3DCreate9(sdk, uint32_t *status) -> proxy (EAX) and
 * Direct3DCreate9Ex(sdk, IDirect3D9Ex **out, uint32_t *status) -> HRESULT (EAX):
 * status is D3D9W_CREATE_*. */
#define D3D9W_CREATE_OK       0u
#define D3D9W_CREATE_BUSY     1u   /* this process is no longer the owner: use the full frontend */
#define D3D9W_CREATE_FAILED   2u   /* the native frontend returned no object */

/* WindowState(IDirect3DDevice9 *device, struct d3d9w_guest_window_state *out) -> HRESULT:
 * interfaces/d3d9w_native.h's struct d3d9w_window_state in i386 layout. */
struct d3d9w_guest_window_state
{
    uint32_t focus_window, device_window;   /* HWND values */
    uint32_t windowed, ex;
    uint32_t backbuffer_width, backbuffer_height;
    uint32_t behavior_flags;
    uint32_t virtual_mode;
    uint32_t mode_width, mode_height, mode_refresh_rate, mode_format;   /* D3DDISPLAYMODE */
};

/* FocusActivation(IDirect3DDevice9 *device, BOOL active, RECT *pos) -> DWORD:
 * d3d9w_native_focus_activation's bits, the same values as interfaces/d3d9w_native.h's
 * D3D9W_FOCUS_MINIMIZE and D3D9W_FOCUS_MOVE (native/d3d9w_extras.c asserts it; the thunk does
 * not include the native seam header). *pos is a RECT: left, top, right, bottom. */
#define D3D9W_GUEST_FOCUS_MINIMIZE 0x1u
#define D3D9W_GUEST_FOCUS_MOVE     0x2u

/* Unpin(proxy) -> HRESULT: the thunk dropped its state for this proxy: clear PINNED, and give
 * the slot back when the native object is already gone. */

#endif /* D3D9W_PROXY_H */
