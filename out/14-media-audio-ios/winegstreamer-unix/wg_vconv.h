// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 5 item 6: raw video conversion for wg_transform (the video processor). Plain C,
// no Wine or Apple headers. wg_vconv.c plans and runs a conversion out of the plane operations
// below; wg_vconv_vimage.c implements those with Accelerate's vImage (the app), the host tests
// with plain C (tests/wg_vconv_ops_c.c).
//
// Every conversion goes through 4:2:0 bi-planar (a Y plane and an interleaved CbCr plane); when
// the input or output is that, the planes are used in place. YUY2 input keeps the chroma of its
// even rows. Sizes are even (4:2:0) and the same on both sides (no scaling).

#ifndef WG_VCONV_H
#define WG_VCONV_H

#include <stddef.h>
#include <stdint.h>

#include "wg_parser_pure.h"

/* the plane operations (8-bit samples; widths in bytes per row of the result unless noted) */
void wgv_op_interleave( const uint8_t *u, size_t us, const uint8_t *v, size_t vs, uint8_t *uv, size_t uvs,
                        uint32_t cw, uint32_t ch );              /* U, V planes -> CbCr pairs */
void wgv_op_deinterleave( const uint8_t *uv, size_t uvs, uint8_t *u, size_t us, uint8_t *v, size_t vs,
                          uint32_t cw, uint32_t ch );            /* CbCr pairs -> U, V planes */
void wgv_op_yuy2_split( const uint8_t *src, size_t ss, uint8_t *y, size_t ys, uint8_t *uv, size_t uvs,
                        uint32_t w, uint32_t h );                /* Y of every row, CbCr of even rows */
void wgv_op_yuy2_join( const uint8_t *y, size_t ys, const uint8_t *uv, size_t uvs, uint8_t *dst, size_t ds,
                       uint32_t w, uint32_t h );                 /* row r: Y row r, CbCr row r / 2 */
/* 4:2:0 bi-planar to 32-bit RGB (BGRX/BGRA/RGBA, alpha 255); 0, or -1 when it can't */
int wgv_op_to_rgb( const uint8_t *y, size_t ys, const uint8_t *uv, size_t uvs, uint8_t *dst, size_t ds,
                   uint32_t w, uint32_t h, enum wgv_format format, int bt709, int full_range );

/* MF subtype (the Data1 of an {XXXXXXXX-0000-0010-8000-00AA00389B71} GUID) to a format: 0, or
 * -1 when it isn't one of NV12, I420, IYUV, YV12, YUY2 (and, with rgb, RGB32, ARGB32, ABGR32) */
int wgv_format_from_subtype( uint32_t data1, int rgb, enum wgv_format *format );
const char *wgv_subtype_name( uint32_t data1 );
static inline int wgv_is_rgb( enum wgv_format f ) { return f == WGV_BGRX || f == WGV_BGRA || f == WGV_RGBA; }

/* the layout of a frame whose first plane has `stride0` bytes per row (0: the default layout),
 * as GStreamer pads a 2D buffer: 0, or -1 */
int wgv_layout_stride( enum wgv_format format, uint32_t w, uint32_t h, uint32_t stride0, struct wgv_layout *l );

struct wgv_conv
{
    enum wgv_format in, out;
    uint32_t width, height;
    int bt709, full_range;
    int bottom_up;            /* RGB output rows written bottom-up */
    uint8_t *scratch;         /* the bi-planar intermediate, when needed */
    size_t scratch_size;
};
/* 0, or -1 for a conversion that isn't supported (odd or too large size, RGB input) */
int wgv_conv_init( struct wgv_conv *c, enum wgv_format in, enum wgv_format out, uint32_t width, uint32_t height,
                   int bt709, int full_range, int bottom_up );
void wgv_conv_free( struct wgv_conv *c );
/* converts one frame between the layouts; 0, or -1 (no memory, a bad layout) */
int wgv_conv_run( struct wgv_conv *c, const uint8_t *src, const struct wgv_layout *sl, uint8_t *dst,
                  const struct wgv_layout *dl );

#endif /* WG_VCONV_H */
