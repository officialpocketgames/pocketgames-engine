/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 Part E: the i386 thunk DLL (d3d9.dll for 32-bit programs). Its interfaces are proxies
 * whose vtables hold generated stubs (gen/); this header joins those with the hand-written guest
 * code (B4's seam, clarification 7) that does what native code must not: window, focus, cursor
 * and GDI calls, and the program's own COM objects kept as private data. */

#ifndef D3D9_THUNK_H
#define D3D9_THUNK_H

#define COBJMACROS
#include <stdint.h>
#include <windows.h>
#include <d3d9.h>

#include "d3d9w_ids.h"          /* gen/ */
#include "d3d9w_proxy.h"        /* include/ */
#include "d3d9w_stubs_i386.h"   /* gen/ */
#include "d3d9w_guest_i386.h"   /* gen/ */

/* gen/d3d9w_vtables_i386.c */
extern const void *const *const d3d9w_vtables[D3D9W_NUM_KINDS];
extern const unsigned int d3d9w_vtable_lengths[D3D9W_NUM_KINDS];

/* d3d9_thunk.c: one line to Wine's debug output. */
void d3d9t_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* d3d9_thunk_resource.c: guest-side state on proxies (private data, GDI DCs). */
void d3d9t_resource_init(void);
void d3d9t_set_shared(uint32_t shared_guest);
void d3d9t_sweep_if_needed(void);

/* d3d9_thunk_device.c: the device's window, focus-window and cursor state. */
void d3d9t_device_init(void);
void d3d9t_device_created(IDirect3DDevice9 *device);
void d3d9t_device_reset(IDirect3DDevice9 *device);
void d3d9t_device_released(IDirect3DDevice9 *device);

#endif /* D3D9_THUNK_H */
