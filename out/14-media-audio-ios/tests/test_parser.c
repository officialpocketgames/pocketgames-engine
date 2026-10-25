// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F, parser, with stub platform decoders (stubs_platform.c): WAV in every PCM format,
// MPEG layer II (FFmpeg's mp2 encoder), MP3 (hand-made silent layer III frames, with and without
// ID3 cover art), a synthetic MP4 with stub H.264 (B-frame order) and AAC (late answers); the
// read protocol with a scripted reader (short reads, end of file, a read error, disconnect while
// waiting); refusals (low probe score, compressed output, PG_WG_VIDEO=0); seek with exact
// trim and stop; audio conversion and S24 packing; the reorder rule; the 32-bit table.

#if 0
#pragma makedep unix
#endif

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>

#include "wg_ios.h"
#include "check.h"
#include "test_stubs.h"
#include "test_stubs_platform.h"

extern const unixlib_entry_t winegstreamer_unix_call_funcs[];
extern const unixlib_entry_t winegstreamer_unix_call_wow64_funcs[];

#define CALL( func, args ) winegstreamer_unix_call_funcs[func]( args )
#define CALL32( func, args ) winegstreamer_unix_call_wow64_funcs[func]( args )

/* ---------------------------------------------------------------- the scripted reader (rule 1) */

struct reader
{
    wg_parser_t parser;
    const uint8_t *data;
    size_t size;
    int short_reads;          /* answer half of each request */
    int64_t error_at;         /* answer NULL once for the request covering this offset; -1: never */
    int stall;                /* take a request and never answer it */
    int wow64;                /* answer through the 32-bit table, from guest memory */
    volatile int stop;
    int requests;
    pthread_t thread;
};

static void *reader_main( void *arg )
{
    struct reader *r = arg;
    struct wg_parser_get_next_read_offset_params next;
    struct { UINT64 parser; UINT32 data; UINT32 size; } *push32 = NULL;
    struct wg_parser_push_data_params push;
    NTSTATUS status;

    if (r->wow64) push32 = test_arena_alloc( sizeof(*push32) );
    while (!r->stop)
    {
        uint64_t n;

        memset( &next, 0, sizeof(next) );
        next.parser = r->parser;
        status = CALL( unix_wg_parser_get_next_read_offset, &next );
        if (status == STATUS_INVALID_HANDLE) break;
        if (status)
        {
            usleep( 1000 );
            continue;
        }
        r->requests++;
        if (r->stall)
        {
            while (!r->stop) usleep( 1000 );
            break;
        }
        /* as quartz's read thread: clamp to the file, 0 bytes at its end */
        n = next.offset >= r->size ? 0 : next.offset + next.size > r->size ? r->size - next.offset : next.size;
        if (n && r->short_reads && n > 1) n /= 2;
        memset( &push, 0, sizeof(push) );
        push.parser = r->parser;
        push.size = (UINT32)n;
        push.data = r->data + next.offset;
        if (!n) push.data = r->data;
        if (r->error_at >= 0 && (uint64_t)r->error_at >= next.offset && (uint64_t)r->error_at < next.offset + n)
        {
            push.data = NULL;   /* a read error */
            r->error_at = -1;
        }
        if (push32)
        {
            push32->parser = push.parser;
            push32->data = push.data ? test_guest( push.data ) : 0;
            push32->size = push.size;
            CALL32( unix_wg_parser_push_data, push32 );
        }
        else CALL( unix_wg_parser_push_data, &push );
    }
    return NULL;
}

struct session
{
    wg_parser_t parser;
    struct reader rd;
};

static NTSTATUS open_session( struct session *s, const uint8_t *data, size_t size, int output_compressed,
                              int short_reads, int64_t error_at )
{
    struct wg_parser_create_params create;
    struct wg_parser_connect_params connect;

    memset( s, 0, sizeof(*s) );
    memset( &create, 0, sizeof(create) );
    create.output_compressed = output_compressed;
    CHECK_INT( CALL( unix_wg_parser_create, &create ), S_OK );
    s->parser = create.parser;
    s->rd.parser = create.parser;
    s->rd.data = data;
    s->rd.size = size;
    s->rd.short_reads = short_reads;
    s->rd.error_at = error_at;
    pthread_create( &s->rd.thread, NULL, reader_main, &s->rd );   /* before connect, as the PE side */

    memset( &connect, 0, sizeof(connect) );
    connect.parser = s->parser;
    connect.file_size = size;
    return CALL( unix_wg_parser_connect, &connect );
}

static void close_session( struct session *s )
{
    s->rd.stop = 1;
    CHECK_INT( CALL( unix_wg_parser_disconnect, &s->parser ), S_OK );
    pthread_join( s->rd.thread, NULL );
    CHECK_INT( CALL( unix_wg_parser_destroy, &s->parser ), S_OK );
}

static wg_parser_stream_t get_stream( wg_parser_t parser, UINT32 index )
{
    struct wg_parser_get_stream_params params = { parser, index, 0 };
    CALL( unix_wg_parser_get_stream, &params );
    return params.stream;
}

static UINT32 stream_count( wg_parser_t parser )
{
    struct wg_parser_get_stream_count_params params = { parser, 0 };
    CALL( unix_wg_parser_get_stream_count, &params );
    return params.count;
}

