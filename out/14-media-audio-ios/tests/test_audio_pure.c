// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F, audio: the downmix gains, the mixer on synthetic rings, the pacing decision,
// ring capacity clamps, the endpoint buffer layout, the mix format bytes, the lead switch and
// the format classes (audio-unix/ios_audio_pure.c).

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "ios_audio_pure.h"

#define H 0.7071f
#define LFE 0.316f

static void check_gains( uint32_t channels, uint32_t mask, int downmix, unsigned int expect_count,
                         const float *el, const float *er, int line )
{
    float gl[IA_MAX_GAINS], gr[IA_MAX_GAINS];
    unsigned int n = ia_downmix_gains( channels, mask, downmix, gl, gr ), i;

    if (n != expect_count) fprintf( stderr, "line %d:\n", line );
    CHECK_INT( n, expect_count );
    for (i = 0; i < n && i < IA_MAX_GAINS; i++)
    {
        if (fabsf( gl[i] - el[i] ) > 1e-6f || fabsf( gr[i] - er[i] ) > 1e-6f)
            fprintf( stderr, "line %d: channel %u got %g/%g\n", line, i, gl[i], gr[i] );
        CHECK_NEAR( gl[i], el[i], 1e-6 );
        CHECK_NEAR( gr[i], er[i], 1e-6 );
    }
}

static void test_gains( void )
{
    {
        const float l[] = { 1 }, r[] = { 1 };
        check_gains( 1, 0, 1, 1, l, r, __LINE__ );
        check_gains( 1, 0x4, 0, 1, l, r, __LINE__ );   /* mono goes to both even with downmix off */
    }
    {
        const float l[] = { 1, 0 }, r[] = { 0, 1 };
        check_gains( 2, 0, 1, 2, l, r, __LINE__ );
        check_gains( 2, 0x3, 1, 2, l, r, __LINE__ );
    }
    {
        /* 5.1 by mask and without: FL FR FC LFE BL BR */
        const float l[] = { 1, 0, H, LFE, H, 0 }, r[] = { 0, 1, H, LFE, 0, H };
        check_gains( 6, 0x3f, 1, 6, l, r, __LINE__ );
        check_gains( 6, 0, 1, 6, l, r, __LINE__ );
    }
    {
        /* 5.1 side: FL FR FC LFE SL SR */
        const float l[] = { 1, 0, H, LFE, H, 0 }, r[] = { 0, 1, H, LFE, 0, H };
        check_gains( 6, 0x60f, 1, 6, l, r, __LINE__ );
    }
    {
        /* 7.1: FL FR FC LFE BL BR SL SR */
        const float l[] = { 1, 0, H, LFE, H, 0, H, 0 }, r[] = { 0, 1, H, LFE, 0, H, 0, H };
        check_gains( 8, 0x63f, 1, 8, l, r, __LINE__ );
        check_gains( 8, 0, 1, 8, l, r, __LINE__ );
    }
    {
        /* 7.1.4: + TFL TFR TBL TBR */
        const float l[] = { 1, 0, H, LFE, H, 0, H, 0, H, 0, H, 0 }, r[] = { 0, 1, H, LFE, 0, H, 0, H, 0, H, 0, H };
        check_gains( 12, 0x2d63f, 1, 12, l, r, __LINE__ );
    }
    {
        /* a short mask: channels it doesn't name get 0.5 to both */
        const float l[] = { 1, 0, 0.5f, 0.5f }, r[] = { 0, 1, 0.5f, 0.5f };
        check_gains( 4, 0x3, 1, 4, l, r, __LINE__ );
    }
    {
        /* centre-type speakers */
        const float l[] = { H, H, H, H }, r[] = { H, H, H, H };
        check_gains( 4, 0x4 | 0x100 | 0x800 | 0x2000, 1, 4, l, r, __LINE__ );
    }
    {
        /* MADEIRA_AUDIO_DOWNMIX=0 */
        const float l[] = { 1, 0, 0, 0, 0, 0 }, r[] = { 0, 1, 0, 0, 0, 0 };
        check_gains( 6, 0x3f, 0, 6, l, r, __LINE__ );
    }
    {
        /* more than 12 channels: only 12 get a gain */
        float gl[IA_MAX_GAINS], gr[IA_MAX_GAINS];
        CHECK_INT( ia_downmix_gains( 16, 0, 1, gl, gr ), 12 );
    }
    CHECK_INT( ia_standard_mask( 3 ), 0xb );
    CHECK_INT( ia_standard_mask( 5 ), 0x3b );
    CHECK_INT( ia_standard_mask( 7 ), 0x13f );
    CHECK_INT( ia_standard_mask( 10 ), 0x63f );
}

