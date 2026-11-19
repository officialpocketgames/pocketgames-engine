/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 F4: the structs that cross between the i386 program and the native frontend.
 *
 * - Structs whose i386 (i686-w64-mingw32) and aarch64 layouts are equal are copied as bytes.
 *   D3D9W_COPIED_STRUCTS lists them with their size; native/d3d9w_layout_check.c asserts the
 *   size (and the offsets of every field that is not 4-byte aligned) on both targets.
 * - Structs with pointer or handle fields have an i386 mirror here (struct d3d9w_g_<name>) and
 *   a converter (native/d3d9w_dispatch.h). On i686 the check file asserts that each mirror has
 *   the d3d9.h struct's size and offsets, so the mirror is the program's layout.
 *
 * The mirrors use fixed-width fields only, so they have the same layout wherever they compile. */

#ifndef D3D9W_LAYOUT_H
#define D3D9W_LAYOUT_H

#include <stdint.h>

/* X(type, i386 size, 64-bit size). The first i386-size bytes are laid out the same on both
 * targets and are what is copied. Only D3DADAPTER_IDENTIFIER9 differs in size: d3d9types.h
 * packs to 4, which on i386 keeps its LARGE_INTEGER at 4-byte alignment and drops the 4 bytes
 * of tail padding the 64-bit struct has. */
#define D3D9W_COPIED_STRUCTS(X) \
    X(D3DADAPTER_IDENTIFIER9, 1100, 1104) \
    X(D3DCAPS9, 304, 304) \
    X(D3DDISPLAYMODE, 16, 16) \
    X(D3DDISPLAYMODEEX, 24, 24) \
    X(D3DDISPLAYMODEFILTER, 12, 12) \
    X(D3DDISPLAYROTATION, 4, 4) \
    X(LUID, 8, 8) \
    X(D3DRASTER_STATUS, 8, 8) \
    X(D3DGAMMARAMP, 1536, 1536) \
    X(RECT, 16, 16) \
    X(POINT, 8, 8) \
    X(D3DRECT, 16, 16) \
    X(D3DMATRIX, 64, 64) \
    X(D3DVIEWPORT9, 24, 24) \
    X(D3DMATERIAL9, 68, 68) \
    X(D3DLIGHT9, 104, 104) \
    X(D3DCLIPSTATUS9, 8, 8) \
    X(PALETTEENTRY, 4, 4) \
    X(D3DRECTPATCH_INFO, 28, 28) \
    X(D3DTRIPATCH_INFO, 16, 16) \
    X(D3DSURFACE_DESC, 32, 32) \
    X(D3DVOLUME_DESC, 28, 28) \
    X(D3DVERTEXBUFFER_DESC, 24, 24) \
    X(D3DINDEXBUFFER_DESC, 20, 20) \
    X(D3DBOX, 24, 24) \
    X(D3DVERTEXELEMENT9, 8, 8) \
    X(RGNDATAHEADER, 32, 32) \
    X(GUID, 16, 16) \
    X(DWORD, 4, 4) \
    X(UINT, 4, 4) \
    X(BOOL, 4, 4) \
    X(INT, 4, 4) \
    X(int, 4, 4) \
    X(float, 4, 4)

/* The bytes copied for one T: its i386 size (D3D9W_GSIZE_D3DCAPS9 and so on). */
#define D3D9W_GSIZE_ENUM_(T, i386, native) D3D9W_GSIZE_##T = (i386),
enum d3d9w_gsizes { D3D9W_COPIED_STRUCTS(D3D9W_GSIZE_ENUM_) D3D9W_GSIZE_END_ = 0 };

/* Fields of copied structs that are 8 bytes wide or follow one: offsets asserted on both. */
#define D3D9W_COPIED_WIDE_FIELDS(X) \
    X(D3DADAPTER_IDENTIFIER9, DriverVersion, 1056) \
    X(D3DADAPTER_IDENTIFIER9, VendorId, 1064) \
    X(D3DADAPTER_IDENTIFIER9, DeviceIdentifier, 1080) \
    X(D3DADAPTER_IDENTIFIER9, WHQLLevel, 1096) \
    X(LUID, HighPart, 4)

