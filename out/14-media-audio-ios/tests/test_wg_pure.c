// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F: the pure parts of winegstreamer's unix side: magic-checked handles (B1), frame
// layouts and every conversion from 4:2:0 bi-planar (B3 rule 9: both matrices and ranges,
// bottom-up, padding and cropping), S24 packing, the reorder rule, trims, queue limits, the AAC
// PTS queue, channel masks, the log dedupe (B3 rule 12), avcC/hvcC parsing and the
// ES_Descriptor cookie bytes (B4).

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "media_log.h"
#include "wg_handles.h"
#include "wg_parser_pure.h"
#include "wg_platform.h"

/* ---------------------------------------------------------------- handles */

static void test_handles( void )
{
    int a, b;
    uint64_t h1, h2, h3;

    h1 = wgh_alloc( WG_HANDLE_TRANSFORM, &a );
    h2 = wgh_alloc( WG_HANDLE_PARSER, &b );
    CHECK( h1 && h2 && h1 != h2 );
    CHECK( wgh_get( h1, WG_HANDLE_TRANSFORM ) == &a );
    CHECK( wgh_get( h1, WG_HANDLE_STREAM ) == NULL );      /* the wrong type */
    CHECK( wgh_get( h2, WG_HANDLE_STREAM ) == NULL );      /* a parser passed as a stream */
    CHECK( wgh_get( 0, WG_HANDLE_TRANSFORM ) == NULL );
    CHECK( wgh_get( 0x1234567812345678ull, WG_HANDLE_TRANSFORM ) == NULL );
    CHECK( wgh_get( h1 ^ 1, WG_HANDLE_TRANSFORM ) == NULL );
    CHECK( wgh_free( h1, WG_HANDLE_TRANSFORM ) == &a );
    CHECK( wgh_get( h1, WG_HANDLE_TRANSFORM ) == NULL );   /* stale */
    CHECK( wgh_free( h1, WG_HANDLE_TRANSFORM ) == NULL );
    h3 = wgh_alloc( WG_HANDLE_TRANSFORM, &b );             /* the slot is reused */
    CHECK( h3 && h3 != h1 );
    CHECK( wgh_get( h1, WG_HANDLE_TRANSFORM ) == NULL );   /* the old handle stays stale */
    CHECK( wgh_get( h3, WG_HANDLE_TRANSFORM ) == &b );
    wgh_free( h2, WG_HANDLE_PARSER );
    wgh_free( h3, WG_HANDLE_TRANSFORM );
}

/* ---------------------------------------------------------------- video */

#define SW 6
#define SH 6
static uint8_t src_y[SH][8], src_uv[(SH + 1) / 2][8];

static void make_source( struct wgv_source *s, uint32_t w, uint32_t h, int full_range )
{
    uint32_t x, y;

    for (y = 0; y < SH; y++) for (x = 0; x < 8; x++) src_y[y][x] = (uint8_t)(20 + 10 * y + x);
    for (y = 0; y < (SH + 1) / 2; y++)
        for (x = 0; x < 4; x++)
        {
            src_uv[y][2 * x] = (uint8_t)(100 + 10 * y + x);       /* Cb */
            src_uv[y][2 * x + 1] = (uint8_t)(150 + 10 * y + x);   /* Cr */
        }
    s->width = w;
    s->height = h;
    s->y = &src_y[0][0];
    s->y_stride = 8;
    s->uv = &src_uv[0][0];
    s->uv_stride = 8;
    s->full_range = full_range;
}

static void expect_rgb( int y, int cb, int cr, int bt709, int full, int rgb[3] )
{
    double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1 - kr - kb;
    double ys = full ? y : (y - 16) * 255.0 / 219.0;
    double u = full ? cb - 128 : (cb - 128) * 255.0 / 224.0, v = full ? cr - 128 : (cr - 128) * 255.0 / 224.0;
    double r = ys + 2 * (1 - kr) * v, b = ys + 2 * (1 - kb) * u;
    double g = ys - 2 * kb * (1 - kb) / kg * u - 2 * kr * (1 - kr) / kg * v;
    double c[3] = { r, g, b };
    int i;

    for (i = 0; i < 3; i++) rgb[i] = c[i] < 0 ? 0 : c[i] > 255 ? 255 : (int)(c[i] + 0.5);
}

