// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B3: the pure parts of the media parser (no FFmpeg, no Wine, no Apple), shared with
// the host tests: frame layouts and conversions (rule 9), S24 packing (rule 8), the reorder rule
// (rule 9), trimming (rules 8, 10), the queue limits (rule 7), the AAC PTS queue (rule 8) and the
// channel-mask rule (rule 5).

#ifndef WG_PARSER_PURE_H
#define WG_PARSER_PURE_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- video (rule 9) */

enum wgv_format
{
    WGV_NV12,
    WGV_I420,
    WGV_YV12,
    WGV_YUY2,
    WGV_BGRX,    /* "rgb32": B G R x, x = 0xff */
    WGV_BGRA,    /* "argb32": B G R A */
    WGV_RGBA,    /* "abgr32": R G B A */
};

static inline uint32_t wgv_align4( uint32_t v ) { return (v + 3) & ~3u; }

/* the frame layout GStreamer uses (what the PE side assumes) */
struct wgv_layout
{
    uint32_t stride[3];      /* bytes per row of each plane */
    uint32_t offset[3];      /* plane offsets from the frame start */
    uint32_t rows[3];        /* rows of each plane */
    uint32_t planes;
    uint32_t size;           /* the whole frame */
};
/* 0 on success, -1 for a bad size */
int wgv_layout_for( enum wgv_format format, uint32_t width, uint32_t height, struct wgv_layout *layout );

/* A decoded 4:2:0 bi-planar picture (Y plane, interleaved CbCr plane). */
struct wgv_source
{
    uint32_t width, height;
    const uint8_t *y;
    size_t y_stride;
    const uint8_t *uv;
    size_t uv_stride;
    int full_range;          /* 0: video range (16-235 / 16-240) */
};

/* Writes one frame of `format` at width x height (the stream's native size) into dst
 * (layout.size bytes). A smaller picture is padded with black (Y 16 / chroma 128, or opaque
 * black), a larger one cropped. bt709: Kr/Kb 0.2126/0.0722, else 0.299/0.114. bottom_up writes
 * the rows of every plane in reverse. */
void wgv_convert( const struct wgv_source *src, enum wgv_format format, uint32_t width, uint32_t height,
                  int bt709, int bottom_up, uint8_t *dst );

/* ---------------------------------------------------------------- audio (rule 8) */

/* S24 is produced as S32 and packed to 3 bytes: the top three bytes of each little-endian
 * sample. In place is allowed (dst == src bytes). */
void wg_pack_s24( const int32_t *src, size_t samples, uint8_t *dst );

/* Channel masks (rule 5): the layout's mask when it is a native-order layout with one bit per
 * channel within the 18 WinMM speaker bits, else the default mask for the count. */
uint32_t wg_channel_mask( int native_order, uint64_t mask, int channels, uint32_t default_mask );

/* ---------------------------------------------------------------- the reorder rule (rule 9) */

#define WG_REORDER_MAX 16
struct wg_reorder_item
{
    int64_t pts;             /* stream time base */
    int64_t duration;        /* stream time base, 0 if unknown */
    void *picture;
};
struct wg_reorder
{
    struct wg_reorder_item items[WG_REORDER_MAX + 1];
    unsigned int count;
};
/* Inserts sorted by PTS (a picture with an equal PTS goes after the others). Returns 0, or -1
 * when full (the caller releases the first first). */
int wg_reorder_insert( struct wg_reorder *r, const struct wg_reorder_item *item );
/* Whether the first picture may be released: the last decoded packet's DTS has reached it (no
 * later packet can present before it), more than 16 are held, or draining. */
int wg_reorder_ready( const struct wg_reorder *r, int has_dts, int64_t last_dts, int draining );
/* Removes and returns the first picture. */
struct wg_reorder_item wg_reorder_pop( struct wg_reorder *r );

/* ---------------------------------------------------------------- trimming (rules 8, 10) */

/* Frames to drop from the start of a buffer at pts (100 ns) of `frames` frames at `rate` so that
 * it starts at `target`: ceil((target - pts) * rate / 10^7), clamped to [0, frames]. */
uint32_t wg_trim_start( int64_t pts, uint32_t frames, uint32_t rate, int64_t target );
/* Frames to keep of a buffer at pts when the stream stops at `stop`: frames before stop,
 * clamped to [0, frames]. */
uint32_t wg_trim_stop( int64_t pts, uint32_t frames, uint32_t rate, int64_t stop );

/* ---------------------------------------------------------------- queues (rule 7) */

#define WG_QUEUE_MAX_PACKETS 16384
#define WG_QUEUE_MAX_BYTES (64u * 1024 * 1024)
/* 1 when a packet of `size` bytes may join a queue holding `count` packets of `bytes` bytes */
int wg_queue_admit( uint32_t count, uint64_t bytes, uint32_t size );

/* ---------------------------------------------------------------- the AAC PTS queue (rule 8) */

#define WG_PTS_FIFO_MAX 8
struct wg_pts_fifo
{
    int64_t pts[WG_PTS_FIFO_MAX];
    unsigned int head, count;
};
/* Keeps the PTS of up to 8 unanswered packets in order (the oldest is forgotten when full). */
void wg_pts_fifo_push( struct wg_pts_fifo *f, int64_t pts );
/* The oldest unanswered PTS; 0 when empty. */
int wg_pts_fifo_pop( struct wg_pts_fifo *f, int64_t *pts );
void wg_pts_fifo_clear( struct wg_pts_fifo *f );

#endif /* WG_PARSER_PURE_H */
