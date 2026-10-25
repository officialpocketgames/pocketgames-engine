// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B2: the WMA decoder (rules 1-14) on libavcodec and libswresample.
//
// Input arrives in arbitrary pieces; the decoder wants exactly one block-align-sized packet per
// call, so bytes are staged and whole packets fed. Each packet is "attempted" (decoded and
// converted into a scratch buffer), judged (rule 8), and its output committed to the PCM queue
// the caller reads from; a packet that produced nothing commits silence of its length (rule 6).
// For WMA v1/v2 a bad packet may start the parameter search of rule 7, which retries the same
// packet with other bit rates and flags words.

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "media_log.h"
#include "wg_avlog.h"
#include "wma_core.h"
#include "wma_params.h"

#define WMA_LOG_LINES_PER_APP 200
#define WMA_FAIL_LINES_PER_TRANSFORM 8
#define WMA_DUMP_PACKETS 64
#define WMA_ACCEPT_RUN 3
#define WMA_SANE_PEAK 3.0f
#define WMA_SUPERFRAME_STATS 16

static struct iom_cap wma_cap;
static volatile int32_t transform_counter;
static volatile int32_t dump_counter;

void wma_log( const char *fmt, ... )
{
    va_list args;

    if (!iom_diag() && !iom_cap_take( &wma_cap, WMA_LOG_LINES_PER_APP, "[wma] " )) return;
    va_start( args, fmt );
    iom_logv( "[wma] ", fmt, args );
    va_end( args );
}

struct byte_buf
{
    uint8_t *data;
    size_t len, cap;
};

static int buf_reserve( struct byte_buf *b, size_t extra )
{
    size_t want = b->len + extra, cap;
    uint8_t *p;

    if (want <= b->cap) return 1;
    cap = b->cap ? b->cap : 4096;
    while (cap < want) cap *= 2;
    if (!(p = realloc( b->data, cap ))) return 0;
    b->data = p;
    b->cap = cap;
    return 1;
}

static void buf_free( struct byte_buf *b )
{
    free( b->data );
    memset( b, 0, sizeof(*b) );
}

struct attempt
{
    int send_failed;
    int rejected;
    int err;
    int64_t samples;       /* samples per channel kept (not rejected) */
};

struct wma_core
{
    unsigned int id;
    struct wma_core_format in;
    uint8_t *priv;               /* our copy */
    int version;                 /* 1 or 2 for WMA v1/v2, 0 for the rest of the family */
    enum AVCodecID codec_id;
    const AVCodec *codec;
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    int frame_len_bits;

    /* the parameters open now, and the inputs of the candidate list (rule 7) */
    int64_t open_rate;
    unsigned int open_flags;
    int64_t first_rate, alt_rate, header_rate;
    unsigned int given_flags;
    size_t patched_size;         /* private data size the v1/v2 decoder gets */

    /* output */
    struct wma_core_output out;
    uint32_t out_frame_bytes;
    AVChannelLayout out_layout;
    struct SwrContext *swr;
    int swr_in_format;
    AVChannelLayout swr_in_layout;

    struct byte_buf stage;       /* staged input */
    size_t stage_pos;
    struct byte_buf pcm;         /* converted PCM waiting to be read */
    size_t pcm_pos;
    struct byte_buf scratch;     /* output of the packet being attempted */

    /* timing (rule 10, 11) */
    int has_pts;
    int64_t pts;
    int discontinuity;

    /* rules 6-9 */
    int64_t last_good_frames;
    int slack;
    int search_enabled, accepted, conceal, muted, search_ran;
    int cursor;
    int good_run;
    int64_t best_rate;
    unsigned int best_flags;
    int best_run;
    uint32_t sf_packets, sf_frames;

    /* counters (rule 13) */
    uint64_t packets, failed_packets, silence_frames, tail_bytes;
    uint64_t summary_packets, summary_failed, summary_silence, summary_tail;   /* at the last summary */
    int fail_lines;

    /* rule 14 */
    FILE *dump;
    int dump_packets;
};

/* ---------------------------------------------------------------- formats and layouts */

