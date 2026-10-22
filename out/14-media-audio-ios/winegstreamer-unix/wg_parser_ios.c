// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B3: wg_parser_* on iOS, the media parser behind quartz's splitters, Media
// Foundation's fallback source and the WM reader. libavformat demuxes (mp3, wav, mov/mp4);
// libavcodec decodes MPEG audio and PCM; H.264/HEVC and AAC go to the platform decoders (B4,
// wg_platform.h). Decoding is lazy, on the thread that asks for a buffer.
//
// Locks: parser->lock guards the demuxer and the streams; parser->io_lock guards only the read
// protocol, so the PE side's read thread (get_next_read_offset / push_data) never needs the
// parser lock, which a thread waiting for bytes holds.

#if 0
#pragma makedep unix
#endif

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "wg_ios.h"
#include "ios_media_wow.h"
#include "media_log.h"
#include "wg_avlog.h"
#include "wg_handles.h"
#include "wg_parser_pure.h"
#include "wg_platform.h"

#define IO_BUFFER_SIZE (64 * 1024)
#define ANALYSIS_BYTES (256 * 1024)
#define MAX_READ_SIZE (1024 * 1024)
#define UNDECODABLE_LIMIT 512
#define MAX_STREAMS 2

/* AM_SEEKING_PositioningBitsMask / NoPositioning (strmif.h) */
#define SEEK_POSITIONING_MASK 0x3
#define SEEK_NO_POSITIONING 0x0

static const AVRational time_100ns = { 1, 10000000 };

/* ---------------------------------------------------------------- logging (rule 12) */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct iom_dedupe log_table;

static void parser_log( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));
static void parser_log( const char *fmt, ... )
{
    char line[512];
    uint32_t count;
    va_list args;
    int print;

    va_start( args, fmt );
    vsnprintf( line, sizeof(line), fmt, args );
    va_end( args );

    pthread_mutex_lock( &log_lock );
    print = iom_dedupe_check( &log_table, line, &count );
    pthread_mutex_unlock( &log_lock );
    if (!print) return;
    if (count > 1) iom_log( "[wg-parser] ", "%s (seen %u times)", line, count );
    else iom_log( "[wg-parser] ", "%s", line );
}

/* ---------------------------------------------------------------- objects */

enum container { CONTAINER_NONE, CONTAINER_MP3, CONTAINER_WAV, CONTAINER_MP4 };
enum backend { BACKEND_FFMPEG, BACKEND_AAC, BACKEND_VT };

struct out_buf
{
    struct out_buf *next;
    uint8_t *data;
    uint32_t size;
    int64_t pts, duration;      /* 100 ns */
    int discontinuity;
};

struct pkt_node
{
    struct pkt_node *next;
    AVPacket *pkt;
};

struct parser;

struct stream
{
    uint64_t handle;
    struct parser *parser;
    uint32_t number;
    AVStream *st;
    int is_video;
    enum backend backend;
    enum AVCodecID codec_id;
    int64_t start_tb;           /* subtracted from timestamps (MP3/WAV start at their own start time) */
    uint64_t duration;          /* 100 ns, 0 if unknown */

    struct wg_format native;    /* get_current_format */
    struct wg_format codec_fmt; /* get_codec_format */
    struct wg_format enabled_fmt;
    int enabled;

    /* audio */
    AVCodecContext *actx;
    AVFrame *frame;
    struct wgp_aac *aac;
    uint32_t aac_rate, aac_channels;
    float *aac_buf;
    struct wg_pts_fifo aac_pts;
    uint32_t native_mask;
    struct SwrContext *swr;
    int swr_format, swr_rate;
    AVChannelLayout swr_layout;

    /* video */
    struct wgp_video *vt;
    struct wg_reorder reorder;
    int has_dts;
    int64_t last_dts;
    int has_latest;
    int64_t latest_pts;
    int64_t frame_tb;           /* one frame in the stream's time base */
    int bt709;
    enum wgv_format vformat;
    int bottom_up;
    int lock_failures;

    /* packets read for this stream while another one was wanted */
    struct pkt_node *q_head, *q_tail;
    uint32_t q_count;
    uint64_t q_bytes;
    int q_overflow_logged;

    /* output */
    struct out_buf *out_head, *out_tail;
    struct out_buf *held;
    int input_ended, drained, eos;
    int undecodable;
    int discontinuity;
    int has_next_pts;
    int64_t next_pts;
    int has_seek_target;
    int64_t seek_target;
};

struct parser
{
    uint64_t handle;
    pthread_mutex_t lock;
    int output_compressed;

    /* the read protocol (io_lock) */
    pthread_mutex_t io_lock;
    pthread_cond_t io_request_cond, io_done_cond;
    int io_open;
    int req_pending, req_done;
    uint64_t req_offset;
    uint32_t req_size;
    uint8_t *req_buf;
    int32_t req_result;         /* bytes, 0 at end of file, -1 on a read error */
    uint64_t file_size;
    int64_t pos;

    /* the demuxer (lock) */
    int connected;
    enum container container;
    AVFormatContext *fmt;
    AVIOContext *avio;
    AVPacket *pkt;
    struct stream *streams[MAX_STREAMS];
    uint32_t stream_count;
    int read_error, demux_eof;
    int has_stop;
    int64_t stop;
};

/* ---------------------------------------------------------------- the read protocol (rule 1) */

static void io_close( struct parser *p )
{
    pthread_mutex_lock( &p->io_lock );
    p->io_open = 0;
    pthread_cond_broadcast( &p->io_request_cond );
    pthread_cond_broadcast( &p->io_done_cond );
    pthread_mutex_unlock( &p->io_lock );
}

static int read_cb( void *opaque, uint8_t *buf, int buf_size )
{
    struct parser *p = opaque;
    uint32_t size = buf_size > MAX_READ_SIZE ? MAX_READ_SIZE : (uint32_t)buf_size;
    int ret;

    if (buf_size <= 0) return 0;
    if (p->pos < 0 || (uint64_t)p->pos >= p->file_size) return AVERROR_EOF;

    pthread_mutex_lock( &p->io_lock );
    if (!p->io_open)
    {
        pthread_mutex_unlock( &p->io_lock );
        return AVERROR_EXIT;
    }
    p->req_offset = (uint64_t)p->pos;
    p->req_size = size;
    p->req_buf = buf;
    p->req_done = 0;
    p->req_pending = 1;
    pthread_cond_broadcast( &p->io_request_cond );
    while (p->io_open && !p->req_done) pthread_cond_wait( &p->io_done_cond, &p->io_lock );
    if (!p->req_done) ret = AVERROR_EXIT;
    else if (p->req_result > 0) ret = p->req_result;
    else if (!p->req_result) ret = AVERROR_EOF;
    else ret = AVERROR(EIO);
    p->req_pending = p->req_done = 0;
    p->req_buf = NULL;
    pthread_mutex_unlock( &p->io_lock );

    if (ret > 0) p->pos += ret;
    return ret;
}

static int64_t seek_cb( void *opaque, int64_t offset, int whence )
{
    struct parser *p = opaque;
    int64_t pos;

    if (whence & AVSEEK_SIZE) return (int64_t)p->file_size;
    switch (whence & ~AVSEEK_FORCE)
    {
    case SEEK_SET: pos = offset; break;
    case SEEK_CUR: pos = p->pos + offset; break;
    case SEEK_END: pos = (int64_t)p->file_size + offset; break;
    default: return AVERROR(EINVAL);
    }
    if (pos < 0) return AVERROR(EINVAL);
    p->pos = pos;   /* seeking moves the offset only */
    return pos;
}

static NTSTATUS get_next_read_offset( struct wg_parser_get_next_read_offset_params *params )
{
    struct parser *p;

    if (!(p = wgh_get( params->parser, WG_HANDLE_PARSER ))) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &p->io_lock );
    while (p->io_open && !(p->req_pending && !p->req_done))
        pthread_cond_wait( &p->io_request_cond, &p->io_lock );
    if (!p->io_open)
    {
        pthread_mutex_unlock( &p->io_lock );
        return VFW_E_WRONG_STATE;
    }
    params->offset = p->req_offset;
    params->size = p->req_size;
    pthread_mutex_unlock( &p->io_lock );
    return S_OK;
}

static NTSTATUS push_data( uint64_t handle, const void *data, UINT32 size )
{
    struct parser *p;

    if (!(p = wgh_get( handle, WG_HANDLE_PARSER ))) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &p->io_lock );
    if (!p->req_pending || p->req_done)
    {
        pthread_mutex_unlock( &p->io_lock );
        return VFW_E_WRONG_STATE;
    }
    if (!data) p->req_result = -1;          /* read error */
    else if (!size) p->req_result = 0;      /* end of file */
    else
    {
        uint32_t n = size < p->req_size ? size : p->req_size;
        memcpy( p->req_buf, data, n );
        p->req_result = (int32_t)n;
    }
    p->req_done = 1;
    pthread_cond_broadcast( &p->io_done_cond );
    pthread_mutex_unlock( &p->io_lock );
    return S_OK;
}

/* ---------------------------------------------------------------- formats */

static const char *container_name( enum container c )
{
    switch (c)
    {
    case CONTAINER_MP3: return "mp3";
    case CONTAINER_WAV: return "wav";
    case CONTAINER_MP4: return "mp4";
    default: return "?";
    }
}

static int is_pcm( enum AVCodecID id )
{
    return id == AV_CODEC_ID_PCM_U8 || id == AV_CODEC_ID_PCM_S16LE || id == AV_CODEC_ID_PCM_S24LE ||
           id == AV_CODEC_ID_PCM_S32LE || id == AV_CODEC_ID_PCM_F32LE || id == AV_CODEC_ID_PCM_F64LE;
}

