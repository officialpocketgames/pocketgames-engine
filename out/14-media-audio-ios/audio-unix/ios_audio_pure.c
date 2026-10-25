// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part C: the pure parts of the audio driver (see ios_audio_pure.h).

#include <stdlib.h>
#include <string.h>

#include "ios_audio_pure.h"

/* ---------------------------------------------------------------- formats (C4) */

/* {0000000X-0000-0010-8000-00AA00389B71} as bytes in memory */
static int subformat_is( const uint8_t *g, uint8_t data1 )
{
    static const uint8_t tail[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
    return g[0] == data1 && !g[1] && !g[2] && !g[3] && !memcmp( g + 4, tail, 12 );
}

enum ia_format ia_classify_format( uint16_t tag, uint16_t bits, const uint8_t *subformat,
                                   uint32_t channels, uint32_t block_align )
{
    int is_float = tag == 0x0003, is_pcm = tag == 0x0001;
    enum ia_format f = IA_FMT_NONE;

    if (tag == 0xfffe && subformat)
    {
        is_float = subformat_is( subformat, 0x03 );
        is_pcm = subformat_is( subformat, 0x01 );
    }
    if (is_float && bits == 32) f = IA_FMT_F32;
    else if (is_pcm && bits == 16) f = IA_FMT_S16;
    else if (is_pcm && bits == 32) f = IA_FMT_S32;
    if (!channels || block_align < channels * (bits / 8)) return IA_FMT_NONE;
    return f;
}

const char *ia_format_name( enum ia_format f )
{
    switch (f)
    {
    case IA_FMT_F32: return "f32";
    case IA_FMT_S16: return "s16";
    case IA_FMT_S32: return "s32";
    default: return "unmixable";
    }
}

/* ---------------------------------------------------------------- C3, C8 */

uint32_t ia_ring_capacity( int64_t duration, uint32_t rate )
{
    uint64_t frames = duration > 0 ? (uint64_t)duration * rate / 10000000 : 0;
    uint64_t lo = rate / 10, hi = (uint64_t)rate * 4;

    if (frames < lo) frames = lo;
    if (frames > hi) frames = hi;
    if (!frames) frames = 1;
    return (uint32_t)frames;
}

uint32_t ia_standard_mask( uint32_t channels )
{
    switch (channels)
    {
    case 0: return 0;
    case 1: return 0x4;      /* front centre */
    case 2: return 0x3;      /* stereo */
    case 3: return 0xb;      /* stereo + LFE */
    case 4: return 0x33;     /* quad */
    case 5: return 0x3b;     /* quad + LFE */
    case 6: return 0x3f;     /* 5.1 */
    case 7: return 0x13f;    /* 5.1 + back centre */
    default: return 0x63f;   /* 7.1 */
    }
}

#define G_HALF 0.7071f
#define G_LFE 0.316f

unsigned int ia_downmix_gains( uint32_t channels, uint32_t mask, int downmix, float *gl, float *gr )
{
    unsigned int n = channels < IA_MAX_GAINS ? channels : IA_MAX_GAINS, c = 0, bit;

    memset( gl, 0, IA_MAX_GAINS * sizeof(float) );
    memset( gr, 0, IA_MAX_GAINS * sizeof(float) );
    if (!n) return 0;
    if (channels == 1)
    {
        gl[0] = gr[0] = 1.0f;
        return 1;
    }
    if (!downmix)
    {
        /* PG_AUDIO_DOWNMIX=0: channel 0 -> L, channel 1 -> R, the rest dropped */
        gl[0] = 1.0f;
        gr[1] = 1.0f;
        return n;
    }
    if (!mask) mask = ia_standard_mask( channels );

    for (bit = 0; bit < 32 && c < n; bit++)
    {
        if (!(mask & (1u << bit))) continue;
        switch (bit)
        {
        case 0:  gl[c] = 1.0f; break;                    /* front left */
        case 1:  gr[c] = 1.0f; break;                    /* front right */
        case 2:                                          /* front centre */
        case 8:                                          /* back centre */
        case 11:                                         /* top centre */
        case 13:                                         /* top front centre */
        case 16: gl[c] = gr[c] = G_HALF; break;          /* top back centre */
        case 3:  gl[c] = gr[c] = G_LFE; break;           /* LFE */
        case 4:                                          /* back left */
        case 6:                                          /* front left of centre */
        case 9:                                          /* side left */
        case 12:                                         /* top front left */
        case 15: gl[c] = G_HALF; break;                  /* top back left */
        case 5:                                          /* back right */
        case 7:                                          /* front right of centre */
        case 10:                                         /* side right */
        case 14:                                         /* top front right */
        case 17: gr[c] = G_HALF; break;                  /* top back right */
        default: gl[c] = gr[c] = 0.5f; break;            /* not a speaker position */
        }
        c++;
    }
    for (; c < n; c++) gl[c] = gr[c] = 0.5f;   /* channels the mask doesn't name */
    return n;
}

uint32_t ia_parse_lead_ms( const char *value )
{
    unsigned long v = 0;
    const char *p;

    if (!value || !*value) return 60;
    if (value[0] == '0') return 0;
    for (p = value; *p; p++)
    {
        if (*p < '0' || *p > '9') return 60;
        v = v * 10 + (unsigned long)(*p - '0');
        if (v > 100000) return 60;
    }
    if (v < 10 || v > 500) return 60;
    return (uint32_t)v;
}

/* ---------------------------------------------------------------- C2 */

static const char endpoint_name[] = "iOS Null";
static const char endpoint_device[] = "ios-null";

int ia_endpoint_list( void *buf, unsigned int size, unsigned int *needed, unsigned int *count )
{
    unsigned int name_len = sizeof(endpoint_name), device_len = sizeof(endpoint_device);   /* with the NUL */
    unsigned int offset = 2 * sizeof(unsigned int), i;
    unsigned int total = offset + name_len * 2 + ((device_len + 1) & ~1u);
    uint8_t *base = buf;
    unsigned int ids[2];

    *count = 1;
    *needed = total;
    if (size < total || !buf) return -1;

    ids[0] = offset;                                 /* name */
    for (i = 0; i < name_len; i++)
    {
        base[offset + 2 * i] = (uint8_t)endpoint_name[i];   /* UTF-16LE, ASCII only */
        base[offset + 2 * i + 1] = 0;
    }
    offset += name_len * 2;
    ids[1] = offset;                                 /* device */
    memcpy( base + offset, endpoint_device, device_len );
    if (device_len & 1) base[offset + device_len] = 0;
    memcpy( base, ids, sizeof(ids) );
    return 0;
}

void ia_mix_format( uint8_t out[IA_MIX_FORMAT_SIZE] )
{
    static const uint8_t ieee_float[16] =
        { 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
    uint32_t rate = 48000, avg = 48000 * 8, mask = 0x3;

    memset( out, 0, IA_MIX_FORMAT_SIZE );
    out[0] = 0xfe; out[1] = 0xff;                    /* WAVE_FORMAT_EXTENSIBLE */
    out[2] = 2;                                      /* nChannels */
    memcpy( out + 4, &rate, 4 );                     /* nSamplesPerSec */
    memcpy( out + 8, &avg, 4 );                      /* nAvgBytesPerSec */
    out[12] = 8;                                     /* nBlockAlign */
    out[14] = 32;                                    /* wBitsPerSample */
    out[16] = 22;                                    /* cbSize */
    out[18] = 32;                                    /* wValidBitsPerSample */
    memcpy( out + 20, &mask, 4 );                    /* dwChannelMask */
    memcpy( out + 24, ieee_float, 16 );              /* SubFormat */
}

/* ---------------------------------------------------------------- the mixer (C6) */

static inline float sample_at( const uint8_t *p, enum ia_format f )
{
    switch (f)
    {
    case IA_FMT_F32: { float v; memcpy( &v, p, 4 ); return v; }
    case IA_FMT_S16: { int16_t v; memcpy( &v, p, 2 ); return v * (1.0f / 32768.0f); }
    case IA_FMT_S32: { int32_t v; memcpy( &v, p, 4 ); return (float)v * (1.0f / 2147483648.0f); }
    default: return 0.0f;
    }
}

static inline uint32_t sample_bytes( enum ia_format f )
{
    return f == IA_FMT_S16 ? 2 : 4;
}

uint32_t ia_padding( const struct ia_shared *s )
{
    uint64_t played = __atomic_load_n( &s->played, __ATOMIC_ACQUIRE );
    uint64_t written = __atomic_load_n( &s->written, __ATOMIC_ACQUIRE );
    uint64_t d;

    if (written <= played) return 0;
    d = written - played;
    return d > s->capacity ? s->capacity : (uint32_t)d;
}

void ia_mix( struct ia_mixer *m, float *out, uint32_t frames, uint32_t bytes )
{
    uint32_t oc = m->device_channels == 1 ? 1 : 2, rd = m->device_rate, n = frames, i, f;
    uint64_t clamped = 0, k, total;

    if (n > bytes / (4 * oc)) n = bytes / (4 * oc);
    total = (uint64_t)n * oc;
    memset( out, 0, total * sizeof(float) );

    for (i = 0; i < IA_MAX_STREAMS && rd; i++)
    {
        struct ia_shared *s = __atomic_load_n( &m->slots[i], __ATOMIC_ACQUIRE );
        uint64_t need, reads, steps, phase, played, written, avail, take;
        uint32_t rs, bps;

        if (!s || s->format == IA_FMT_NONE || !s->capacity) continue;
        if (!__atomic_load_n( &s->started, __ATOMIC_ACQUIRE )) continue;
        if (!(rs = __atomic_load_n( &s->rate, __ATOMIC_RELAXED ))) continue;
        bps = sample_bytes( s->format );

        /* The read position carries its fraction between passes (Clarification 4): `phase` is
         * the fraction in units of 1/rd stream frame. Device frame f reads stream frame
         * floor((phase + f * rs) / rd) counted from the play position; the pass consumes the
         * whole frames it stepped over and keeps the remainder. Equal rates: one for one. */
        if (__atomic_exchange_n( &s->phase_reset, 0, __ATOMIC_ACQ_REL ) || s->phase >= rd) s->phase = 0;
        phase = s->phase;
        steps = phase + (uint64_t)n * rs;
        need = steps / rd;                                                /* frames consumed */
        reads = n ? (phase + (uint64_t)(n - 1) * rs) / rd + 1 : 0;        /* frames read */
        played = __atomic_load_n( &s->played, __ATOMIC_RELAXED );
        written = __atomic_load_n( &s->written, __ATOMIC_ACQUIRE );
        avail = written > played ? written - played : 0;
        if (avail > s->capacity) avail = s->capacity;
        take = need < avail ? need : avail;

        for (f = 0; f < n; f++)
        {
            uint64_t sf = (phase + (uint64_t)f * rs) / rd;
            const uint8_t *p;
            float l = 0.0f, r = 0.0f;
            unsigned int c;

            if (sf >= avail) break;   /* an underrun mixes what is there */
            p = s->ring + ((played + sf) % s->capacity) * s->frame_bytes;
            for (c = 0; c < s->gain_count; c++)
            {
                float v = sample_at( p + c * bps, s->format );
                l += v * s->gain_l[c];
                r += v * s->gain_r[c];
            }
            if (oc == 2)
            {
                out[2 * f] += l;
                out[2 * f + 1] += r;
            }
            else out[f] += 0.5f * (l + r);
        }
        __atomic_store_n( &s->played, played + take, __ATOMIC_RELEASE );
        if (reads > avail)
        {
            /* an underrun pass: the position pauses, the fraction restarts */
            s->phase = 0;
            __atomic_add_fetch( &s->short_passes, 1, __ATOMIC_RELAXED );
            __atomic_add_fetch( &s->missing_frames, reads - avail, __ATOMIC_RELAXED );
        }
        else s->phase = steps % rd;
    }

    for (k = 0; k < total; k++)
    {
        float v = out[k];
        if (v > 1.0f) { out[k] = 1.0f; clamped++; }
        else if (v < -1.0f) { out[k] = -1.0f; clamped++; }
        else if (v != v) { out[k] = 0.0f; clamped++; }
    }
    if (clamped) __atomic_add_fetch( &m->clamped, clamped, __ATOMIC_RELAXED );
    __atomic_add_fetch( &m->passes, 1, __ATOMIC_RELEASE );
}

/* ---------------------------------------------------------------- pacing (C10) */

#define MS_NS 1000000ull

uint32_t ia_lead_frames( uint32_t lead_ms, uint32_t rate, uint32_t capacity, uint32_t period )
{
    uint64_t lead = (uint64_t)lead_ms * rate / 1000;

    if (!period) period = 1;
    if (lead + period > capacity) lead = capacity > period ? capacity - period : 0;
    if (lead < period) lead = period;
    return (uint32_t)lead;
}

void ia_pace_step( struct ia_pace *pace, uint32_t padding, uint32_t lead, uint32_t rate, uint64_t writes,
                   uint64_t now_ns, struct ia_pace_decision *out )
{
    out->signal = 0;
    if (padding < lead)
    {
        int wrote = writes != pace->writes_at_signal;

        if (wrote) pace->unanswered = 0;
        if (wrote || !pace->signalled || now_ns - pace->last_signal_ns >= 10 * MS_NS)
        {
            if (!wrote && pace->signalled) pace->unanswered++;   /* the previous signal went unanswered */
            out->signal = 1;
            pace->signalled = 1;
            pace->last_signal_ns = now_ns;
            pace->writes_at_signal = writes;
        }
        out->sleep_ns = pace->unanswered >= 2 ? 10 * MS_NS : 1 * MS_NS;
    }
    else
    {
        /* sleep until the padding has used up what is above the lead, 1-10 ms */
        uint64_t ns = rate ? ((uint64_t)(padding - lead) + 1) * 1000000000ull / rate : 10 * MS_NS;
        if (ns < MS_NS) ns = MS_NS;
        if (ns > 10 * MS_NS) ns = 10 * MS_NS;
        out->sleep_ns = ns;
    }
}

/* ---------------------------------------------------------------- kept sessions (spec 22 Part F) */

int ia_keep_unit_running( int session_reuse, int unit_running )
{
    return session_reuse && unit_running;
}

int ia_plain_beat( int has_event, int started, int null_mode, uint32_t lead_ms, int unit_running )
{
    return !has_event || !started || null_mode || !lead_ms || !unit_running;
}