static void test_capacity_and_lead( void )
{
    CHECK_INT( ia_ring_capacity( 0, 48000 ), 4800 );           /* 100 ms floor */
    CHECK_INT( ia_ring_capacity( 200000, 48000 ), 4800 );      /* 20 ms asked */
    CHECK_INT( ia_ring_capacity( 5000000, 48000 ), 24000 );    /* 500 ms */
    CHECK_INT( ia_ring_capacity( 100000000, 48000 ), 192000 ); /* 10 s asked: 4 s */
    CHECK_INT( ia_ring_capacity( 10000000, 44100 ), 44100 );

    CHECK_INT( ia_parse_lead_ms( NULL ), 60 );
    CHECK_INT( ia_parse_lead_ms( "" ), 60 );
    CHECK_INT( ia_parse_lead_ms( "0" ), 0 );
    CHECK_INT( ia_parse_lead_ms( "05" ), 0 );
    CHECK_INT( ia_parse_lead_ms( "10" ), 10 );
    CHECK_INT( ia_parse_lead_ms( "100" ), 100 );
    CHECK_INT( ia_parse_lead_ms( "500" ), 500 );
    CHECK_INT( ia_parse_lead_ms( "5" ), 60 );
    CHECK_INT( ia_parse_lead_ms( "501" ), 60 );
    CHECK_INT( ia_parse_lead_ms( "abc" ), 60 );

    CHECK_INT( ia_lead_frames( 60, 48000, 4800, 480 ), 2880 );
    CHECK_INT( ia_lead_frames( 60, 48000, 3000, 480 ), 2520 );   /* leaves a period of room */
    CHECK_INT( ia_lead_frames( 60, 48000, 400, 480 ), 480 );     /* at least a period */
    CHECK_INT( ia_lead_frames( 10, 48000, 48000, 480 ), 480 );
}

static void test_endpoint_and_format( void )
{
    unsigned char buf[64], expect_fmt[IA_MIX_FORMAT_SIZE], fmt[IA_MIX_FORMAT_SIZE];
    static const unsigned char name[] = { 'i',0, 'O',0, 'S',0, ' ',0, 'N',0, 'u',0, 'l',0, 'l',0, 0,0 };
    unsigned int needed = 0, count = 0, ids[2];

    CHECK_INT( ia_endpoint_list( buf, 10, &needed, &count ), -1 );
    CHECK_INT( needed, 36 );
    CHECK_INT( count, 1 );
    memset( buf, 0xcc, sizeof(buf) );
    CHECK_INT( ia_endpoint_list( buf, 36, &needed, &count ), 0 );
    memcpy( ids, buf, sizeof(ids) );
    CHECK_INT( ids[0], 8 );
    CHECK_INT( ids[1], 26 );
    CHECK_MEM( buf + 8, name, sizeof(name) );
    CHECK_MEM( buf + 26, "ios-null", 9 );
    CHECK_INT( buf[35], 0 );   /* padded to an even length */

    memset( expect_fmt, 0, sizeof(expect_fmt) );
    {
        static const unsigned char head[24] =
        {
            0xfe, 0xff, 0x02, 0x00, 0x80, 0xbb, 0x00, 0x00,   /* tag, channels, 48000 */
            0x00, 0xdc, 0x05, 0x00, 0x08, 0x00, 0x20, 0x00,   /* 384000, align 8, 32 bits */
            0x16, 0x00, 0x20, 0x00, 0x03, 0x00, 0x00, 0x00,   /* cbSize 22, valid 32, mask 3 */
        };
        static const unsigned char ieee[16] =
            { 0x03, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71 };
        memcpy( expect_fmt, head, 24 );
        memcpy( expect_fmt + 24, ieee, 16 );
    }
    ia_mix_format( fmt );
    CHECK_MEM( fmt, expect_fmt, IA_MIX_FORMAT_SIZE );
}

