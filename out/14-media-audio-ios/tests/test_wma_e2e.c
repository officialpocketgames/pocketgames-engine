// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F, WMA end to end: WMA v1 and v2 streams of a 440 Hz tone made with FFmpeg's own
// encoders at several rates and channel counts, decoded through the wg_transform entries as
// wma_decoder.c calls them (create, push until MF_E_NOTACCEPTING, read until
// MF_E_TRANSFORM_NEED_MORE_INPUT, drain), to s16 and float, search on and off, back-pressure,
// whole-frame reads, flush, the refusals, and the same through the 32-bit table. The 440 Hz
// fundamental and the level must come back.

#if 0
#pragma makedep unix
#endif

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include "wg_ios.h"
#include "check.h"
#include "test_stubs.h"

extern const unixlib_entry_t winegstreamer_unix_call_funcs[];
extern const unixlib_entry_t winegstreamer_unix_call_wow64_funcs[];

#define CALL( func, args ) winegstreamer_unix_call_funcs[func]( args )
#define CALL32( func, args ) winegstreamer_unix_call_wow64_funcs[func]( args )

struct encoded
{
    uint8_t *data;
    size_t size;
    uint32_t block_align;
    uint8_t extradata[16];
    int extradata_size;
};

/* a 440 Hz tone at amplitude 0.5, `seconds` long, encoded with FFmpeg's wmav1/wmav2 encoder */
static int encode_tone( int version, int rate, int channels, int64_t bit_rate, double seconds, struct encoded *out )
{
    const AVCodec *codec = avcodec_find_encoder( version == 1 ? AV_CODEC_ID_WMAV1 : AV_CODEC_ID_WMAV2 );
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pkt;
    int64_t total = (int64_t)(seconds * rate), done = 0;
    size_t cap = 0;
    int ret, ch, i;

    memset( out, 0, sizeof(*out) );
    if (!codec || !(ctx = avcodec_alloc_context3( codec ))) return -1;
    ctx->sample_rate = rate;
    ctx->bit_rate = bit_rate;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default( &ctx->ch_layout, channels );
    if (avcodec_open2( ctx, codec, NULL ) < 0)
    {
        avcodec_free_context( &ctx );
        return -1;
    }
    out->block_align = ctx->block_align;
    out->extradata_size = ctx->extradata_size;
    memcpy( out->extradata, ctx->extradata, ctx->extradata_size );

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    for (;;)
    {
        if (done < total)
        {
            frame->nb_samples = ctx->frame_size;
            frame->format = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_copy( &frame->ch_layout, &ctx->ch_layout );
            frame->sample_rate = rate;
            av_frame_get_buffer( frame, 0 );
            for (ch = 0; ch < channels; ch++)
                for (i = 0; i < frame->nb_samples; i++)
                    ((float *)frame->extended_data[ch])[i] = 0.5f * (float)sin( 2 * M_PI * 440.0 * (done + i) / rate );
            frame->pts = done;
            done += frame->nb_samples;
            ret = avcodec_send_frame( ctx, frame );
            av_frame_unref( frame );
        }
        else ret = avcodec_send_frame( ctx, NULL );
        while ((ret = avcodec_receive_packet( ctx, pkt )) >= 0)
        {
            if (out->size + pkt->size > cap)
            {
                cap = (cap ? cap * 2 : 65536) + pkt->size;
                out->data = realloc( out->data, cap );
            }
            memcpy( out->data + out->size, pkt->data, pkt->size );
            out->size += pkt->size;
            CHECK_INT( pkt->size, out->block_align );
            av_packet_unref( pkt );
        }
        if (ret == AVERROR_EOF) break;
    }
    av_packet_free( &pkt );
    av_frame_free( &frame );
    avcodec_free_context( &ctx );
    return 0;
}

struct wfx_ext
{
    WAVEFORMATEX wfx;
    BYTE extra[16];
};

static void input_format( struct wfx_ext *f, int version, int rate, int channels, int64_t bit_rate,
                          const struct encoded *enc )
{
    memset( f, 0, sizeof(*f) );
    f->wfx.wFormatTag = version == 1 ? 0x0160 : 0x0161;
    f->wfx.nChannels = channels;
    f->wfx.nSamplesPerSec = rate;
    f->wfx.nAvgBytesPerSec = (DWORD)(bit_rate / 8);
    f->wfx.nBlockAlign = enc->block_align;
    f->wfx.wBitsPerSample = 16;
    f->wfx.cbSize = enc->extradata_size;
    memcpy( f->extra, enc->extradata, enc->extradata_size );
}