static int is_mpeg_audio( enum AVCodecID id )
{
    return id == AV_CODEC_ID_MP1 || id == AV_CODEC_ID_MP2 || id == AV_CODEC_ID_MP3;
}

static wg_audio_format native_audio_format( enum AVCodecID id )
{
    switch (id)
    {
    case AV_CODEC_ID_PCM_U8: return WG_AUDIO_FORMAT_U8;
    case AV_CODEC_ID_PCM_S16LE: return WG_AUDIO_FORMAT_S16LE;
    case AV_CODEC_ID_PCM_S24LE: return WG_AUDIO_FORMAT_S24LE;
    case AV_CODEC_ID_PCM_S32LE: return WG_AUDIO_FORMAT_S32LE;
    case AV_CODEC_ID_PCM_F32LE: return WG_AUDIO_FORMAT_F32LE;
    case AV_CODEC_ID_PCM_F64LE: return WG_AUDIO_FORMAT_F64LE;
    case AV_CODEC_ID_MP1: case AV_CODEC_ID_MP2: case AV_CODEC_ID_MP3: return WG_AUDIO_FORMAT_S16LE;
    case AV_CODEC_ID_AAC: return WG_AUDIO_FORMAT_F32LE;
    default: return WG_AUDIO_FORMAT_UNKNOWN;
    }
}

/* the sample format the converter produces for an enabled format (S24 is produced as S32) */
static enum AVSampleFormat av_format_for( wg_audio_format f )
{
    switch (f)
    {
    case WG_AUDIO_FORMAT_U8: return AV_SAMPLE_FMT_U8;
    case WG_AUDIO_FORMAT_S16LE: return AV_SAMPLE_FMT_S16;
    case WG_AUDIO_FORMAT_S24LE: return AV_SAMPLE_FMT_S32;
    case WG_AUDIO_FORMAT_S32LE: return AV_SAMPLE_FMT_S32;
    case WG_AUDIO_FORMAT_F32LE: return AV_SAMPLE_FMT_FLT;
    case WG_AUDIO_FORMAT_F64LE: return AV_SAMPLE_FMT_DBL;
    default: return AV_SAMPLE_FMT_NONE;
    }
}

static uint32_t bytes_per_sample( wg_audio_format f )
{
    switch (f)
    {
    case WG_AUDIO_FORMAT_U8: return 1;
    case WG_AUDIO_FORMAT_S16LE: return 2;
    case WG_AUDIO_FORMAT_S24LE: return 3;
    case WG_AUDIO_FORMAT_S32LE: case WG_AUDIO_FORMAT_F32LE: return 4;
    case WG_AUDIO_FORMAT_F64LE: return 8;
    default: return 0;
    }
}

static const char *audio_format_name( wg_audio_format f )
{
    static const char *names[] = { "?", "u8", "s16", "s24", "s32", "f32", "f64" };
    return f < sizeof(names) / sizeof(names[0]) ? names[f] : "?";
}

/* the default layout's mask for a channel count */
static uint32_t default_mask( int channels )
{
    AVChannelLayout layout = { 0 };
    uint32_t mask = 0;

    av_channel_layout_default( &layout, channels );
    if (layout.order == AV_CHANNEL_ORDER_NATIVE && !(layout.u.mask & ~(uint64_t)0x3ffff))
        mask = (uint32_t)layout.u.mask;
    av_channel_layout_uninit( &layout );
    return mask;
}

/* the AAC decoder's output order (WAVE speaker order) */
static uint32_t aac_mask( uint32_t channels )
{
    switch (channels)
    {
    case 1: return 0x4;       /* front centre */
    case 2: return 0x3;
    case 6: return 0x3f;      /* 5.1 */
    case 8: return 0x63f;     /* 7.1 */
    }
    return default_mask( channels );
}

static uint32_t layout_mask( const AVChannelLayout *layout )
{
    return wg_channel_mask( layout->order == AV_CHANNEL_ORDER_NATIVE,
                            layout->order == AV_CHANNEL_ORDER_NATIVE ? layout->u.mask : 0,
                            layout->nb_channels, default_mask( layout->nb_channels ) );
}

static void layout_from_mask( AVChannelLayout *layout, uint32_t channels, uint32_t mask )
{
    int n = 0;
    uint32_t m;

    for (m = mask; m; m &= m - 1) n++;
    av_channel_layout_uninit( layout );
    if (mask && n == (int)channels && !av_channel_layout_from_mask( layout, mask )) return;
    av_channel_layout_default( layout, channels );
}

/* the native video format: NV12 unless MADEIRA_WG_VIDEO_FORMAT names another (rule 5) */
static void native_video_format( enum wgv_format *v, wg_video_format *w )
{
    static const struct { const char *name; enum wgv_format v; wg_video_format w; } names[] =
    {
        { "nv12", WGV_NV12, WG_VIDEO_FORMAT_NV12 },
        { "i420", WGV_I420, WG_VIDEO_FORMAT_I420 },
        { "iyuv", WGV_I420, WG_VIDEO_FORMAT_I420 },
        { "yv12", WGV_YV12, WG_VIDEO_FORMAT_YV12 },
        { "yuy2", WGV_YUY2, WG_VIDEO_FORMAT_YUY2 },
        { "rgb32", WGV_BGRX, WG_VIDEO_FORMAT_BGRx },
        { "argb32", WGV_BGRA, WG_VIDEO_FORMAT_BGRA },
        { "abgr32", WGV_RGBA, WG_VIDEO_FORMAT_RGBA },
    };
    const char *env = getenv( "MADEIRA_WG_VIDEO_FORMAT" );
    unsigned int i;

    *v = WGV_NV12;
    *w = WG_VIDEO_FORMAT_NV12;
    if (!env || !*env) return;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (strcasecmp( env, names[i].name )) continue;
        *v = names[i].v;
        *w = names[i].w;
        return;
    }
    parser_log( "MADEIRA_WG_VIDEO_FORMAT=%s is not a known format; using NV12", env );
}

static int video_format_from_wg( wg_video_format w, enum wgv_format *v )
{
    switch (w)
    {
    case WG_VIDEO_FORMAT_NV12: *v = WGV_NV12; return 1;
    case WG_VIDEO_FORMAT_I420: *v = WGV_I420; return 1;
    case WG_VIDEO_FORMAT_YV12: *v = WGV_YV12; return 1;
    case WG_VIDEO_FORMAT_YUY2: *v = WGV_YUY2; return 1;
    case WG_VIDEO_FORMAT_BGRx: *v = WGV_BGRX; return 1;
    case WG_VIDEO_FORMAT_BGRA: *v = WGV_BGRA; return 1;
    case WG_VIDEO_FORMAT_RGBA: *v = WGV_RGBA; return 1;
    default: return 0;
    }
}

static const char *wgv_name( enum wgv_format v )
{
    static const char *names[] = { "NV12", "I420", "YV12", "YUY2", "BGRx", "BGRA", "RGBA" };
    return names[v];
}

static int64_t to_100ns( const struct stream *s, int64_t ts )
{
    return av_rescale_q( ts - s->start_tb, s->st->time_base, time_100ns );
}

/* ---------------------------------------------------------------- output buffers */

static void free_out_list( struct out_buf **head, struct out_buf **tail )
{
    struct out_buf *b, *next;

    for (b = *head; b; b = next)
    {
        next = b->next;
        free( b->data );
        free( b );
    }
    *head = NULL;
    if (tail) *tail = NULL;
}

static void drop_held( struct stream *s )
{
    if (!s->held) return;
    free( s->held->data );
    free( s->held );
    s->held = NULL;
}

static void emit( struct stream *s, uint8_t *data, uint32_t size, int64_t pts, int64_t duration )
{
    struct out_buf *b;

    if (!(b = calloc( 1, sizeof(*b) )))
    {
        free( data );
        return;
    }
    b->data = data;
    b->size = size;
    b->pts = pts;
    b->duration = duration;
    b->discontinuity = s->discontinuity;
    s->discontinuity = 0;
    if (s->out_tail) s->out_tail->next = b;
    else s->out_head = b;
    s->out_tail = b;
    s->next_pts = pts + duration;
    s->has_next_pts = 1;
}

static void free_queue( struct stream *s )
{
    struct pkt_node *n, *next;

    for (n = s->q_head; n; n = next)
    {
        next = n->next;
        av_packet_free( &n->pkt );
        free( n );
    }
    s->q_head = s->q_tail = NULL;
    s->q_count = 0;
    s->q_bytes = 0;
}

static void release_pictures( struct stream *s )
{
    while (s->reorder.count)
    {
        struct wg_reorder_item item = wg_reorder_pop( &s->reorder );
        wgp_ops->picture_release( item.picture );
    }
}

/* ---------------------------------------------------------------- audio output (rule 8) */

static int ensure_converter( struct stream *s, int in_format, int in_rate, const AVChannelLayout *in_layout )
{
    AVChannelLayout out_layout = { 0 };
    int ret;

    if (s->swr && s->swr_format == in_format && s->swr_rate == in_rate &&
        !av_channel_layout_compare( &s->swr_layout, in_layout )) return 0;

    swr_free( &s->swr );
    av_channel_layout_uninit( &s->swr_layout );
    layout_from_mask( &out_layout, s->enabled_fmt.u.audio.channels, s->enabled_fmt.u.audio.channel_mask );
    ret = swr_alloc_set_opts2( &s->swr, &out_layout, av_format_for( s->enabled_fmt.u.audio.format ),
                               s->enabled_fmt.u.audio.rate, in_layout, in_format, in_rate, 0, NULL );
    av_channel_layout_uninit( &out_layout );
    if (ret >= 0) ret = swr_init( s->swr );
    if (ret < 0)
    {
        swr_free( &s->swr );
        return ret;
    }
    s->swr_format = in_format;
    s->swr_rate = in_rate;
    if ((ret = av_channel_layout_copy( &s->swr_layout, in_layout )) < 0) swr_free( &s->swr );
    return ret;
}