static enum AVCodecID codec_from_tag( uint16_t tag, int *version )
{
    *version = 0;
    switch (tag)
    {
    case WMA_TAG_V1: *version = 1; return AV_CODEC_ID_WMAV1;
    case WMA_TAG_V2: *version = 2; return AV_CODEC_ID_WMAV2;
    case WMA_TAG_PRO: return AV_CODEC_ID_WMAPRO;
    case WMA_TAG_LOSSLESS: return AV_CODEC_ID_WMALOSSLESS;
    case WMA_TAG_XMA1: return AV_CODEC_ID_XMA1;
    case WMA_TAG_XMA2: return AV_CODEC_ID_XMA2;
    }
    return AV_CODEC_ID_NONE;
}

static int popcount32( uint32_t v )
{
    int n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

/* the layout from a channel mask with as many bits as channels, else the default one */
static void layout_for( AVChannelLayout *layout, uint32_t channels, uint32_t mask )
{
    av_channel_layout_uninit( layout );
    if (mask && popcount32( mask ) == (int)channels &&
        !av_channel_layout_from_mask( layout, mask )) return;
    av_channel_layout_default( layout, channels );
}

static void set_output( struct wma_core *c, const struct wma_core_output *out )
{
    c->out = *out;
    c->out_frame_bytes = out->channels * (out->is_float ? 4 : 2);
    layout_for( &c->out_layout, out->channels, out->channel_mask );
}

/* ---------------------------------------------------------------- the decoder */

/* the private data with the flags word patched in (bytes 2-3 for v1, 4-5 for v2) */
static uint8_t *patched_private( struct wma_core *c, unsigned int flags, size_t *size )
{
    size_t need = c->version == 1 ? 4 : c->version == 2 ? 6 : 0, n = c->in.priv_size;
    uint8_t *p;

    if (n < need) n = need;
    if (!(p = av_mallocz( n + AV_INPUT_BUFFER_PADDING_SIZE ))) return NULL;
    if (c->in.priv_size) memcpy( p, c->priv, c->in.priv_size );
    if (c->version == 1)
    {
        p[2] = flags & 0xff;
        p[3] = (flags >> 8) & 0xff;
    }
    else if (c->version == 2)
    {
        p[4] = flags & 0xff;
        p[5] = (flags >> 8) & 0xff;
    }
    *size = n;
    return p;
}

static int open_decoder( struct wma_core *c, int64_t bit_rate, unsigned int flags )
{
    AVCodecContext *ctx;
    size_t size = 0;
    int ret;

    if (!(ctx = avcodec_alloc_context3( c->codec ))) return AVERROR(ENOMEM);
    ctx->sample_rate = c->in.rate;
    ctx->block_align = c->in.block_align;
    ctx->bit_rate = bit_rate;
    ctx->bits_per_coded_sample = c->in.bits_per_sample;
    layout_for( &ctx->ch_layout, c->in.channels, c->in.channel_mask );

    if (c->version)
        ctx->extradata = patched_private( c, flags, &size );
    else if (c->in.priv_size && (ctx->extradata = av_mallocz( c->in.priv_size + AV_INPUT_BUFFER_PADDING_SIZE )))
    {
        memcpy( ctx->extradata, c->priv, c->in.priv_size );
        size = c->in.priv_size;
    }
    if ((c->version || c->in.priv_size) && !ctx->extradata)
    {
        avcodec_free_context( &ctx );
        return AVERROR(ENOMEM);
    }
    ctx->extradata_size = (int)size;

    if ((ret = avcodec_open2( ctx, c->codec, NULL )) < 0)
    {
        avcodec_free_context( &ctx );
        return ret;
    }
    avcodec_free_context( &c->ctx );
    c->ctx = ctx;
    c->open_rate = bit_rate;
    c->open_flags = flags;
    if (c->version) c->patched_size = size;
    return 0;
}

static int superframe_stream( const struct wma_core *c )
{
    return c->version && (c->open_flags & 0x0002);
}

/* ---------------------------------------------------------------- conversion (rule 4, 5) */

static int frame_is_insane( const AVFrame *frame )
{
    int planar, channels = frame->ch_layout.nb_channels, ch, i, n, planes;

    if (frame->format != AV_SAMPLE_FMT_FLT && frame->format != AV_SAMPLE_FMT_FLTP) return 0;
    planar = frame->format == AV_SAMPLE_FMT_FLTP;
    planes = planar ? channels : 1;
    n = planar ? frame->nb_samples : frame->nb_samples * channels;
    for (ch = 0; ch < planes; ch++)
    {
        const float *s = (const float *)frame->extended_data[ch];
        for (i = 0; i < n; i++)
        {
            float v = s[i];
            if (!isfinite( v ) || v > WMA_SANE_PEAK || v < -WMA_SANE_PEAK) return 1;
        }
    }
    return 0;
}

static int ensure_converter( struct wma_core *c, const AVFrame *frame )
{
    AVChannelLayout in_layout = { 0 };
    int ret;

    if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !frame->ch_layout.nb_channels)
        av_channel_layout_default( &in_layout, frame->ch_layout.nb_channels ? frame->ch_layout.nb_channels
                                                                           : (int)c->in.channels );
    else if (av_channel_layout_copy( &in_layout, &frame->ch_layout ) < 0)
        return AVERROR(ENOMEM);

    if (c->swr && c->swr_in_format == frame->format &&
        !av_channel_layout_compare( &c->swr_in_layout, &in_layout ))
    {
        av_channel_layout_uninit( &in_layout );
        return 0;
    }

    swr_free( &c->swr );
    av_channel_layout_uninit( &c->swr_in_layout );
    ret = swr_alloc_set_opts2( &c->swr, &c->out_layout,
                               c->out.is_float ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16, c->in.rate,
                               &in_layout, frame->format, c->in.rate, 0, NULL );
    if (ret >= 0) ret = swr_init( c->swr );
    if (ret < 0)
    {
        swr_free( &c->swr );
        av_channel_layout_uninit( &in_layout );
        return ret;
    }
    c->swr_in_format = frame->format;
    c->swr_in_layout = in_layout;
    return 0;
}