static void test_layouts( void )
{
    struct wgv_layout l;

    CHECK_INT( wgv_layout_for( WGV_NV12, 5, 3, &l ), 0 );
    CHECK_INT( l.stride[0], 8 );
    CHECK_INT( l.offset[1], 24 );
    CHECK_INT( l.rows[1], 2 );
    CHECK_INT( l.size, 24 + 16 );
    CHECK_INT( wgv_layout_for( WGV_I420, 5, 3, &l ), 0 );
    CHECK_INT( l.stride[1], 4 );                 /* align4((5+1)/2) */
    CHECK_INT( l.offset[1], 24 );                /* U first */
    CHECK_INT( l.offset[2], 32 );
    CHECK_INT( l.size, 40 );
    CHECK_INT( wgv_layout_for( WGV_YV12, 5, 3, &l ), 0 );
    CHECK_INT( l.offset[2], 24 );                /* V first */
    CHECK_INT( l.offset[1], 32 );
    CHECK_INT( wgv_layout_for( WGV_YUY2, 5, 3, &l ), 0 );
    CHECK_INT( l.stride[0], 12 );                /* align4(10) */
    CHECK_INT( l.size, 36 );
    CHECK_INT( wgv_layout_for( WGV_BGRA, 5, 3, &l ), 0 );
    CHECK_INT( l.stride[0], 20 );
    CHECK_INT( l.size, 60 );
    CHECK_INT( wgv_layout_for( WGV_NV12, 0, 3, &l ), -1 );
    CHECK_INT( wgv_layout_for( WGV_NV12, 8193, 3, &l ), -1 );
}

static void test_conversions( void )
{
    struct wgv_source s;
    struct wgv_layout l;
    uint8_t out[512];
    int rgb[3], m, full;
    uint32_t x, y;

    /* NV12 -> NV12, same size */
    make_source( &s, 4, 4, 0 );
    wgv_layout_for( WGV_NV12, 4, 4, &l );
    wgv_convert( &s, WGV_NV12, 4, 4, 1, 0, out );
    for (y = 0; y < 4; y++) CHECK_MEM( out + y * 4, src_y[y], 4 );
    CHECK_MEM( out + 16, src_uv[0], 4 );
    CHECK_MEM( out + 20, src_uv[1], 4 );

    /* bottom-up: rows of every plane in reverse */
    wgv_convert( &s, WGV_NV12, 4, 4, 1, 1, out );
    CHECK_MEM( out + 0, src_y[3], 4 );
    CHECK_MEM( out + 12, src_y[0], 4 );
    CHECK_MEM( out + 16, src_uv[1], 4 );
    CHECK_MEM( out + 20, src_uv[0], 4 );

    /* padding: a 2x2 picture in a 4x4 frame */
    make_source( &s, 2, 2, 0 );
    wgv_convert( &s, WGV_NV12, 4, 4, 1, 0, out );
    CHECK_INT( out[0], src_y[0][0] );
    CHECK_INT( out[1], src_y[0][1] );
    CHECK_INT( out[2], 16 );
    CHECK_INT( out[4 * 3], 16 );
    CHECK_INT( out[16], src_uv[0][0] );
    CHECK_INT( out[16 + 2], 128 );
    CHECK_INT( out[20], 128 );

    /* cropping: a 6x6 picture in a 4x4 frame */
    make_source( &s, 6, 6, 0 );
    wgv_convert( &s, WGV_NV12, 4, 4, 1, 0, out );
    CHECK_MEM( out + 12, src_y[3], 4 );
    CHECK_MEM( out + 20, src_uv[1], 4 );

    /* I420 and YV12: the chroma de-interleaved */
    make_source( &s, 4, 4, 0 );
    wgv_layout_for( WGV_I420, 4, 4, &l );
    wgv_convert( &s, WGV_I420, 4, 4, 1, 0, out );
    CHECK_INT( out[l.offset[1]], src_uv[0][0] );
    CHECK_INT( out[l.offset[1] + 1], src_uv[0][2] );
    CHECK_INT( out[l.offset[2]], src_uv[0][1] );
    CHECK_INT( out[l.offset[2] + l.stride[2] + 1], src_uv[1][3] );
    CHECK_INT( l.offset[1], 16 );
    wgv_layout_for( WGV_YV12, 4, 4, &l );
    wgv_convert( &s, WGV_YV12, 4, 4, 1, 0, out );
    CHECK_INT( l.offset[2], 16 );
    CHECK_INT( out[16], src_uv[0][1] );          /* V first */
    CHECK_INT( out[l.offset[1]], src_uv[0][0] );

    /* YUY2: Y0 U Y1 V, the last luma repeated for an odd width */
    wgv_convert( &s, WGV_YUY2, 4, 4, 1, 0, out );
    CHECK_INT( out[0], src_y[0][0] );
    CHECK_INT( out[1], src_uv[0][0] );
    CHECK_INT( out[2], src_y[0][1] );
    CHECK_INT( out[3], src_uv[0][1] );
    CHECK_INT( out[8 * 3 + 4], src_y[3][2] );
    CHECK_INT( out[8 * 3 + 5], src_uv[1][2] );
    make_source( &s, 3, 2, 0 );
    wgv_convert( &s, WGV_YUY2, 3, 2, 1, 0, out );
    CHECK_INT( out[4], src_y[0][2] );
    CHECK_INT( out[6], src_y[0][2] );

    /* RGB, both matrices and both ranges */
    for (m = 0; m < 2; m++)
        for (full = 0; full < 2; full++)
        {
            make_source( &s, 4, 4, full );
            wgv_convert( &s, WGV_BGRX, 4, 4, m, 0, out );
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                {
                    const uint8_t *p = out + 16 * y + 4 * x;
                    expect_rgb( src_y[y][x], src_uv[y / 2][2 * (x / 2)], src_uv[y / 2][2 * (x / 2) + 1], m, full, rgb );
                    CHECK_NEAR( p[2], rgb[0], 1 );
                    CHECK_NEAR( p[1], rgb[1], 1 );
                    CHECK_NEAR( p[0], rgb[2], 1 );
                    CHECK_INT( p[3], 0xff );
                }
            wgv_convert( &s, WGV_RGBA, 4, 4, m, 0, out );
            expect_rgb( src_y[1][1], src_uv[0][0], src_uv[0][1], m, full, rgb );
            CHECK_NEAR( out[16 + 4], rgb[0], 1 );    /* R first */
            CHECK_NEAR( out[16 + 6], rgb[2], 1 );
            CHECK_INT( out[16 + 7], 0xff );
            wgv_convert( &s, WGV_BGRA, 4, 4, m, 1, out );   /* bottom-up */
            expect_rgb( src_y[0][0], src_uv[0][0], src_uv[0][1], m, full, rgb );
            CHECK_NEAR( out[16 * 3 + 2], rgb[0], 1 );
        }

    /* video-range black and white, opaque black padding */
    memset( src_y, 16, sizeof(src_y) );
    memset( src_uv, 128, sizeof(src_uv) );
    s.width = 2; s.height = 2; s.full_range = 0;
    wgv_convert( &s, WGV_BGRX, 4, 4, 1, 0, out );
    CHECK_INT( out[0] + out[1] + out[2], 0 );
    CHECK_INT( out[3 * 16 + 3 * 4 + 0] + out[3 * 16 + 3 * 4 + 1] + out[3 * 16 + 3 * 4 + 2], 0 );
    CHECK_INT( out[3 * 16 + 3 * 4 + 3], 0xff );
    memset( src_y, 235, sizeof(src_y) );
    wgv_convert( &s, WGV_RGBA, 2, 2, 0, 0, out );
    CHECK_INT( out[0], 255 );
    CHECK_INT( out[1], 255 );
    CHECK_INT( out[2], 255 );
}