/* converts (in != NULL) or flushes the converter's tail (in == NULL) into a new buffer */
static void audio_out( struct parser *p, struct stream *s, const uint8_t * const *in, int nb_samples,
                       int has_pts, int64_t pts )
{
    wg_audio_format format = s->enabled_fmt.u.audio.format;
    uint32_t channels = s->enabled_fmt.u.audio.channels, rate = s->enabled_fmt.u.audio.rate;
    uint32_t conv_frame = channels * (format == WG_AUDIO_FORMAT_S24LE ? 4 : bytes_per_sample( format ));
    uint32_t out_frame = channels * bytes_per_sample( format );
    uint32_t frames, drop, keep;
    uint8_t *data;
    int max, n;

    if (!s->swr || !conv_frame) return;
    max = swr_get_out_samples( s->swr, in ? nb_samples : 0 );
    if (max <= 0) return;
    if (!(data = malloc( (size_t)max * conv_frame ))) return;
    n = swr_convert( s->swr, &data, max, in, in ? nb_samples : 0 );
    if (n <= 0)
    {
        free( data );
        return;
    }
    frames = (uint32_t)n;
    if (format == WG_AUDIO_FORMAT_S24LE) wg_pack_s24( (const int32_t *)data, (size_t)frames * channels, data );

    if (!has_pts) pts = s->has_next_pts ? s->next_pts : 0;

    if (s->has_seek_target)
    {
        drop = wg_trim_start( pts, frames, rate, s->seek_target );
        if (drop == frames)
        {
            free( data );
            return;
        }
        if (drop)
        {
            memmove( data, data + (size_t)drop * out_frame, (size_t)(frames - drop) * out_frame );
            pts += (int64_t)drop * 10000000 / rate;
            frames -= drop;
        }
        s->has_seek_target = 0;
    }
    if (p->has_stop)
    {
        keep = wg_trim_stop( pts, frames, rate, p->stop );
        if (keep < frames || pts + (int64_t)frames * 10000000 / rate >= p->stop)
            s->eos = 1;   /* this is the last buffer: the stream ends at the stop position */
        if (!keep)
        {
            free( data );
            return;
        }
        frames = keep;
    }
    emit( s, data, frames * out_frame, pts, (int64_t)frames * 10000000 / rate );
}

static void audio_frame_out( struct parser *p, struct stream *s, const AVFrame *frame )
{
    AVChannelLayout layout = { 0 };
    int64_t ts = frame->best_effort_timestamp;

    if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !frame->ch_layout.nb_channels)
        av_channel_layout_default( &layout, frame->ch_layout.nb_channels ? frame->ch_layout.nb_channels
                                                                       : (int)s->native.u.audio.channels );
    else if (av_channel_layout_copy( &layout, &frame->ch_layout ) < 0) return;

    if (ensure_converter( s, frame->format, frame->sample_rate ? frame->sample_rate : (int)s->native.u.audio.rate,
                          &layout ) < 0)
    {
        parser_log( "stream %u: can't convert %s %d Hz to %s %u Hz %u ch", s->number,
                    av_get_sample_fmt_name( frame->format ) ? av_get_sample_fmt_name( frame->format ) : "?",
                    frame->sample_rate, audio_format_name( s->enabled_fmt.u.audio.format ),
                    s->enabled_fmt.u.audio.rate, s->enabled_fmt.u.audio.channels );
        av_channel_layout_uninit( &layout );
        return;
    }
    av_channel_layout_uninit( &layout );
    audio_out( p, s, (const uint8_t * const *)frame->extended_data, frame->nb_samples,
               ts != AV_NOPTS_VALUE, ts != AV_NOPTS_VALUE ? to_100ns( s, ts ) : 0 );
}

static void note_undecodable( struct stream *s, const char *what )
{
    if (++s->undecodable == UNDECODABLE_LIMIT)
    {
        parser_log( "stream %u (%s): %d undecodable packets in a row (%s); ending the stream", s->number,
                    avcodec_get_name( s->codec_id ), UNDECODABLE_LIMIT, what );
        s->eos = 1;
    }
}

static void receive_audio( struct parser *p, struct stream *s, int *error )
{
    int ret;

    while ((ret = avcodec_receive_frame( s->actx, s->frame )) >= 0)
    {
        if (s->enabled) audio_frame_out( p, s, s->frame );
        av_frame_unref( s->frame );
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) *error = 1;
}

static void decode_audio( struct parser *p, struct stream *s, AVPacket *pkt )
{
    int error = 0, ret;

    if (s->backend == BACKEND_AAC)
    {
        uint32_t frames = 0;
        int64_t pts;

        wg_pts_fifo_push( &s->aac_pts, pkt->pts );
        if (wgp_ops->aac_decode( s->aac, pkt->data, pkt->size, s->aac_buf, WGP_AAC_MAX_FRAMES, &frames ))
        {
            note_undecodable( s, "AudioConverter" );
            return;
        }
        s->undecodable = 0;
        if (!frames) return;   /* legal: the decoder answers a packet late */
        if (!wg_pts_fifo_pop( &s->aac_pts, &pts )) pts = AV_NOPTS_VALUE;
        if (s->enabled)
        {
            AVChannelLayout layout = { 0 };
            const uint8_t *planes[1] = { (const uint8_t *)s->aac_buf };

            layout_from_mask( &layout, s->aac_channels, aac_mask( s->aac_channels ) );
            if (ensure_converter( s, AV_SAMPLE_FMT_FLT, s->aac_rate, &layout ) >= 0)
                audio_out( p, s, planes, frames, pts != AV_NOPTS_VALUE,
                           pts != AV_NOPTS_VALUE ? to_100ns( s, pts ) : 0 );
            av_channel_layout_uninit( &layout );
        }
        return;
    }

    ret = avcodec_send_packet( s->actx, pkt );
    if (ret < 0 && ret != AVERROR(EAGAIN)) error = 1;
    receive_audio( p, s, &error );
    if (error) note_undecodable( s, "libavcodec" );
    else s->undecodable = 0;
}

static void drain_audio( struct parser *p, struct stream *s )
{
    int error = 0;

    if (s->backend == BACKEND_FFMPEG && s->actx)
    {
        if (avcodec_send_packet( s->actx, NULL ) >= 0) receive_audio( p, s, &error );
        avcodec_flush_buffers( s->actx );
    }
    /* the converter's tail */
    if (s->enabled && s->swr && !s->eos) audio_out( p, s, NULL, 0, 0, 0 );
}

/* ---------------------------------------------------------------- video output (rule 9) */

struct picture_call
{
    struct stream *s;
    int64_t duration;
};

static void picture_cb( void *context, void *picture, int64_t pts )
{
    struct picture_call *call = context;
    struct stream *s = call->s;
    struct wg_reorder_item item;

    if (pts == WGP_NO_PTS) pts = s->has_latest ? s->latest_pts + s->frame_tb : 0;
    if (!s->has_latest || pts > s->latest_pts)
    {
        s->latest_pts = pts;
        s->has_latest = 1;
    }
    item.pts = pts;
    item.duration = call->duration;
    item.picture = picture;
    if (wg_reorder_insert( &s->reorder, &item ))
        wgp_ops->picture_release( picture );   /* can't happen: the list is drained past 16 */
}

static void release_first( struct parser *p, struct stream *s )
{
    struct wg_reorder_item item = wg_reorder_pop( &s->reorder );
    int64_t pts = to_100ns( s, item.pts ), duration;
    struct wgv_layout layout;
    struct wgp_picture pic;
    struct wgv_source src;
    uint32_t w = s->native.u.video.width, h = s->native.u.video.height;
    uint8_t *data;

    duration = av_rescale_q( item.duration > 0 ? item.duration : s->frame_tb, s->st->time_base, time_100ns );
    if (!s->enabled || s->eos) goto done;
    if (s->has_seek_target)
    {
        if (pts + duration <= s->seek_target) goto done;   /* before the target */
        s->has_seek_target = 0;
    }
    if (p->has_stop && pts >= p->stop)
    {
        s->eos = 1;
        goto done;
    }
    if (wgv_layout_for( s->vformat, w, h, &layout )) goto done;
    if (wgp_ops->picture_lock( item.picture, &pic ))
    {
        if (!s->lock_failures++)
            parser_log( "stream %u: a decoded picture is not 4:2:0 bi-planar; dropped", s->number );
        goto done;
    }
    if ((data = malloc( layout.size )))
    {
        src.width = pic.width;
        src.height = pic.height;
        src.y = pic.y;
        src.y_stride = pic.y_stride;
        src.uv = pic.uv;
        src.uv_stride = pic.uv_stride;
        src.full_range = pic.full_range;
        wgv_convert( &src, s->vformat, w, h, s->bt709, s->bottom_up, data );
    }
    wgp_ops->picture_unlock( item.picture );
    if (data) emit( s, data, layout.size, pts, duration );
done:
    wgp_ops->picture_release( item.picture );
}