static struct wg_format current_format( wg_parser_stream_t stream )
{
    struct wg_format f;
    struct wg_parser_stream_get_current_format_params params = { stream, &f };
    memset( &f, 0xcc, sizeof(f) );
    CALL( unix_wg_parser_stream_get_current_format, &params );
    return f;
}

static NTSTATUS enable( wg_parser_stream_t stream, const struct wg_format *f )
{
    struct wg_parser_stream_enable_params params = { stream, f };
    return CALL( unix_wg_parser_stream_enable, &params );
}

struct collected
{
    uint8_t *data;
    size_t size, cap;
    int buffers, discontinuities;
    UINT64 first_pts, last_pts, last_end;
    int pts_monotonic;
};

/* reads every buffer of one stream (or of all, stream 0) */
static void collect( wg_parser_t parser, wg_parser_stream_t stream, struct collected *c, int max_buffers )
{
    struct wg_parser_stream_get_buffer_params get;
    struct wg_parser_buffer buf;

    memset( c, 0, sizeof(*c) );
    c->pts_monotonic = 1;
    while (!max_buffers || c->buffers < max_buffers)
    {
        wg_parser_stream_t owner;
        struct wg_parser_stream_copy_buffer_params copy;

        memset( &buf, 0, sizeof(buf) );
        get.parser = parser;
        get.stream = stream;
        get.buffer = &buf;
        if (CALL( unix_wg_parser_stream_get_buffer, &get ) != S_OK) break;
        CHECK( buf.has_pts && buf.has_duration );
        owner = stream ? stream : get_stream( parser, buf.stream );
        if (c->size + buf.size > c->cap)
        {
            c->cap = (c->cap ? c->cap * 2 : 1 << 20) + buf.size;
            c->data = realloc( c->data, c->cap );
        }
        copy.stream = owner;
        copy.data = c->data + c->size;
        copy.offset = 0;
        copy.size = buf.size;
        CHECK_INT( CALL( unix_wg_parser_stream_copy_buffer, &copy ), S_OK );
        copy.offset = buf.size;
        copy.size = 1;
        CHECK_INT( CALL( unix_wg_parser_stream_copy_buffer, &copy ), STATUS_INVALID_PARAMETER );   /* checked */
        if (!c->buffers) c->first_pts = buf.pts;
        else if (buf.pts < c->last_pts) c->pts_monotonic = 0;
        c->last_pts = buf.pts;
        c->last_end = buf.pts + buf.duration;
        if (buf.discontinuity) c->discontinuities++;
        c->size += buf.size;
        c->buffers++;
        CALL( unix_wg_parser_stream_release_buffer, &owner );
    }
}

/* ---------------------------------------------------------------- media */

static void put16( uint8_t *p, uint32_t v ) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32( uint8_t *p, uint32_t v ) { put16( p, v & 0xffff ); put16( p + 2, v >> 16 ); }

struct wav { uint8_t *file; size_t size; const uint8_t *pcm; size_t pcm_size; };

static void make_wav( struct wav *w, wg_audio_format format, int rate, int channels, int frames )
{
    static const int bits[] = { 0, 8, 16, 24, 32, 32, 64 };
    int b = bits[format], align = channels * b / 8, i, c;
    size_t data = (size_t)frames * align;
    uint8_t *p;

    w->size = 44 + data;
    w->file = p = malloc( w->size );
    memcpy( p, "RIFF", 4 ); put32( p + 4, (uint32_t)(36 + data ) ); memcpy( p + 8, "WAVEfmt ", 8 );
    put32( p + 16, 16 );
    put16( p + 20, format >= WG_AUDIO_FORMAT_F32LE ? 3 : 1 );
    put16( p + 22, channels );
    put32( p + 24, rate );
    put32( p + 28, rate * align );
    put16( p + 32, align );
    put16( p + 34, b );
    memcpy( p + 36, "data", 4 ); put32( p + 40, (uint32_t)data );
    w->pcm = p + 44;
    w->pcm_size = data;
    p += 44;
    for (i = 0; i < frames; i++)
        for (c = 0; c < channels; c++)
        {
            switch (format)
            {
            case WG_AUDIO_FORMAT_U8: *p++ = (uint8_t)(i * 7 + c * 50); break;
            case WG_AUDIO_FORMAT_S16LE: put16( p, (uint32_t)(i * 37 + c * 1000) ); p += 2; break;
            case WG_AUDIO_FORMAT_S24LE: { uint32_t v = (uint32_t)(i * 12345 + c * 999); p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p += 3; break; }
            case WG_AUDIO_FORMAT_S32LE: put32( p, (uint32_t)(i * 123457u + c * 99991u) ); p += 4; break;
            case WG_AUDIO_FORMAT_F32LE: { float v = 0.5f * sinf( i * 0.01f + c ); memcpy( p, &v, 4 ); p += 4; break; }
            case WG_AUDIO_FORMAT_F64LE: { double v = 0.5 * sin( i * 0.01 + c ); memcpy( p, &v, 8 ); p += 8; break; }
            default: break;
            }
        }
}