/* ---------------------------------------------------------------- audio, reorder, trims, queues */

static void test_audio_helpers( void )
{
    int32_t in[3] = { 0x12345600, -256, 0x7fffff00 };
    uint8_t out[12];
    static const uint8_t expect[9] = { 0x56, 0x34, 0x12, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f };

    wg_pack_s24( in, 3, out );
    CHECK_MEM( out, expect, 9 );
    wg_pack_s24( in, 3, (uint8_t *)in );   /* in place */
    CHECK_MEM( in, expect, 9 );

    CHECK_INT( wg_channel_mask( 1, 0x3, 2, 0x99 ), 0x3 );
    CHECK_INT( wg_channel_mask( 1, 0x3f, 5, 0x99 ), 0x99 );            /* not one bit per channel */
    CHECK_INT( wg_channel_mask( 1, (1u << 20) | 0x3, 3, 0x99 ), 0x99 ); /* outside the 18 bits */
    CHECK_INT( wg_channel_mask( 0, 0x3, 2, 0x99 ), 0x99 );             /* not a native order */

    CHECK_INT( wg_trim_start( 0, 48000, 48000, 5000000 ), 24000 );
    CHECK_INT( wg_trim_start( 5000000, 480, 48000, 5000000 ), 0 );
    CHECK_INT( wg_trim_start( 0, 480, 48000, 5000000 ), 480 );
    CHECK_INT( wg_trim_start( 0, 480, 48000, 1 ), 1 );                 /* to the sample: rounded up */
    CHECK_INT( wg_trim_stop( 0, 48000, 48000, 7500000 ), 36000 );
    CHECK_INT( wg_trim_stop( 0, 480, 48000, 7500000 ), 480 );
    CHECK_INT( wg_trim_stop( 7500000, 480, 48000, 7500000 ), 0 );

    CHECK_INT( wg_queue_admit( WG_QUEUE_MAX_PACKETS - 1, 0, 10 ), 1 );
    CHECK_INT( wg_queue_admit( WG_QUEUE_MAX_PACKETS, 0, 10 ), 0 );
    CHECK_INT( wg_queue_admit( 0, WG_QUEUE_MAX_BYTES - 10, 10 ), 1 );
    CHECK_INT( wg_queue_admit( 0, WG_QUEUE_MAX_BYTES - 9, 10 ), 0 );

    {
        struct wg_pts_fifo f;
        int64_t pts;
        int i;

        memset( &f, 0, sizeof(f) );
        CHECK_INT( wg_pts_fifo_pop( &f, &pts ), 0 );
        for (i = 1; i <= 9; i++) wg_pts_fifo_push( &f, i );   /* 9 pushes: the oldest is forgotten */
        for (i = 2; i <= 9; i++)
        {
            CHECK_INT( wg_pts_fifo_pop( &f, &pts ), 1 );
            CHECK_INT( pts, i );
        }
        CHECK_INT( wg_pts_fifo_pop( &f, &pts ), 0 );
    }
}

