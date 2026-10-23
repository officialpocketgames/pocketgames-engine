// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F: stub platform decoders for the parser's host tests (in place of
// wg_platform_apple.c). The video stub turns each packet into a 4:2:0 bi-planar picture whose
// luma is the packet's first byte and whose chroma is 128; the AAC stub answers each packet one
// call late (as AudioConverter may) with 1024 frames whose value is the packet's first byte / 100.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wg_platform.h"
#include "test_stubs_platform.h"

int stub_pictures_alive, stub_pictures_made, stub_video_flushes, stub_aac_flushes;
int stub_aac_channels = 2;

struct wgp_video
{
    uint32_t width, height;
};

struct stub_picture
{
    uint32_t width, height;
    uint8_t *y, *uv;
};

static int video_create( enum wgp_codec codec, const uint8_t *config, size_t config_size, uint32_t width,
                         uint32_t height, struct wgp_video **out, char *why, size_t why_size )
{
    struct wgp_param_sets sets;
    const char *reason;
    int ret = codec == WGP_CODEC_H264 ? wgp_parse_avcc( config, config_size, &sets, &reason )
                                      : wgp_parse_hvcc( config, config_size, &sets, &reason );

    if (ret)
    {
        snprintf( why, why_size, "%s", reason );
        return WGP_E_FAIL;
    }
    *out = calloc( 1, sizeof(**out) );
    (*out)->width = width;
    (*out)->height = height;
    return WGP_OK;
}

static int video_decode( struct wgp_video *v, const uint8_t *data, size_t size, int64_t pts, int64_t duration,
                         int32_t tb_num, int32_t tb_den, int keyframe, wgp_picture_cb cb, void *context )
{
    struct stub_picture *pic;

    if (!size) return WGP_OK;
    pic = calloc( 1, sizeof(*pic) );
    pic->width = v->width;
    pic->height = v->height;
    pic->y = malloc( (size_t)v->width * v->height );
    pic->uv = malloc( (size_t)v->width * ((v->height + 1) / 2) );
    memset( pic->y, data[0], (size_t)v->width * v->height );
    memset( pic->uv, 128, (size_t)v->width * ((v->height + 1) / 2) );
    stub_pictures_alive++;
    stub_pictures_made++;
    cb( context, pic, pts );   /* synchronously, in decode order */
    return WGP_OK;
}

static void video_flush( struct wgp_video *v )
{
    stub_video_flushes++;
}

static void video_destroy( struct wgp_video *v )
{
    free( v );
}

static int picture_lock( void *picture, struct wgp_picture *out )
{
    struct stub_picture *pic = picture;

    out->width = pic->width;
    out->height = pic->height;
    out->full_range = 0;
    out->y = pic->y;
    out->y_stride = pic->width;
    out->uv = pic->uv;
    out->uv_stride = pic->width;
    return WGP_OK;
}

static void picture_unlock( void *picture )
{
}

static void picture_release( void *picture )
{
    struct stub_picture *pic = picture;

    free( pic->y );
    free( pic->uv );
    free( pic );
    stub_pictures_alive--;
}

struct wgp_aac
{
    int have_previous;
    uint8_t previous;
    uint32_t channels;
};

static int aac_create( const uint8_t *asc, size_t asc_size, uint32_t container_rate, uint32_t container_channels,
                       struct wgp_aac **out, uint32_t *rate, uint32_t *channels, uint32_t *frames_per_packet,
                       char *why, size_t why_size )
{
    uint8_t cookie[WGP_ESDS_MAX];

    if (!wgp_esds_cookie( asc, asc_size, cookie, sizeof(cookie) ))
    {
        snprintf( why, why_size, "bad AudioSpecificConfig" );
        return WGP_E_FAIL;
    }
    *out = calloc( 1, sizeof(**out) );
    (*out)->channels = stub_aac_channels;
    *rate = container_rate;
    *channels = stub_aac_channels;
    *frames_per_packet = 1024;
    return WGP_OK;
}

static int aac_decode( struct wgp_aac *a, const uint8_t *data, size_t size, float *out, uint32_t max_frames,
                       uint32_t *frames )
{
    uint32_t i;

    *frames = 0;
    if (!size) return WGP_OK;
    if (a->have_previous)
    {
        for (i = 0; i < 1024 * a->channels; i++) out[i] = a->previous / 100.0f;
        *frames = 1024;
    }
    a->previous = data[0];
    a->have_previous = 1;
    return WGP_OK;
}

static void aac_flush( struct wgp_aac *a )
{
    a->have_previous = 0;
    stub_aac_flushes++;
}

static void aac_destroy( struct wgp_aac *a )
{
    free( a );
}

const struct wgp_ops stub_platform_ops =
{
    video_create, video_decode, video_flush, video_destroy,
    picture_lock, picture_unlock, picture_release,
    aac_create, aac_decode, aac_flush, aac_destroy,
};

const struct wgp_ops *wgp_ops = &stub_platform_ops;
