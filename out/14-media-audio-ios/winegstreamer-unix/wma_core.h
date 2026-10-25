// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B2: the WMA decoder behind wg_transform_* (libavcodec + libswresample). Plain C
// interface, no Wine types: wg_transform_ios.c translates media types and samples.

#ifndef WMA_CORE_H
#define WMA_CORE_H

#include <stdint.h>

struct wma_core;

struct wma_core_format
{
    uint16_t tag;              /* effective WAVE format tag (rule 1) */
    uint32_t rate;
    uint32_t channels;
    uint32_t block_align;
    uint32_t avg_bytes;        /* nAvgBytesPerSec */
    uint32_t bits_per_sample;  /* wBitsPerSample */
    uint32_t channel_mask;     /* 0 if none */
    const uint8_t *priv;       /* codec-private data (cbSize bytes after the WAVEFORMATEX) */
    uint32_t priv_size;
};

struct wma_core_output
{
    int is_float;              /* 0: 16-bit PCM, 1: 32-bit float */
    uint32_t channels;
    uint32_t channel_mask;     /* 0 if none */
};

enum
{
    WMA_CORE_OK = 0,
    WMA_CORE_E_UNSUPPORTED = -1,
    WMA_CORE_E_NOMEM = -2,
    WMA_CORE_E_OPEN = -3,
};

int wma_core_create( const struct wma_core_format *in, const struct wma_core_output *out,
                     struct wma_core **ret );
void wma_core_destroy( struct wma_core *core );
int wma_core_set_output( struct wma_core *core, const struct wma_core_output *out );

/* bytes of decoded PCM waiting to be read */
uint32_t wma_core_pending( const struct wma_core *core );
/* bytes of one output frame */
uint32_t wma_core_frame_bytes( const struct wma_core *core );

/* Stage input bytes and decode every whole packet (rules 3-10). */
int wma_core_push( struct wma_core *core, const uint8_t *data, uint32_t size,
                   int has_pts, int64_t pts, int discontinuity );

struct wma_core_read_info
{
    uint32_t bytes, frames;
    int has_pts;
    int64_t pts;               /* 100 ns */
    int64_t duration;          /* 100 ns */
    int discontinuity;
    int incomplete;            /* PCM remains after this read */
};
/* Copy as much PCM as fits in `max` bytes, whole frames only (rule 11). */
void wma_core_read( struct wma_core *core, uint8_t *dst, uint32_t max, struct wma_core_read_info *info );

void wma_core_drain( struct wma_core *core );   /* rule 12 */
void wma_core_flush( struct wma_core *core );   /* rule 12 */

/* "[wma] " lines: at most 200 per app, then one "suppressed" line (PG_DIAG=1 lifts it) */
void wma_log( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));

/* for tests and logs */
struct wma_core_stats
{
    uint64_t packets, failed_packets, silence_frames, tail_bytes;
    int accepted, muted, concealing, search_ran;
    int64_t bit_rate;          /* the bit rate open now */
    unsigned int flags;        /* the flags word open now */
};
void wma_core_get_stats( const struct wma_core *core, struct wma_core_stats *stats );

#endif /* WMA_CORE_H */