static void decode_video( struct parser *p, struct stream *s, AVPacket *pkt )
{
    struct picture_call call = { s, pkt->duration };
    int ret;

    ret = wgp_ops->video_decode( s->vt, pkt->data, pkt->size, pkt->pts == AV_NOPTS_VALUE ? WGP_NO_PTS : pkt->pts,
                                 pkt->duration, s->st->time_base.num, s->st->time_base.den,
                                 !!(pkt->flags & AV_PKT_FLAG_KEY), picture_cb, &call );
    if (ret == WGP_E_SESSION_LOST)
        parser_log( "stream %u: the video decoder session was lost (app in the background?); recovering at the next keyframe",
                    s->number );
    else if (ret) note_undecodable( s, "VideoToolbox" );
    else s->undecodable = 0;

    if (pkt->dts != AV_NOPTS_VALUE)
    {
        s->last_dts = pkt->dts;
        s->has_dts = 1;
    }
    while (wg_reorder_ready( &s->reorder, s->has_dts, s->last_dts, 0 )) release_first( p, s );
}

static void drain_video( struct parser *p, struct stream *s )
{
    while (wg_reorder_ready( &s->reorder, 0, 0, 1 )) release_first( p, s );
}

/* ---------------------------------------------------------------- demuxing (rule 7) */

static struct stream *stream_for_index( struct parser *p, int index )
{
    uint32_t i;

    for (i = 0; i < p->stream_count; i++)
        if (p->streams[i]->st->index == index) return p->streams[i];
    return NULL;
}

static void enqueue( struct stream *s, AVPacket *pkt )
{
    struct pkt_node *n;

    if (!wg_queue_admit( s->q_count, s->q_bytes, (uint32_t)pkt->size ))
    {
        if (!s->q_overflow_logged)
        {
            s->q_overflow_logged = 1;
            parser_log( "stream %u: more than %u packets or %u MiB queued while another stream was read; dropping",
                        s->number, WG_QUEUE_MAX_PACKETS, WG_QUEUE_MAX_BYTES >> 20 );
        }
        s->discontinuity = 1;
        av_packet_free( &pkt );
        return;
    }
    if (!(n = calloc( 1, sizeof(*n) )))
    {
        av_packet_free( &pkt );
        return;
    }
    n->pkt = pkt;
    if (s->q_tail) s->q_tail->next = n;
    else s->q_head = n;
    s->q_tail = n;
    s->q_count++;
    s->q_bytes += (uint32_t)pkt->size;
}

static AVPacket *dequeue( struct stream *s )
{
    struct pkt_node *n = s->q_head;
    AVPacket *pkt;

    if (!n) return NULL;
    s->q_head = n->next;
    if (!s->q_head) s->q_tail = NULL;
    s->q_count--;
    s->q_bytes -= (uint32_t)n->pkt->size;
    pkt = n->pkt;
    free( n );
    return pkt;
}

static void end_input( struct parser *p )
{
    uint32_t i;
    for (i = 0; i < p->stream_count; i++) p->streams[i]->input_ended = 1;
}

/* the next packet for s: its queue first, then the demuxer (queueing packets of other streams) */
static AVPacket *next_packet( struct parser *p, struct stream *s )
{
    AVPacket *pkt;
    struct stream *t;
    int ret;

    if ((pkt = dequeue( s ))) return pkt;
    if (s->input_ended) return NULL;

    for (;;)
    {
        if (p->demux_eof || p->read_error)
        {
            end_input( p );
            return NULL;
        }
        ret = av_read_frame( p->fmt, p->pkt );
        if (ret < 0)
        {
            if (ret == AVERROR_EOF) p->demux_eof = 1;
            else
            {
                char err[64];
                av_strerror( ret, err, sizeof(err) );
                if (ret != AVERROR_EXIT) parser_log( "read error: %s; streams end here until a seek", err );
                p->read_error = 1;   /* latched until a seek */
            }
            continue;
        }
        if (!(t = stream_for_index( p, p->pkt->stream_index )))
        {
            av_packet_unref( p->pkt );
            continue;
        }
        if (!(pkt = av_packet_alloc()))
        {
            av_packet_unref( p->pkt );
            continue;
        }
        av_packet_move_ref( pkt, p->pkt );
        if (t == s) return pkt;
        if (t->enabled && !t->eos && !t->input_ended) enqueue( t, pkt );
        else av_packet_free( &pkt );
    }
}

/* Makes sure s has a ready buffer, or is at its end. */
static void fill( struct parser *p, struct stream *s )
{
    AVPacket *pkt;

    while (!s->out_head && !s->eos)
    {
        if ((pkt = next_packet( p, s )))
        {
            if (s->is_video) decode_video( p, s, pkt );
            else decode_audio( p, s, pkt );
            av_packet_free( &pkt );
            continue;
        }
        if (!s->drained)
        {
            s->drained = 1;
            if (s->is_video) drain_video( p, s );
            else drain_audio( p, s );
            continue;
        }
        s->eos = 1;
    }
}

/* ---------------------------------------------------------------- streams */

static void flush_stream( struct stream *s )
{
    drop_held( s );
    free_out_list( &s->out_head, &s->out_tail );
    free_queue( s );
    if (s->actx) avcodec_flush_buffers( s->actx );
    if (s->aac) wgp_ops->aac_flush( s->aac );
    if (s->vt) wgp_ops->video_flush( s->vt );
    release_pictures( s );
    swr_free( &s->swr );
    av_channel_layout_uninit( &s->swr_layout );
    wg_pts_fifo_clear( &s->aac_pts );
    s->has_dts = s->has_latest = 0;
    s->input_ended = s->drained = s->eos = 0;
    s->undecodable = 0;
}

static void free_stream( struct stream *s )
{
    if (!s) return;
    wgh_free( s->handle, WG_HANDLE_STREAM );
    drop_held( s );
    free_out_list( &s->out_head, &s->out_tail );
    free_queue( s );
    release_pictures( s );
    if (s->vt) wgp_ops->video_destroy( s->vt );
    if (s->aac) wgp_ops->aac_destroy( s->aac );
    avcodec_free_context( &s->actx );
    av_frame_free( &s->frame );
    swr_free( &s->swr );
    av_channel_layout_uninit( &s->swr_layout );
    free( s->aac_buf );
    free( s );
}

static void teardown( struct parser *p )
{
    uint32_t i;

    for (i = 0; i < p->stream_count; i++) free_stream( p->streams[i] );
    memset( p->streams, 0, sizeof(p->streams) );
    p->stream_count = 0;
    if (p->fmt) avformat_close_input( &p->fmt );
    if (p->avio)
    {
        av_freep( &p->avio->buffer );
        avio_context_free( &p->avio );
    }
    av_packet_free( &p->pkt );
    p->connected = 0;
    p->read_error = p->demux_eof = 0;
    p->has_stop = 0;
    p->container = CONTAINER_NONE;
}

static uint64_t stream_duration( struct parser *p, AVStream *st, int64_t start_tb )
{
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        return av_rescale_q( st->duration, st->time_base, time_100ns );
    if (p->fmt->duration != AV_NOPTS_VALUE && p->fmt->duration > 0)
        return av_rescale_q( p->fmt->duration, AV_TIME_BASE_Q, time_100ns );
    (void)start_tb;
    return 0;
}

static struct stream *new_stream( struct parser *p, AVStream *st )
{
    struct stream *s;

    if (!(s = calloc( 1, sizeof(*s) ))) return NULL;
    s->parser = p;
    s->st = st;
    s->codec_id = st->codecpar->codec_id;
    s->start_tb = (p->container != CONTAINER_MP4 && st->start_time != AV_NOPTS_VALUE) ? st->start_time : 0;
    s->duration = stream_duration( p, st, s->start_tb );
    s->discontinuity = 1;
    return s;
}

/* Builds an audio stream. Returns NULL with *why set when it can't be decoded. */
static struct stream *audio_stream( struct parser *p, AVStream *st, char *why, size_t why_size )
{
    AVCodecParameters *par = st->codecpar;
    int channels = par->ch_layout.nb_channels;
    struct stream *s;
    struct wg_format *f;

    if (!is_pcm( par->codec_id ) && !is_mpeg_audio( par->codec_id ) && par->codec_id != AV_CODEC_ID_AAC)
    {
        snprintf( why, why_size, "audio codec %s (id %d) is not supported", avcodec_get_name( par->codec_id ),
                  par->codec_id );
        return NULL;
    }
    if (channels < 1 || channels > 8 || par->sample_rate <= 0)
    {
        snprintf( why, why_size, "audio %s with %d channels at %d Hz", avcodec_get_name( par->codec_id ),
                  channels, par->sample_rate );
        return NULL;
    }
    if (!(s = new_stream( p, st )))
    {
        snprintf( why, why_size, "out of memory" );
        return NULL;
    }

    f = &s->native;
    f->major_type = WG_MAJOR_TYPE_AUDIO;
    f->u.audio.format = native_audio_format( par->codec_id );
    f->u.audio.channels = channels;
    f->u.audio.rate = par->sample_rate;
    f->u.audio.channel_mask = layout_mask( &par->ch_layout );