static void test_reorder( void )
{
    struct wg_reorder r;
    struct wg_reorder_item it = { 0 }, first;
    int i;

    memset( &r, 0, sizeof(r) );
    CHECK_INT( wg_reorder_ready( &r, 1, 100, 1 ), 0 );   /* nothing held */
    it.pts = 3; wg_reorder_insert( &r, &it );
    it.pts = 1; wg_reorder_insert( &r, &it );
    it.pts = 2; wg_reorder_insert( &r, &it );
    CHECK_INT( r.items[0].pts, 1 );
    CHECK_INT( r.items[1].pts, 2 );
    CHECK_INT( r.items[2].pts, 3 );
    CHECK_INT( wg_reorder_ready( &r, 0, 0, 0 ), 0 );     /* no DTS yet */
    CHECK_INT( wg_reorder_ready( &r, 1, 0, 0 ), 0 );     /* a later packet could still present before 1 */
    CHECK_INT( wg_reorder_ready( &r, 1, 1, 0 ), 1 );     /* the DTS reached it */
    CHECK_INT( wg_reorder_ready( &r, 0, 0, 1 ), 1 );     /* draining */
    first = wg_reorder_pop( &r );
    CHECK_INT( first.pts, 1 );
    CHECK_INT( r.count, 2 );
    for (i = 0; i < 15; i++)
    {
        it.pts = 100 + i;
        wg_reorder_insert( &r, &it );
    }
    CHECK_INT( r.count, 17 );
    CHECK_INT( wg_reorder_ready( &r, 0, 0, 0 ), 1 );     /* more than 16 held */
    CHECK_INT( wg_reorder_insert( &r, &it ), -1 );       /* full */
}

static void test_dedupe( void )
{
    struct iom_dedupe t;
    uint32_t count;
    int i, printed = 0;
    char line[32];

    memset( &t, 0, sizeof(t) );
    for (i = 1; i <= 9; i++)
    {
        int p = iom_dedupe_check( &t, "refused mp3", &count );
        CHECK_INT( count, i );
        CHECK_INT( p, i == 1 || i == 2 || i == 4 || i == 8 );
        printed += p;
    }
    CHECK_INT( printed, 4 );
    for (i = 1; i < IOM_DEDUPE_MAX; i++)
    {
        snprintf( line, sizeof(line), "line %d", i );
        CHECK_INT( iom_dedupe_check( &t, line, &count ), 1 );
    }
    CHECK_INT( iom_dedupe_check( &t, "one too many", &count ), 0 );   /* 64 distinct lines per app */
    CHECK_INT( iom_dedupe_check( &t, "refused mp3", &count ), 0 );    /* 10th */
    CHECK_INT( count, 10 );
}

/* ---------------------------------------------------------------- B4 */

