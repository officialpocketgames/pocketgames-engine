/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 F4 / J2: layout checks of every struct that crosses, against Wine 11.4's d3d9.h.
 * Compiled (never run) for i686-w64-mingw32 and for aarch64 by tests/run-host-tests.sh (and for
 * the Linux host). build-ios.sh compiles it for aarch64-apple-ios too.
 *   - copied structs: the same size on both targets, and the same offset of every 8-byte field;
 *   - converted structs, on i686: the d3d9.h struct has exactly the mirror's size and offsets. */

#include <stddef.h>
#include <windows.h>
#include <d3d9.h>
#include "d3d9w_layout.h"
#include "d3d9w_proxy.h"

#if defined(__i386__)
#define COPIED(T, i386, native) _Static_assert(sizeof(T) == (i386), #T " is " #i386 " bytes on i386");
#else
#define COPIED(T, i386, native) _Static_assert(sizeof(T) == (native) && (i386) <= (native), \
                                               #T " is " #native " bytes on 64-bit targets");
#endif
D3D9W_COPIED_STRUCTS(COPIED)

#define WIDE(T, f, off) _Static_assert(offsetof(T, f) == (off), #T "." #f " at " #off);
D3D9W_COPIED_WIDE_FIELDS(WIDE)

#if defined(__i386__)
#define FIELD(f) _Static_assert(offsetof(CUR_T, f) == offsetof(CUR_M, f) && \
                                sizeof(((CUR_T *)0)->f) == sizeof(((CUR_M *)0)->f), "field " #f);

#define CUR_T D3DPRESENT_PARAMETERS
#define CUR_M struct d3d9w_g_D3DPRESENT_PARAMETERS
_Static_assert(sizeof(CUR_T) == sizeof(CUR_M), "D3DPRESENT_PARAMETERS mirror size");
D3D9W_FIELDS_D3DPRESENT_PARAMETERS(FIELD)
#undef CUR_T
#undef CUR_M

#define CUR_T D3DDEVICE_CREATION_PARAMETERS
#define CUR_M struct d3d9w_g_D3DDEVICE_CREATION_PARAMETERS
_Static_assert(sizeof(CUR_T) == sizeof(CUR_M), "D3DDEVICE_CREATION_PARAMETERS mirror size");
D3D9W_FIELDS_D3DDEVICE_CREATION_PARAMETERS(FIELD)
#undef CUR_T
#undef CUR_M

#define CUR_T D3DLOCKED_RECT
#define CUR_M struct d3d9w_g_D3DLOCKED_RECT
_Static_assert(sizeof(CUR_T) == sizeof(CUR_M), "D3DLOCKED_RECT mirror size");
D3D9W_FIELDS_D3DLOCKED_RECT(FIELD)
#undef CUR_T
#undef CUR_M

#define CUR_T D3DLOCKED_BOX
#define CUR_M struct d3d9w_g_D3DLOCKED_BOX
_Static_assert(sizeof(CUR_T) == sizeof(CUR_M), "D3DLOCKED_BOX mirror size");
D3D9W_FIELDS_D3DLOCKED_BOX(FIELD)
#undef CUR_T
#undef CUR_M

#define CUR_T D3DPRESENTSTATS
#define CUR_M struct d3d9w_g_D3DPRESENTSTATS
_Static_assert(sizeof(CUR_T) == sizeof(CUR_M), "D3DPRESENTSTATS mirror size");
D3D9W_FIELDS_D3DPRESENTSTATS(FIELD)
#undef CUR_T
#undef CUR_M

/* The i386 window state the thunk reads is the seam's struct with 32-bit handles. */
_Static_assert(sizeof(struct d3d9w_guest_window_state) == 48, "window state");
_Static_assert(sizeof(RGNDATA) == 36, "RGNDATA: header plus one byte of Buffer, padded");
#else
/* On the 64-bit side the converted structs really differ: the converters are needed. */
_Static_assert(sizeof(D3DPRESENT_PARAMETERS) == 64, "D3DPRESENT_PARAMETERS, 64-bit");
_Static_assert(sizeof(D3DDEVICE_CREATION_PARAMETERS) == 24, "D3DDEVICE_CREATION_PARAMETERS, 64-bit");
_Static_assert(sizeof(D3DLOCKED_RECT) == 16, "D3DLOCKED_RECT, 64-bit");
_Static_assert(sizeof(D3DLOCKED_BOX) == 16, "D3DLOCKED_BOX, 64-bit");
_Static_assert(sizeof(D3DPRESENTSTATS) == 32 && offsetof(D3DPRESENTSTATS, SyncQPCTime) == 16,
               "D3DPRESENTSTATS, 64-bit");
_Static_assert(sizeof(HWND) == 8 && sizeof(void *) == 8, "a 64-bit target");
#endif

/* Mirrors have one layout everywhere: fixed-width fields, no padding. */
_Static_assert(sizeof(struct d3d9w_g_D3DPRESENT_PARAMETERS) == 56, "mirror");
_Static_assert(sizeof(struct d3d9w_g_D3DDEVICE_CREATION_PARAMETERS) == 16, "mirror");
_Static_assert(sizeof(struct d3d9w_g_D3DLOCKED_RECT) == 8, "mirror");
_Static_assert(sizeof(struct d3d9w_g_D3DLOCKED_BOX) == 12, "mirror");
_Static_assert(sizeof(struct d3d9w_g_D3DPRESENTSTATS) == 28, "mirror");
_Static_assert(sizeof(struct d3d9w_proxy) == D3D9W_SLOT_BYTES, "proxy slot");
_Static_assert(sizeof(struct d3d9w_register_params) == 16, "registration parameters");

/* An empty translation unit is not ISO C. */
typedef int d3d9w_layout_check_unit;