    if (par->codec_id == AV_CODEC_ID_AAC)
    {
        uint32_t rate, ch, fpp;

        s->backend = BACKEND_AAC;
        if (wgp_ops->aac_create( par->extradata, par->extradata_size, par->sample_rate, channels, &s->aac,
                                 &rate, &ch, &fpp, why, why_size ))
        {
            free_stream( s );
            return NULL;
        }
        if (!(s->aac_buf = malloc( (size_t)WGP_AAC_MAX_FRAMES * 4 * ch )))
        {
            snprintf( why, why_size, "out of memory" );
            free_stream( s );
            return NULL;
        }
        s->aac_rate = rate;
        s->aac_channels = ch;
        f->u.audio.channels = ch;
        f->u.audio.rate = rate;
        f->u.audio.channel_mask = aac_mask( ch );

        s->codec_fmt.major_type = WG_MAJOR_TYPE_AUDIO_MPEG4;
        s->codec_fmt.u.audio.payload_type = 0;
        s->codec_fmt.u.audio.channels = ch;
        s->codec_fmt.u.audio.rate = rate;
        if (par->extradata_size > 0 && par->extradata_size <= (int)sizeof(s->codec_fmt.u.audio.codec_data))
        {
            s->codec_fmt.u.audio.codec_data_len = par->extradata_size;
            memcpy( s->codec_fmt.u.audio.codec_data, par->extradata, par->extradata_size );
        }
    }
    else
    {
        const AVCodec *codec = avcodec_find_decoder( par->codec_id );
        int ret;

        s->backend = BACKEND_FFMPEG;
        if (!codec || !(s->actx = avcodec_alloc_context3( codec )) || !(s->frame = av_frame_alloc()) ||
            avcodec_parameters_to_context( s->actx, par ) < 0)
        {
            snprintf( why, why_size, "no %s decoder", avcodec_get_name( par->codec_id ) );
            free_stream( s );
            return NULL;
        }
        s->actx->pkt_timebase = st->time_base;
        if ((ret = avcodec_open2( s->actx, codec, NULL )) < 0)
        {
            char err[64];
            av_strerror( ret, err, sizeof(err) );
            snprintf( why, why_size, "can't open the %s decoder: %s", avcodec_get_name( par->codec_id ), err );
            free_stream( s );
            return NULL;
        }
        if (is_mpeg_audio( par->codec_id ))
        {
            s->codec_fmt.major_type = WG_MAJOR_TYPE_AUDIO_MPEG1;
            s->codec_fmt.u.audio.layer = par->codec_id == AV_CODEC_ID_MP1 ? 1 : par->codec_id == AV_CODEC_ID_MP2 ? 2 : 3;
            s->codec_fmt.u.audio.channels = channels;
            s->codec_fmt.u.audio.rate = par->sample_rate;
            s->codec_fmt.u.audio.bitrate = (uint32_t)par->bit_rate;
        }
        else s->codec_fmt = s->native;
    }
    /* MP4 audio starts with a seek target of 0: encoder-delay packets before 0 are trimmed */
    if (p->container == CONTAINER_MP4)
    {
        s->has_seek_target = 1;
        s->seek_target = 0;
    }
    return s;
}

static struct stream *video_stream( struct parser *p, AVStream *st, char *why, size_t why_size )
{
    AVCodecParameters *par = st->codecpar;
    struct wgp_param_sets sets;
    const char *reason;
    struct stream *s;
    wg_video_format wformat;
    AVRational fps;
    int bt709;

    if (par->codec_id != AV_CODEC_ID_H264 && par->codec_id != AV_CODEC_ID_HEVC)
    {
        snprintf( why, why_size, "video codec %s (id %d) is not supported", avcodec_get_name( par->codec_id ),
                  par->codec_id );
        return NULL;
    }
    if (par->width < 1 || par->width > 8192 || par->height < 1 || par->height > 8192)
    {
        snprintf( why, why_size, "video size %dx%d", par->width, par->height );
        return NULL;
    }
    if (!(s = new_stream( p, st )))
    {
        snprintf( why, why_size, "out of memory" );
        return NULL;
    }
    s->is_video = 1;
    s->backend = BACKEND_VT;
    if (wgp_ops->video_create( par->codec_id == AV_CODEC_ID_H264 ? WGP_CODEC_H264 : WGP_CODEC_HEVC,
                               par->extradata, par->extradata_size, par->width, par->height, &s->vt,
                               why, why_size ))
    {
        free_stream( s );
        return NULL;
    }

    fps = st->avg_frame_rate;
    if (fps.num <= 0 || fps.den <= 0) fps = st->r_frame_rate;
    if (fps.num <= 0 || fps.den <= 0)
    {
        fps.num = 30;
        fps.den = 1;
        parser_log( "stream %u: no frame rate in the container; assuming 30/1", s->number );
    }
    s->frame_tb = av_rescale_q( 1, (AVRational){ fps.den, fps.num }, st->time_base );
    if (s->frame_tb <= 0) s->frame_tb = 1;

    switch (par->color_space)
    {
    case AVCOL_SPC_BT709: bt709 = 1; break;
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_FCC: bt709 = 0; break;
    default: bt709 = par->height >= 720; break;
    }
    s->bt709 = bt709;

    native_video_format( &s->vformat, &wformat );
    s->native.major_type = WG_MAJOR_TYPE_VIDEO;
    s->native.u.video.format = wformat;
    s->native.u.video.width = par->width;
    s->native.u.video.height = par->height;
    s->native.u.video.fps_n = fps.num;
    s->native.u.video.fps_d = fps.den;

    if (par->codec_id == AV_CODEC_ID_H264)
    {
        s->codec_fmt.major_type = WG_MAJOR_TYPE_VIDEO_H264;
        s->codec_fmt.u.video.width = par->width;
        s->codec_fmt.u.video.height = par->height;
        s->codec_fmt.u.video.fps_n = fps.num;
        s->codec_fmt.u.video.fps_d = fps.den;
        if (!wgp_parse_avcc( par->extradata, par->extradata_size, &sets, &reason ))
        {
            s->codec_fmt.u.video.profile = sets.profile;
            s->codec_fmt.u.video.level = sets.level;
        }
        else
        {
            s->codec_fmt.u.video.profile = par->profile > 0 ? (par->profile & 0xff) : 0;
            s->codec_fmt.u.video.level = par->level > 0 ? par->level : 0;
        }
        /* no codec data: wg_format's H.264 codec data is Annex B and the container holds avcC */
    }
    else s->codec_fmt = s->native;
    return s;
}

/* ---------------------------------------------------------------- connect (rules 2, 3, 4) */

static int add_stream( struct parser *p, struct stream *s )
{
    if (p->stream_count >= MAX_STREAMS) return 0;
    s->number = p->stream_count;
    if (!(s->handle = wgh_alloc( WG_HANDLE_STREAM, s ))) return 0;
    p->streams[p->stream_count++] = s;
    return 1;
}

static int stream_is_cover( const AVStream *st )
{
    return !!(st->disposition & AV_DISPOSITION_ATTACHED_PIC);
}