static void output_format( WAVEFORMATEX *f, int rate, int channels, int is_float )
{
    memset( f, 0, sizeof(*f) );
    f->wFormatTag = is_float ? 3 : 1;
    f->nChannels = channels;
    f->nSamplesPerSec = rate;
    f->wBitsPerSample = is_float ? 32 : 16;
    f->nBlockAlign = channels * f->wBitsPerSample / 8;
    f->nAvgBytesPerSec = rate * f->nBlockAlign;
}

struct pcm
{
    uint8_t *data;
    size_t size, cap;
    int discontinuities, incomplete_reads;
    int after_flush;
};

static void pcm_append( struct pcm *p, const uint8_t *data, size_t size )
{
    if (p->size + size > p->cap)
    {
        p->cap = (p->cap ? p->cap * 2 : 1 << 20) + size;
        p->data = realloc( p->data, p->cap );
    }
    memcpy( p->data + p->size, data, size );
    p->size += size;
}

/* reads until NEED_MORE_INPUT; `chunk` is the output buffer size (deliberately not a multiple of a frame) */
static void read_all( wg_transform_t t, uint32_t frame_bytes, uint32_t chunk, struct pcm *pcm )
{
    uint8_t *buf = malloc( chunk );
    struct wg_transform_read_data_params params;
    struct wg_sample sample;

    for (;;)
    {
        memset( &sample, 0, sizeof(sample) );
        sample.max_size = chunk;
        sample.data = (UINT64)(UINT_PTR)buf;
        memset( &params, 0, sizeof(params) );
        params.transform = t;
        params.sample = &sample;
        CHECK_INT( CALL( unix_wg_transform_read_data, &params ), STATUS_SUCCESS );
        if (params.result == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            CHECK_INT( sample.size, 0 );
            break;
        }
        CHECK_INT( params.result, S_OK );
        CHECK_INT( sample.size % frame_bytes, 0 );           /* whole frames only */
        CHECK( sample.flags & WG_SAMPLE_FLAG_SYNC_POINT );
        CHECK( sample.flags & WG_SAMPLE_FLAG_HAS_DURATION );
        /* known from the first pushed sample; forgotten by a flush (rule 12) */
        CHECK( !!(sample.flags & WG_SAMPLE_FLAG_HAS_PTS) == !pcm->after_flush );
        if (sample.flags & WG_SAMPLE_FLAG_DISCONTINUITY) pcm->discontinuities++;
        if (sample.flags & WG_SAMPLE_FLAG_INCOMPLETE) pcm->incomplete_reads++;
        pcm_append( pcm, buf, sample.size );
    }
    free( buf );
}

static double sample_at( const struct pcm *p, size_t frame, int channels, int is_float )
{
    if (is_float) return ((const float *)p->data)[frame * channels];
    return ((const int16_t *)p->data)[frame * channels] / 32768.0;
}

/* the fundamental (positive-going zero crossings) and the RMS of channel 0, skipping the start */
static void analyse( const struct pcm *p, int rate, int channels, int is_float, double *freq, double *rms )
{
    size_t frames = p->size / (channels * (is_float ? 4 : 2)), start = rate / 5, i, crossings = 0;
    double sum = 0, prev = 0;

    *freq = *rms = 0;
    if (frames <= start + (size_t)rate / 5) return;
    for (i = start; i < frames - rate / 10; i++)
    {
        double v = sample_at( p, i, channels, is_float );
        if (i > start && prev < 0 && v >= 0) crossings++;
        sum += v * v;
        prev = v;
    }
    *freq = (double)crossings * rate / (double)(frames - rate / 10 - start);
    *rms = sqrt( sum / (double)(frames - rate / 10 - start) );
}

