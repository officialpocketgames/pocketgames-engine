// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 5 item 6: the plan of a raw video conversion (wg_vconv.h). Row copies are
// memcpy; every per-sample step is one of the wgv_op_* operations (vImage in the app).

#include <stdlib.h>
#include <string.h>

#include "wg_vconv.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static const struct { uint32_t data1; const char *name; enum wgv_format format; int rgb; } subtypes[] =
{
    { FOURCC('N','V','1','2'), "NV12", WGV_NV12, 0 },
    { FOURCC('I','4','2','0'), "I420", WGV_I420, 0 },
    { FOURCC('I','Y','U','V'), "IYUV", WGV_I420, 0 },   /* the same layout as I420 */
    { FOURCC('Y','V','1','2'), "YV12", WGV_YV12, 0 },
    { FOURCC('Y','U','Y','2'), "YUY2", WGV_YUY2, 0 },
    { 22, "RGB32", WGV_BGRX, 1 },                         /* D3DFMT_X8R8G8B8: B G R x in memory */
    { 21, "ARGB32", WGV_BGRA, 1 },                        /* D3DFMT_A8R8G8B8: B G R A */
    { 32, "ABGR32", WGV_RGBA, 1 },                        /* D3DFMT_A8B8G8R8: R G B A */
};

int wgv_format_from_subtype( uint32_t data1, int rgb, enum wgv_format *format )
{
    unsigned int i;

    for (i = 0; i < sizeof(subtypes) / sizeof(subtypes[0]); i++)
        if (subtypes[i].data1 == data1 && (rgb || !subtypes[i].rgb))
        {
            *format = subtypes[i].format;
            return 0;
        }
    return -1;
}

const char *wgv_subtype_name( uint32_t data1 )
{
    unsigned int i;

    for (i = 0; i < sizeof(subtypes) / sizeof(subtypes[0]); i++)
        if (subtypes[i].data1 == data1) return subtypes[i].name;
    return "?";
}

int wgv_layout_stride( enum wgv_format format, uint32_t w, uint32_t h, uint32_t stride0, struct wgv_layout *l )
{
    uint32_t ch = (h + 1) / 2;

    if (wgv_layout_for( format, w, h, l )) return -1;
    if (!stride0 || stride0 == l->stride[0]) return 0;
    if (stride0 < l->stride[0] && stride0 < (wgv_is_rgb( format ) ? 4 * w : format == WGV_YUY2 ? 2 * w : w))
        return -1;
    switch (format)
    {
    case WGV_NV12:
        l->stride[0] = l->stride[1] = stride0;
        l->offset[1] = stride0 * h;
        l->size = l->offset[1] + stride0 * ch;
        break;
    case WGV_I420:
    case WGV_YV12:
        if (stride0 & 1) return -1;
        l->stride[0] = stride0;
        l->stride[1] = l->stride[2] = stride0 / 2;
        if (format == WGV_I420)
        {
            l->offset[1] = stride0 * h;
            l->offset[2] = l->offset[1] + l->stride[1] * ch;
        }
        else
        {
            l->offset[2] = stride0 * h;
            l->offset[1] = l->offset[2] + l->stride[2] * ch;
        }
        l->size = stride0 * h + 2 * l->stride[1] * ch;
        break;
    default:
        l->stride[0] = stride0;
        l->size = stride0 * h;
        break;
    }
    return 0;
}

static int is_planar( enum wgv_format f ) { return f == WGV_I420 || f == WGV_YV12; }

int wgv_conv_init( struct wgv_conv *c, enum wgv_format in, enum wgv_format out, uint32_t width, uint32_t height,
                   int bt709, int full_range, int bottom_up )
{
    size_t need = 0;

    memset( c, 0, sizeof(*c) );
    if (wgv_is_rgb( in ) || !width || !height || (width & 1) || (height & 1) || width > 8192 || height > 8192)
        return -1;
    c->in = in;
    c->out = out;
    c->width = width;
    c->height = height;
    c->bt709 = bt709;
    c->full_range = full_range;
    c->bottom_up = bottom_up && wgv_is_rgb( out );   /* YUV frames are always top-down */

    /* the bi-planar intermediate, unless the output holds it or no conversion needs it */
    if (is_planar( in ) && out != WGV_NV12 && !is_planar( out ))
        need = (size_t)width * (height / 2);
    else if (in == WGV_YUY2 && out != WGV_NV12 && out != WGV_YUY2)
        need = (size_t)width * height + (size_t)width * (height / 2);
    if (need && !(c->scratch = malloc( need ))) return -1;
    c->scratch_size = need;
    return 0;
}

void wgv_conv_free( struct wgv_conv *c )
{
    free( c->scratch );
    c->scratch = NULL;
}

static void copy_plane( const uint8_t *src, size_t ss, uint8_t *dst, size_t ds, uint32_t bytes, uint32_t rows )
{
    uint32_t r;

    if (ss == ds && ss == bytes)
    {
        memcpy( dst, src, (size_t)bytes * rows );
        return;
    }
    for (r = 0; r < rows; r++) memcpy( dst + r * ds, src + r * ss, bytes );
}