static NTSTATUS do_connect( struct parser *p, uint64_t file_size, char *why, size_t why_size, int *score_out,
                            AVStream **refused_stream )
{
    const AVInputFormat *ifmt = NULL;
    AVStream *video = NULL, *audio = NULL;
    uint8_t *buffer;
    unsigned int i;
    int score, ret;

    *refused_stream = NULL;
    if (!(buffer = av_malloc( IO_BUFFER_SIZE )) ||
        !(p->avio = avio_alloc_context( buffer, IO_BUFFER_SIZE, 0, p, read_cb, NULL, seek_cb )))
    {
        av_free( buffer );
        snprintf( why, why_size, "out of memory" );
        return E_OUTOFMEMORY;
    }
    p->avio->seekable = AVIO_SEEKABLE_NORMAL;

    score = av_probe_input_buffer2( p->avio, &ifmt, "", NULL, 0, ANALYSIS_BYTES );
    *score_out = score;
    if (score < 0 || !ifmt)
    {
        snprintf( why, why_size, "no demuxer recognises the data" );
        return VFW_E_UNSUPPORTED_STREAM;
    }
    if (!strcmp( ifmt->name, "mp3" )) p->container = CONTAINER_MP3;
    else if (!strcmp( ifmt->name, "wav" )) p->container = CONTAINER_WAV;
    else if (!strncmp( ifmt->name, "mov,", 4 )) p->container = CONTAINER_MP4;
    else
    {
        snprintf( why, why_size, "container %s", ifmt->name );
        return VFW_E_UNSUPPORTED_STREAM;
    }
    if ((p->container == CONTAINER_MP3 || p->container == CONTAINER_WAV) && score < AVPROBE_SCORE_EXTENSION / 4)
    {
        /* lower scores misread other MPEG files */
        snprintf( why, why_size, "%s probe score %d is below %d", ifmt->name, score, AVPROBE_SCORE_EXTENSION / 4 );
        return VFW_E_UNSUPPORTED_STREAM;
    }
    if (p->container == CONTAINER_MP4)
    {
        if (iom_env_is( "MADEIRA_WG_VIDEO", "0" ))
        {
            snprintf( why, why_size, "mp4 refused (MADEIRA_WG_VIDEO=0)" );
            return VFW_E_UNSUPPORTED_STREAM;
        }
        if (score < AVPROBE_SCORE_MAX / 2)
        {
            snprintf( why, why_size, "mp4 probe score %d is below %d", score, AVPROBE_SCORE_MAX / 2 );
            return VFW_E_UNSUPPORTED_STREAM;
        }
    }
    if (p->output_compressed && p->container != CONTAINER_WAV)
    {
        /* rule 4: this splitter would hand the compressed stream to a decoder this port lacks */
        snprintf( why, why_size, "compressed output of %s is not supported (only PCM)", container_name( p->container ) );
        return VFW_E_UNSUPPORTED_STREAM;
    }

    if (!(p->fmt = avformat_alloc_context()))
    {
        snprintf( why, why_size, "out of memory" );
        return E_OUTOFMEMORY;
    }
    p->fmt->pb = p->avio;
    p->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    p->fmt->probesize = ANALYSIS_BYTES;
    if ((ret = avformat_open_input( &p->fmt, "", ifmt, NULL )) < 0)
    {
        char err[64];
        av_strerror( ret, err, sizeof(err) );
        snprintf( why, why_size, "%s header: %s", ifmt->name, err );
        p->fmt = NULL;   /* freed by avformat_open_input */
        return ret == AVERROR(ENOMEM) ? E_OUTOFMEMORY : VFW_E_UNSUPPORTED_STREAM;
    }

    /* the streams this parser may expose (rule 3); everything else is excluded from analysis */
    for (i = 0; i < p->fmt->nb_streams; i++)
    {
        AVStream *st = p->fmt->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;

        if (type == AVMEDIA_TYPE_VIDEO && !stream_is_cover( st ))
        {
            if (p->container != CONTAINER_MP4)
            {
                *refused_stream = st;
                snprintf( why, why_size, "a video stream (%s) in an audio file", avcodec_get_name( st->codecpar->codec_id ) );
                return VFW_E_UNSUPPORTED_STREAM;
            }
            if (!video) video = st;
            else st->discard = AVDISCARD_ALL;
        }
        else if (type == AVMEDIA_TYPE_AUDIO && !audio) audio = st;
        else st->discard = AVDISCARD_ALL;
    }
    if (!audio && !video)
    {
        snprintf( why, why_size, "no audio or video stream" );
        return VFW_E_UNSUPPORTED_STREAM;
    }
    if (p->output_compressed && (!audio || !is_pcm( audio->codecpar->codec_id )))
    {
        *refused_stream = audio;
        snprintf( why, why_size, "compressed output of %s is not supported (only PCM)",
                  audio ? avcodec_get_name( audio->codecpar->codec_id ) : "nothing" );
        return VFW_E_UNSUPPORTED_STREAM;
    }

    p->fmt->probesize = ANALYSIS_BYTES;
    if ((ret = avformat_find_stream_info( p->fmt, NULL )) < 0)
    {
        char err[64];
        av_strerror( ret, err, sizeof(err) );
        parser_log( "%s: stream analysis failed (%s); using the header's parameters", ifmt->name, err );
    }

    if (video)
    {
        struct stream *s = video_stream( p, video, why, why_size );
        if (!s)
        {
            *refused_stream = video;   /* undecodable video refuses the file */
            return VFW_E_UNSUPPORTED_STREAM;
        }
        if (!add_stream( p, s ))
        {
            free_stream( s );
            snprintf( why, why_size, "out of memory" );
            return E_OUTOFMEMORY;
        }
    }
    if (audio)
    {
        char audio_why[160];
        struct stream *s = audio_stream( p, audio, audio_why, sizeof(audio_why) );

        if (!s)
        {
            if (!video)
            {
                *refused_stream = audio;
                snprintf( why, why_size, "%s", audio_why );
                return VFW_E_UNSUPPORTED_STREAM;
            }
            parser_log( "%s: audio left out next to the video: %s", container_name( p->container ), audio_why );
        }
        else if (!add_stream( p, s ))
        {
            free_stream( s );
            snprintf( why, why_size, "out of memory" );
            return E_OUTOFMEMORY;
        }
    }
    /* expose in container order */
    if (p->stream_count == 2 && p->streams[0]->st->index > p->streams[1]->st->index)
    {
        struct stream *t = p->streams[0];
        p->streams[0] = p->streams[1];
        p->streams[1] = t;
        p->streams[0]->number = 0;
        p->streams[1]->number = 1;
    }
    if (!(p->pkt = av_packet_alloc()))
    {
        snprintf( why, why_size, "out of memory" );
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

static void describe_stream( const struct stream *s, char *buf, size_t size )
{
    if (s->is_video)
        snprintf( buf, size, "video %s (VideoToolbox) %dx%d %u/%u %s %s", avcodec_get_name( s->codec_id ),
                  s->native.u.video.width, s->native.u.video.height, s->native.u.video.fps_n,
                  s->native.u.video.fps_d, wgv_name( s->vformat ), s->bt709 ? "BT.709" : "BT.601" );
    else
        snprintf( buf, size, "audio %s (%s) %u Hz %u ch mask %#x %s", avcodec_get_name( s->codec_id ),
                  s->backend == BACKEND_AAC ? "AudioToolbox" : "libavcodec", s->native.u.audio.rate,
                  s->native.u.audio.channels, s->native.u.audio.channel_mask,
                  audio_format_name( s->native.u.audio.format ) );
}

static NTSTATUS connect_parser( uint64_t handle, const WCHAR *uri, UINT64 file_size )
{
    char why[256] = "", a[160] = "", b[160] = "";
    AVStream *refused = NULL;
    struct parser *p;
    NTSTATUS status;
    int score = -1, prev_owner;
    uint64_t duration = 0;
    uint32_t i;

    if (!(p = wgh_get( handle, WG_HANDLE_PARSER ))) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &p->lock );
    if (p->connected)
    {
        pthread_mutex_unlock( &p->lock );
        return VFW_E_WRONG_STATE;
    }
    pthread_mutex_lock( &p->io_lock );
    p->io_open = 1;
    p->req_pending = p->req_done = 0;
    p->file_size = file_size;
    p->pos = 0;
    pthread_mutex_unlock( &p->io_lock );

    wg_avlog_install();
    prev_owner = wg_avlog_owner( WG_AV_OWNER_PARSER );
    status = do_connect( p, file_size, why, sizeof(why), &score, &refused );
    wg_avlog_owner( prev_owner );

    if (status != S_OK)
    {
        if (refused)
            parser_log( "refused %s (score %d, %llu bytes): %s (codec id %d)", container_name( p->container ), score,
                        (unsigned long long)file_size, why, refused->codecpar->codec_id );
        else
            parser_log( "refused %s (score %d, %llu bytes): %s", container_name( p->container ), score,
                        (unsigned long long)file_size, why );
        teardown( p );
        io_close( p );   /* a failed connect ends the read protocol */
        pthread_mutex_unlock( &p->lock );
        return status;
    }

    p->connected = 1;
    for (i = 0; i < p->stream_count; i++)
        if (p->streams[i]->duration > duration) duration = p->streams[i]->duration;
    if (p->stream_count > 0) describe_stream( p->streams[0], a, sizeof(a) );
    if (p->stream_count > 1) describe_stream( p->streams[1], b, sizeof(b) );
    parser_log( "connect: %s (score %d, %llu bytes), %s%s%s, duration %.3f s%s", container_name( p->container ),
                score, (unsigned long long)file_size, a, p->stream_count > 1 ? ", " : "", b, duration / 1e7,
                p->output_compressed ? ", PCM output only" : "" );
    pthread_mutex_unlock( &p->lock );
    (void)uri;
    return S_OK;
}

/* ---------------------------------------------------------------- entry points */

NTSTATUS wg_ios_parser_create( void *args )
{
    struct wg_parser_create_params *params = args;
    struct parser *p;

    params->parser = 0;
    if (iom_env_is( "MADEIRA_WG_PARSER", "0" )) return STATUS_NOT_IMPLEMENTED;
    if (!(p = calloc( 1, sizeof(*p) ))) return E_OUTOFMEMORY;
    pthread_mutex_init( &p->lock, NULL );
    pthread_mutex_init( &p->io_lock, NULL );
    pthread_cond_init( &p->io_request_cond, NULL );
    pthread_cond_init( &p->io_done_cond, NULL );
    p->output_compressed = !!params->output_compressed;
    p->io_open = 1;   /* the PE side's read thread may ask before connect */
    if (!(p->handle = params->parser = wgh_alloc( WG_HANDLE_PARSER, p )))
    {
        pthread_mutex_destroy( &p->lock );
        pthread_mutex_destroy( &p->io_lock );
        pthread_cond_destroy( &p->io_request_cond );
        pthread_cond_destroy( &p->io_done_cond );
        free( p );
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

NTSTATUS wg_ios_parser_destroy( void *args )
{
    struct parser *p;

    if (!(p = wgh_free( *(wg_parser_t *)args, WG_HANDLE_PARSER ))) return STATUS_INVALID_HANDLE;
    io_close( p );
    pthread_mutex_lock( &p->lock );
    teardown( p );
    pthread_mutex_unlock( &p->lock );
    pthread_mutex_destroy( &p->lock );
    pthread_mutex_destroy( &p->io_lock );
    pthread_cond_destroy( &p->io_request_cond );
    pthread_cond_destroy( &p->io_done_cond );
    free( p );
    return S_OK;
}

NTSTATUS wg_ios_parser_connect( void *args )
{
    struct wg_parser_connect_params *params = args;
    return connect_parser( params->parser, params->uri, params->file_size );
}

NTSTATUS wg_ios_parser_disconnect( void *args )
{
    struct parser *p;

    if (!(p = wgh_get( *(wg_parser_t *)args, WG_HANDLE_PARSER ))) return STATUS_INVALID_HANDLE;
    /* wake both sides first: a waiting read may hold the parser lock */
    io_close( p );
    pthread_mutex_lock( &p->lock );
    teardown( p );
    pthread_mutex_unlock( &p->lock );
    return S_OK;
}

NTSTATUS wg_ios_parser_get_next_read_offset( void *args )
{
    return get_next_read_offset( args );
}

NTSTATUS wg_ios_parser_push_data( void *args )
{
    struct wg_parser_push_data_params *params = args;
    return push_data( params->parser, params->data, params->size );
}

NTSTATUS wg_ios_parser_get_stream_count( void *args )
{
    struct wg_parser_get_stream_count_params *params = args;
    struct parser *p;

    params->count = 0;
    if (!(p = wgh_get( params->parser, WG_HANDLE_PARSER ))) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &p->lock );
    params->count = p->stream_count;
    pthread_mutex_unlock( &p->lock );
    return S_OK;
}

NTSTATUS wg_ios_parser_get_stream( void *args )
{
    struct wg_parser_get_stream_params *params = args;
    struct parser *p;

    params->stream = 0;
    if (!(p = wgh_get( params->parser, WG_HANDLE_PARSER ))) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &p->lock );
    if (params->index < p->stream_count) params->stream = p->streams[params->index]->handle;
    pthread_mutex_unlock( &p->lock );
    return params->stream ? S_OK : STATUS_INVALID_PARAMETER;
}

/* a stream and its parser, locked; NULL for a bad handle */
static struct stream *lock_stream( uint64_t handle )
{
    struct stream *s;

    if (!(s = wgh_get( handle, WG_HANDLE_STREAM ))) return NULL;
    pthread_mutex_lock( &s->parser->lock );
    if (wgh_get( handle, WG_HANDLE_STREAM ) != s)   /* disconnected meanwhile */
    {
        pthread_mutex_unlock( &s->parser->lock );
        return NULL;
    }
    return s;
}

static void unlock_stream( struct stream *s )
{
    pthread_mutex_unlock( &s->parser->lock );
}

static NTSTATUS get_current_format( uint64_t handle, struct wg_format *format )
{
    struct stream *s;

    if (!format) return STATUS_INVALID_PARAMETER;
    if (!(s = lock_stream( handle )))
    {
        memset( format, 0, sizeof(*format) );
        return STATUS_INVALID_HANDLE;
    }
    *format = s->native;
    unlock_stream( s );
    return S_OK;
}

static NTSTATUS get_codec_format( uint64_t handle, struct wg_format *format )
{
    struct stream *s;

    if (!format) return STATUS_INVALID_PARAMETER;
    if (!(s = lock_stream( handle )))
    {
        memset( format, 0, sizeof(*format) );
        return STATUS_INVALID_HANDLE;
    }
    *format = s->codec_fmt;
    unlock_stream( s );
    return S_OK;
}

NTSTATUS wg_ios_parser_stream_get_current_format( void *args )
{
    struct wg_parser_stream_get_current_format_params *params = args;
    return get_current_format( params->stream, params->format );
}

NTSTATUS wg_ios_parser_stream_get_codec_format( void *args )
{
    struct wg_parser_stream_get_codec_format_params *params = args;
    return get_codec_format( params->stream, params->format );
}

static void disable_stream( struct stream *s )
{
    s->enabled = 0;
    drop_held( s );
    free_out_list( &s->out_head, &s->out_tail );
    free_queue( s );
}

/* rule 6 */
static NTSTATUS enable_stream( uint64_t handle, const struct wg_format *format )
{
    struct stream *s;
    NTSTATUS status = S_OK;

    if (!(s = lock_stream( handle ))) return STATUS_INVALID_HANDLE;
    if (!format)
    {
        disable_stream( s );
        status = STATUS_INVALID_PARAMETER;
    }
    else if (!s->is_video && format->major_type == WG_MAJOR_TYPE_AUDIO)
    {
        const typeof(format->u.audio) *a = &format->u.audio;

        if (a->format < WG_AUDIO_FORMAT_U8 || a->format > WG_AUDIO_FORMAT_F64LE || !a->rate || a->rate > 768000 ||
            !a->channels || a->channels > 8)
        {
            parser_log( "stream %u: refused audio format %u, %u Hz, %u ch", s->number, a->format, a->rate, a->channels );
            disable_stream( s );
            status = STATUS_INVALID_PARAMETER;
        }
        else
        {
            if (!s->enabled || memcmp( &s->enabled_fmt, format, sizeof(*format) ))
            {
                /* a different format drops the held buffer and the converter */
                drop_held( s );
                free_out_list( &s->out_head, &s->out_tail );
                swr_free( &s->swr );
                av_channel_layout_uninit( &s->swr_layout );
            }
            s->enabled_fmt = *format;
            s->enabled = 1;
        }
    }
    else if (s->is_video && format->major_type == WG_MAJOR_TYPE_VIDEO)
    {
        const typeof(format->u.video) *v = &format->u.video;
        enum wgv_format vf;
        int32_t h = v->height < 0 ? -v->height : v->height;

        if (!video_format_from_wg( v->format, &vf ) || v->width != s->native.u.video.width ||
            h != s->native.u.video.height)
        {
            parser_log( "stream %u: refused video format %u %dx%d (native %dx%d; no scaler)", s->number, v->format,
                        v->width, v->height, s->native.u.video.width, s->native.u.video.height );
            disable_stream( s );
            status = STATUS_INVALID_PARAMETER;
        }
        else
        {
            if (!s->enabled || memcmp( &s->enabled_fmt, format, sizeof(*format) ))
            {
                drop_held( s );
                free_out_list( &s->out_head, &s->out_tail );
            }
            s->vformat = vf;
            s->bottom_up = v->height < 0;
            s->enabled_fmt = *format;
            s->enabled = 1;
        }
    }
    else
    {
        parser_log( "stream %u: refused major type %u", s->number, format->major_type );
        disable_stream( s );
        status = STATUS_NOT_SUPPORTED;
    }
    unlock_stream( s );
    return status;
}

NTSTATUS wg_ios_parser_stream_enable( void *args )
{
    struct wg_parser_stream_enable_params *params = args;
    return enable_stream( params->stream, params->format );
}

NTSTATUS wg_ios_parser_stream_disable( void *args )
{
    struct stream *s;

    if (!(s = lock_stream( *(wg_parser_stream_t *)args ))) return STATUS_INVALID_HANDLE;
    disable_stream( s );
    unlock_stream( s );
    return S_OK;
}

static void describe_buffer( const struct stream *s, struct wg_parser_buffer *buffer )
{
    const struct out_buf *b = s->held;

    memset( buffer, 0, sizeof(*buffer) );
    buffer->pts = (UINT64)b->pts;
    buffer->duration = (UINT64)b->duration;
    buffer->size = b->size;
    buffer->stream = s->number;
    buffer->discontinuity = !!b->discontinuity;
    buffer->has_pts = 1;
    buffer->has_duration = 1;
}

static void hold_next( struct stream *s )
{
    s->held = s->out_head;
    s->out_head = s->out_head->next;
    if (!s->out_head) s->out_tail = NULL;
    s->held->next = NULL;
}

/* rule 7 */
static NTSTATUS get_buffer( uint64_t parser_handle, uint64_t stream_handle, struct wg_parser_buffer *buffer )
{
    struct stream *s, *best = NULL;
    struct parser *p;
    int prev_owner;
    uint32_t i;

    if (!buffer) return STATUS_INVALID_PARAMETER;
    if (!(p = wgh_get( parser_handle, WG_HANDLE_PARSER ))) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &p->lock );
    if (!p->connected)
    {
        pthread_mutex_unlock( &p->lock );
        return VFW_E_WRONG_STATE;
    }
    prev_owner = wg_avlog_owner( WG_AV_OWNER_PARSER );

    if (stream_handle)
    {
        if (!(s = wgh_get( stream_handle, WG_HANDLE_STREAM )) || s->parser != p)
        {
            wg_avlog_owner( prev_owner );
            pthread_mutex_unlock( &p->lock );
            return STATUS_INVALID_HANDLE;
        }
        if (s->enabled && !s->held)
        {
            fill( p, s );
            if (s->out_head) hold_next( s );
        }
        if (s->enabled && s->held) best = s;
    }
    else
    {
        /* the earliest buffer of all enabled streams by PTS */
        for (i = 0; i < p->stream_count; i++)
        {
            const struct out_buf *b;

            s = p->streams[i];
            if (!s->enabled) continue;
            if (!s->held) fill( p, s );
            b = s->held ? s->held : s->out_head;
            if (!b) continue;
            if (!best) best = s;
            else
            {
                const struct out_buf *c = best->held ? best->held : best->out_head;
                if (b->pts < c->pts) best = s;
            }
        }
        if (best && !best->held) hold_next( best );
    }

    wg_avlog_owner( prev_owner );
    if (!best)
    {
        pthread_mutex_unlock( &p->lock );
        return S_FALSE;   /* end of stream, or disabled */
    }
    describe_buffer( best, buffer );
    pthread_mutex_unlock( &p->lock );
    return S_OK;
}

