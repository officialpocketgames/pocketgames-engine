// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 5 item 6, host tests only: the plane operations of wg_vconv.h in plain C, the
// reference the vImage ones (winegstreamer-unix/wg_vconv_vimage.c, device only) must match.

#include <math.h>

#include "wg_vconv.h"

void wgv_op_interleave( const uint8_t *u, size_t us, const uint8_t *v, size_t vs, uint8_t *uv, size_t uvs,
                        uint32_t cw, uint32_t ch )
{
    uint32_t x, r;

    for (r = 0; r < ch; r++)
        for (x = 0; x < cw; x++)
        {
            uv[r * uvs + 2 * x] = u[r * us + x];
            uv[r * uvs + 2 * x + 1] = v[r * vs + x];
        }
}

void wgv_op_deinterleave( const uint8_t *uv, size_t uvs, uint8_t *u, size_t us, uint8_t *v, size_t vs,
                          uint32_t cw, uint32_t ch )
{
    uint32_t x, r;

    for (r = 0; r < ch; r++)
        for (x = 0; x < cw; x++)
        {
            u[r * us + x] = uv[r * uvs + 2 * x];
            v[r * vs + x] = uv[r * uvs + 2 * x + 1];
        }
}

void wgv_op_yuy2_split( const uint8_t *src, size_t ss, uint8_t *y, size_t ys, uint8_t *uv, size_t uvs,
                        uint32_t w, uint32_t h )
{
    uint32_t x, r;

    for (r = 0; r < h; r++)
        for (x = 0; x < w; x++)
        {
            y[r * ys + x] = src[r * ss + 2 * x];
            if (!(r & 1)) uv[(r / 2) * uvs + x] = src[r * ss + 2 * x + 1];
        }
}

void wgv_op_yuy2_join( const uint8_t *y, size_t ys, const uint8_t *uv, size_t uvs, uint8_t *dst, size_t ds,
                       uint32_t w, uint32_t h )
{
    uint32_t x, r;

    for (r = 0; r < h; r++)
        for (x = 0; x < w; x++)
        {
            dst[r * ds + 2 * x] = y[r * ys + x];
            dst[r * ds + 2 * x + 1] = uv[(r / 2) * uvs + x];
        }
}

static uint8_t clamp8( double v )
{
    return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)lrint( v );
}

int wgv_op_to_rgb( const uint8_t *y, size_t ys, const uint8_t *uv, size_t uvs, uint8_t *dst, size_t ds,
                   uint32_t w, uint32_t h, enum wgv_format format, int bt709, int full_range )
{
    double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114;
    uint32_t x, r;

    for (r = 0; r < h; r++)
        for (x = 0; x < w; x++)
        {
            double yy = y[r * ys + x], cb = uv[(r / 2) * uvs + (x & ~1u)] - 128.0, cr = uv[(r / 2) * uvs + (x & ~1u) + 1] - 128.0;
            double R, G, B;
            uint8_t *p = dst + r * ds + 4 * x;

            if (!full_range)
            {
                yy = (yy - 16) * 255.0 / 219.0;
                cb *= 255.0 / 224.0;
                cr *= 255.0 / 224.0;
            }
            R = yy + 2 * (1 - kr) * cr;
            B = yy + 2 * (1 - kb) * cb;
            G = (yy - kr * R - kb * B) / (1 - kr - kb);
            if (format == WGV_RGBA)
            {
                p[0] = clamp8( R ); p[1] = clamp8( G ); p[2] = clamp8( B );
            }
            else
            {
                p[0] = clamp8( B ); p[1] = clamp8( G ); p[2] = clamp8( R );
            }
            p[3] = 255;
        }
    return 0;
}
