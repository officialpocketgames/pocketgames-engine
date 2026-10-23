// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B3: the pure parts of the media parser (see wg_parser_pure.h).

#include <string.h>

#include "wg_parser_pure.h"

/* ---------------------------------------------------------------- layouts */

int wgv_layout_for( enum wgv_format format, uint32_t w, uint32_t h, struct wgv_layout *l )
{
    uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;

    memset( l, 0, sizeof(*l) );
    if (!w || !h || w > 8192 || h > 8192) return -1;
    switch (format)
    {
    case WGV_NV12:
        l->planes = 2;
        l->stride[0] = l->stride[1] = wgv_align4( w );
        l->rows[0] = h;
        l->rows[1] = ch;
        l->offset[1] = l->stride[0] * h;
        l->size = l->offset[1] + l->stride[1] * ch;
        return 0;
    case WGV_I420:
    case WGV_YV12:
        l->planes = 3;
        l->stride[0] = wgv_align4( w );
        l->stride[1] = l->stride[2] = wgv_align4( cw );
        l->rows[0] = h;
        l->rows[1] = l->rows[2] = ch;
        /* plane 1 is U, plane 2 is V; I420 stores U first, YV12 V first */
        if (format == WGV_I420)
        {
            l->offset[1] = l->stride[0] * h;
            l->offset[2] = l->offset[1] + l->stride[1] * ch;
        }
        else
        {
            l->offset[2] = l->stride[0] * h;
            l->offset[1] = l->offset[2] + l->stride[2] * ch;
        }
        l->size = l->stride[0] * h + 2 * l->stride[1] * ch;
        return 0;
    case WGV_YUY2:
        l->planes = 1;
        l->stride[0] = wgv_align4( 2 * w );
        l->rows[0] = h;
        l->size = l->stride[0] * h;
        return 0;
    case WGV_BGRX:
    case WGV_BGRA:
    case WGV_RGBA:
        l->planes = 1;
        l->stride[0] = 4 * w;
        l->rows[0] = h;
        l->size = l->stride[0] * h;
        return 0;
    }
    return -1;
}

/* ---------------------------------------------------------------- conversions */

struct rgb_coefs
{
    int32_t y_off;           /* 16 (video range) or 0 */
    int32_t cy, crv, cbu, cgu, cgv;   /* 16.16 fixed point */
};

static int32_t fix16( double v )
{
    return (int32_t)(v * 65536.0 + 0.5);
}

static void rgb_coefs_for( int bt709, int full_range, struct rgb_coefs *k )
{
    double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1.0 - kr - kb;
    double sy = full_range ? 1.0 : 255.0 / 219.0, sc = full_range ? 1.0 : 255.0 / 224.0;

    k->y_off = full_range ? 0 : 16;
    k->cy = fix16( sy );
    k->crv = fix16( sc * 2.0 * (1.0 - kr) );
    k->cbu = fix16( sc * 2.0 * (1.0 - kb) );
    k->cgu = fix16( sc * 2.0 * kb * (1.0 - kb) / kg );
    k->cgv = fix16( sc * 2.0 * kr * (1.0 - kr) / kg );
}

static uint8_t clamp8( int32_t v )
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void yuv_to_rgb( const struct rgb_coefs *k, int32_t y, int32_t cb, int32_t cr, uint8_t rgb[3] )
{
    int32_t ys = k->cy * (y - k->y_off);

    cb -= 128;
    cr -= 128;
    rgb[0] = clamp8( (ys + k->crv * cr + 32768) >> 16 );
    rgb[1] = clamp8( (ys - k->cgu * cb - k->cgv * cr + 32768) >> 16 );
    rgb[2] = clamp8( (ys + k->cbu * cb + 32768) >> 16 );
}

/* the destination row of plane row `r` out of `rows` */
static uint32_t dst_row( uint32_t r, uint32_t rows, int bottom_up )
{
    return bottom_up ? rows - 1 - r : r;
}