/* converts a frame and appends it to `dst`; returns the frames appended or < 0 */
static int convert_append( struct wma_core *c, const AVFrame *frame, struct byte_buf *dst )
{
    uint8_t *out;
    int ret;

    if ((ret = ensure_converter( c, frame )) < 0) return ret;
    if (!buf_reserve( dst, (size_t)(frame->nb_samples + 32) * c->out_frame_bytes )) return AVERROR(ENOMEM);
    out = dst->data + dst->len;
    ret = swr_convert( c->swr, &out, frame->nb_samples + 32,
                       (const uint8_t * const *)frame->extended_data, frame->nb_samples );
    if (ret < 0) return ret;
    dst->len += (size_t)ret * c->out_frame_bytes;
    return ret;
}

/* the converter's own tail (none without a rate change, but drained anyway) */
static void converter_tail( struct wma_core *c )
{
    uint8_t *out;
    int ret;

    if (!c->swr) return;
    if (!buf_reserve( &c->pcm, 256 * (size_t)c->out_frame_bytes )) return;
    out = c->pcm.data + c->pcm.len;
    ret = swr_convert( c->swr, &out, 256, NULL, 0 );
    if (ret > 0) c->pcm.len += (size_t)ret * c->out_frame_bytes;
}

/* ---------------------------------------------------------------- packets */

static void receive_frames( struct wma_core *c, struct attempt *a, struct byte_buf *dst )
{
    int ret;

    for (;;)
    {
        ret = avcodec_receive_frame( c->ctx, c->frame );
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0)
        {
            a->send_failed = 1;
            a->err = ret;
            break;
        }
        if (frame_is_insane( c->frame )) a->rejected = 1;
        else if (convert_append( c, c->frame, dst ) >= 0) a->samples += c->frame->nb_samples;
        av_frame_unref( c->frame );
    }
}