/* raw MPEG-1 layer II frames of a 440 Hz tone, from FFmpeg's mp2 encoder */
static int make_mp2( uint8_t **out, size_t *size, int rate, int seconds )
{
    const AVCodec *codec = avcodec_find_encoder( AV_CODEC_ID_MP2 );
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pkt;
    int64_t done = 0, total = (int64_t)rate * seconds;
    size_t cap = 1 << 20;
    int ret, i;

    *size = 0;
    if (!codec || !(ctx = avcodec_alloc_context3( codec ))) return -1;
    ctx->sample_rate = rate;
    ctx->bit_rate = 192000;
    ctx->sample_fmt = AV_SAMPLE_FMT_S16;
    av_channel_layout_default( &ctx->ch_layout, 2 );
    if (avcodec_open2( ctx, codec, NULL ) < 0) return -1;
    *out = malloc( cap );
    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    for (;;)
    {
        if (done < total)
        {
            frame->nb_samples = ctx->frame_size;
            frame->format = AV_SAMPLE_FMT_S16;
            av_channel_layout_copy( &frame->ch_layout, &ctx->ch_layout );
            frame->sample_rate = rate;
            av_frame_get_buffer( frame, 0 );
            for (i = 0; i < frame->nb_samples; i++)
            {
                int16_t v = (int16_t)(16384 * sin( 2 * M_PI * 440.0 * (done + i) / rate ));
                ((int16_t *)frame->data[0])[2 * i] = v;
                ((int16_t *)frame->data[0])[2 * i + 1] = v;
            }
            frame->pts = done;
            done += frame->nb_samples;
            avcodec_send_frame( ctx, frame );
            av_frame_unref( frame );
        }
        else avcodec_send_frame( ctx, NULL );
        while ((ret = avcodec_receive_packet( ctx, pkt )) >= 0)
        {
            if (*size + pkt->size > cap) *out = realloc( *out, cap = cap * 2 + pkt->size );
            memcpy( *out + *size, pkt->data, pkt->size );
            *size += pkt->size;
            av_packet_unref( pkt );
        }
        if (ret == AVERROR_EOF) break;
    }
    av_packet_free( &pkt );
    av_frame_free( &frame );
    avcodec_free_context( &ctx );
    return 0;
}

/* silent MPEG-1 layer III frames, 128 kbit/s, 44100 Hz, stereo; optional ID3v2 cover art */
static size_t make_mp3( uint8_t **out, int frames, int cover )
{
    static const uint8_t header[4] = { 0xff, 0xfb, 0x90, 0x00 };
    size_t frame_size = 417, tag = 0, i;
    uint8_t *p;

    *out = p = calloc( 1, 2048 + frames * frame_size );
    if (cover)
    {
        static const char mime[] = "image/png";
        static const uint8_t png[16] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
        size_t body = 1 + sizeof(mime) + 1 + 1 + sizeof(png), total = 10 + body;
        memcpy( p, "ID3\x03\x00\x00", 6 );
        p[6] = (total >> 21) & 0x7f; p[7] = (total >> 14) & 0x7f; p[8] = (total >> 7) & 0x7f; p[9] = total & 0x7f;
        memcpy( p + 10, "APIC", 4 );
        p[14] = (body >> 24) & 0xff; p[15] = (body >> 16) & 0xff; p[16] = (body >> 8) & 0xff; p[17] = body & 0xff;
        p[18] = p[19] = 0;
        p[20] = 0;                                   /* text encoding */
        memcpy( p + 21, mime, sizeof(mime) );        /* with its NUL */
        p[21 + sizeof(mime)] = 3;                    /* front cover */
        p[22 + sizeof(mime)] = 0;                    /* empty description */
        memcpy( p + 23 + sizeof(mime), png, sizeof(png) );
        tag = 10 + total;
    }
    for (i = 0; i < (size_t)frames; i++) memcpy( p + tag + i * frame_size, header, 4 );
    return tag + frames * frame_size;
}

/* ---- a synthetic MP4: stub H.264 (B-frame order) and AAC, written with libavformat in memory */

struct membuf { uint8_t *data; size_t size, cap, pos; };

#if LIBAVFORMAT_VERSION_MAJOR < 61
#define AVIO_WBUF uint8_t
#else
#define AVIO_WBUF const uint8_t
#endif

static int mem_write( void *opaque, AVIO_WBUF *buf, int size )
{
    struct membuf *m = opaque;
    if (m->pos + size > m->cap)
    {
        m->cap = (m->pos + size) * 2;
        m->data = realloc( m->data, m->cap );
    }
    memcpy( m->data + m->pos, buf, size );
    m->pos += size;
    if (m->pos > m->size) m->size = m->pos;
    return size;
}

static int64_t mem_seek( void *opaque, int64_t offset, int whence )
{
    struct membuf *m = opaque;
    if (whence & AVSEEK_SIZE) return m->size;
    switch (whence & ~AVSEEK_FORCE)
    {
    case SEEK_SET: m->pos = offset; break;
    case SEEK_CUR: m->pos += offset; break;
    case SEEK_END: m->pos = m->size + offset; break;
    default: return -1;
    }
    return m->pos;
}

static const uint8_t test_avcc[] = { 0x01, 0x64, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x04, 0x67, 0x64, 0x00, 0x1f,
                                     0x01, 0x00, 0x03, 0x68, 0xee, 0x3c };
static const uint8_t test_asc[] = { 0x12, 0x10 };
/* presentation index of each video packet in decode order */
static const int decode_order[] = { 0, 3, 1, 2, 6, 4, 5, 9, 7, 8 };
#define VIDEO_FRAMES 10
#define AUDIO_PACKETS 20