static void test_classify( void )
{
    static const uint8_t pcm[16] = { 0x01, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71 };
    static const uint8_t flt[16] = { 0x03, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71 };

    CHECK_INT( ia_classify_format( 3, 32, NULL, 2, 8 ), IA_FMT_F32 );
    CHECK_INT( ia_classify_format( 1, 16, NULL, 2, 4 ), IA_FMT_S16 );
    CHECK_INT( ia_classify_format( 1, 32, NULL, 2, 8 ), IA_FMT_S32 );
    CHECK_INT( ia_classify_format( 1, 24, NULL, 2, 6 ), IA_FMT_NONE );
    CHECK_INT( ia_classify_format( 1, 8, NULL, 1, 1 ), IA_FMT_NONE );
    CHECK_INT( ia_classify_format( 0xfffe, 32, flt, 6, 24 ), IA_FMT_F32 );
    CHECK_INT( ia_classify_format( 0xfffe, 16, pcm, 2, 4 ), IA_FMT_S16 );
    CHECK_INT( ia_classify_format( 0xfffe, 32, pcm, 2, 8 ), IA_FMT_S32 );
    CHECK_INT( ia_classify_format( 0x0161, 16, NULL, 2, 4 ), IA_FMT_NONE );
    CHECK_INT( ia_classify_format( 3, 32, NULL, 2, 4 ), IA_FMT_NONE );   /* block align too small */
}

/* ---------------------------------------------------------------- the mixer */

static void init_stream( struct ia_shared *s, enum ia_format f, uint32_t channels, uint32_t rate, uint32_t capacity )
{
    uint32_t bps = f == IA_FMT_S16 ? 2 : 4;

    memset( s, 0, sizeof(*s) );
    s->format = f;
    s->channels = channels;
    s->rate = rate;
    s->frame_bytes = bps * channels;
    s->capacity = capacity;
    s->ring = calloc( capacity, s->frame_bytes );
    s->gain_count = ia_downmix_gains( channels, 0, 1, s->gain_l, s->gain_r );
    s->started = 1;
}

static void put_f32( struct ia_shared *s, uint64_t frame, float l, float r )
{
    float *p = (float *)(s->ring + (frame % s->capacity) * s->frame_bytes);
    p[0] = l;
    if (s->channels > 1) p[1] = r;
}