static void test_param_sets( void )
{
    static const uint8_t avcc[] = { 0x01, 0x64, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x04, 0x67, 0x64, 0x00, 0x1f,
                                    0x01, 0x00, 0x03, 0x68, 0xee, 0x3c };
    static const uint8_t avcc_sps_only[] = { 0x01, 0x64, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x01, 0x67, 0x00 };
    uint8_t hvcc[23 + 4 * 7], bad[sizeof(avcc)];
    struct wgp_param_sets sets;
    const char *why;
    size_t n = 0;
    int i;

    CHECK_INT( wgp_parse_avcc( avcc, sizeof(avcc), &sets, &why ), 0 );
    CHECK_INT( sets.count, 2 );
    CHECK_INT( sets.nal_length_size, 4 );
    CHECK_INT( sets.profile, 100 );
    CHECK_INT( sets.level, 31 );
    CHECK_INT( sets.size[0], 4 );
    CHECK_INT( sets.data[0][0], 0x67 );
    CHECK_INT( sets.size[1], 3 );
    CHECK_INT( sets.data[1][0], 0x68 );
    CHECK_INT( wgp_parse_avcc( avcc, sizeof(avcc) - 1, &sets, &why ), -1 );   /* truncated */
    CHECK( why != NULL );
    memcpy( bad, avcc, sizeof(avcc) );
    bad[0] = 2;
    CHECK_INT( wgp_parse_avcc( bad, sizeof(bad), &sets, &why ), -1 );
    memcpy( bad, avcc, sizeof(avcc) );
    bad[4] = 0xfd;   /* NAL length size 2 */
    CHECK_INT( wgp_parse_avcc( bad, sizeof(bad), &sets, &why ), 0 );
    CHECK_INT( sets.nal_length_size, 2 );
    CHECK_INT( wgp_parse_avcc( avcc_sps_only, sizeof(avcc_sps_only), &sets, &why ), -1 );
    CHECK_INT( wgp_parse_avcc( NULL, 0, &sets, &why ), -1 );

    memset( hvcc, 0, sizeof(hvcc) );
    hvcc[0] = 1;
    hvcc[21] = 0x0f;   /* NAL length size 4 */
    hvcc[22] = 4;
    n = 23;
    for (i = 0; i < 4; i++)
    {
        static const uint8_t types[4] = { 32, 33, 34, 39 };   /* VPS, SPS, PPS, SEI */
        hvcc[n++] = 0x80 | types[i];
        hvcc[n++] = 0;
        hvcc[n++] = 1;
        hvcc[n++] = 0;
        hvcc[n++] = 2;
        hvcc[n++] = (uint8_t)(types[i] << 1);
        hvcc[n++] = 1;
    }
    CHECK_INT( wgp_parse_hvcc( hvcc, n, &sets, &why ), 0 );
    CHECK_INT( sets.count, 3 );   /* the SEI is not kept */
    CHECK_INT( sets.nal_length_size, 4 );
    CHECK_INT( sets.data[0][0], 32 << 1 );
    CHECK_INT( sets.data[2][0], 34 << 1 );
    hvcc[22] = 2;   /* VPS and SPS only */
    CHECK_INT( wgp_parse_hvcc( hvcc, 23 + 14, &sets, &why ), -1 );
    CHECK_INT( wgp_parse_hvcc( hvcc, 20, &sets, &why ), -1 );
}

static void test_esds( void )
{
    static const uint8_t asc[2] = { 0x12, 0x10 };
    static const uint8_t expect[24] =
    {
        0x03, 0x16, 0x00, 0x00, 0x00,
        0x04, 0x11, 0x40, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x05, 0x02, 0x12, 0x10,
    };
    uint8_t out[WGP_ESDS_MAX], big[64];

    CHECK_INT( wgp_esds_cookie( asc, sizeof(asc), out, sizeof(out) ), 24 );
    CHECK_MEM( out, expect, 24 );
    CHECK_INT( wgp_esds_cookie( asc, 1, out, sizeof(out) ), 0 );      /* 2-64 bytes */
    memset( big, 0x11, sizeof(big) );
    /* ES payload 3 + (2 + DCD payload 13 + (2 + 64)) */
    CHECK_INT( wgp_esds_cookie( big, 64, out, sizeof(out) ), 2 + 3 + 2 + 13 + 2 + 64 );
    CHECK_INT( out[1], 3 + 2 + 13 + 2 + 64 );
    CHECK_INT( out[6], 13 + 2 + 64 );
    CHECK_INT( wgp_esds_cookie( big, 65, out, sizeof(out) ), 0 );
    CHECK_INT( wgp_esds_cookie( asc, sizeof(asc), out, 23 ), 0 );    /* output too small */
}

int main( void )
{
    test_handles();
    test_layouts();
    test_conversions();
    test_audio_helpers();
    test_reorder();
    test_dedupe();
    test_param_sets();
    test_esds();
    return test_report( "test_wg_pure" );
}