static size_t make_mp4( uint8_t **out, int with_audio )
{
    AVFormatContext *oc = NULL;
    AVStream *vs, *as = NULL;
    struct membuf m = { 0 };
    uint8_t *iobuf = av_malloc( 4096 );
    AVPacket *pkt = av_packet_alloc();
    int i;

    avformat_alloc_output_context2( &oc, NULL, "mp4", NULL );
    oc->pb = avio_alloc_context( iobuf, 4096, 1, &m, NULL, mem_write, mem_seek );
    oc->pb->seekable = AVIO_SEEKABLE_NORMAL;

    vs = avformat_new_stream( oc, NULL );
    vs->time_base = (AVRational){ 1, 30 };
    vs->avg_frame_rate = (AVRational){ 30, 1 };
    vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vs->codecpar->codec_id = AV_CODEC_ID_H264;
    vs->codecpar->width = 64;
    vs->codecpar->height = 48;
    vs->codecpar->extradata = av_mallocz( sizeof(test_avcc) + AV_INPUT_BUFFER_PADDING_SIZE );
    memcpy( vs->codecpar->extradata, test_avcc, sizeof(test_avcc) );
    vs->codecpar->extradata_size = sizeof(test_avcc);
    if (with_audio)
    {
        as = avformat_new_stream( oc, NULL );
        as->time_base = (AVRational){ 1, 44100 };
        as->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        as->codecpar->codec_id = AV_CODEC_ID_AAC;
        as->codecpar->sample_rate = 44100;
        as->codecpar->frame_size = 1024;
        av_channel_layout_default( &as->codecpar->ch_layout, 2 );
        as->codecpar->extradata = av_mallocz( sizeof(test_asc) + AV_INPUT_BUFFER_PADDING_SIZE );
        memcpy( as->codecpar->extradata, test_asc, sizeof(test_asc) );
        as->codecpar->extradata_size = sizeof(test_asc);
    }
    CHECK( avformat_write_header( oc, NULL ) >= 0 );

    for (i = 0; i < VIDEO_FRAMES; i++)
    {
        av_new_packet( pkt, 8 );
        memset( pkt->data, 0, 8 );
        pkt->data[0] = (uint8_t)(decode_order[i] * 10);   /* the picture's luma */
        pkt->stream_index = vs->index;
        pkt->dts = av_rescale_q( i, (AVRational){ 1, 30 }, vs->time_base );
        pkt->pts = av_rescale_q( decode_order[i] + 2, (AVRational){ 1, 30 }, vs->time_base );
        pkt->duration = av_rescale_q( 1, (AVRational){ 1, 30 }, vs->time_base );
        if (!i) pkt->flags |= AV_PKT_FLAG_KEY;
        CHECK( av_interleaved_write_frame( oc, pkt ) >= 0 );
    }
    for (i = 0; as && i < AUDIO_PACKETS; i++)
    {
        av_new_packet( pkt, 10 );
        memset( pkt->data, 0, 10 );
        pkt->data[0] = (uint8_t)i;
        pkt->stream_index = as->index;
        pkt->dts = pkt->pts = av_rescale_q( (int64_t)i * 1024, (AVRational){ 1, 44100 }, as->time_base );
        pkt->duration = av_rescale_q( 1024, (AVRational){ 1, 44100 }, as->time_base );
        pkt->flags |= AV_PKT_FLAG_KEY;
        CHECK( av_interleaved_write_frame( oc, pkt ) >= 0 );
    }
    CHECK( av_write_trailer( oc ) >= 0 );
    av_packet_free( &pkt );
    av_freep( &oc->pb->buffer );
    avio_context_free( &oc->pb );
    avformat_free_context( oc );
    *out = m.data;
    return m.size;
}

/* ---------------------------------------------------------------- tests */