static void attempt_packet( struct wma_core *c, const uint8_t *data, uint32_t size, struct attempt *a )
{
    int ret;

    memset( a, 0, sizeof(*a) );
    c->scratch.len = 0;
    if ((ret = av_new_packet( c->pkt, (int)size )) < 0)   /* zero-padded as libavcodec requires */
    {
        a->send_failed = 1;
        a->err = ret;
        return;
    }
    memcpy( c->pkt->data, data, size );
    ret = avcodec_send_packet( c->ctx, c->pkt );
    av_packet_unref( c->pkt );
    if (ret < 0)
    {
        a->send_failed = 1;
        a->err = ret;
        /* frames of earlier packets may still be waiting */
    }
    receive_frames( c, a, &c->scratch );
}

static int judge( const struct wma_core *c, const struct attempt *a, const uint8_t *data )
{
    return wma_packet_good( superframe_stream( c ), data[0] & 0x0f, a->samples, c->frame_len_bits,
                            c->slack, a->send_failed, a->rejected );
}

static void commit_scratch( struct wma_core *c )
{
    if (!c->scratch.len || !buf_reserve( &c->pcm, c->scratch.len )) return;
    memcpy( c->pcm.data + c->pcm.len, c->scratch.data, c->scratch.len );
    c->pcm.len += c->scratch.len;
}

static void append_silence( struct wma_core *c, uint32_t bytes )
{
    int64_t frames = c->last_good_frames;
    size_t size;

    if (frames <= 0 && c->in.avg_bytes)
        frames = (int64_t)bytes * c->in.rate / c->in.avg_bytes;
    if (frames <= 0) return;
    size = (size_t)frames * c->out_frame_bytes;
    if (!buf_reserve( &c->pcm, size )) return;
    memset( c->pcm.data + c->pcm.len, 0, size );   /* 0 is silence for s16 and float */
    c->pcm.len += size;
    c->silence_frames += frames;
}

static void log_failure( struct wma_core *c, const struct attempt *a, const uint8_t *data )
{
    char err[64] = "";

    if (c->fail_lines >= WMA_FAIL_LINES_PER_TRANSFORM) return;
    c->fail_lines++;
    if (a->err) av_strerror( a->err, err, sizeof(err) );
    wma_log( "transform %u packet %llu failed: %s%s%s samples %lld declared %d bit rate %lld flags %#x slack %d%s",
             c->id, (unsigned long long)c->packets, a->send_failed ? "decode error " : "",
             a->rejected ? "rejected (insane float) " : "", err, (long long)a->samples,
             superframe_stream( c ) ? data[0] & 0x0f : -1, (long long)c->open_rate, c->open_flags,
             c->slack, c->fail_lines == WMA_FAIL_LINES_PER_TRANSFORM ? " (no more failure lines)" : "" );
}

static void note_good( struct wma_core *c, const struct attempt *a )
{
    commit_scratch( c );
    c->last_good_frames = a->samples;
    c->slack = 0;
    c->good_run++;
    if (c->good_run > c->best_run)
    {
        c->best_run = c->good_run;
        c->best_rate = c->open_rate;
        c->best_flags = c->open_flags;
    }
    if (c->search_enabled && !c->accepted && c->good_run >= WMA_ACCEPT_RUN)
    {
        c->accepted = 1;   /* the search stops for good */
        wma_log( "accepted bit rate %lld flags %#x (transform %u, %s)", (long long)c->open_rate,
                 c->open_flags, c->id, c->search_ran ? "found by the search" : "as opened" );
    }
}

/* rule 9: the declared frame counts of the first 16 superframe packets with a non-zero count */
static void note_superframe( struct wma_core *c, const uint8_t *data )
{
    int declared = data[0] & 0x0f;

    if (!superframe_stream( c ) || !declared || c->sf_packets >= WMA_SUPERFRAME_STATS) return;
    c->sf_packets++;
    c->sf_frames += declared;
}

static int build_list( struct wma_core *c, struct wma_candidate *list )
{
    struct wma_search_inputs in;

    in.version = c->version;
    in.rate = c->in.rate;
    in.channels = c->in.channels;
    in.extradata_size = c->patched_size;
    in.first_rate = c->first_rate;
    in.alt_rate = c->alt_rate;
    in.implied_rate = wma_implied_rate( c->in.block_align, c->in.rate, c->sf_packets, c->sf_frames,
                                        c->frame_len_bits );
    in.header_rate = c->header_rate;
    in.flags = c->given_flags;
    return wma_build_candidates( &in, list, WMA_MAX_CANDIDATES );
}

