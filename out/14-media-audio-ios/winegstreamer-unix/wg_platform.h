// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part A1 / B4: the parser's interface to the platform decoders (VideoToolbox for H.264
// and HEVC, AudioToolbox's AudioConverter for AAC). Plain C types only: the Apple object
// (wg_platform_apple.c) includes no Wine header, and the host tests plug in stub decoders by
// replacing wgp_ops.

#ifndef WG_PLATFORM_H
#define WG_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

enum wgp_codec
{
    WGP_CODEC_H264 = 1,
    WGP_CODEC_HEVC = 2,
};

enum
{
    WGP_OK = 0,
    WGP_E_FAIL = -1,
    WGP_E_NOMEM = -2,
    WGP_E_SESSION_LOST = -3,   /* kVTInvalidSessionErr: the session was dropped, the next packet makes a new one */
};

/* ---------------------------------------------------------------- pure helpers (wg_platform_pure.c) */

#define WGP_MAX_PARAM_SETS 32
struct wgp_param_sets
{
    int nal_length_size;               /* 1, 2 or 4 */
    unsigned int count;
    const uint8_t *data[WGP_MAX_PARAM_SETS];   /* point into the configuration record */
    size_t size[WGP_MAX_PARAM_SETS];
    uint8_t profile, level;            /* avcC only: AVCProfileIndication, AVCLevelIndication */
};
/* avcC (ISO/IEC 14496-15): version 1, NAL length size from byte 4, SPS then PPS lists, at least
 * 2 sets. Returns 0, or -1 with *why set. */
int wgp_parse_avcc( const uint8_t *data, size_t size, struct wgp_param_sets *out, const char **why );
/* hvcC: version 1, NAL length size from byte 21, the VPS/SPS/PPS arrays (NAL types 32-34), at
 * least 3 sets. Returns 0, or -1 with *why set. */
int wgp_parse_hvcc( const uint8_t *data, size_t size, struct wgp_param_sets *out, const char **why );

/* The MPEG-4 ES_Descriptor magic cookie for an AudioSpecificConfig of 2-64 bytes (ISO/IEC
 * 14496-1: ES_Descriptor -> DecoderConfigDescriptor (object type 0x40, stream type audio) ->
 * DecoderSpecificInfo holding the config). Returns its size, or 0 when the config or the
 * output buffer is too small or too big. WGP_ESDS_MAX bytes always suffice. */
#define WGP_ESDS_MAX 96
size_t wgp_esds_cookie( const uint8_t *asc, size_t asc_size, uint8_t *out, size_t out_size );

/* ---------------------------------------------------------------- the decoders */

struct wgp_video;
struct wgp_aac;

/* A decoded picture, mapped for reading: 4:2:0 bi-planar (Y, then interleaved CbCr). */
struct wgp_picture
{
    uint32_t width, height;
    int full_range;
    const uint8_t *y;
    size_t y_stride;
    const uint8_t *uv;
    size_t uv_stride;
};

/* a packet without a PTS */
#define WGP_NO_PTS INT64_MIN

/* Called during video_decode for every finished picture, with the PTS of its packet. The
 * picture is retained for the callee, which releases it with picture_release. */
typedef void (*wgp_picture_cb)( void *context, void *picture, int64_t pts );

struct wgp_ops
{
    /* config: the avcC / hvcC record. Fails with a reason for a malformed or missing one, and
     * for HEVC without hardware decoding. */
    int (*video_create)( enum wgp_codec codec, const uint8_t *config, size_t config_size,
                         uint32_t width, uint32_t height, struct wgp_video **out,
                         char *why, size_t why_size );
    /* One packet (length-prefixed NAL units), synchronously: every picture it finishes is
     * handed to cb before this returns. pts/duration are in the stream's time base
     * (tb_num/tb_den seconds). */
    int (*video_decode)( struct wgp_video *video, const uint8_t *data, size_t size, int64_t pts,
                         int64_t duration, int32_t tb_num, int32_t tb_den, int keyframe,
                         wgp_picture_cb cb, void *context );
    /* after a seek: wait for, drop and invalidate the session */
    void (*video_flush)( struct wgp_video *video );
    void (*video_destroy)( struct wgp_video *video );

    /* 0 and a mapped picture, or -1 when it isn't 4:2:0 bi-planar with two planes */
    int (*picture_lock)( void *picture, struct wgp_picture *out );
    void (*picture_unlock)( void *picture );
    void (*picture_release)( void *picture );

    /* asc: the AudioSpecificConfig (2-64 bytes). Reports the output rate and channels (float32
     * interleaved, WAVE speaker order) and the frames per packet. */
    int (*aac_create)( const uint8_t *asc, size_t asc_size, uint32_t container_rate,
                       uint32_t container_channels, struct wgp_aac **out, uint32_t *rate,
                       uint32_t *channels, uint32_t *frames_per_packet, char *why, size_t why_size );
    /* One packet in, up to max_frames frames out (0 is legal). */
    int (*aac_decode)( struct wgp_aac *aac, const uint8_t *data, size_t size, float *out,
                       uint32_t max_frames, uint32_t *frames );
    void (*aac_flush)( struct wgp_aac *aac );
    void (*aac_destroy)( struct wgp_aac *aac );
};

#define WGP_AAC_MAX_FRAMES 8192

/* The decoders in use: the Apple ones by default (wg_platform_apple.c); tests replace them. */
extern const struct wgp_ops *wgp_ops;
extern const struct wgp_ops wgp_apple_ops;

#endif /* WG_PLATFORM_H */