void wgv_convert( const struct wgv_source *src, enum wgv_format format, uint32_t w, uint32_t h,
                  int bt709, int bottom_up, uint8_t *dst )
{
    uint32_t cw_luma = src->width < w ? src->width : w;      /* copied luma columns */
    uint32_t ch_luma = src->height < h ? src->height : h;    /* copied luma rows */
    uint32_t src_cw = (src->width + 1) / 2, src_ch = (src->height + 1) / 2;
    uint32_t out_cw = (w + 1) / 2, out_ch = (h + 1) / 2;
    uint32_t cw = src_cw < out_cw ? src_cw : out_cw, chr = src_ch < out_ch ? src_ch : out_ch;
    struct wgv_layout l;
    uint32_t x, y;

    if (wgv_layout_for( format, w, h, &l )) return;

    switch (format)
    {
    case WGV_NV12:
    case WGV_I420:
    case WGV_YV12:
        /* luma */
        for (y = 0; y < h; y++)
        {
            uint8_t *row = dst + l.offset[0] + (size_t)dst_row( y, h, bottom_up ) * l.stride[0];
            memset( row, 16, l.stride[0] );
            if (y < ch_luma) memcpy( row, src->y + (size_t)y * src->y_stride, cw_luma );
        }
        if (format == WGV_NV12)
        {
            for (y = 0; y < out_ch; y++)
            {
                uint8_t *row = dst + l.offset[1] + (size_t)dst_row( y, out_ch, bottom_up ) * l.stride[1];
                memset( row, 128, l.stride[1] );
                if (y < chr) memcpy( row, src->uv + (size_t)y * src->uv_stride, 2 * (size_t)cw );
            }
        }
        else
        {
            for (y = 0; y < out_ch; y++)
            {
                uint8_t *u = dst + l.offset[1] + (size_t)dst_row( y, out_ch, bottom_up ) * l.stride[1];
                uint8_t *v = dst + l.offset[2] + (size_t)dst_row( y, out_ch, bottom_up ) * l.stride[2];
                const uint8_t *s = src->uv + (size_t)y * src->uv_stride;

                memset( u, 128, l.stride[1] );
                memset( v, 128, l.stride[2] );
                if (y >= chr) continue;
                for (x = 0; x < cw; x++)
                {
                    u[x] = s[2 * x];
                    v[x] = s[2 * x + 1];
                }
            }
        }
        return;

    case WGV_YUY2:
        for (y = 0; y < h; y++)
        {
            uint8_t *row = dst + (size_t)dst_row( y, h, bottom_up ) * l.stride[0];
            const uint8_t *sy = y < ch_luma ? src->y + (size_t)y * src->y_stride : NULL;
            const uint8_t *suv = y < ch_luma ? src->uv + (size_t)(y / 2) * src->uv_stride : NULL;

            memset( row, 0, l.stride[0] );
            for (x = 0; x < w; x += 2)
            {
                uint8_t *p = row + 2 * x;
                int in0 = y < ch_luma && x < cw_luma;
                int in1 = y < ch_luma && x + 1 < cw_luma;

                p[0] = in0 ? sy[x] : 16;
                if (x + 1 < w) p[2] = in1 ? sy[x + 1] : 16;
                else p[2] = p[0];   /* odd width: repeat the last luma */
                p[1] = in0 ? suv[x & ~1u] : 128;
                p[3] = in0 ? suv[(x & ~1u) + 1] : 128;
            }
        }
        return;

    case WGV_BGRX:
    case WGV_BGRA:
    case WGV_RGBA:
    {
        struct rgb_coefs k;

        rgb_coefs_for( bt709, src->full_range, &k );
        for (y = 0; y < h; y++)
        {
            uint8_t *row = dst + (size_t)dst_row( y, h, bottom_up ) * l.stride[0];
            const uint8_t *sy = y < ch_luma ? src->y + (size_t)y * src->y_stride : NULL;
            const uint8_t *suv = y < ch_luma ? src->uv + (size_t)(y / 2) * src->uv_stride : NULL;

            for (x = 0; x < w; x++)
            {
                uint8_t *p = row + 4 * (size_t)x, rgb[3] = { 0, 0, 0 };

                if (y < ch_luma && x < cw_luma)
                    yuv_to_rgb( &k, sy[x], suv[2 * (x / 2)], suv[2 * (x / 2) + 1], rgb );
                if (format == WGV_RGBA)
                {
                    p[0] = rgb[0];
                    p[1] = rgb[1];
                    p[2] = rgb[2];
                }
                else
                {
                    p[0] = rgb[2];
                    p[1] = rgb[1];
                    p[2] = rgb[0];
                }
                p[3] = 0xff;
            }
        }
        return;
    }
    }
}