static void run_tone( int version, int rate, int channels, int64_t bit_rate, int is_float, int search )
{
    struct wg_transform_create_params create;
    struct wg_transform_push_data_params push;
    struct wg_transform_get_status_params status;
    struct wg_sample sample;
    struct encoded enc;
    struct wfx_ext in;
    WAVEFORMATEX out;
    struct pcm pcm = { 0 };
    uint32_t frame_bytes = channels * (is_float ? 4 : 2);
    size_t pos = 0, chunk = 1000;   /* not a multiple of the block align: staging */
    double freq, rms;
    int notaccepting = 0;

    if (encode_tone( version, rate, channels, bit_rate, 2.0, &enc ))
    {
        fprintf( stderr, "can't encode v%d %d Hz %d ch %lld bps; skipped\n", version, rate, channels, (long long)bit_rate );
        return;
    }
    if (search) unsetenv( "MADEIRA_WMA_SEARCH" );
    else setenv( "MADEIRA_WMA_SEARCH", "0", 1 );

    input_format( &in, version, rate, channels, bit_rate, &enc );
    output_format( &out, rate, channels, is_float );
    memset( &create, 0, sizeof(create) );
    create.input_type.major = wg_ios_mf_audio;
    create.input_type.format_size = sizeof(WAVEFORMATEX) + enc.extradata_size;
    create.input_type.u.audio = &in.wfx;
    create.output_type.major = wg_ios_mf_audio;
    create.output_type.format_size = sizeof(WAVEFORMATEX);
    create.output_type.u.audio = &out;
    CHECK_INT( CALL( unix_wg_transform_create, &create ), STATUS_SUCCESS );
    CHECK( create.transform != 0 );

    while (pos < enc.size)
    {
        size_t n = enc.size - pos < chunk ? enc.size - pos : chunk;

        memset( &sample, 0, sizeof(sample) );
        sample.size = sample.max_size = (UINT32)n;
        sample.data = (UINT64)(UINT_PTR)(enc.data + pos);
        if (!pos)
        {
            sample.flags = WG_SAMPLE_FLAG_HAS_PTS;
            sample.pts = 0;
        }
        memset( &push, 0, sizeof(push) );
        push.transform = create.transform;
        push.sample = &sample;
        CHECK_INT( CALL( unix_wg_transform_push_data, &push ), STATUS_SUCCESS );
        if (push.result == MF_E_NOTACCEPTING)
        {
            /* back-pressure: PCM waits; input and output alternate */
            notaccepting++;
            status.transform = create.transform;
            CHECK_INT( CALL( unix_wg_transform_get_status, &status ), STATUS_SUCCESS );
            CHECK_INT( status.accepts_input, 0 );
            if (notaccepting == 1)
            {
                /* Clarification 2: no room for one whole frame answers as if there were no PCM */
                struct wg_transform_read_data_params read;
                uint8_t room[8];

                memset( &sample, 0, sizeof(sample) );
                sample.max_size = frame_bytes - 1;
                sample.data = (UINT64)(UINT_PTR)room;
                memset( &read, 0, sizeof(read) );
                read.transform = create.transform;
                read.sample = &sample;
                CHECK_INT( CALL( unix_wg_transform_read_data, &read ), STATUS_SUCCESS );
                CHECK_INT( read.result, MF_E_TRANSFORM_NEED_MORE_INPUT );
                CHECK_INT( sample.size, 0 );
                CHECK_INT( CALL( unix_wg_transform_get_status, &status ), STATUS_SUCCESS );
                CHECK_INT( status.accepts_input, 0 );   /* the PCM stays queued */
            }
            read_all( create.transform, frame_bytes, 4099, &pcm );
            continue;
        }
        CHECK_INT( push.result, S_OK );
        pos += n;
    }
    read_all( create.transform, frame_bytes, 4099, &pcm );
    CHECK_INT( CALL( unix_wg_transform_drain, &create.transform ), STATUS_SUCCESS );
    read_all( create.transform, frame_bytes, 4099, &pcm );
    CHECK( notaccepting > 0 );
    CHECK( pcm.incomplete_reads > 0 );

    analyse( &pcm, rate, channels, is_float, &freq, &rms );
    printf( "v%d %d Hz %d ch %lld bps %s search %s: %zu frames, %.1f Hz, rms %.3f\n", version, rate, channels,
            (long long)bit_rate, is_float ? "f32" : "s16", search ? "on" : "off", pcm.size / frame_bytes, freq, rms );
    CHECK_NEAR( freq, 440.0, 6.0 );
    CHECK_NEAR( rms, 0.5 / sqrt( 2.0 ), 0.08 );
    CHECK_NEAR( (double)(pcm.size / frame_bytes), 2.0 * rate, rate / 5.0 );

    /* flush: everything dropped, the next output is a discontinuity */
    CHECK_INT( CALL( unix_wg_transform_flush, &create.transform ), STATUS_SUCCESS );
    memset( &sample, 0, sizeof(sample) );
    sample.size = sample.max_size = enc.block_align * 8;
    sample.data = (UINT64)(UINT_PTR)enc.data;
    push.transform = create.transform;
    push.sample = &sample;
    CHECK_INT( CALL( unix_wg_transform_push_data, &push ), STATUS_SUCCESS );
    pcm.discontinuities = 0;
    pcm.size = 0;
    pcm.after_flush = 1;
    read_all( create.transform, frame_bytes, 1 << 20, &pcm );
    CHECK_INT( pcm.discontinuities, 1 );

    CHECK_INT( CALL( unix_wg_transform_destroy, &create.transform ), STATUS_SUCCESS );
    CHECK_INT( CALL( unix_wg_transform_destroy, &create.transform ), STATUS_INVALID_HANDLE );   /* stale */
    free( pcm.data );
    free( enc.data );
}

