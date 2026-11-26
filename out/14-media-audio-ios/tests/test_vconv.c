// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 5 item 6: the conversion plan (winegstreamer-unix/wg_vconv.c) with the plain C
// operations (wg_vconv_ops_c.c): the 16x16 NV12 -> BGRx bottom-up pattern of known colours,
// RGBA order, full range, round trips between the YUV formats, 2D strides, and refusals.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wg_vconv.h"

/* limited-range BT.601 Y Cb Cr and the RGB they stand for */
static const struct { uint8_t y, cb, cr, r, g, b; } colours[5] =
{
    {  16, 128, 128,   0,   0,   0 },   /* black */
    { 235, 128, 128, 255, 255, 255 },   /* white */
    {  81,  90, 240, 255,   0,   0 },   /* red */
    { 145,  54,  34,   0, 255,   0 },   /* green */
    {  41, 240, 110,   0,   0, 255 },   /* blue */
};

/* the colour of a 16x16 pattern pixel: bands of rows (0-3, 4-7, 8-11, 12-13, 14-15), shifted by
 * one band in the right half, so both the rows and the columns can be told apart */
static int pattern( uint32_t x, uint32_t y )
{
    int band = y < 4 ? 0 : y < 8 ? 1 : y < 12 ? 2 : y < 14 ? 3 : 4;
    return (band + (x >= 8)) % 5;
}

static void make_nv12( uint8_t *frame, const struct wgv_layout *l, uint32_t w, uint32_t h )
{
    uint32_t x, y;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            int c = pattern( x, y );
            frame[l->offset[0] + y * l->stride[0] + x] = colours[c].y;
            if (!(x & 1) && !(y & 1))
            {
                frame[l->offset[1] + (y / 2) * l->stride[1] + x] = colours[c].cb;
                frame[l->offset[1] + (y / 2) * l->stride[1] + x + 1] = colours[c].cr;
            }
        }
}

static int near( int a, int b ) { return abs( a - b ) <= 2; }

static void test_pattern_bgrx_bottom_up( void )
{
    struct wgv_layout sl, dl;
    struct wgv_conv c;
    uint8_t src[16 * 16 * 2], dst[16 * 16 * 4];
    uint32_t x, y, bad = 0;

    CHECK_INT( wgv_layout_for( WGV_NV12, 16, 16, &sl ), 0 );
    CHECK_INT( wgv_layout_for( WGV_BGRX, 16, 16, &dl ), 0 );
    memset( src, 0, sizeof(src) );
    make_nv12( src, &sl, 16, 16 );
    CHECK_INT( wgv_conv_init( &c, WGV_NV12, WGV_BGRX, 16, 16, 0, 0, 1 ), 0 );
    CHECK_INT( c.bottom_up, 1 );
    CHECK_INT( wgv_conv_run( &c, src, &sl, dst, &dl ), 0 );
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++)
        {
            const uint8_t *p = dst + (15 - y) * dl.stride[0] + 4 * x;   /* row y is written bottom-up */
            int k = pattern( x, y );
            if (!near( p[0], colours[k].b ) || !near( p[1], colours[k].g ) || !near( p[2], colours[k].r ) || p[3] != 255)
                bad++;
        }
    CHECK_INT( bad, 0 );
    wgv_conv_free( &c );

    /* RGBA order, top-down */
    CHECK_INT( wgv_conv_init( &c, WGV_NV12, WGV_RGBA, 16, 16, 0, 0, 0 ), 0 );
    CHECK_INT( wgv_conv_run( &c, src, &sl, dst, &dl ), 0 );
    CHECK( near( dst[8 * dl.stride[0] + 0], 255 ) && near( dst[8 * dl.stride[0] + 2], 0 ) );   /* row 8: red */
    CHECK( near( dst[0], 0 ) && dst[3] == 255 );                                                /* row 0: black */
    wgv_conv_free( &c );
}

static void test_full_range_and_709( void )
{
    struct wgv_layout sl, dl;
    struct wgv_conv c;
    uint8_t src[4 * 2 * 3 / 2 + 8], dst[4 * 2 * 4];

    CHECK_INT( wgv_layout_for( WGV_NV12, 4, 2, &sl ), 0 );
    CHECK_INT( wgv_layout_for( WGV_BGRA, 4, 2, &dl ), 0 );
    memset( src, 255, sl.offset[1] );            /* Y 255 */
    memset( src + sl.offset[1], 128, 4 );        /* neutral chroma */
    CHECK_INT( wgv_conv_init( &c, WGV_NV12, WGV_BGRA, 4, 2, 1, 1, 0 ), 0 );
    CHECK_INT( wgv_conv_run( &c, src, &sl, dst, &dl ), 0 );
    CHECK( dst[0] == 255 && dst[1] == 255 && dst[2] == 255 && dst[3] == 255 );
    wgv_conv_free( &c );
}

static void fill_random( uint8_t *p, size_t n, unsigned int seed )
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        seed = seed * 1103515245u + 12345u;
        p[i] = (uint8_t)(seed >> 16);
    }
}