NTSTATUS wg_ios_parser_stream_get_buffer( void *args )
{
    struct wg_parser_stream_get_buffer_params *params = args;
    return get_buffer( params->parser, params->stream, params->buffer );
}

static NTSTATUS copy_buffer( uint64_t handle, void *data, UINT32 offset, UINT32 size )
{
    struct stream *s;
    NTSTATUS status = S_OK;

    if (!(s = lock_stream( handle ))) return STATUS_INVALID_HANDLE;
    if (!s->held) status = VFW_E_WRONG_STATE;
    else if (offset > s->held->size || size > s->held->size - offset || (size && !data))
        status = STATUS_INVALID_PARAMETER;
    else if (size) memcpy( data, s->held->data + offset, size );
    unlock_stream( s );
    return status;
}

NTSTATUS wg_ios_parser_stream_copy_buffer( void *args )
{
    struct wg_parser_stream_copy_buffer_params *params = args;
    return copy_buffer( params->stream, params->data, params->offset, params->size );
}

NTSTATUS wg_ios_parser_stream_release_buffer( void *args )
{
    struct stream *s;

    if (!(s = lock_stream( *(wg_parser_stream_t *)args ))) return STATUS_INVALID_HANDLE;
    drop_held( s );
    unlock_stream( s );
    return S_OK;
}