static void test_mixer( void )
{
    struct ia_mixer m;
    struct ia_shared a, b;
    float out[1024 * 2 + 8];
    uint32_t i;

    memset( &m, 0, sizeof(m) );
    m.device_rate = 48000;
    m.device_channels = 2;

    /* same rate, float */
    init_stream( &a, IA_FMT_F32, 2, 48000, 1000 );
    for (i = 0; i < 512; i++) put_f32( &a, i, 0.25f, -0.5f );
    a.written = 512;
    m.slots[0] = &a;
    ia_mix( &m, out, 256, sizeof(out) );
    CHECK_NEAR( out[0], 0.25, 1e-6 );
    CHECK_NEAR( out[1], -0.5, 1e-6 );
    CHECK_NEAR( out[2 * 255 + 1], -0.5, 1e-6 );
    CHECK_INT( a.played, 256 );
    CHECK_INT( m.passes, 1 );
    CHECK_INT( a.short_passes, 0 );
    CHECK_INT( ia_padding( &a ), 256 );

    /* underrun: what is there is mixed, the rest is silence, counted */
    ia_mix( &m, out, 512, sizeof(out) );
    CHECK_INT( a.played, 512 );
    CHECK_NEAR( out[2 * 255], 0.25, 1e-6 );
    CHECK_NEAR( out[2 * 256], 0.0, 1e-6 );
    CHECK_INT( a.short_passes, 1 );
    CHECK_INT( a.missing_frames, 256 );
    CHECK_INT( m.passes, 2 );

    /* rate mapping: a 24 kHz stream on a 48 kHz device; the half frame carries to the next pass */
    free( a.ring );
    init_stream( &a, IA_FMT_F32, 2, 24000, 1000 );
    for (i = 0; i < 200; i++) put_f32( &a, i, i / 1000.0f, 0 );
    a.written = 200;
    ia_mix( &m, out, 101, sizeof(out) );
    CHECK_INT( a.played, 50 );                         /* floor(101 * 24000 / 48000) */
    CHECK_INT( a.phase, 24000 );                       /* half a stream frame kept */
    CHECK_NEAR( out[2 * 0], 0.0, 1e-6 );
    CHECK_NEAR( out[2 * 1], 0.0, 1e-6 );               /* frame 1 reads stream frame 0 */
    CHECK_NEAR( out[2 * 2], 0.001, 1e-6 );             /* frame 2 reads stream frame 1 */
    CHECK_NEAR( out[2 * 100], 0.050, 1e-6 );
    ia_mix( &m, out, 1, sizeof(out) );
    CHECK_NEAR( out[0], 0.050, 1e-6 );                 /* frame 50 again: the carried half */
    CHECK_INT( a.played, 51 );
    CHECK_INT( a.phase, 0 );
    a.phase_reset = 1;                                 /* start, reset, set_sample_rate */
    a.phase = 123;
    ia_mix( &m, out, 2, sizeof(out) );
    CHECK_INT( a.played, 52 );
    CHECK_INT( a.phase_reset, 0 );

    /* Clarification 4: over many passes the frames consumed equal device frames x rs / rd
     * within one frame (44.1 kHz on 48 kHz, 512-frame passes) */
    free( a.ring );
    init_stream( &a, IA_FMT_F32, 2, 44100, 4096 );
    {
        uint64_t device = 0;
        int pass;

        for (pass = 0; pass < 3000; pass++)
        {
            a.written = a.played + 2048;               /* never short */
            ia_mix( &m, out, 512, sizeof(out) );
            device += 512;
            if (pass == 999 || pass == 2999)
                CHECK_NEAR( (double)a.played, (double)device * 44100 / 48000, 1.0 );
        }
        CHECK_INT( a.short_passes, 0 );
        CHECK_INT( a.played, 1536000ull * 44100 / 48000 );   /* exact: 1411200 */
    }
    /* an underrun pass restarts the fraction */
    a.written = a.played + 10;
    ia_mix( &m, out, 512, sizeof(out) );
    CHECK_INT( a.short_passes, 1 );
    CHECK_INT( a.phase, 0 );

    /* 16- and 32-bit integer streams */
    free( a.ring );
    init_stream( &a, IA_FMT_S16, 2, 48000, 100 );
    ((int16_t *)a.ring)[0] = 16384;
    ((int16_t *)a.ring)[1] = -32768;
    a.written = 1;
    init_stream( &b, IA_FMT_S32, 1, 48000, 100 );
    ((int32_t *)b.ring)[0] = 1 << 29;                  /* 0.25, mono: to both sides */
    b.written = 1;
    m.slots[1] = &b;
    ia_mix( &m, out, 1, sizeof(out) );
    CHECK_NEAR( out[0], 0.5 + 0.25, 1e-6 );
    CHECK_NEAR( out[1], -1.0 + 0.25, 1e-6 );

    /* clamping, counted */
    free( a.ring );
    free( b.ring );
    init_stream( &a, IA_FMT_F32, 2, 48000, 100 );
    init_stream( &b, IA_FMT_F32, 2, 48000, 100 );
    for (i = 0; i < 10; i++)
    {
        put_f32( &a, i, 0.75f, -0.75f );
        put_f32( &b, i, 0.75f, -0.75f );
    }
    a.written = b.written = 10;
    m.clamped = 0;
    ia_mix( &m, out, 10, sizeof(out) );
    CHECK_NEAR( out[0], 1.0, 1e-6 );
    CHECK_NEAR( out[1], -1.0, 1e-6 );
    CHECK_INT( m.clamped, 20 );

    /* a stream that isn't started is not mixed and doesn't move */
    b.started = 0;
    a.written = 20;
    for (i = 10; i < 20; i++) put_f32( &a, i, 0.1f, 0.1f );
    ia_mix( &m, out, 10, sizeof(out) );
    CHECK_NEAR( out[0], 0.1, 1e-6 );
    CHECK_INT( b.played, 10 );

    /* never past the byte size handed over */
    for (i = 0; i < 20; i++) out[i] = 7.0f;
    a.written = a.played + 50;
    ia_mix( &m, out, 50, 4 * sizeof(float) );          /* room for 2 stereo frames */
    CHECK_NEAR( out[4], 7.0, 0 );
    CHECK_INT( a.played - 20, 2 );

    /* a mono device: (L + R) / 2 */
    m.device_channels = 1;
    m.slots[1] = NULL;
    put_f32( &a, a.played, 0.5f, 0.1f );
    ia_mix( &m, out, 1, sizeof(out) );
    CHECK_NEAR( out[0], 0.3, 1e-6 );

    free( a.ring );
    free( b.ring );
}