static int candidate_is_open( const struct wma_core *c, const struct wma_candidate *cand )
{
    struct wma_candidate now;

    now.bit_rate = c->open_rate;
    now.flags = c->open_flags;
    if (wma_derive( c->version, c->in.rate, c->in.channels, c->open_rate, c->open_flags,
                    c->patched_size, &now.d )) return 0;
    return wma_candidates_equivalent( &now, cand );
}

static int reopen( struct wma_core *c, int64_t rate, unsigned int flags )
{
    int64_t prev_rate = c->open_rate;
    unsigned int prev_flags = c->open_flags;

    if (open_decoder( c, rate, flags ) < 0)
    {
        /* open_decoder keeps the old context on failure; make sure it is really the old one */
        if (c->open_rate != prev_rate || c->open_flags != prev_flags) open_decoder( c, prev_rate, prev_flags );
        return 0;
    }
    c->slack = 3;
    c->good_run = 0;
    return 1;
}

/* Rule 7: try other parameters on this packet. Returns 1 when one decoded it well (committed). */
static int search( struct wma_core *c, const uint8_t *data, uint32_t size, struct attempt *a )
{
    struct wma_candidate list[WMA_MAX_CANDIDATES];
    int n;

    for (;;)
    {
        n = build_list( c, list );   /* rebuilt each step: the implied rate improves */
        while (c->cursor < n && candidate_is_open( c, &list[c->cursor] )) c->cursor++;
        if (c->cursor >= n) break;
        c->search_ran = 1;
        if (!reopen( c, list[c->cursor].bit_rate, list[c->cursor].flags ))
        {
            c->cursor++;
            continue;
        }
        c->cursor++;
        attempt_packet( c, data, size, a );
        if (judge( c, a, data ))
        {
            wma_log( "transform %u search: bit rate %lld flags %#x decodes packet %llu", c->id,
                     (long long)c->open_rate, c->open_flags, (unsigned long long)c->packets );
            note_good( c, a );
            return 1;
        }
    }

    /* the list ran out */
    if (c->best_run > 0)
    {
        if (c->best_rate != c->open_rate || c->best_flags != c->open_flags)
        {
            if (reopen( c, c->best_rate, c->best_flags ))
            {
                attempt_packet( c, data, size, a );
                if (judge( c, a, data ))
                {
                    c->conceal = 1;
                    wma_log( "transform %u search exhausted; back to bit rate %lld flags %#x, concealing failures",
                             c->id, (long long)c->open_rate, c->open_flags );
                    note_good( c, a );
                    return 1;
                }
            }
        }
        c->conceal = 1;
        wma_log( "transform %u search exhausted; keeping bit rate %lld flags %#x, concealing failures",
                 c->id, (long long)c->open_rate, c->open_flags );
    }
    else if (c->search_ran)
    {
        c->muted = 1;
        wma_log( "transform %u: no parameters decode this stream; muted (silence from now on)", c->id );
    }
    else c->conceal = 1;
    return 0;
}

static void dump_packet( struct wma_core *c, const uint8_t *data, uint32_t size )
{
    uint8_t len[4];

    if (!c->dump || c->dump_packets >= WMA_DUMP_PACKETS) return;
    len[0] = size & 0xff; len[1] = (size >> 8) & 0xff; len[2] = (size >> 16) & 0xff; len[3] = size >> 24;
    fwrite( len, 1, 4, c->dump );
    fwrite( data, 1, size, c->dump );
    if (++c->dump_packets == WMA_DUMP_PACKETS)
    {
        fclose( c->dump );
        c->dump = NULL;
    }
    else fflush( c->dump );
}

