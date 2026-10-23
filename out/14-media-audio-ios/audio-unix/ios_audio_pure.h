// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part C: the pure parts of the audio driver (no Wine, no Apple), shared with the host
// tests: the downmix gains (C8), the mixer that runs on the output unit's real-time thread (C6),
// the timer thread's pacing decision (C10), the ring capacity (C3), the endpoint buffer layout
// and the mix format (C2), and the format classes (C4). Also the plain-C interface of the
// output-unit object (ios_audio_apple.c).

#ifndef IOS_AUDIO_PURE_H
#define IOS_AUDIO_PURE_H

#include <stddef.h>
#include <stdint.h>

#define IA_MAX_STREAMS 16
#define IA_MAX_GAINS 12          /* 7.1.4 is Wine's spatial audio bed */

/* ---------------------------------------------------------------- formats (C4) */

enum ia_format
{
    IA_FMT_NONE = 0,             /* not mixable: the stream runs in null mode */
    IA_FMT_F32,
    IA_FMT_S16,
    IA_FMT_S32,
};

/* tag: wFormatTag; subformat: the extensible SubFormat (16 bytes) or NULL. */
enum ia_format ia_classify_format( uint16_t tag, uint16_t bits, const uint8_t *subformat,
                                   uint32_t channels, uint32_t block_align );
const char *ia_format_name( enum ia_format f );

/* ---------------------------------------------------------------- C3, C8 */

/* the requested duration (100 ns) in frames, clamped to [rate/10, 4 * rate] */
uint32_t ia_ring_capacity( int64_t duration, uint32_t rate );

/* the standard layout's mask for 2-8 channels, 7.1 beyond */
uint32_t ia_standard_mask( uint32_t channels );

/* Fills gain_l/gain_r (IA_MAX_GAINS entries each) and returns how many channels get a gain
 * (min(channels, 12)). downmix == 0: MADEIRA_AUDIO_DOWNMIX=0. */
unsigned int ia_downmix_gains( uint32_t channels, uint32_t mask, int downmix, float *gain_l, float *gain_r );

/* MADEIRA_AUDIO_LEAD_MS: 10-500, default 60; a value starting with '0' turns the lead off (0);
 * anything else outside 10-500 means 60. */
uint32_t ia_parse_lead_ms( const char *value );

/* ---------------------------------------------------------------- C2 */

/* The render endpoint list: one endpoint, name "iOS Null" (UTF-16), device "ios-null", laid out
 * after the endpoint array (offsets from the buffer start). Returns 0 and fills buf when
 * `size` suffices; otherwise returns -1 with *needed set. `buf` holds `struct { unsigned name,
 * device; }` entries as mmdevapi's struct endpoint. */
int ia_endpoint_list( void *buf, unsigned int size, unsigned int *needed, unsigned int *count );

/* The shared-mode mix format: WAVEFORMATEXTENSIBLE (40 bytes), 48000 Hz, 2 channels, 32-bit
 * IEEE float, mask 0x3, 32 valid bits, SubFormat KSDATAFORMAT_SUBTYPE_IEEE_FLOAT. */
#define IA_MIX_FORMAT_SIZE 40
void ia_mix_format( uint8_t out[IA_MIX_FORMAT_SIZE] );

/* ---------------------------------------------------------------- the mixer (C6) */

/* What the mixer reads of a stream. Written fully before the stream is published; afterwards
 * only the atomics change. */
struct ia_shared
{
    uint32_t started;            /* atomic */
    uint32_t rate;               /* atomic: set_sample_rate changes it */
    enum ia_format format;
    uint32_t channels;
    uint32_t frame_bytes;        /* nBlockAlign */
    uint32_t capacity;           /* frames */
    uint8_t *ring;               /* capacity * frame_bytes */
    unsigned int gain_count;
    float gain_l[IA_MAX_GAINS], gain_r[IA_MAX_GAINS];
    uint64_t written;            /* atomic: frames written, published with release */
    uint64_t played;             /* atomic: frames played, published with release by the mixer */
    uint64_t short_passes;       /* atomic counters, written by the mixer only */
    uint64_t missing_frames;
    uint64_t phase;              /* the read position's fraction, 1/rd frames; the mixer's own */
    uint32_t phase_reset;        /* atomic: set by start, reset and set_sample_rate */
};

struct ia_mixer
{
    struct ia_shared *slots[IA_MAX_STREAMS];   /* atomic pointers */
    uint32_t device_rate;
    uint32_t device_channels;    /* 1 or 2 */
    uint64_t passes;             /* atomic */
    uint64_t clamped;            /* atomic */
};

/* One render pass: no locks, allocation, logging or Wine calls. `out` holds `frames` frames of
 * device_channels float32; nothing is written past `bytes`. */
void ia_mix( struct ia_mixer *mixer, float *out, uint32_t frames, uint32_t bytes );

/* padding = written - played (read played first so the difference can't wrap), clamped to
 * [0, capacity] */
uint32_t ia_padding( const struct ia_shared *s );

/* ---------------------------------------------------------------- pacing (C10) */

struct ia_pace
{
    uint64_t last_signal_ns;
    uint64_t writes_at_signal;
    uint32_t unanswered;
    int signalled;
};

struct ia_pace_decision
{
    int signal;
    uint64_t sleep_ns;
};

/* The lead in frames: lead_ms at rate, reduced to capacity - period if it doesn't leave a
 * period of room, at least one period. */
uint32_t ia_lead_frames( uint32_t lead_ms, uint32_t rate, uint32_t capacity, uint32_t period );

/* writes: how many times the client has written so far (a counter). */
void ia_pace_step( struct ia_pace *pace, uint32_t padding, uint32_t lead, uint32_t rate, uint64_t writes,
                   uint64_t now_ns, struct ia_pace_decision *out );

/* ---------------------------------------------------------------- kept sessions (spec 22 Part F) */

/* F2: the last stream stopped; 1 when the running unit must keep running (silent) because the
 * session is kept (MYIOSDECK_SESSION_REUSE is 1). */
int ia_keep_unit_running( int session_reuse, int unit_running );

/* F3: 1 when a stream's timer gives the plain 10 ms beat (null mode's): no event, not started,
 * null mode, no pacing lead, or no running unit to drain the buffer. */
int ia_plain_beat( int has_event, int started, int null_mode, uint32_t lead_ms, int unit_running );

/* ---------------------------------------------------------------- the output unit (ios_audio_apple.c) */

typedef void (*ia_render_fn)( float *out, uint32_t frames, uint32_t bytes );

/* Creates the one RemoteIO output unit of the app: float32 packed interleaved at rate, channels
 * 1 or 2. 0 on success; otherwise -1 and a reason. */
int ia_unit_create( uint32_t rate, uint32_t channels, ia_render_fn render, char *why, size_t why_size );
int ia_unit_start( void );
int ia_unit_stop( void );

uint64_t ia_now_ns( void );                       /* monotonic */
void ia_sleep_until_ns( uint64_t deadline_ns );   /* absolute, monotonic */
/* the time-constraint policy on the calling thread (period, computation, constraint in ns) */
int ia_set_time_constraint( uint64_t period_ns, uint64_t computation_ns, uint64_t constraint_ns, int preemptible );

#endif /* IOS_AUDIO_PURE_H */
