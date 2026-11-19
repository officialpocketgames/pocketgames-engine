/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 E3 and H: the thunk's decisions that need no Windows call, as inline functions the
 * host tests compile on Linux too. Each matches what the PE build of the frontend
 * (out/05-dxmt-ios/dxmt/src/d3d9) does. */

#ifndef D3D9_THUNK_PURE_H
#define D3D9_THUNK_PURE_H

/* Part H: PG_D3D9_NATIVE must be exactly "1" (GetEnvironmentVariableA's length and buffer). */
static inline int d3d9t_env_wants_native(const char *value, unsigned long len)
{
    return len == 1 && value[0] == '1';
}

/* Part H's log line: why there is no gate (D3D9W_NOGATE_*). */
static inline const char *d3d9t_nogate_reason(unsigned int reason)
{
    switch (reason)
    {
    case D3D9W_NOGATE_NO_CPU_GATE: return "the app's 32-bit CPU has no gate (an App Store build)";
    case D3D9W_NOGATE_REFUSED: return "the app's 32-bit CPU refused to register the library";
    case D3D9W_NOGATE_VERSION: return "the thunk and the native library come from different builds";
    case D3D9W_NOGATE_NOT_WOW64: return "the caller is not a 32-bit process";
    default: return "the native library returned no gate";
    }
}

/* SetCursorProperties' checks before the bitmap is read (the PE build's cursor validation):
 * A8R8G8B8, power-of-two extents (0 passes), the hotspot inside the bitmap, and a bitmap no
 * larger than the adapter's display mode, windowed or not. */
static inline HRESULT d3d9t_validate_cursor(D3DFORMAT format, UINT width, UINT height, UINT hot_x,
                                            UINT hot_y, UINT mode_width, UINT mode_height)
{
    if (format != D3DFMT_A8R8G8B8)
        return D3DERR_INVALIDCALL;
    if ((width & (width - 1)) || (height & (height - 1)))
        return D3DERR_INVALIDCALL;
    if ((width && hot_x >= width) || (height && hot_y >= height))
        return D3DERR_INVALIDCALL;
    if (width > mode_width || height > mode_height)
        return D3DERR_INVALIDCALL;
    return D3D_OK;
}

/* Clarification 12: the x87 control word a device sets on the creating thread unless
 * D3DCREATE_FPU_PRESERVE, as the PE build's device does: every exception masked (bits 0-5),
 * single precision (precision control, bits 8-9, 00), round to nearest (bits 10-11, 00); bits 6-7
 * and 12-15 kept. */
static inline unsigned short d3d9t_fpu_control(unsigned short cw)
{
    return (unsigned short)((cw & 0xf0c0u) | 0x003fu);
}

/* The surface formats GetDC paints into (the PE build's list). */
static inline int d3d9t_getdc_format_ok(D3DFORMAT format)
{
    switch (format)
    {
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8R8G8B8:
        return 1;
    default:
        return 0;
    }
}

/* GetPrivateData of an IUnknown entry the thunk keeps: the size is always sizeof(IUnknown *)
 * (4 here); no buffer is a size query; a buffer smaller than that gets D3DERR_MOREDATA.
 * Returns whether the pointer is copied out (and so AddRef'd). */
static inline HRESULT d3d9t_private_iface_size(int have_buffer, unsigned long in_size, int *copy)
{
    *copy = 0;
    if (!have_buffer)
        return D3D_OK;
    if (in_size < 4)
        return D3DERR_MOREDATA;
    *copy = 1;
    return D3D_OK;
}

#endif /* D3D9_THUNK_PURE_H */