static void test_wav_formats( void )
{
    wg_audio_format f;

    for (f = WG_AUDIO_FORMAT_U8; f <= WG_AUDIO_FORMAT_F64LE; f++)
    {
        struct session s;
        struct collected c;
        struct wav w;
        struct wg_format fmt;
        wg_parser_stream_t st;

        make_wav( &w, f, 48000, 2, 48000 );
        CHECK_INT( open_session( &s, w.file, w.size, 0, f == WG_AUDIO_FORMAT_S16LE, -1 ), S_OK );
        CHECK_INT( stream_count( s.parser ), 1 );
        st = get_stream( s.parser, 0 );
        fmt = current_format( st );
        CHECK_INT( fmt.major_type, WG_MAJOR_TYPE_AUDIO );
        CHECK_INT( fmt.u.audio.format, f );
        CHECK_INT( fmt.u.audio.channels, 2 );
        CHECK_INT( fmt.u.audio.rate, 48000 );
        CHECK_INT( fmt.u.audio.channel_mask, 3 );
        {
            struct wg_parser_stream_get_duration_params d = { st, 0 };
            CALL( unix_wg_parser_stream_get_duration, &d );
            CHECK_NEAR( (double)d.duration, 10000000.0, 1000.0 );
        }
        CHECK_INT( enable( st, &fmt ), S_OK );
        collect( s.parser, st, &c, 0 );
        /* the native format: a WAVE parser hands PCM on unchanged (S24 packed back to 3 bytes) */
        CHECK_INT( c.size, w.pcm_size );
        if (c.size == w.pcm_size) CHECK_MEM( c.data, w.pcm, w.pcm_size );
        CHECK_INT( c.first_pts, 0 );
        CHECK( c.pts_monotonic );
        CHECK_INT( c.discontinuities, 1 );   /* the first buffer */
        CHECK_NEAR( (double)c.last_end, 10000000.0, 1.0 );
        free( c.data );

        /* a conversion: to s16, 44100 Hz, mono */
        if (f == WG_AUDIO_FORMAT_F32LE)
        {
            struct wg_parser_stream_seek_params seek = { st, 1.0, 0, 0, 1, 0 };
            struct wg_format s16 = fmt;

            s16.u.audio.format = WG_AUDIO_FORMAT_S16LE;
            CHECK_INT( enable( st, &s16 ), S_OK );
            CHECK_INT( CALL( unix_wg_parser_stream_seek, &seek ), S_OK );
            collect( s.parser, st, &c, 0 );
            CHECK_INT( c.size, 48000 * 4 );
            if (c.size >= 4000)
                CHECK_NEAR( ((int16_t *)c.data)[2 * 500], 32768.0 * 0.5 * sin( 500 * 0.01 ), 2.0 );
            free( c.data );
            s16.u.audio.rate = 44100;
            s16.u.audio.channels = 1;
            s16.u.audio.channel_mask = 4;
            CHECK_INT( enable( st, &s16 ), S_OK );
            CHECK_INT( CALL( unix_wg_parser_stream_seek, &seek ), S_OK );
            collect( s.parser, st, &c, 0 );
            CHECK_NEAR( (double)c.size, 44100.0 * 2, 2 * 256.0 );
            free( c.data );
        }
        /* a refused format disables the stream */
        if (f == WG_AUDIO_FORMAT_U8)
        {
            struct wg_format bad = fmt;
            struct wg_parser_stream_get_buffer_params get;
            struct wg_parser_buffer buf;

            bad.u.audio.channels = 9;
            CHECK_INT( enable( st, &bad ), STATUS_INVALID_PARAMETER );
            get.parser = s.parser;
            get.stream = st;
            get.buffer = &buf;
            CHECK_INT( CALL( unix_wg_parser_stream_get_buffer, &get ), S_FALSE );
            bad.major_type = WG_MAJOR_TYPE_VIDEO;
            CHECK_INT( enable( st, &bad ), STATUS_NOT_SUPPORTED );
            bad.major_type = WG_MAJOR_TYPE_AUDIO_MPEG1;
            CHECK_INT( enable( st, &bad ), STATUS_NOT_SUPPORTED );
        }
        close_session( &s );
        free( w.file );
    }
}

static void test_seek_trim_stop( void )
{
    struct session s;
    struct collected c;
    struct wav w;
    struct wg_format fmt;
    wg_parser_stream_t st;
    struct wg_parser_stream_seek_params seek;

    make_wav( &w, WG_AUDIO_FORMAT_S16LE, 48000, 2, 96000 );
    CHECK_INT( open_session( &s, w.file, w.size, 0, 0, -1 ), S_OK );
    st = get_stream( s.parser, 0 );
    fmt = current_format( st );
    CHECK_INT( enable( st, &fmt ), S_OK );

    memset( &seek, 0, sizeof(seek) );
    seek.stream = st;
    seek.rate = 1.0;
    seek.start_pos = 5000000;   /* 0.5 s */
    seek.stop_pos = 7500000;    /* 0.75 s */
    seek.start_flags = 1;       /* absolute */
    seek.stop_flags = 1;
    CHECK_INT( CALL( unix_wg_parser_stream_seek, &seek ), S_OK );
    collect( s.parser, st, &c, 0 );
    CHECK_INT( c.first_pts, 5000000 );
    CHECK_INT( c.size, 12000 * 4 );
    if (c.size >= 4) CHECK_MEM( c.data, w.pcm + 24000 * 4, 4 );
    CHECK_INT( c.discontinuities, 1 );
    CHECK_NEAR( (double)c.last_end, 7500000.0, 1.0 );
    free( c.data );

    /* an unaligned start: trimmed to the sample */
    seek.start_pos = 5000001;
    seek.stop_flags = 0;        /* keeps the stop */
    CHECK_INT( CALL( unix_wg_parser_stream_seek, &seek ), S_OK );
    collect( s.parser, st, &c, 0 );
    CHECK_INT( c.size, (36000 - 24001) * 4 );
    if (c.size >= 4) CHECK_MEM( c.data, w.pcm + 24001 * 4, 4 );
    free( c.data );

    /* a stop at or beyond the duration means none */
    seek.start_pos = 0;
    seek.stop_pos = 20000000;
    seek.stop_flags = 1;
    CHECK_INT( CALL( unix_wg_parser_stream_seek, &seek ), S_OK );
    collect( s.parser, st, &c, 0 );
    CHECK_INT( c.size, 96000 * 4 );
    free( c.data );

    {
        struct wg_parser_stream_get_tag_params tag;
        UINT32 size = 0;
        memset( &tag, 0, sizeof(tag) );
        tag.stream = st;
        tag.size = &size;
        CHECK_INT( CALL( unix_wg_parser_stream_get_tag, &tag ), STATUS_NOT_FOUND );
    }
    close_session( &s );
    free( w.file );
}