/* D3DPRESENTSTATS: its two LARGE_INTEGERs sit at 12 and 20 on i386 (28 bytes), at 16 and 24
 * on 64-bit targets (32 bytes). */
struct d3d9w_g_D3DPRESENTSTATS
{
    uint32_t PresentCount;
    uint32_t PresentRefreshCount;
    uint32_t SyncRefreshCount;
    uint32_t SyncQPCTime[2];    /* low, high */
    uint32_t SyncGPUTime[2];
};
#define D3D9W_FIELDS_D3DPRESENTSTATS(X) \
    X(PresentCount) X(PresentRefreshCount) X(SyncRefreshCount) X(SyncQPCTime) X(SyncGPUTime)

/* D3DPRESENT_PARAMETERS: hDeviceWindow is a 4-byte HWND on i386 (56 bytes), 8 on aarch64. */
struct d3d9w_g_D3DPRESENT_PARAMETERS
{
    uint32_t BackBufferWidth;
    uint32_t BackBufferHeight;
    uint32_t BackBufferFormat;
    uint32_t BackBufferCount;
    uint32_t MultiSampleType;
    uint32_t MultiSampleQuality;
    uint32_t SwapEffect;
    uint32_t hDeviceWindow;
    uint32_t Windowed;
    uint32_t EnableAutoDepthStencil;
    uint32_t AutoDepthStencilFormat;
    uint32_t Flags;
    uint32_t FullScreen_RefreshRateInHz;
    uint32_t PresentationInterval;
};
#define D3D9W_FIELDS_D3DPRESENT_PARAMETERS(X) \
    X(BackBufferWidth) X(BackBufferHeight) X(BackBufferFormat) X(BackBufferCount) \
    X(MultiSampleType) X(MultiSampleQuality) X(SwapEffect) X(hDeviceWindow) X(Windowed) \
    X(EnableAutoDepthStencil) X(AutoDepthStencilFormat) X(Flags) \
    X(FullScreen_RefreshRateInHz) X(PresentationInterval)

/* D3DDEVICE_CREATION_PARAMETERS: hFocusWindow (16 bytes on i386). */
struct d3d9w_g_D3DDEVICE_CREATION_PARAMETERS
{
    uint32_t AdapterOrdinal;
    uint32_t DeviceType;
    uint32_t hFocusWindow;
    uint32_t BehaviorFlags;
};
#define D3D9W_FIELDS_D3DDEVICE_CREATION_PARAMETERS(X) \
    X(AdapterOrdinal) X(DeviceType) X(hFocusWindow) X(BehaviorFlags)

/* D3DLOCKED_RECT: pBits is a guest address (8 bytes on i386, F5). */
struct d3d9w_g_D3DLOCKED_RECT
{
    int32_t Pitch;
    uint32_t pBits;
};
#define D3D9W_FIELDS_D3DLOCKED_RECT(X) X(Pitch) X(pBits)

/* D3DLOCKED_BOX: pBits is a guest address (12 bytes on i386, F5). */
struct d3d9w_g_D3DLOCKED_BOX
{
    int32_t RowPitch;
    int32_t SlicePitch;
    uint32_t pBits;
};
#define D3D9W_FIELDS_D3DLOCKED_BOX(X) X(RowPitch) X(SlicePitch) X(pBits)

#define D3D9W_CONVERTED_STRUCTS(X) \
    X(D3DPRESENT_PARAMETERS) X(D3DDEVICE_CREATION_PARAMETERS) X(D3DLOCKED_RECT) \
    X(D3DLOCKED_BOX) X(D3DPRESENTSTATS)

#endif /* D3D9W_LAYOUT_H */
