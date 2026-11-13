// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The seam between spec 33's native Direct3D 9 frontend (out/05-dxmt-ios/dxmt built with
// ios_wow_native, session N) and its dispatch (out/33-d3d9-native-wow64, session T): what the
// frontend provides the dispatch beyond Wine 11.4's d3d9.h interfaces, and the one function the
// dispatch provides the frontend. Spec 33 clarifications 5 and 7.

#ifndef MYIOSDECK_D3D9W_NATIVE_H
#define MYIOSDECK_D3D9W_NATIVE_H

#include <windows.h>
#include <d3d9.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Creation: the native halves of Direct3DCreate9 and Direct3DCreate9Ex. */
IDirect3D9 *d3d9w_native_create9( UINT sdk_version );
HRESULT d3d9w_native_create9ex( UINT sdk_version, IDirect3D9Ex **out );

/* Lifetime. The frontend calls this exactly once for every COM object it destroys, with the
 * interface address it handed out for that object, on whatever thread runs the destructor,
 * before the memory is freed. The dispatch defines it (weak on the frontend's side, so the
 * frontend's own tests link without a dispatch). */
void d3d9w_object_gone( void *iface ) __attribute__((weak));

/* The window state the thunk needs to do, in guest code, what the PE build's device does with
 * windows around CreateDevice, Reset, ResetEx and the device's final release. */
struct d3d9w_window_state
{
    HWND focus_window, device_window;   /* 32-bit handle values */
    BOOL windowed, ex;                  /* D3DPRESENT_PARAMETERS.Windowed; a 9Ex device */
    UINT backbuffer_width, backbuffer_height;
    DWORD behavior_flags;               /* the CreateDevice flags (D3DCREATE_NOWINDOWCHANGES) */
    BOOL virtual_mode;                  /* the frontend's iOS virtual display-mode switch */
    D3DDISPLAYMODE mode;                /* the mode a fullscreen device asks for */
};
HRESULT d3d9w_native_window_state( IDirect3DDevice9 *device, struct d3d9w_window_state *out );

/* The device side of the focus window's WM_ACTIVATEAPP: what the PE build's focus-activation
 * handler does to the device's state (lost, not reset, occluded, display mode). Returns the
 * window actions for the thunk to take. */
#define D3D9W_FOCUS_MINIMIZE  0x1u   /* ShowWindow(SW_MINIMIZE): the window is visible and window changes are allowed */
#define D3D9W_FOCUS_MOVE      0x2u   /* SetWindowPos(NOACTIVATE | NOZORDER) to *pos: a RECT, left and top the monitor
                                         origin, right = left + width, bottom = top + height (clarification 12) */
DWORD d3d9w_native_focus_activation( IDirect3DDevice9 *device, BOOL active, RECT *pos );

/* The calling process is ending (clarification 13; the dispatch calls it for the gate's reserved
 * method 0xffffff): every object still alive becomes an orphan that no longer counts for
 * clarification 10's one-process rule. Nothing is released or destroyed. */
void d3d9w_native_process_gone( void );

#ifdef __cplusplus
}
#endif

#endif /* MYIOSDECK_D3D9W_NATIVE_H */