static void test_read_protocol( void )
{
    struct session s;
    struct collected c;
    struct wav w;
    struct wg_format fmt;
    wg_parser_stream_t st;

    make_wav( &w, WG_AUDIO_FORMAT_S16LE, 48000, 2, 96000 );

    /* a read error in the middle: the stream ends there; a seek starts over */
    CHECK_INT( open_session( &s, w.file, w.size, 0, 0, 44 + 96000 * 2 ), S_OK );
    st = get_stream( s.parser, 0 );
    fmt = current_format( st );
    CHECK_INT( enable( st, &fmt ), S_OK );
    collect( s.parser, st, &c, 0 );
    CHECK( c.size > 0 && c.size < w.pcm_size );
    free( c.data );
    {
        struct wg_parser_stream_seek_params seek = { st, 1.0, 0, 0, 1, 0 };
        CHECK_INT( CALL( unix_wg_parser_stream_seek, &seek ), S_OK );
    }
    collect( s.parser, st, &c, 0 );
    CHECK_INT( c.size, w.pcm_size );
    free( c.data );

    /* push_data with no request pending */
    {
        struct wg_parser_push_data_params push = { s.parser, w.file, 10 };
        CHECK_INT( CALL( unix_wg_parser_push_data, &push ), VFW_E_WRONG_STATE );
    }
    close_session( &s );

    /* disconnect while a read waits: connect fails, the protocol ends */
    {
        struct wg_parser_create_params create;
        struct wg_parser_connect_params connect;
        struct wg_parser_get_next_read_offset_params next;
        pthread_t t;
        NTSTATUS result = 0;

        memset( &create, 0, sizeof(create) );
        CHECK_INT( CALL( unix_wg_parser_create, &create ), S_OK );
        memset( &s, 0, sizeof(s) );
        s.parser = create.parser;
        s.rd.parser = create.parser;
        s.rd.data = w.file;
        s.rd.size = w.size;
        s.rd.error_at = -1;
        s.rd.stall = 1;
        pthread_create( &s.rd.thread, NULL, reader_main, &s.rd );
        connect.parser = create.parser;
        connect.file_size = w.size;
        connect.uri = NULL;
        {
            struct { struct wg_parser_connect_params *p; NTSTATUS *r; } arg = { &connect, &result };
            void *connect_main( void *a );
            pthread_create( &t, NULL, connect_main, &arg );
            while (!s.rd.requests) usleep( 1000 );
            usleep( 20000 );
            CHECK_INT( CALL( unix_wg_parser_disconnect, &create.parser ), S_OK );
            pthread_join( t, NULL );
        }
        CHECK( result != S_OK );
        next.parser = create.parser;
        CHECK_INT( CALL( unix_wg_parser_get_next_read_offset, &next ), VFW_E_WRONG_STATE );
        s.rd.stop = 1;
        pthread_join( s.rd.thread, NULL );
        CHECK_INT( CALL( unix_wg_parser_destroy, &create.parser ), S_OK );
    }
    free( w.file );
}

void *connect_main( void *a )
{
    struct { struct wg_parser_connect_params *p; NTSTATUS *r; } *arg = a;
    *arg->r = CALL( unix_wg_parser_connect, arg->p );
    return NULL;
}

static void test_mpeg_audio( void )
{
    struct session s;
    struct collected c;
    struct wg_format fmt, codec;
    wg_parser_stream_t st;
    uint8_t *data;
    size_t size;

    /* layer II with a 440 Hz tone */
    if (!make_mp2( &data, &size, 44100, 2 ))
    {
        CHECK_INT( open_session( &s, data, size, 0, 0, -1 ), S_OK );
        st = get_stream( s.parser, 0 );
        fmt = current_format( st );
        CHECK_INT( fmt.u.audio.format, WG_AUDIO_FORMAT_S16LE );
        CHECK_INT( fmt.u.audio.rate, 44100 );
        CHECK_INT( fmt.u.audio.channels, 2 );
        {
            struct wg_parser_stream_get_codec_format_params p = { st, &codec };
            CALL( unix_wg_parser_stream_get_codec_format, &p );
            CHECK_INT( codec.major_type, WG_MAJOR_TYPE_AUDIO_MPEG1 );
            CHECK_INT( codec.u.audio.layer, 2 );
        }
        CHECK_INT( enable( st, &fmt ), S_OK );
        collect( s.parser, st, &c, 0 );
        CHECK_NEAR( (double)c.size / 4, 2.0 * 44100, 4000.0 );
        {
            size_t frames = c.size / 4, i, crossings = 0;
            int16_t prev = 0;
            for (i = 4410; i + 4410 < frames; i++)
            {
                int16_t v = ((int16_t *)c.data)[2 * i];
                if (prev < 0 && v >= 0) crossings++;
                prev = v;
            }
            CHECK_NEAR( crossings * 44100.0 / (frames - 8820), 440.0, 5.0 );
        }
        free( c.data );
        close_session( &s );

        /* a compressed-output splitter refuses it before analysis */
        CHECK( open_session( &s, data, size, 1, 0, -1 ) != S_OK );
        close_session( &s );
        free( data );
    }
    else fprintf( stderr, "no mp2 encoder in the host FFmpeg: layer II test skipped\n" );

    /* layer III, silent, with and without cover art (which is left out of analysis) */
    {
        int cover;
        for (cover = 0; cover < 2; cover++)
        {
            size = make_mp3( &data, 200, cover );
            CHECK_INT( open_session( &s, data, size, 0, 0, -1 ), S_OK );
            CHECK_INT( stream_count( s.parser ), 1 );
            st = get_stream( s.parser, 0 );
            fmt = current_format( st );
            CHECK_INT( fmt.major_type, WG_MAJOR_TYPE_AUDIO );
            CHECK_INT( fmt.u.audio.rate, 44100 );
            CHECK_INT( enable( st, &fmt ), S_OK );
            collect( s.parser, st, &c, 0 );
            CHECK_NEAR( (double)c.size / 4, 200.0 * 1152, 3 * 1152.0 );
            CHECK( c.first_pts < 10000000 / 44100 * 1152 );
            free( c.data );
            close_session( &s );
            free( data );
        }
    }
}