/* in -> mid -> in through the plan; a 4:2:0 picture survives every hop */
static void round_trip( enum wgv_format a, enum wgv_format b, uint32_t w, uint32_t h, uint32_t stride_b )
{
    struct wgv_layout la, lb;
    struct wgv_conv c1, c2;
    uint8_t *src, *mid, *back;
    uint32_t r;

    CHECK_INT( wgv_layout_for( a, w, h, &la ), 0 );
    CHECK_INT( wgv_layout_stride( b, w, h, stride_b, &lb ), 0 );
    src = calloc( 1, la.size );
    mid = calloc( 1, lb.size );
    back = calloc( 1, la.size );
    fill_random( src, la.size, w * 7 + h + a * 31 + b );
    if (a == WGV_YUY2)   /* 4:2:2 input: give each odd row the chroma of the row above (4:2:0 content) */
        for (r = 1; r < h; r += 2)
        {
            uint32_t x;
            for (x = 1; x < 2 * w; x += 2) src[r * la.stride[0] + x] = src[(r - 1) * la.stride[0] + x];
        }
    CHECK_INT( wgv_conv_init( &c1, a, b, w, h, 0, 0, 0 ), 0 );
    CHECK_INT( wgv_conv_init( &c2, b, a, w, h, 0, 0, 0 ), 0 );
    CHECK_INT( wgv_conv_run( &c1, src, &la, mid, &lb ), 0 );
    CHECK_INT( wgv_conv_run( &c2, mid, &lb, back, &la ), 0 );
    /* compare the samples (not the padding at the row ends) */
    for (r = 0; r < la.planes; r++)
    {
        uint32_t bytes = a == WGV_YUY2 ? 2 * w : r == 0 || a == WGV_NV12 ? w : w / 2, y;
        for (y = 0; y < la.rows[r]; y++)
            CHECK_MEM( back + la.offset[r] + y * la.stride[r], src + la.offset[r] + y * la.stride[r], bytes );
    }
    wgv_conv_free( &c1 );
    wgv_conv_free( &c2 );
    free( src );
    free( mid );
    free( back );
}

static void test_round_trips( void )
{
    static const enum wgv_format yuv[4] = { WGV_NV12, WGV_I420, WGV_YV12, WGV_YUY2 };
    unsigned int i, j;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
        {
            round_trip( yuv[i], yuv[j], 18, 6, 0 );
            round_trip( yuv[i], yuv[j], 18, 6, yuv[j] == WGV_YUY2 ? 64 : 32 );   /* a 2D output buffer */
        }
}

/* the video processor's test conversion: I420 -> NV12 at 1920x1080 */
static void test_i420_nv12_1080p( void )
{
    struct wgv_layout sl, dl;
    struct wgv_conv c;
    uint8_t *src, *dst;

    CHECK_INT( wgv_layout_for( WGV_I420, 1920, 1080, &sl ), 0 );
    CHECK_INT( wgv_layout_for( WGV_NV12, 1920, 1080, &dl ), 0 );
    src = malloc( sl.size );
    dst = malloc( dl.size );
    fill_random( src, sl.size, 99 );
    CHECK_INT( wgv_conv_init( &c, WGV_I420, WGV_NV12, 1920, 1080, 1, 0, 0 ), 0 );
    CHECK_INT( wgv_conv_run( &c, src, &sl, dst, &dl ), 0 );
    CHECK_MEM( dst, src, 1920 );                                           /* Y row 0 */
    CHECK_INT( dst[dl.offset[1] + 539 * 1920 + 2 * 959], src[sl.offset[1] + 539 * 960 + 959] );       /* last U */
    CHECK_INT( dst[dl.offset[1] + 539 * 1920 + 2 * 959 + 1], src[sl.offset[2] + 539 * 960 + 959] );   /* last V */
    wgv_conv_free( &c );
    free( src );
    free( dst );
}

static void test_refusals_and_names( void )
{
    struct wgv_conv c;
    struct wgv_layout l;
    enum wgv_format f;

    CHECK_INT( wgv_conv_init( &c, WGV_BGRX, WGV_NV12, 16, 16, 0, 0, 0 ), -1 );   /* RGB input */
    CHECK_INT( wgv_conv_init( &c, WGV_NV12, WGV_BGRX, 15, 16, 0, 0, 0 ), -1 );   /* odd width */
    CHECK_INT( wgv_conv_init( &c, WGV_NV12, WGV_NV12, 16, 0, 0, 0, 0 ), -1 );
    CHECK_INT( wgv_conv_init( &c, WGV_NV12, WGV_NV12, 16, 16, 0, 0, 1 ), 0 );
    CHECK_INT( c.bottom_up, 0 );                                                   /* YUV stays top-down */
    wgv_conv_free( &c );
    CHECK_INT( wgv_format_from_subtype( 0x56555949, 0, &f ), 0 );                 /* IYUV */
    CHECK_INT( f, WGV_I420 );
    CHECK_INT( wgv_format_from_subtype( 22, 0, &f ), -1 );                        /* RGB32 isn't an input */
    CHECK_INT( wgv_format_from_subtype( 32, 1, &f ), 0 );
    CHECK_INT( f, WGV_RGBA );
    CHECK( !strcmp( wgv_subtype_name( 0x3231564e ), "NV12" ) );
    CHECK_INT( wgv_layout_stride( WGV_NV12, 1920, 1080, 2048, &l ), 0 );
    CHECK_INT( l.offset[1], 2048 * 1080 );
    CHECK_INT( l.size, 2048 * 1080 * 3 / 2 );
    CHECK_INT( wgv_layout_stride( WGV_BGRX, 16, 16, 32, &l ), -1 );               /* narrower than a row */
}

int main( void )
{
    test_pattern_bgrx_bottom_up();
    test_full_range_and_709();
    test_round_trips();
    test_i420_nv12_1080p();
    test_refusals_and_names();
    return test_report( "test_vconv" );
}
