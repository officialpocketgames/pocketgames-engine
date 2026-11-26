// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 5 item 6: the plane operations of wg_vconv.h with Accelerate's vImage (public
// API, Apple's vImage Conversion.h). The app links -framework Accelerate. The host tests use
// tests/wg_vconv_ops_c.c instead; only the device runs this file (TESTS.md).

#include <Accelerate/Accelerate.h>

#include "wg_vconv.h"

static vImage_Buffer buf( const uint8_t *p, uint32_t width, uint32_t height, size_t row_bytes )
{
    vImage_Buffer b;

    b.data = (void *)p;
    b.width = width;
    b.height = height;
    b.rowBytes = row_bytes;
    return b;
}

void wgv_op_interleave( const uint8_t *u, size_t us, const uint8_t *v, size_t vs, uint8_t *uv, size_t uvs,
                        uint32_t cw, uint32_t ch )
{
    vImage_Buffer bu = buf( u, cw, ch, us ), bv = buf( v, cw, ch, vs );
    const vImage_Buffer *planes[2] = { &bu, &bv };
    void *dest[2] = { uv, uv + 1 };

    vImageConvert_PlanarToChunky8( planes, dest, 2, 2, cw, ch, uvs, kvImageNoFlags );
}

void wgv_op_deinterleave( const uint8_t *uv, size_t uvs, uint8_t *u, size_t us, uint8_t *v, size_t vs,
                          uint32_t cw, uint32_t ch )
{
    vImage_Buffer bu = buf( u, cw, ch, us ), bv = buf( v, cw, ch, vs );
    const vImage_Buffer *planes[2] = { &bu, &bv };
    const void *src[2] = { uv, uv + 1 };

    vImageConvert_ChunkyToPlanar8( src, planes, 2, 2, cw, ch, uvs, kvImageNoFlags );
}

void wgv_op_yuy2_split( const uint8_t *src, size_t ss, uint8_t *y, size_t ys, uint8_t *uv, size_t uvs,
                        uint32_t w, uint32_t h )
{
    /* Y: every even byte of every row */
    vImage_Buffer by = buf( y, w, h, ys );
    const vImage_Buffer *py[1] = { &by };
    const void *sy[1] = { src };
    /* CbCr: every odd byte (Cb0 Cr0 Cb1 Cr1 ...: already the bi-planar order) of every even row */
    vImage_Buffer bc = buf( uv, w, h / 2, uvs );
    const vImage_Buffer *pc[1] = { &bc };
    const void *sc[1] = { src + 1 };

    vImageConvert_ChunkyToPlanar8( sy, py, 1, 2, w, h, ss, kvImageNoFlags );
    vImageConvert_ChunkyToPlanar8( sc, pc, 1, 2, w, h / 2, 2 * ss, kvImageNoFlags );
}

void wgv_op_yuy2_join( const uint8_t *y, size_t ys, const uint8_t *uv, size_t uvs, uint8_t *dst, size_t ds,
                       uint32_t w, uint32_t h )
{
    unsigned int odd;

    /* even rows, then odd rows: each pair of rows shares one CbCr row */
    for (odd = 0; odd < 2; odd++)
    {
        vImage_Buffer by = buf( y + odd * ys, w, h / 2, 2 * ys ), bc = buf( uv, w, h / 2, uvs );
        const vImage_Buffer *planes[2] = { &by, &bc };
        void *dest[2] = { dst + odd * ds, dst + odd * ds + 1 };

        vImageConvert_PlanarToChunky8( planes, dest, 2, 2, w, h / 2, 2 * ds, kvImageNoFlags );
    }
}

int wgv_op_to_rgb( const uint8_t *y, size_t ys, const uint8_t *uv, size_t uvs, uint8_t *dst, size_t ds,
                   uint32_t w, uint32_t h, enum wgv_format format, int bt709, int full_range )
{
    static const vImage_YpCbCrPixelRange video = { 16, 128, 235, 240, 235, 16, 240, 16 };
    static const vImage_YpCbCrPixelRange full = { 0, 128, 255, 255, 255, 0, 255, 0 };
    /* the destination's bytes, as indices into A R G B */
    static const uint8_t to_bgra[4] = { 3, 2, 1, 0 }, to_rgba[4] = { 1, 2, 3, 0 };
    vImage_YpCbCrToARGB info;
    vImage_Buffer by = buf( y, w, h, ys ), bc = buf( uv, w / 2, h / 2, uvs ), bd = buf( dst, w, h, ds );

    if (vImageConvert_YpCbCrToARGB_GenerateConversion(
            bt709 ? kvImage_YpCbCrToARGBMatrix_ITU_R_709_2 : kvImage_YpCbCrToARGBMatrix_ITU_R_601_4,
            full_range ? &full : &video, &info, kvImage420Yp8_CbCr8, kvImageARGB8888, kvImageNoFlags ))
        return -1;
    return vImageConvert_420Yp8_CbCr8ToARGB8888( &by, &bc, &bd, &info, format == WGV_RGBA ? to_rgba : to_bgra,
                                                 255, kvImageNoFlags ) ? -1 : 0;
}