static void test_refusals( void )
{
    struct session s;
    struct wav w;
    uint8_t text[8192];
    size_t i;

    for (i = 0; i < sizeof(text); i++) text[i] = "the quick brown fox jumps over the lazy dog\n"[i % 44];
    test_log_clear();
    CHECK_INT( open_session( &s, text, sizeof(text), 0, 0, -1 ), VFW_E_UNSUPPORTED_STREAM );
    CHECK( test_log_count( "[wg-parser] refused" ) == 1 );
    close_session( &s );

    /* a compressed-output (WAVE) parser accepts PCM */
    make_wav( &w, WG_AUDIO_FORMAT_S16LE, 22050, 1, 2205 );
    CHECK_INT( open_session( &s, w.file, w.size, 1, 0, -1 ), S_OK );
    close_session( &s );
    free( w.file );

    setenv( "PG_WG_PARSER", "0", 1 );
    {
        struct wg_parser_create_params create;
        memset( &create, 0, sizeof(create) );
        CHECK_INT( CALL( unix_wg_parser_create, &create ), STATUS_NOT_IMPLEMENTED );
    }
    unsetenv( "PG_WG_PARSER" );
}

static void test_mp4( void )
{
    struct session s;
    struct collected c;
    struct wg_format v, a, codec;
    wg_parser_stream_t vs, as;
    uint8_t *data;
    size_t size = make_mp4( &data, 1 ), i;

    CHECK_INT( open_session( &s, data, size, 0, 0, -1 ), S_OK );
    CHECK_INT( stream_count( s.parser ), 2 );
    vs = get_stream( s.parser, 0 );   /* container order: video first */
    as = get_stream( s.parser, 1 );
    v = current_format( vs );
    a = current_format( as );
    CHECK_INT( v.major_type, WG_MAJOR_TYPE_VIDEO );
    CHECK_INT( v.u.video.format, WG_VIDEO_FORMAT_NV12 );
    CHECK_INT( v.u.video.width, 64 );
    CHECK_INT( v.u.video.height, 48 );
    CHECK_INT( v.u.video.fps_n, 30 );
    CHECK_INT( v.u.video.fps_d, 1 );
    CHECK_INT( a.major_type, WG_MAJOR_TYPE_AUDIO );
    CHECK_INT( a.u.audio.format, WG_AUDIO_FORMAT_F32LE );
    CHECK_INT( a.u.audio.rate, 44100 );
    CHECK_INT( a.u.audio.channels, 2 );
    CHECK_INT( a.u.audio.channel_mask, 3 );
    {
        struct wg_parser_stream_get_codec_format_params p = { vs, &codec };
        CALL( unix_wg_parser_stream_get_codec_format, &p );
        CHECK_INT( codec.major_type, WG_MAJOR_TYPE_VIDEO_H264 );
        CHECK_INT( codec.u.video.profile, 100 );
        CHECK_INT( codec.u.video.level, 31 );
        CHECK_INT( codec.u.video.codec_data_len, 0 );   /* no Annex B codec data */
        p.stream = as;
        CALL( unix_wg_parser_stream_get_codec_format, &p );
        CHECK_INT( codec.major_type, WG_MAJOR_TYPE_AUDIO_MPEG4 );
        CHECK_INT( codec.u.audio.payload_type, 0 );
        CHECK_INT( codec.u.audio.codec_data_len, 2 );
        CHECK_MEM( codec.u.audio.codec_data, test_asc, 2 );
    }

    /* video: presentation order out of B-frame decode order (the reorder rule) */
    CHECK_INT( enable( vs, &v ), S_OK );
    CHECK_INT( enable( as, &a ), S_OK );
    collect( s.parser, vs, &c, 0 );
    CHECK_INT( c.buffers, VIDEO_FRAMES );
    CHECK( c.pts_monotonic );
    for (i = 0; i < (size_t)c.buffers && i < VIDEO_FRAMES; i++)
        CHECK_INT( c.data[i * (64 * 48 * 3 / 2)], i * 10 );   /* the luma of picture i */
    CHECK_INT( stub_pictures_alive, 0 );
    free( c.data );

    /* audio was queued meanwhile; answered one packet late, each with the oldest PTS */
    collect( s.parser, as, &c, 0 );
    CHECK_INT( c.buffers, AUDIO_PACKETS - 1 );
    CHECK_INT( c.first_pts, 0 );
    for (i = 0; i < (size_t)c.buffers; i++)
        CHECK_NEAR( ((float *)c.data)[i * 2048], i / 100.0, 1e-6 );
    CHECK_NEAR( (double)c.last_pts, (AUDIO_PACKETS - 2) * 1024 * 1e7 / 44100, 2.0 );
    free( c.data );

    /* stream 0: the earliest buffer of all enabled streams */
    {
        struct wg_parser_stream_seek_params seek = { vs, 1.0, 0, 0, 1, 0 };
        CHECK_INT( CALL( unix_wg_parser_stream_seek, &seek ), S_OK );
        CHECK( stub_video_flushes > 0 && stub_aac_flushes > 0 );
        collect( s.parser, 0, &c, 0 );
        CHECK_INT( c.buffers, VIDEO_FRAMES + AUDIO_PACKETS - 1 );
        CHECK( c.pts_monotonic );
        free( c.data );
    }

    /* a native-size video format in another layout, bottom-up */
    {
        struct wg_format rgb = v;
        rgb.u.video.format = WG_VIDEO_FORMAT_BGRx;
        rgb.u.video.height = -48;
        CHECK_INT( enable( vs, &rgb ), S_OK );
        rgb.u.video.width = 32;   /* no scaler */
        CHECK_INT( enable( vs, &rgb ), STATUS_INVALID_PARAMETER );
    }
    close_session( &s );
    CHECK_INT( stub_pictures_alive, 0 );

    setenv( "PG_WG_VIDEO", "0", 1 );
    CHECK( open_session( &s, data, size, 0, 0, -1 ) != S_OK );
    close_session( &s );
    unsetenv( "PG_WG_VIDEO" );
    free( data );
}