static void test_pacing( void )
{
    struct ia_pace pace;
    struct ia_pace_decision d;
    const uint64_t ms = 1000000;
    uint64_t t = 5000 * ms;

    memset( &pace, 0, sizeof(pace) );
    /* below the lead: the first time signals */
    ia_pace_step( &pace, 100, 2880, 48000, 0, t, &d );
    CHECK_INT( d.signal, 1 );
    CHECK_INT( d.sleep_ns, 1 * ms );
    /* no write since, 2 ms later: no signal, look again in 1 ms */
    ia_pace_step( &pace, 100, 2880, 48000, 0, t + 2 * ms, &d );
    CHECK_INT( d.signal, 0 );
    CHECK_INT( d.sleep_ns, 1 * ms );
    /* 10 ms after the last signal: signal again (one unanswered) */
    ia_pace_step( &pace, 100, 2880, 48000, 0, t + 10 * ms, &d );
    CHECK_INT( d.signal, 1 );
    CHECK_INT( pace.unanswered, 1 );
    CHECK_INT( d.sleep_ns, 1 * ms );
    /* again: two unanswered, look again in 10 ms */
    ia_pace_step( &pace, 100, 2880, 48000, 0, t + 20 * ms, &d );
    CHECK_INT( d.signal, 1 );
    CHECK_INT( pace.unanswered, 2 );
    CHECK_INT( d.sleep_ns, 10 * ms );
    /* the client wrote: signal at once, the count is reset */
    ia_pace_step( &pace, 500, 2880, 48000, 1, t + 21 * ms, &d );
    CHECK_INT( d.signal, 1 );
    CHECK_INT( pace.unanswered, 0 );
    CHECK_INT( d.sleep_ns, 1 * ms );
    /* at or above the lead: sleep until what is above it is used up, 1-10 ms */
    ia_pace_step( &pace, 2880 + 479, 2880, 48000, 1, t + 22 * ms, &d );
    CHECK_INT( d.signal, 0 );
    CHECK_INT( d.sleep_ns, 10 * ms );
    ia_pace_step( &pace, 2880 + 95, 2880, 48000, 1, t + 23 * ms, &d );
    CHECK_INT( d.sleep_ns, 2 * ms );
    ia_pace_step( &pace, 2880, 2880, 48000, 1, t + 24 * ms, &d );
    CHECK_INT( d.sleep_ns, 1 * ms );
    ia_pace_step( &pace, 48000, 2880, 48000, 1, t + 25 * ms, &d );
    CHECK_INT( d.sleep_ns, 10 * ms );
}

/* spec 22 Part F2, F3 */
static void test_kept_session( void )
{
    CHECK_INT( ia_keep_unit_running( 1, 1 ), 1 );
    CHECK_INT( ia_keep_unit_running( 0, 1 ), 0 );
    CHECK_INT( ia_keep_unit_running( 1, 0 ), 0 );   /* never started: nothing to keep */

    /* has_event, started, null_mode, lead_ms, unit_running */
    CHECK_INT( ia_plain_beat( 1, 1, 0, 60, 1 ), 0 );   /* paced by the lead */
    CHECK_INT( ia_plain_beat( 1, 1, 0, 60, 0 ), 1 );   /* no running unit: the 10 ms beat */
    CHECK_INT( ia_plain_beat( 1, 1, 1, 60, 1 ), 1 );   /* null mode */
    CHECK_INT( ia_plain_beat( 1, 1, 0, 0, 1 ), 1 );    /* pacing off */
    CHECK_INT( ia_plain_beat( 0, 1, 0, 60, 1 ), 1 );
    CHECK_INT( ia_plain_beat( 1, 0, 0, 60, 1 ), 1 );
}

int main( void )
{
    test_kept_session();
    test_gains();
    test_capacity_and_lead();
    test_endpoint_and_format();
    test_classify();
    test_mixer();
    test_pacing();
    return test_report( "test_audio_pure" );
}