/* reverses the rows of a plane in place: swaps row pairs through a small stack buffer */
static void flip_rows( uint8_t *p, size_t stride, uint32_t bytes, uint32_t rows )
{
    uint8_t tmp[4096];
    uint32_t top, done, n;

    for (top = 0; top < rows / 2; top++)
    {
        uint8_t *a = p + top * stride, *b = p + (rows - 1 - top) * stride;

        for (done = 0; done < bytes; done += n)
        {
            n = bytes - done < sizeof(tmp) ? bytes - done : (uint32_t)sizeof(tmp);
            memcpy( tmp, a + done, n );
            memcpy( a + done, b + done, n );
            memcpy( b + done, tmp, n );
        }
    }
}

static int layout_fits( const struct wgv_layout *l, enum wgv_format f, uint32_t w, uint32_t h )
{
    struct wgv_layout def;

    if (wgv_layout_for( f, w, h, &def ) || l->planes != def.planes) return 0;
    return l->stride[0] >= (wgv_is_rgb( f ) ? 4 * w : f == WGV_YUY2 ? 2 * w : w);
}

int wgv_conv_run( struct wgv_conv *c, const uint8_t *src, const struct wgv_layout *sl, uint8_t *dst,
                  const struct wgv_layout *dl )
{
    uint32_t w = c->width, h = c->height, cw = w / 2, ch = h / 2;
    const uint8_t *y = NULL, *uv = NULL;
    size_t ys = 0, uvs = 0;

    if (!layout_fits( sl, c->in, w, h ) || !layout_fits( dl, c->out, w, h )) return -1;

    /* 1. the source as bi-planar 4:2:0 (or a direct copy) */
    switch (c->in)
    {
    case WGV_NV12:
        y = src + sl->offset[0];
        ys = sl->stride[0];
        uv = src + sl->offset[1];
        uvs = sl->stride[1];
        break;
    case WGV_I420:
    case WGV_YV12:
        y = src + sl->offset[0];
        ys = sl->stride[0];
        if (is_planar( c->out ))
        {
            copy_plane( y, ys, dst + dl->offset[0], dl->stride[0], w, h );
            copy_plane( src + sl->offset[1], sl->stride[1], dst + dl->offset[1], dl->stride[1], cw, ch );
            copy_plane( src + sl->offset[2], sl->stride[2], dst + dl->offset[2], dl->stride[2], cw, ch );
            return 0;
        }
        if (c->out == WGV_NV12)
        {
            uv = dst + dl->offset[1];
            uvs = dl->stride[1];
        }
        else
        {
            uv = c->scratch;
            uvs = w;
        }
        wgv_op_interleave( src + sl->offset[1], sl->stride[1], src + sl->offset[2], sl->stride[2],
                           (uint8_t *)uv, uvs, cw, ch );
        break;
    case WGV_YUY2:
        if (c->out == WGV_YUY2)
        {
            copy_plane( src, sl->stride[0], dst, dl->stride[0], 2 * w, h );
            return 0;
        }
        if (c->out == WGV_NV12)
        {
            y = dst + dl->offset[0];
            ys = dl->stride[0];
            uv = dst + dl->offset[1];
            uvs = dl->stride[1];
        }
        else
        {
            y = c->scratch;
            ys = w;
            uv = c->scratch + (size_t)w * h;
            uvs = w;
        }
        wgv_op_yuy2_split( src + sl->offset[0], sl->stride[0], (uint8_t *)y, ys, (uint8_t *)uv, uvs, w, h );
        break;
    default:
        return -1;
    }

    /* 2. bi-planar to the output */
    switch (c->out)
    {
    case WGV_NV12:
        if (y != dst + dl->offset[0]) copy_plane( y, ys, dst + dl->offset[0], dl->stride[0], w, h );
        if (uv != dst + dl->offset[1]) copy_plane( uv, uvs, dst + dl->offset[1], dl->stride[1], w, ch );
        break;
    case WGV_I420:
    case WGV_YV12:
        copy_plane( y, ys, dst + dl->offset[0], dl->stride[0], w, h );
        wgv_op_deinterleave( uv, uvs, dst + dl->offset[1], dl->stride[1], dst + dl->offset[2], dl->stride[2], cw, ch );
        break;
    case WGV_YUY2:
        wgv_op_yuy2_join( y, ys, uv, uvs, dst + dl->offset[0], dl->stride[0], w, h );
        break;
    default:
        if (wgv_op_to_rgb( y, ys, uv, uvs, dst + dl->offset[0], dl->stride[0], w, h, c->out, c->bt709,
                           c->full_range ))
            return -1;
        if (c->bottom_up) flip_rows( dst + dl->offset[0], dl->stride[0], 4 * w, h );
        break;
    }
    return 0;
}