static void decode_packet( struct wma_core *c, const uint8_t *data )
{
    uint32_t size = c->in.block_align;
    struct attempt a;

    c->packets++;
    dump_packet( c, data, size );
    if (c->muted)
    {
        c->failed_packets++;
        append_silence( c, size );
        return;
    }

    note_superframe( c, data );
    attempt_packet( c, data, size, &a );
    if (judge( c, &a, data ))
    {
        note_good( c, &a );
        return;
    }

    log_failure( c, &a, data );
    if (c->search_enabled && !c->accepted && !c->conceal)
    {
        if (search( c, data, size, &a )) return;
        if (c->muted)
        {
            c->failed_packets++;
            append_silence( c, size );
            return;
        }
    }

    /* not good: keep what decoded (no padding), or silence of its length if nothing did */
    c->failed_packets++;
    c->good_run = 0;
    c->slack = 1;
    if (a.samples > 0) commit_scratch( c );
    else append_silence( c, size );
}

static void decode_staged( struct wma_core *c )
{
    size_t block = c->in.block_align;

    while (c->stage.len - c->stage_pos >= block)
    {
        decode_packet( c, c->stage.data + c->stage_pos );
        c->stage_pos += block;
    }
    if (c->stage_pos)
    {
        memmove( c->stage.data, c->stage.data + c->stage_pos, c->stage.len - c->stage_pos );
        c->stage.len -= c->stage_pos;
        c->stage_pos = 0;
    }
}

/* Clarification 3: only when packets failed or tail bytes were dropped since the previous
 * summary; the counts then start again. A stream that decodes cleanly prints none. */
static void summary( struct wma_core *c, const char *when )
{
    uint64_t failed = c->failed_packets - c->summary_failed, tail = c->tail_bytes - c->summary_tail;

    if (!failed && !tail) return;
    wma_log( "transform %u %s: %llu packets, %llu failed, %llu silent frames emitted, %llu tail bytes dropped%s%s",
             c->id, when, (unsigned long long)(c->packets - c->summary_packets), (unsigned long long)failed,
             (unsigned long long)(c->silence_frames - c->summary_silence), (unsigned long long)tail,
             c->accepted ? ", accepted" : "", c->muted ? ", muted" : "" );
    c->summary_packets = c->packets;
    c->summary_failed = c->failed_packets;
    c->summary_silence = c->silence_frames;
    c->summary_tail = c->tail_bytes;
}

/* ---------------------------------------------------------------- dump (rule 14) */

static void put_u32( FILE *f, uint32_t v )
{
    uint8_t b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24 };
    fwrite( b, 1, 4, f );
}

static void open_dump( struct wma_core *c )
{
    const char *dir = getenv( "PG_DOCS_DIR" );
    char path[1024];
    int n;

    if (!iom_env_set( "PG_WMA_DUMP" ) || !dir || !*dir) return;
    n = __atomic_fetch_add( &dump_counter, 1, __ATOMIC_RELAXED );
    snprintf( path, sizeof(path), "%s/wma-dump-%d.bin", dir, n );
    if (!(c->dump = fopen( path, "wb" )))
    {
        wma_log( "transform %u: can't write %s", c->id, path );
        return;
    }
    fwrite( "MADWMA01", 1, 8, c->dump );
    put_u32( c->dump, c->in.tag );
    put_u32( c->dump, c->in.rate );
    put_u32( c->dump, c->in.channels );
    put_u32( c->dump, c->in.block_align );
    put_u32( c->dump, c->in.avg_bytes );
    put_u32( c->dump, c->in.priv_size );
    if (c->in.priv_size) fwrite( c->priv, 1, c->in.priv_size, c->dump );
    fflush( c->dump );
    wma_log( "transform %u: dumping its first %d packets to %s", c->id, WMA_DUMP_PACKETS, path );
}

/* ---------------------------------------------------------------- the interface */

int wma_core_create( const struct wma_core_format *in, const struct wma_core_output *out,
                     struct wma_core **ret )
{
    struct wma_core *c;
    int err, prev_owner;

    *ret = NULL;
    if (!in->rate || !in->channels || !in->block_align) return WMA_CORE_E_UNSUPPORTED;
    if (!out->channels) return WMA_CORE_E_UNSUPPORTED;

    if (!(c = calloc( 1, sizeof(*c) ))) return WMA_CORE_E_NOMEM;
    c->id = (unsigned int)__atomic_fetch_add( &transform_counter, 1, __ATOMIC_RELAXED );
    c->in = *in;
    if (in->priv_size)
    {
        if (!(c->priv = malloc( in->priv_size )))
        {
            free( c );
            return WMA_CORE_E_NOMEM;
        }
        memcpy( c->priv, in->priv, in->priv_size );
    }
    c->in.priv = c->priv;