/* Clarification 3: a summary after failures or a dropped tail, then the counts start again */
static void test_summary( void )
{
    struct wg_transform_create_params create;
    struct wg_transform_push_data_params push;
    struct wg_sample sample;
    struct encoded enc;
    struct wfx_ext in;
    WAVEFORMATEX out;
    struct pcm pcm = { 0 };
    uint8_t *junk;

    CHECK_INT( encode_tone( 2, 44100, 2, 128000, 0.5, &enc ), 0 );
    setenv( "MADEIRA_WMA_SEARCH", "0", 1 );
    input_format( &in, 2, 44100, 2, 128000, &enc );
    output_format( &out, 44100, 2, 0 );
    memset( &create, 0, sizeof(create) );
    create.input_type.major = wg_ios_mf_audio;
    create.input_type.format_size = sizeof(WAVEFORMATEX) + enc.extradata_size;
    create.input_type.u.audio = &in.wfx;
    create.output_type.major = wg_ios_mf_audio;
    create.output_type.format_size = sizeof(WAVEFORMATEX);
    create.output_type.u.audio = &out;
    CHECK_INT( CALL( unix_wg_transform_create, &create ), STATUS_SUCCESS );

    /* four packets of noise and a short tail */
    junk = malloc( enc.block_align * 4 + 7 );
    memset( junk, 0xa5, enc.block_align * 4 + 7 );
    memset( &sample, 0, sizeof(sample) );
    sample.size = sample.max_size = enc.block_align * 4 + 7;
    sample.data = (UINT64)(UINT_PTR)junk;
    memset( &push, 0, sizeof(push) );
    push.transform = create.transform;
    push.sample = &sample;
    test_log_clear();
    CHECK_INT( CALL( unix_wg_transform_push_data, &push ), STATUS_SUCCESS );
    pcm.after_flush = 1;   /* no PTS was given */
    read_all( create.transform, 4, 1 << 20, &pcm );
    CHECK( pcm.size > 0 );   /* failed packets still produce time (silence) */
    CHECK_INT( CALL( unix_wg_transform_drain, &create.transform ), STATUS_SUCCESS );
    CHECK_INT( test_log_count( " drained: " ), 1 );
    CHECK_INT( test_log_count( "7 tail bytes dropped" ), 1 );
    CHECK_INT( CALL( unix_wg_transform_flush, &create.transform ), STATUS_SUCCESS );
    CHECK_INT( test_log_count( " flushed: " ), 0 );   /* nothing new since the last summary */
    CHECK_INT( CALL( unix_wg_transform_destroy, &create.transform ), STATUS_SUCCESS );
    CHECK_INT( test_log_count( " destroyed: " ), 0 );
    unsetenv( "MADEIRA_WMA_SEARCH" );
    free( pcm.data );
    free( junk );
    free( enc.data );
}