/* the 32-bit table: connect, push, formats, buffers and copies with guest addresses (A4) */
static void test_wow64( void )
{
    struct { UINT64 parser; UINT32 uri; UINT64 file_size; } *connect32;
    struct { UINT64 stream; UINT32 format; } *format32;
    struct { UINT64 parser; UINT64 stream; UINT32 buffer; } *get32;
    struct { UINT64 stream; UINT32 data; UINT32 offset; UINT32 size; } *copy32;
    struct wg_parser_create_params create;
    struct wg_parser_buffer *buf;
    struct wg_format *fmt;
    struct reader rd;
    struct wav w;
    uint8_t *file, *dst;
    size_t got = 0;

    make_wav( &w, WG_AUDIO_FORMAT_S16LE, 22050, 2, 22050 );
    test_enter_window();
    connect32 = test_arena_alloc( sizeof(*connect32) );
    format32 = test_arena_alloc( sizeof(*format32) );
    get32 = test_arena_alloc( sizeof(*get32) );
    copy32 = test_arena_alloc( sizeof(*copy32) );
    buf = test_arena_alloc( sizeof(*buf) );
    fmt = test_arena_alloc( sizeof(*fmt) );
    file = test_arena_alloc( w.size );
    dst = test_arena_alloc( w.pcm_size + 65536 );
    memcpy( file, w.file, w.size );

    memset( &create, 0, sizeof(create) );
    CHECK_INT( CALL32( unix_wg_parser_create, &create ), S_OK );
    memset( &rd, 0, sizeof(rd) );
    rd.parser = create.parser;
    rd.data = file;
    rd.size = w.size;
    rd.error_at = -1;
    rd.wow64 = 1;
    pthread_create( &rd.thread, NULL, reader_main, &rd );
    connect32->parser = create.parser;
    connect32->uri = 0;
    connect32->file_size = w.size;
    CHECK_INT( CALL32( unix_wg_parser_connect, connect32 ), S_OK );

    format32->stream = get_stream( create.parser, 0 );
    format32->format = test_guest( fmt );
    CHECK_INT( CALL32( unix_wg_parser_stream_get_current_format, format32 ), S_OK );
    CHECK_INT( fmt->u.audio.rate, 22050 );
    CHECK_INT( CALL32( unix_wg_parser_stream_enable, format32 ), S_OK );
    for (;;)
    {
        get32->parser = create.parser;
        get32->stream = format32->stream;
        get32->buffer = test_guest( buf );
        if (CALL32( unix_wg_parser_stream_get_buffer, get32 ) != S_OK) break;
        copy32->stream = format32->stream;
        copy32->data = test_guest( dst + got );
        copy32->offset = 0;
        copy32->size = buf->size;
        CHECK_INT( CALL32( unix_wg_parser_stream_copy_buffer, copy32 ), S_OK );
        got += buf->size;
        CALL32( unix_wg_parser_stream_release_buffer, &format32->stream );
    }
    CHECK_INT( got, w.pcm_size );
    CHECK_MEM( dst, w.pcm, w.pcm_size );

    rd.stop = 1;
    CALL32( unix_wg_parser_disconnect, &create.parser );
    pthread_join( rd.thread, NULL );
    CALL32( unix_wg_parser_destroy, &create.parser );
    test_leave_window();
    free( w.file );
}

int main( void )
{
    test_wav_formats();
    test_seek_trim_stop();
    test_read_protocol();
    test_mpeg_audio();
    test_refusals();
    test_mp4();
    test_wow64();
    CHECK_INT( stub_pictures_alive, 0 );
    return test_report( "test_parser" );
}