NTSTATUS wg_ios_parser_stream_notify_qos( void *args )
{
    struct wg_parser_stream_notify_qos_params *params = args;

    if (!wgh_get( params->stream, WG_HANDLE_STREAM )) return STATUS_INVALID_HANDLE;
    return S_OK;
}

NTSTATUS wg_ios_parser_stream_get_duration( void *args )
{
    struct wg_parser_stream_get_duration_params *params = args;
    struct stream *s;

    params->duration = 0;
    if (!(s = lock_stream( params->stream ))) return STATUS_INVALID_HANDLE;
    params->duration = s->duration;
    unlock_stream( s );
    return S_OK;
}

NTSTATUS wg_ios_parser_stream_get_tag( void *args )
{
    struct wg_parser_stream_get_tag_params *params = args;

    if (!wgh_get( params->stream, WG_HANDLE_STREAM )) return STATUS_INVALID_HANDLE;
    return STATUS_NOT_FOUND;   /* no tags */
}

/* rule 10 */
NTSTATUS wg_ios_parser_stream_seek( void *args )
{
    struct wg_parser_stream_seek_params *params = args;
    struct stream *s, *target;
    struct parser *p;
    uint64_t duration = 0;
    int64_t ts;
    uint32_t i;
    int ret, prev_owner;

    if (!(s = lock_stream( params->stream ))) return STATUS_INVALID_HANDLE;
    p = s->parser;
    for (i = 0; i < p->stream_count; i++)
        if (p->streams[i]->duration > duration) duration = p->streams[i]->duration;

    if ((params->stop_flags & SEEK_POSITIONING_MASK) != SEEK_NO_POSITIONING)
    {
        /* a stop at or beyond the duration means none */
        p->has_stop = !(duration && params->stop_pos >= duration);
        p->stop = (int64_t)params->stop_pos;
    }
    if ((params->start_flags & SEEK_POSITIONING_MASK) == SEEK_NO_POSITIONING)
    {
        unlock_stream( s );
        return S_OK;
    }

    /* seek on the video stream if there is one, so it lands on a keyframe at or before the target */
    target = p->streams[0];
    for (i = 0; i < p->stream_count; i++) if (p->streams[i]->is_video) target = p->streams[i];

    p->read_error = 0;   /* a seek is how the PE side starts over */
    p->demux_eof = 0;
    if (p->avio)
    {
        p->avio->error = 0;          /* libavformat latches a read error in the I/O context too */
        p->avio->eof_reached = 0;
    }
    ts = av_rescale_q( (int64_t)params->start_pos, time_100ns, target->st->time_base ) + target->start_tb;
    prev_owner = wg_avlog_owner( WG_AV_OWNER_PARSER );
    ret = avformat_seek_file( p->fmt, target->st->index, INT64_MIN, ts, ts, 0 );
    wg_avlog_owner( prev_owner );
    if (ret < 0)
    {
        char err[64];
        av_strerror( ret, err, sizeof(err) );
        parser_log( "seek to %.3f s failed: %s", params->start_pos / 1e7, err );
        unlock_stream( s );
        return E_FAIL;
    }

    for (i = 0; i < p->stream_count; i++)
    {
        struct stream *t = p->streams[i];

        flush_stream( t );
        t->discontinuity = 1;
        t->has_seek_target = 1;
        t->seek_target = (int64_t)params->start_pos;
        t->has_next_pts = 1;
        t->next_pts = (int64_t)params->start_pos;
    }
    unlock_stream( s );
    return S_OK;
}

/* ---------------------------------------------------------------- 32-bit callers (A4) */

struct wg_parser_connect_params32
{
    wg_parser_t parser;
    PTR32 uri;
    UINT64 file_size;
};
_Static_assert( offsetof(struct wg_parser_connect_params32, uri) == 8, "connect32" );
_Static_assert( offsetof(struct wg_parser_connect_params32, file_size) == 16, "connect32" );
_Static_assert( sizeof(struct wg_parser_connect_params32) == 24, "connect32" );

struct wg_parser_push_data_params32
{
    wg_parser_t parser;
    PTR32 data;
    UINT32 size;
};
_Static_assert( offsetof(struct wg_parser_push_data_params32, data) == 8, "push32" );
_Static_assert( offsetof(struct wg_parser_push_data_params32, size) == 12, "push32" );
_Static_assert( sizeof(struct wg_parser_push_data_params32) == 16, "push32" );

struct wg_parser_stream_format_params32   /* get_current_format, get_codec_format, enable */
{
    wg_parser_stream_t stream;
    PTR32 format;
};
_Static_assert( offsetof(struct wg_parser_stream_format_params32, format) == 8, "format32" );
_Static_assert( sizeof(struct wg_parser_stream_format_params32) == 16, "format32" );

struct wg_parser_stream_get_buffer_params32
{
    wg_parser_t parser;
    wg_parser_stream_t stream;
    PTR32 buffer;
};
_Static_assert( offsetof(struct wg_parser_stream_get_buffer_params32, buffer) == 16, "get_buffer32" );
_Static_assert( sizeof(struct wg_parser_stream_get_buffer_params32) == 24, "get_buffer32" );

struct wg_parser_stream_copy_buffer_params32
{
    wg_parser_stream_t stream;
    PTR32 data;
    UINT32 offset;
    UINT32 size;
};
_Static_assert( offsetof(struct wg_parser_stream_copy_buffer_params32, data) == 8, "copy32" );
_Static_assert( offsetof(struct wg_parser_stream_copy_buffer_params32, offset) == 12, "copy32" );
_Static_assert( offsetof(struct wg_parser_stream_copy_buffer_params32, size) == 16, "copy32" );
_Static_assert( sizeof(struct wg_parser_stream_copy_buffer_params32) == 24, "copy32" );

struct wg_parser_stream_get_tag_params32
{
    wg_parser_stream_t stream;
    wg_parser_tag tag;
    PTR32 buffer;
    PTR32 size;
};
_Static_assert( offsetof(struct wg_parser_stream_get_tag_params32, buffer) == 16, "tag32" );
_Static_assert( offsetof(struct wg_parser_stream_get_tag_params32, size) == 20, "tag32" );
_Static_assert( sizeof(struct wg_parser_stream_get_tag_params32) == 24, "tag32" );

/* the blocks shared with the 64-bit entries have no pointers; their layouts match */
_Static_assert( sizeof(struct wg_parser_create_params) == 16, "create" );
_Static_assert( sizeof(struct wg_parser_get_next_read_offset_params) == 24, "next_read_offset" );
_Static_assert( offsetof(struct wg_parser_get_next_read_offset_params, offset) == 16, "next_read_offset" );
_Static_assert( sizeof(struct wg_parser_get_stream_count_params) == 16, "stream_count" );
_Static_assert( sizeof(struct wg_parser_get_stream_params) == 24, "get_stream" );
_Static_assert( sizeof(struct wg_parser_stream_notify_qos_params) == 40, "notify_qos" );
_Static_assert( offsetof(struct wg_parser_stream_notify_qos_params, proportion) == 16, "notify_qos" );
_Static_assert( sizeof(struct wg_parser_stream_get_duration_params) == 16, "duration" );
_Static_assert( sizeof(struct wg_parser_stream_seek_params) == 40, "seek" );

NTSTATUS wg_ios_wow64_parser_connect( void *args )
{
    struct wg_parser_connect_params32 *params32 = args;
    return connect_parser( params32->parser, iom_guest_ptr( params32->uri ), params32->file_size );
}

NTSTATUS wg_ios_wow64_parser_push_data( void *args )
{
    struct wg_parser_push_data_params32 *params32 = args;
    return push_data( params32->parser, iom_guest_ptr( params32->data ), params32->size );
}

NTSTATUS wg_ios_wow64_parser_stream_get_current_format( void *args )
{
    struct wg_parser_stream_format_params32 *params32 = args;
    return get_current_format( params32->stream, iom_guest_ptr( params32->format ) );
}

NTSTATUS wg_ios_wow64_parser_stream_get_codec_format( void *args )
{
    struct wg_parser_stream_format_params32 *params32 = args;
    return get_codec_format( params32->stream, iom_guest_ptr( params32->format ) );
}

NTSTATUS wg_ios_wow64_parser_stream_enable( void *args )
{
    struct wg_parser_stream_format_params32 *params32 = args;
    return enable_stream( params32->stream, iom_guest_ptr( params32->format ) );
}

NTSTATUS wg_ios_wow64_parser_stream_get_buffer( void *args )
{
    struct wg_parser_stream_get_buffer_params32 *params32 = args;
    return get_buffer( params32->parser, params32->stream, iom_guest_ptr( params32->buffer ) );
}

NTSTATUS wg_ios_wow64_parser_stream_copy_buffer( void *args )
{
    struct wg_parser_stream_copy_buffer_params32 *params32 = args;
    return copy_buffer( params32->stream, iom_guest_ptr( params32->data ), params32->offset, params32->size );
}

NTSTATUS wg_ios_wow64_parser_stream_get_tag( void *args )
{
    struct wg_parser_stream_get_tag_params32 *params32 = args;

    if (!wgh_get( params32->stream, WG_HANDLE_STREAM )) return STATUS_INVALID_HANDLE;
    return STATUS_NOT_FOUND;
}