    if ((c->codec_id = codec_from_tag( in->tag, &c->version )) == AV_CODEC_ID_NONE ||
        !(c->codec = avcodec_find_decoder( c->codec_id )))
    {
        free( c->priv );
        free( c );
        return WMA_CORE_E_UNSUPPORTED;
    }
    wg_avlog_install();
    prev_owner = wg_avlog_owner( WG_AV_OWNER_WMA );

    set_output( c, out );
    c->frame_len_bits = c->version ? wma_frame_len_bits( c->version, in->rate ) : 0;
    c->header_rate = (int64_t)in->avg_bytes * 8;
    c->given_flags = c->version ? wma_private_flags( c->version, c->priv, in->priv_size ) : 0;
    c->first_rate = c->header_rate;
    /* rule 2: invented xWMA private data and one of the encoder's fake bit rates */
    if (c->version == 2 && wma_is_xwma_private( c->priv, in->priv_size ))
    {
        int64_t norm = wma_xwma_normalised_rate( in->channels, in->rate, c->header_rate );
        if (norm)
        {
            c->first_rate = norm;
            c->alt_rate = c->header_rate;
        }
    }
    c->search_enabled = (c->version == 1 || c->version == 2) && !iom_env_is( "PG_WMA_SEARCH", "0" );
    c->slack = 3;
    c->discontinuity = 0;

    if (!(c->pkt = av_packet_alloc()) || !(c->frame = av_frame_alloc()))
        err = AVERROR(ENOMEM);
    else
        err = open_decoder( c, c->first_rate, c->given_flags );

    wg_avlog_owner( prev_owner );
    if (err < 0)
    {
        char msg[64];
        av_strerror( err, msg, sizeof(msg) );
        wma_log( "transform %u: can't open the decoder for tag %#x (%u Hz, %u ch, align %u, %lld bps): %s",
                 c->id, in->tag, in->rate, in->channels, in->block_align, (long long)c->first_rate, msg );
        wma_core_destroy( c );
        return err == AVERROR(ENOMEM) ? WMA_CORE_E_NOMEM : WMA_CORE_E_OPEN;
    }

    wma_log( "transform %u: tag %#x %u Hz %u ch align %u, bit rate %lld%s flags %#x, private %u bytes -> %s %u ch%s",
             c->id, in->tag, in->rate, in->channels, in->block_align, (long long)c->open_rate,
             c->alt_rate ? " (xWMA, header rate kept as a candidate)" : "", c->open_flags, in->priv_size,
             out->is_float ? "float" : "s16", out->channels, c->search_enabled ? "" : ", no search" );
    open_dump( c );
    *ret = c;
    return WMA_CORE_OK;
}

void wma_core_destroy( struct wma_core *c )
{
    if (!c) return;
    if (c->ctx) summary( c, "destroyed" );
    if (c->dump) fclose( c->dump );
    avcodec_free_context( &c->ctx );
    av_packet_free( &c->pkt );
    av_frame_free( &c->frame );
    swr_free( &c->swr );
    av_channel_layout_uninit( &c->swr_in_layout );
    av_channel_layout_uninit( &c->out_layout );
    buf_free( &c->stage );
    buf_free( &c->pcm );
    buf_free( &c->scratch );
    free( c->priv );
    free( c );
}

int wma_core_set_output( struct wma_core *c, const struct wma_core_output *out )
{
    if (!out->channels) return WMA_CORE_E_UNSUPPORTED;
    /* PCM already converted to the old format is discarded, the converter rebuilt */
    c->pcm.len = 0;
    c->pcm_pos = 0;
    swr_free( &c->swr );
    av_channel_layout_uninit( &c->swr_in_layout );
    set_output( c, out );
    return WMA_CORE_OK;
}

uint32_t wma_core_pending( const struct wma_core *c )
{
    return (uint32_t)(c->pcm.len - c->pcm_pos);
}