static void test_refusals_and_types( void )
{
    struct wg_transform_create_params create;
    struct wg_transform_get_output_type_params get;
    struct wg_transform_set_output_type_params set;
    struct encoded enc;
    struct wfx_ext in;
    WAVEFORMATEX out, got;
    MFVIDEOFORMAT video;

    CHECK_INT( encode_tone( 2, 44100, 2, 128000, 0.5, &enc ), 0 );
    input_format( &in, 2, 44100, 2, 128000, &enc );
    output_format( &out, 44100, 2, 0 );
    memset( &create, 0, sizeof(create) );
    create.input_type.major = wg_ios_mf_audio;
    create.input_type.format_size = sizeof(WAVEFORMATEX) + enc.extradata_size;
    create.input_type.u.audio = &in.wfx;
    create.output_type.major = wg_ios_mf_audio;
    create.output_type.format_size = sizeof(WAVEFORMATEX);
    create.output_type.u.audio = &out;

    /* a rate change is the resampler's job */
    out.nSamplesPerSec = 48000;
    CHECK_INT( CALL( unix_wg_transform_create, &create ), STATUS_NOT_SUPPORTED );
    out.nSamplesPerSec = 44100;
    out.wBitsPerSample = 24;
    CHECK_INT( CALL( unix_wg_transform_create, &create ), STATUS_NOT_SUPPORTED );
    output_format( &out, 44100, 2, 0 );
    in.wfx.wFormatTag = 0x1610;   /* AAC */
    CHECK_INT( CALL( unix_wg_transform_create, &create ), STATUS_NOT_SUPPORTED );
    in.wfx.wFormatTag = 0x0161;
    memset( &video, 0, sizeof(video) );
    video.dwSize = sizeof(video);
    create.input_type.major = wg_ios_mf_video;
    create.input_type.format_size = sizeof(video);
    create.input_type.u.video = &video;
    CHECK_INT( CALL( unix_wg_transform_create, &create ), STATUS_NOT_SUPPORTED );
    CHECK( test_log_count( "[wma] create refused" ) >= 4 );

    create.input_type.major = wg_ios_mf_audio;
    create.input_type.format_size = sizeof(WAVEFORMATEX) + enc.extradata_size;
    create.input_type.u.audio = &in.wfx;
    CHECK_INT( CALL( unix_wg_transform_create, &create ), STATUS_SUCCESS );

    /* get_output_type: two-phase, the exact format last set */
    memset( &get, 0, sizeof(get) );
    get.transform = create.transform;
    CHECK_INT( CALL( unix_wg_transform_get_output_type, &get ), STATUS_BUFFER_TOO_SMALL );
    CHECK_INT( get.media_type.format_size, sizeof(WAVEFORMATEX) );
    get.media_type.u.audio = &got;
    CHECK_INT( CALL( unix_wg_transform_get_output_type, &get ), STATUS_SUCCESS );
    CHECK_MEM( &got, &out, sizeof(out) );

    /* set_output_type: float at the input rate; anything else refused */
    output_format( &out, 44100, 2, 1 );
    memset( &set, 0, sizeof(set) );
    set.transform = create.transform;
    set.media_type.major = wg_ios_mf_audio;
    set.media_type.format_size = sizeof(out);
    set.media_type.u.audio = &out;
    CHECK_INT( CALL( unix_wg_transform_set_output_type, &set ), STATUS_SUCCESS );
    get.media_type.format_size = sizeof(got);
    CHECK_INT( CALL( unix_wg_transform_get_output_type, &get ), STATUS_SUCCESS );
    CHECK_INT( got.wFormatTag, 3 );
    out.nSamplesPerSec = 22050;
    CHECK_INT( CALL( unix_wg_transform_set_output_type, &set ), STATUS_NOT_SUPPORTED );

    /* bytes with no buffer */
    {
        struct wg_transform_push_data_params push;
        struct wg_sample sample;

        memset( &sample, 0, sizeof(sample) );
        sample.size = 10;
        memset( &push, 0, sizeof(push) );
        push.transform = create.transform;
        push.sample = &sample;
        CHECK_INT( CALL( unix_wg_transform_push_data, &push ), STATUS_INVALID_PARAMETER );
    }
    /* a handle of the wrong kind */
    {
        struct wg_parser_create_params pc;
        memset( &pc, 0, sizeof(pc) );
        CHECK_INT( CALL( unix_wg_parser_create, &pc ), S_OK );
        CHECK_INT( CALL( unix_wg_transform_flush, &pc.parser ), STATUS_INVALID_HANDLE );
        CHECK_INT( CALL( unix_wg_parser_destroy, &pc.parser ), S_OK );
    }
    /* the muxer is missing, never a silent success */
    {
        UINT64 dummy[4] = { 0 };
        CHECK_INT( CALL( unix_wg_muxer_create, dummy ), STATUS_NOT_IMPLEMENTED );
        CHECK_INT( CALL( unix_wg_muxer_finalize, dummy ), STATUS_NOT_IMPLEMENTED );
    }
    CHECK_INT( CALL( unix_wg_init_gstreamer, NULL ), STATUS_SUCCESS );
    CALL( unix_wg_transform_destroy, &create.transform );
    free( enc.data );
}