/* ---------------------------------------------------------------- audio */

void wg_pack_s24( const int32_t *src, size_t samples, uint8_t *dst )
{
    const uint8_t *in = (const uint8_t *)src;
    size_t i;

    for (i = 0; i < samples; i++)
    {
        int32_t s;
        uint32_t v;

        memcpy( &s, in + 4 * i, 4 );   /* read before the in-place write below reaches it */
        v = (uint32_t)s >> 8;
        dst[3 * i] = v & 0xff;
        dst[3 * i + 1] = (v >> 8) & 0xff;
        dst[3 * i + 2] = (v >> 16) & 0xff;
    }
}

static int popcount64( uint64_t v )
{
    int n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

uint32_t wg_channel_mask( int native_order, uint64_t mask, int channels, uint32_t default_mask )
{
    if (native_order && channels > 0 && popcount64( mask ) == channels && !(mask & ~(uint64_t)0x3ffff))
        return (uint32_t)mask;
    return default_mask;
}

/* ---------------------------------------------------------------- reorder */

int wg_reorder_insert( struct wg_reorder *r, const struct wg_reorder_item *item )
{
    unsigned int i;

    if (r->count >= WG_REORDER_MAX + 1) return -1;
    for (i = r->count; i > 0 && r->items[i - 1].pts > item->pts; i--) r->items[i] = r->items[i - 1];
    r->items[i] = *item;
    r->count++;
    return 0;
}

int wg_reorder_ready( const struct wg_reorder *r, int has_dts, int64_t last_dts, int draining )
{
    if (!r->count) return 0;
    if (draining || r->count > WG_REORDER_MAX) return 1;
    return has_dts && last_dts >= r->items[0].pts;
}

struct wg_reorder_item wg_reorder_pop( struct wg_reorder *r )
{
    struct wg_reorder_item first = { 0 };
    unsigned int i;

    if (!r->count) return first;
    first = r->items[0];
    for (i = 1; i < r->count; i++) r->items[i - 1] = r->items[i];
    r->count--;
    return first;
}

/* ---------------------------------------------------------------- trimming */

uint32_t wg_trim_start( int64_t pts, uint32_t frames, uint32_t rate, int64_t target )
{
    int64_t n;

    if (target <= pts || !rate) return 0;
    n = ((target - pts) * (int64_t)rate + 9999999) / 10000000;
    return n > (int64_t)frames ? frames : (uint32_t)n;
}

uint32_t wg_trim_stop( int64_t pts, uint32_t frames, uint32_t rate, int64_t stop )
{
    int64_t n;

    if (stop <= pts) return 0;
    if (!rate) return frames;
    n = ((stop - pts) * (int64_t)rate + 9999999) / 10000000;
    return n > (int64_t)frames ? frames : (uint32_t)n;
}

/* ---------------------------------------------------------------- queues */

int wg_queue_admit( uint32_t count, uint64_t bytes, uint32_t size )
{
    return count < WG_QUEUE_MAX_PACKETS && bytes + size <= WG_QUEUE_MAX_BYTES;
}

void wg_pts_fifo_push( struct wg_pts_fifo *f, int64_t pts )
{
    if (f->count == WG_PTS_FIFO_MAX)
    {
        f->head = (f->head + 1) % WG_PTS_FIFO_MAX;
        f->count--;
    }
    f->pts[(f->head + f->count) % WG_PTS_FIFO_MAX] = pts;
    f->count++;
}

int wg_pts_fifo_pop( struct wg_pts_fifo *f, int64_t *pts )
{
    if (!f->count) return 0;
    *pts = f->pts[f->head];
    f->head = (f->head + 1) % WG_PTS_FIFO_MAX;
    f->count--;
    return 1;
}

void wg_pts_fifo_clear( struct wg_pts_fifo *f )
{
    f->head = f->count = 0;
}