uint32_t wma_core_frame_bytes( const struct wma_core *c )
{
    return c->out_frame_bytes;
}

int wma_core_push( struct wma_core *c, const uint8_t *data, uint32_t size,
                   int has_pts, int64_t pts, int discontinuity )
{
    int prev_owner;

    if (!c->has_pts && has_pts)
    {
        c->has_pts = 1;
        c->pts = pts;
    }
    if (discontinuity) c->discontinuity = 1;
    if (!size) return WMA_CORE_OK;

    /* reading is done when nothing is pending: start the PCM queue over */
    if (c->pcm_pos == c->pcm.len) c->pcm.len = c->pcm_pos = 0;

    if (!buf_reserve( &c->stage, size )) return WMA_CORE_E_NOMEM;
    memcpy( c->stage.data + c->stage.len, data, size );
    c->stage.len += size;

    prev_owner = wg_avlog_owner( WG_AV_OWNER_WMA );
    decode_staged( c );
    wg_avlog_owner( prev_owner );
    return WMA_CORE_OK;
}

void wma_core_read( struct wma_core *c, uint8_t *dst, uint32_t max, struct wma_core_read_info *info )
{
    size_t pending = c->pcm.len - c->pcm_pos;
    uint32_t frames = (uint32_t)(pending / c->out_frame_bytes), fit = max / c->out_frame_bytes;

    memset( info, 0, sizeof(*info) );
    if (frames > fit) frames = fit;
    info->frames = frames;
    info->bytes = frames * c->out_frame_bytes;
    if (info->bytes) memcpy( dst, c->pcm.data + c->pcm_pos, info->bytes );
    c->pcm_pos += info->bytes;
    if (c->pcm_pos == c->pcm.len) c->pcm.len = c->pcm_pos = 0;
    /* a partial frame can't be left behind: the queue only ever holds whole frames */

    info->duration = (int64_t)frames * 10000000 / c->in.rate;
    if (c->has_pts)
    {
        info->has_pts = 1;
        info->pts = c->pts;
        c->pts += info->duration;
    }
    if (c->discontinuity && frames)
    {
        info->discontinuity = 1;
        c->discontinuity = 0;
    }
    info->incomplete = c->pcm.len > c->pcm_pos;
}

void wma_core_drain( struct wma_core *c )
{
    struct attempt a;
    int prev_owner = wg_avlog_owner( WG_AV_OWNER_WMA );
    size_t tail;

    decode_staged( c );
    tail = c->stage.len - c->stage_pos;
    if (tail)
    {
        c->tail_bytes += tail;   /* a short packet is always an error */
        c->stage.len = c->stage_pos = 0;
    }

    /* the decoder's own lookahead */
    memset( &a, 0, sizeof(a) );
    if (c->ctx && avcodec_send_packet( c->ctx, NULL ) >= 0)
        receive_frames( c, &a, &c->pcm );
    if (c->ctx) avcodec_flush_buffers( c->ctx );
    converter_tail( c );
    c->slack = 3;
    summary( c, "drained" );
    wg_avlog_owner( prev_owner );
}

void wma_core_flush( struct wma_core *c )
{
    int prev_owner = wg_avlog_owner( WG_AV_OWNER_WMA );

    c->stage.len = c->stage_pos = 0;
    c->pcm.len = c->pcm_pos = 0;
    c->has_pts = 0;
    c->discontinuity = 1;
    c->slack = 3;
    if (c->ctx) avcodec_flush_buffers( c->ctx );
    swr_free( &c->swr );
    av_channel_layout_uninit( &c->swr_in_layout );
    summary( c, "flushed" );
    wg_avlog_owner( prev_owner );
}

void wma_core_get_stats( const struct wma_core *c, struct wma_core_stats *s )
{
    s->packets = c->packets;
    s->failed_packets = c->failed_packets;
    s->silence_frames = c->silence_frames;
    s->tail_bytes = c->tail_bytes;
    s->accepted = c->accepted;
    s->muted = c->muted;
    s->concealing = c->conceal;
    s->search_ran = c->search_ran;
    s->bit_rate = c->open_rate;
    s->flags = c->open_flags;
}