/* the same decode through the 32-bit table, every pointer a guest address (A4) */
static void test_wow64( void )
{
    struct create32 { UINT64 transform; struct wg_media_type32 in, out; struct wg_transform_attrs attrs; } *create;
    struct data32 { UINT64 transform; UINT32 sample; HRESULT result; } *data;
    struct encoded enc;
    struct wfx_ext *in;
    WAVEFORMATEX *out;
    struct wg_sample *sample;
    uint8_t *input, *output;
    size_t got = 0;

    CHECK_INT( encode_tone( 2, 22050, 1, 32000, 1.0, &enc ), 0 );
    test_enter_window();
    create = test_arena_alloc( sizeof(*create) );
    data = test_arena_alloc( sizeof(*data) );
    in = test_arena_alloc( sizeof(*in) );
    out = test_arena_alloc( sizeof(*out) );
    sample = test_arena_alloc( sizeof(*sample) );
    input = test_arena_alloc( enc.size );
    output = test_arena_alloc( 1 << 20 );
    memcpy( input, enc.data, enc.size );

    input_format( in, 2, 22050, 1, 32000, &enc );
    output_format( out, 22050, 1, 0 );
    create->in.major = wg_ios_mf_audio;
    create->in.format_size = sizeof(WAVEFORMATEX) + enc.extradata_size;
    create->in.format = test_guest( in );
    create->out.major = wg_ios_mf_audio;
    create->out.format_size = sizeof(WAVEFORMATEX);
    create->out.format = test_guest( out );
    CHECK_INT( CALL32( unix_wg_transform_create, create ), STATUS_SUCCESS );
    CHECK( create->transform != 0 );

    memset( sample, 0, sizeof(*sample) );
    sample->size = sample->max_size = (UINT32)(enc.block_align * 10);
    sample->data = test_guest( input );   /* a guest address in the data field */
    data->transform = create->transform;
    data->sample = test_guest( sample );
    CHECK_INT( CALL32( unix_wg_transform_push_data, data ), STATUS_SUCCESS );
    CHECK_INT( data->result, S_OK );
    for (;;)
    {
        memset( sample, 0, sizeof(*sample) );
        sample->max_size = 1 << 20;
        sample->data = test_guest( output );
        CHECK_INT( CALL32( unix_wg_transform_read_data, data ), STATUS_SUCCESS );
        if (data->result != S_OK) break;
        got += sample->size;
    }
    CHECK_INT( data->result, MF_E_TRANSFORM_NEED_MORE_INPUT );
    CHECK( got > 0 );
    CHECK_INT( CALL32( unix_wg_transform_destroy, &create->transform ), STATUS_SUCCESS );
    test_leave_window();
    free( enc.data );
}

int main( void )
{
    static const struct { int version, rate, channels; int64_t bit_rate; } tones[] =
    {
        { 1, 22050, 1, 32000 },
        { 1, 44100, 2, 128000 },
        { 2, 22050, 1, 32000 },
        { 2, 32000, 2, 64000 },
        { 2, 44100, 2, 128000 },
        { 2, 48000, 1, 96000 },
    };
    unsigned int i;

    for (i = 0; i < sizeof(tones) / sizeof(tones[0]); i++)
    {
        run_tone( tones[i].version, tones[i].rate, tones[i].channels, tones[i].bit_rate, 0, 1 );
        run_tone( tones[i].version, tones[i].rate, tones[i].channels, tones[i].bit_rate, 1, 1 );
        run_tone( tones[i].version, tones[i].rate, tones[i].channels, tones[i].bit_rate, 0, 0 );
    }
    CHECK( test_log_count( "[wma] accepted" ) > 0 );
    /* Clarification 3: clean streams print no summary */
    CHECK_INT( test_log_count( " drained: " ) + test_log_count( " flushed: " ) + test_log_count( " destroyed: " ), 0 );
    test_summary();
    test_refusals_and_types();
    test_wow64();
    return test_report( "test_wma_e2e" );
}
