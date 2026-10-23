// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part C: mmdevapi's unix side on iOS. One render endpoint ("iOS Null"), no capture, on
// the app's one RemoteIO output unit, which mixes every client of every pseudo-process.
//
// The call tables are reached by name (Part A3): winecoreaudio_unix_call_funcs /
// winecoreaudio_unix_call_wow64_funcs, and the same entries as wineios_unix_call_funcs /
// wineios_unix_call_wow64_funcs. Both follow dlls/mmdevapi/unixlib.h's enum unix_funcs (37
// entries). The 32-bit thunks follow the argument layouts upstream's winecoreaudio.drv thunks
// use, with every guest pointer converted by rule A4.
//
// Threads: control calls take `lock`; the mixer (ia_mix, ios_audio_pure.c) runs on the unit's
// real-time thread and touches only the published stream slots and atomics; timer_loop runs on
// a Wine thread per event-driven client and reads atomics only.

#if 0
#pragma makedep unix
#endif

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winternl.h"
#include "wingdi.h"
#include "wine/unixlib.h"
#include "mmsystem.h"
#include "mmreg.h"
#include "unixlib.h"     /* dlls/mmdevapi/unixlib.h (upstream, unchanged) */

#include "ios_audio_pure.h"
#include "ios_media_wow.h"
#include "media_log.h"

#define EXPORTED_TABLE __attribute__((used, visibility("default")))
#define LOG_PREFIX "[ios_audio] "
#define STALE_HANDLE_LOGS 8
#define PERIOD_100NS 100000      /* 10 ms */
#define MIN_PERIOD_100NS 50000   /* 5 ms */
#define MS_NS 1000000ull
#define PACING_LOG_NS (10000ull * MS_NS)
#define RELEASE_WAIT_NS (500ull * MS_NS)
#define HANDLE_MAGIC 0x1aull
#define MAX_RATE 768000

typedef UINT32 PTR32;

struct stream
{
    struct ia_shared sh;         /* what the mixer sees; published after it is complete */
    stream_handle handle;
    unsigned int slot;
    void *owner;                 /* the pseudo-process (PEB) that created it */
    int dead;                    /* atomic */
    int null_mode;
    int wow;                     /* 32-bit caller: the scratch lives in its window */
    DWORD flags;
    REFERENCE_TIME duration, period;
    uint32_t period_frames;
    HANDLE event;
    BYTE *scratch;
    uint32_t scratch_frames;
    SIZE_T scratch_size;
    uint32_t handed_out;
    uint64_t writes;             /* atomic: client writes, for the timer thread */
    uint64_t null_frames;        /* null mode: frames accumulated across stops */
    uint64_t null_start_ns;
    char exe[64];
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct stream *live[IA_MAX_STREAMS];
static uint32_t generation[IA_MAX_STREAMS];
static struct ia_mixer mixer;
static int unit_state;           /* 0 none yet, 1 created, -1 creation failed (never retried) */
static int unit_running;
static unsigned int started_count;
static uint32_t lead_ms;
static int downmix = 1;
static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static volatile int32_t stale_logs;

static void read_config( void )
{
    lead_ms = ia_parse_lead_ms( getenv( "MADEIRA_AUDIO_LEAD_MS" ) );
    downmix = !iom_env_is( "MADEIRA_AUDIO_DOWNMIX", "0" );
}

static void render( float *out, uint32_t frames, uint32_t bytes )
{
    ia_mix( &mixer, out, frames, bytes );
}

/* ---------------------------------------------------------------- handles (C3) */

static stream_handle make_handle( unsigned int slot )
{
    return (HANDLE_MAGIC << 56) | ((uint64_t)generation[slot] << 8) | (slot + 1);
}

/* the live stream of a handle; caller holds `lock` */
static struct stream *lookup_locked( stream_handle h )
{
    unsigned int slot = (unsigned int)(h & 0xff);

    if ((h >> 56) != HANDLE_MAGIC || !slot || slot > IA_MAX_STREAMS) return NULL;
    slot--;
    if (!live[slot] || live[slot]->handle != h) return NULL;
    return live[slot];
}

static void log_stale( stream_handle h, const char *call )
{
    int32_t n = __atomic_add_fetch( &stale_logs, 1, __ATOMIC_RELAXED );
    if (n <= STALE_HANDLE_LOGS)
        iom_log( LOG_PREFIX, "%s: stale or foreign stream handle %#llx%s", call, (unsigned long long)h,
                 n == STALE_HANDLE_LOGS ? " (no more of these lines)" : "" );
}

static struct stream *get_stream( stream_handle h, const char *call )
{
    struct stream *s;

    pthread_mutex_lock( &lock );
    s = lookup_locked( h );
    pthread_mutex_unlock( &lock );
    if (!s) log_stale( h, call );
    return s;
}

/* ---------------------------------------------------------------- the output unit (C5) */

/* caller holds `lock`. unit_running is also read (atomically) by the timer threads. */
static void unit_update( void )
{
    if (started_count && !unit_running && unit_state == 1)
    {
        if (!ia_unit_start()) __atomic_store_n( &unit_running, 1, __ATOMIC_RELEASE );
        else iom_log( LOG_PREFIX, "the output unit failed to start" );
    }
    else if (!started_count && unit_running)
    {
        /* spec 22 Part F2: in a kept session the started unit runs on, silent, until the app ends
         * (stopping and starting the shared unit at every game's end and start was suspected of
         * trapping inside Apple's audio framework) */
        if (ia_keep_unit_running( iom_env_is( "MYIOSDECK_SESSION_REUSE", "1" ), unit_running ))
        {
            static int logged;
            if (!logged++) iom_log( LOG_PREFIX, "no stream plays: the output unit keeps running (silent) for the kept session" );
            return;
        }
        ia_unit_stop();
        __atomic_store_n( &unit_running, 0, __ATOMIC_RELEASE );
    }
}

/* spec 22 Part F1: the streams of a pseudo-process that ended (spec 09 / spec 22 C6 hook) */
static void process_end_hook( const void *peb );

static void register_end_hook( void )
{
    if (ios_process_end_hook_add( process_end_hook ))
        iom_log( LOG_PREFIX, "the process-end hook could not be registered" );
}

/* waits (without `lock`) until `count` render passes have completed, at most 500 ms; a stopped
 * unit never passes */
static void wait_passes( unsigned int count )
{
    uint64_t first = __atomic_load_n( &mixer.passes, __ATOMIC_ACQUIRE ), t0 = ia_now_ns();

    for (;;)
    {
        int running;

        pthread_mutex_lock( &lock );
        running = unit_running;
        pthread_mutex_unlock( &lock );
        if (!running) return;
        if (__atomic_load_n( &mixer.passes, __ATOMIC_ACQUIRE ) >= first + count) return;
        if (ia_now_ns() - t0 >= RELEASE_WAIT_NS) return;
        ia_sleep_until_ns( ia_now_ns() + MS_NS );
    }
}

/* ---------------------------------------------------------------- render scratch (C3, C9) */

static BYTE *alloc_scratch( struct stream *s, uint32_t frames, SIZE_T *size_out )
{
    SIZE_T size = (SIZE_T)frames * s->sh.frame_bytes;
    void *addr = NULL;

    if (!size) size = s->sh.frame_bytes;
    if (!s->wow) return calloc( 1, size );
    /* a 32-bit caller: inside its window (a guest limit below 2 GB, spec 10 Part E4) */
    if (NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0x7fffffff, &size, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE )) return NULL;
    if (!ios_wow_in_window( addr ))
    {
        SIZE_T zero = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &addr, &zero, MEM_RELEASE );
        iom_log( LOG_PREFIX, "a 32-bit client's buffer landed outside its window; refused" );
        return NULL;
    }
    *size_out = size;
    return addr;
}

static void free_scratch( struct stream *s )
{
    if (!s->scratch) return;
    if (s->wow)
    {
        void *addr = s->scratch;
        SIZE_T zero = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &addr, &zero, MEM_RELEASE );
    }
    else free( s->scratch );
    s->scratch = NULL;
    s->scratch_frames = 0;
}

static void free_stream( struct stream *s )
{
    free_scratch( s );
    free( s->sh.ring );
    free( s );
}

/* ---------------------------------------------------------------- endpoints and formats (C2) */

static NTSTATUS drv_process_attach( void *args )
{
    static pthread_once_t hook_once = PTHREAD_ONCE_INIT;

    pthread_once( &config_once, read_config );
    pthread_once( &hook_once, register_end_hook );   /* spec 22 Part F1 */
    return STATUS_SUCCESS;
}

static NTSTATUS drv_main_loop( void *args )
{
    struct main_loop_params *params = args;
    NtSetEvent( params->event, NULL );   /* the PE side waits for it */
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_endpoint_ids( void *args )
{
    struct get_endpoint_ids_params *params = args;
    unsigned int needed, count;

    params->num = 0;
    params->default_idx = 0;
    if (params->flow == eRender)
    {
        if (ia_endpoint_list( params->endpoints, params->size, &needed, &count ))
        {
            params->size = needed;
            params->num = count;
            params->result = HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
        }
        else
        {
            params->num = count;
            params->result = S_OK;
        }
    }
    else if (params->flow == eCapture) params->result = S_OK;   /* no capture endpoints */
    else params->result = E_INVALIDARG;
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_mix_format( void *args )
{
    struct get_mix_format_params *params = args;

    if (!params->fmt)
    {
        params->result = E_POINTER;
        return STATUS_SUCCESS;
    }
    ia_mix_format( (uint8_t *)params->fmt );
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS drv_is_format_supported( void *args )
{
    struct is_format_supported_params *params = args;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_loopback_capture_device( void *args )
{
    struct get_loopback_capture_device_params *params = args;
    params->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_device_period( void *args )
{
    struct get_device_period_params *params = args;

    if (params->def_period) *params->def_period = PERIOD_100NS;
    if (params->min_period) *params->min_period = MIN_PERIOD_100NS;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS drv_test_connect( void *args )
{
    struct test_connect_params *params = args;
    params->priority = Priority_Preferred;
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_prop_value( void *args )
{
    struct get_prop_value_params *params = args;
    params->result = E_FAIL;   /* mmdevapi falls back */
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- streams (C3, C4, C5) */

static void exe_name( const WCHAR *name, char *out, size_t size )
{
    const WCHAR *p, *base = name;
    size_t n = 0;

    if (!name)
    {
        snprintf( out, size, "?" );
        return;
    }
    for (p = name; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    for (p = base; *p && n + 1 < size; p++) out[n++] = *p < 0x80 ? (char)*p : '?';
    out[n] = 0;
}

static HRESULT do_create_stream( const WCHAR *name, EDataFlow flow, DWORD flags, REFERENCE_TIME duration,
                                 REFERENCE_TIME period, const WAVEFORMATEX *fmt, stream_handle *handle,
                                 UINT32 *channel_count )
{
    const uint8_t *subformat = NULL;
    uint32_t rate, channels, frame_bytes, mask = 0;
    unsigned int slot, i;
    struct stream *s;
    char why[128] = "";

    pthread_once( &config_once, read_config );
    if (flow != eRender) return E_INVALIDARG;   /* no capture endpoint */
    if (!fmt || !handle || !channel_count) return E_POINTER;

    rate = fmt->nSamplesPerSec ? fmt->nSamplesPerSec : 48000;
    channels = fmt->nChannels ? fmt->nChannels : 2;
    frame_bytes = fmt->nBlockAlign ? fmt->nBlockAlign : channels * 4;
    if (rate > MAX_RATE) return E_INVALIDARG;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)fmt;
        subformat = (const uint8_t *)&ext->SubFormat;
        mask = ext->dwChannelMask;
    }

    if (!(s = calloc( 1, sizeof(*s) ))) return E_OUTOFMEMORY;
    s->sh.rate = rate;
    s->sh.channels = channels;
    s->sh.frame_bytes = frame_bytes;
    s->sh.format = ia_classify_format( fmt->wFormatTag, fmt->wBitsPerSample, subformat, channels, frame_bytes );
    s->sh.capacity = ia_ring_capacity( duration, rate );
    s->sh.gain_count = ia_downmix_gains( channels, mask, downmix, s->sh.gain_l, s->sh.gain_r );
    s->flags = flags;
    s->duration = duration;
    s->period = period;
    s->period_frames = rate / 100 ? rate / 100 : 1;
    s->owner = ios_current_peb();
    s->wow = ios_wow_base() != 0;
    exe_name( name, s->exe, sizeof(s->exe) );

    if (!(s->sh.ring = calloc( s->sh.capacity, frame_bytes )) ||
        !(s->scratch = alloc_scratch( s, s->sh.capacity, &s->scratch_size )))
    {
        free_stream( s );
        return E_OUTOFMEMORY;
    }
    s->scratch_frames = s->sh.capacity;

    pthread_mutex_lock( &lock );
    for (slot = 0; slot < IA_MAX_STREAMS; slot++) if (!live[slot]) break;
    if (slot == IA_MAX_STREAMS)
    {
        pthread_mutex_unlock( &lock );
        iom_log( LOG_PREFIX, "%s: more than %d live streams; refused", s->exe, IA_MAX_STREAMS );
        free_stream( s );
        return E_OUTOFMEMORY;
    }
    generation[slot] = (generation[slot] + 1) & 0xffffffffu;
    s->slot = slot;
    s->handle = make_handle( slot );
    live[slot] = s;

    if (s->sh.format != IA_FMT_NONE && !unit_state)
    {
        /* the app's one output unit, from the first mixable stream; never per stream or process */
        mixer.device_rate = rate;
        mixer.device_channels = channels == 1 ? 1 : 2;
        if (!ia_unit_create( mixer.device_rate, mixer.device_channels, render, why, sizeof(why) ))
        {
            unit_state = 1;
            iom_log( LOG_PREFIX, "output unit created: RemoteIO, float32, %u Hz, %u ch", mixer.device_rate,
                     mixer.device_channels );
        }
        else
        {
            unit_state = -1;
            iom_log( LOG_PREFIX, "output unit creation failed (%s); every stream runs in null mode", why );
        }
    }
    s->null_mode = s->sh.format == IA_FMT_NONE || unit_state != 1;
    /* published only now: ring and gains are complete */
    if (!s->null_mode) __atomic_store_n( &mixer.slots[slot], &s->sh, __ATOMIC_RELEASE );
    *handle = s->handle;
    *channel_count = channels;
    pthread_mutex_unlock( &lock );

    if (s->sh.format == IA_FMT_NONE)
        iom_log( LOG_PREFIX, "%s: format tag %#x, %u bits is not mixable; the stream runs in null mode",
                 s->exe, fmt->wFormatTag, fmt->wBitsPerSample );
    iom_log( LOG_PREFIX, "stream %u created by %s: flags %#x, duration %lld ms, period %lld ms, %u Hz %u ch %s "
             "(align %u, mask %#x), ring %u frames%s", slot, s->exe, (unsigned int)flags,
             (long long)(duration / 10000), (long long)(period / 10000), rate, channels,
             ia_format_name( s->sh.format ), frame_bytes, mask, s->sh.capacity, s->null_mode ? ", null mode" : "" );
    {
        char line[400];
        int len = 0;
        for (i = 0; i < s->sh.gain_count && len < (int)sizeof(line) - 24; i++)
            len += snprintf( line + len, sizeof(line) - len, "%s%u:%.3f/%.3f", i ? " " : "", i,
                             s->sh.gain_l[i], s->sh.gain_r[i] );
        iom_log( LOG_PREFIX, "stream %u gains (L/R)%s: %s%s", slot, downmix ? "" : " (downmix off)", line,
                 channels > IA_MAX_GAINS ? " (channels beyond 12 dropped)" : "" );
    }
    return S_OK;
}

static NTSTATUS drv_create_stream( void *args )
{
    struct create_stream_params *params = args;

    params->result = do_create_stream( params->name, params->flow, params->flags, params->duration, params->period,
                                       params->fmt, params->stream, params->channel_count );
    return STATUS_SUCCESS;
}

/* unpublishes s from the mixer and the live set; caller holds `lock` */
static void retire_locked( struct stream *s )
{
    if (__atomic_exchange_n( &s->sh.started, 0, __ATOMIC_ACQ_REL ) && !s->null_mode && started_count)
        started_count--;
    __atomic_store_n( &s->dead, 1, __ATOMIC_RELEASE );
    if (__atomic_load_n( &mixer.slots[s->slot], __ATOMIC_ACQUIRE ) == &s->sh)
        __atomic_store_n( &mixer.slots[s->slot], NULL, __ATOMIC_RELEASE );
    if (live[s->slot] == s) live[s->slot] = NULL;
}

static NTSTATUS drv_release_stream( void *args )
{
    struct release_stream_params *params = args;
    struct stream *s;

    pthread_mutex_lock( &lock );
    if (!(s = lookup_locked( params->stream )))
    {
        pthread_mutex_unlock( &lock );
        log_stale( params->stream, "release_stream" );
        params->result = AUDCLNT_E_NOT_INITIALIZED;
        return STATUS_SUCCESS;
    }
    /* dead and not started: the timer thread leaves its loop */
    if (__atomic_exchange_n( &s->sh.started, 0, __ATOMIC_ACQ_REL ) && !s->null_mode && started_count)
        started_count--;
    __atomic_store_n( &s->dead, 1, __ATOMIC_RELEASE );
    pthread_mutex_unlock( &lock );

    if (params->timer_thread)
    {
        NtWaitForSingleObject( params->timer_thread, FALSE, NULL );
        NtClose( params->timer_thread );
    }

    pthread_mutex_lock( &lock );
    retire_locked( s );
    pthread_mutex_unlock( &lock );
    if (!s->null_mode) wait_passes( 2 );   /* the mixer is done with it */

    pthread_mutex_lock( &lock );
    unit_update();
    pthread_mutex_unlock( &lock );
    iom_log( LOG_PREFIX, "stream %u released (%s): %llu frames written, %llu short passes, %llu frames missing",
             s->slot, s->exe, (unsigned long long)s->sh.written, (unsigned long long)s->sh.short_passes,
             (unsigned long long)s->sh.missing_frames );
    free_stream( s );
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* Stops and unpublishes the streams of one pseudo-process. Their timer threads can't be joined
 * here, so they are not freed (the memory stays; the slots are reused). */
static unsigned int retire_process( void *peb )
{
    unsigned int i, n = 0;

    pthread_mutex_lock( &lock );
    for (i = 0; i < IA_MAX_STREAMS; i++)
    {
        struct stream *s = live[i];
        if (!s || s->owner != peb) continue;
        retire_locked( s );
        n++;
    }
    unit_update();
    pthread_mutex_unlock( &lock );
    return n;
}

static NTSTATUS drv_process_detach( void *args )
{
    unsigned int n = retire_process( ios_current_peb() );
    if (n) iom_log( LOG_PREFIX, "process detach: %u stream(s) of this process stopped and unpublished", n );
    return STATUS_SUCCESS;
}

/* A pseudo-process ended without detaching (spec 09 Part D): registered with spec 22 C6's
 * ios_process_end_hook_add (process_end_hook below), so an ended game's stream is neither mixed
 * nor counted. */
__attribute__((used, visibility("default"))) void ios_audio_process_ended( void *peb )
{
    unsigned int n = retire_process( peb );
    if (n) iom_log( LOG_PREFIX, "process ended: %u stream(s) stopped and unpublished", n );
}

static void process_end_hook( const void *peb )
{
    ios_audio_process_ended( (void *)peb );
}

/* ---------------------------------------------------------------- positions (C7) */

static uint64_t null_position( const struct stream *s, uint64_t now )
{
    uint64_t frames = s->null_frames;
    uint32_t rate = __atomic_load_n( &s->sh.rate, __ATOMIC_RELAXED );

    if (__atomic_load_n( &s->sh.started, __ATOMIC_ACQUIRE ) && now > s->null_start_ns)
        frames += (now - s->null_start_ns) * rate / 1000000000ull;
    return frames;
}

static NTSTATUS drv_start( void *args )
{
    struct start_params *params = args;
    struct stream *s;

    pthread_mutex_lock( &lock );
    if (!(s = lookup_locked( params->stream ))) params->result = AUDCLNT_E_NOT_INITIALIZED;
    else if ((s->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !s->event) params->result = AUDCLNT_E_EVENTHANDLE_NOT_SET;
    else if (__atomic_load_n( &s->sh.started, __ATOMIC_ACQUIRE )) params->result = AUDCLNT_E_NOT_STOPPED;
    else
    {
        if (s->null_mode) s->null_start_ns = ia_now_ns();
        __atomic_store_n( &s->sh.phase_reset, 1, __ATOMIC_RELEASE );   /* the fraction restarts (C6) */
        __atomic_store_n( &s->sh.started, 1, __ATOMIC_RELEASE );   /* before the unit: the first pass sees it */
        if (!s->null_mode)
        {
            started_count++;
            unit_update();
        }
        params->result = S_OK;
    }
    pthread_mutex_unlock( &lock );
    if (!s) log_stale( params->stream, "start" );
    return STATUS_SUCCESS;
}

static NTSTATUS drv_stop( void *args )
{
    struct stop_params *params = args;
    struct stream *s;

    pthread_mutex_lock( &lock );
    if (!(s = lookup_locked( params->stream ))) params->result = AUDCLNT_E_NOT_INITIALIZED;
    else if (!__atomic_load_n( &s->sh.started, __ATOMIC_ACQUIRE )) params->result = S_FALSE;
    else
    {
        if (s->null_mode) s->null_frames = null_position( s, ia_now_ns() );
        __atomic_store_n( &s->sh.started, 0, __ATOMIC_RELEASE );
        if (!s->null_mode && started_count)
        {
            started_count--;
            unit_update();
        }
        params->result = S_OK;
    }
    pthread_mutex_unlock( &lock );
    if (!s) log_stale( params->stream, "stop" );
    return STATUS_SUCCESS;
}

static NTSTATUS drv_reset( void *args )
{
    struct reset_params *params = args;
    struct stream *s;

    pthread_mutex_lock( &lock );
    if (!(s = lookup_locked( params->stream )))
    {
        pthread_mutex_unlock( &lock );
        log_stale( params->stream, "reset" );
        params->result = AUDCLNT_E_NOT_INITIALIZED;
        return STATUS_SUCCESS;
    }
    if (__atomic_load_n( &s->sh.started, __ATOMIC_ACQUIRE ))
    {
        pthread_mutex_unlock( &lock );
        params->result = AUDCLNT_E_NOT_STOPPED;
        return STATUS_SUCCESS;
    }
    if (s->handed_out)
    {
        pthread_mutex_unlock( &lock );
        params->result = AUDCLNT_E_BUFFER_OPERATION_PENDING;
        return STATUS_SUCCESS;
    }
    pthread_mutex_unlock( &lock );

    /* a pass that saw the stream started may still be running: let it finish */
    if (!s->null_mode) wait_passes( 1 );

    pthread_mutex_lock( &lock );
    if (lookup_locked( params->stream ) == s)
    {
        __atomic_store_n( &s->sh.played, 0, __ATOMIC_RELEASE );
        __atomic_store_n( &s->sh.written, 0, __ATOMIC_RELEASE );
        __atomic_store_n( &s->sh.phase_reset, 1, __ATOMIC_RELEASE );
        s->null_frames = 0;
        params->result = S_OK;
    }
    else params->result = AUDCLNT_E_NOT_INITIALIZED;
    pthread_mutex_unlock( &lock );
    return STATUS_SUCCESS;
}

static NTSTATUS drv_is_started( void *args )
{
    struct is_started_params *params = args;
    struct stream *s;

    if (!(s = get_stream( params->stream, "is_started" ))) params->result = AUDCLNT_E_NOT_INITIALIZED;
    else params->result = __atomic_load_n( &s->sh.started, __ATOMIC_ACQUIRE ) ? S_OK : S_FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_current_padding( void *args )
{
    struct get_current_padding_params *params = args;
    struct stream *s;

    if (!(s = get_stream( params->stream, "get_current_padding" )))
    {
        params->result = AUDCLNT_E_NOT_INITIALIZED;
        return STATUS_SUCCESS;
    }
    *params->padding = s->null_mode ? 0 : ia_padding( &s->sh );   /* null mode: always hungry */
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_position( void *args )
{
    struct get_position_params *params = args;
    struct stream *s;
    uint64_t now;

    if (!(s = get_stream( params->stream, "get_position" )))
    {
        params->result = AUDCLNT_E_NOT_INITIALIZED;
        return STATUS_SUCCESS;
    }
    now = ia_now_ns();
    pthread_mutex_lock( &lock );
    *params->pos = s->null_mode ? null_position( s, now ) : __atomic_load_n( &s->sh.played, __ATOMIC_ACQUIRE );
    pthread_mutex_unlock( &lock );
    if (params->qpctime) *params->qpctime = now / 100;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_buffer_size( void *args )
{
    struct get_buffer_size_params *params = args;
    struct stream *s;

    if (!(s = get_stream( params->stream, "get_buffer_size" ))) params->result = AUDCLNT_E_NOT_INITIALIZED;
    else
    {
        *params->frames = s->sh.capacity;
        params->result = S_OK;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_latency( void *args )
{
    struct get_latency_params *params = args;
    struct stream *s;

    if (!(s = get_stream( params->stream, "get_latency" ))) params->result = AUDCLNT_E_NOT_INITIALIZED;
    else
    {
        *params->latency = PERIOD_100NS;
        params->result = S_OK;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_next_packet_size( void *args )
{
    struct get_next_packet_size_params *params = args;
    struct stream *s;

    if (!(s = get_stream( params->stream, "get_next_packet_size" ))) params->result = AUDCLNT_E_NOT_INITIALIZED;
    else
    {
        *params->frames = 0;
        params->result = S_OK;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_frequency( void *args )
{
    struct get_frequency_params *params = args;
    struct stream *s;

    if (!(s = get_stream( params->stream, "get_frequency" ))) params->result = AUDCLNT_E_NOT_INITIALIZED;
    else
    {
        *params->freq = __atomic_load_n( &s->sh.rate, __ATOMIC_RELAXED );
        params->result = S_OK;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS drv_set_sample_rate( void *args )
{
    struct set_sample_rate_params *params = args;
    struct stream *s;

    pthread_mutex_lock( &lock );
    if (!(s = lookup_locked( params->stream ))) params->result = AUDCLNT_E_NOT_INITIALIZED;
    else if (!(params->rate > 0.0f) || params->rate > MAX_RATE) params->result = E_INVALIDARG;
    else
    {
        uint64_t now = ia_now_ns();
        if (s->null_mode && __atomic_load_n( &s->sh.started, __ATOMIC_ACQUIRE ))
        {
            s->null_frames = null_position( s, now );   /* the clock so far, at the old rate */
            s->null_start_ns = now;
        }
        __atomic_store_n( &s->sh.rate, (uint32_t)params->rate, __ATOMIC_RELAXED );
        __atomic_store_n( &s->sh.phase_reset, 1, __ATOMIC_RELEASE );
        params->result = S_OK;
    }
    pthread_mutex_unlock( &lock );
    if (!s) log_stale( params->stream, "set_sample_rate" );
    return STATUS_SUCCESS;
}

static NTSTATUS drv_set_volumes( void *args )
{
    return STATUS_SUCCESS;   /* accepted and ignored */
}

static NTSTATUS drv_set_event_handle( void *args )
{
    struct set_event_handle_params *params = args;
    struct stream *s;

    pthread_mutex_lock( &lock );
    if (!(s = lookup_locked( params->stream ))) params->result = AUDCLNT_E_NOT_INITIALIZED;
    else
    {
        s->event = params->event;
        params->result = S_OK;
    }
    pthread_mutex_unlock( &lock );
    if (!s) log_stale( params->stream, "set_event_handle" );
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- render buffers (C9) */

static HRESULT do_get_render_buffer( stream_handle handle, UINT32 frames, BYTE **data )
{
    struct stream *s;
    HRESULT hr = S_OK;

    *data = NULL;
    pthread_mutex_lock( &lock );
    if (!(s = lookup_locked( handle ))) hr = AUDCLNT_E_NOT_INITIALIZED;
    else if (s->handed_out) hr = AUDCLNT_E_OUT_OF_ORDER;
    else if (!frames) hr = S_OK;
    else if ((uint64_t)frames + (s->null_mode ? 0 : ia_padding( &s->sh )) > s->sh.capacity)
        hr = AUDCLNT_E_BUFFER_TOO_LARGE;
    else
    {
        if (frames > s->scratch_frames)
        {
            SIZE_T size = 0;
            BYTE *bigger = alloc_scratch( s, frames, &size );

            if (!bigger) hr = E_OUTOFMEMORY;
            else
            {
                free_scratch( s );
                s->scratch = bigger;
                s->scratch_frames = frames;
                s->scratch_size = size;
            }
        }
        if (hr == S_OK)
        {
            /* zeroed: Wine's spatial audio renderer adds into this buffer without clearing it */
            memset( s->scratch, 0, (size_t)frames * s->sh.frame_bytes );
            s->handed_out = frames;
            *data = s->scratch;
        }
    }
    pthread_mutex_unlock( &lock );
    if (!s) log_stale( handle, "get_render_buffer" );
    return hr;
}

static NTSTATUS drv_get_render_buffer( void *args )
{
    struct get_render_buffer_params *params = args;
    params->result = do_get_render_buffer( params->stream, params->frames, params->data );
    return STATUS_SUCCESS;
}

static void cancel_render_buffer( stream_handle handle )
{
    struct stream *s;

    pthread_mutex_lock( &lock );
    if ((s = lookup_locked( handle ))) s->handed_out = 0;
    pthread_mutex_unlock( &lock );
}

static NTSTATUS drv_release_render_buffer( void *args )
{
    struct release_render_buffer_params *params = args;
    struct stream *s;

    pthread_mutex_lock( &lock );
    if (!(s = lookup_locked( params->stream ))) params->result = AUDCLNT_E_NOT_INITIALIZED;
    else if (!params->written_frames)
    {
        s->handed_out = 0;
        params->result = S_OK;
    }
    else if (!s->handed_out) params->result = AUDCLNT_E_OUT_OF_ORDER;
    else
    {
        uint32_t n = params->written_frames < s->handed_out ? params->written_frames : s->handed_out;

        if (!s->null_mode)
        {
            uint64_t w = __atomic_load_n( &s->sh.written, __ATOMIC_RELAXED );
            uint32_t pos = (uint32_t)(w % s->sh.capacity), first = s->sh.capacity - pos;
            size_t fb = s->sh.frame_bytes;
            BYTE *ring = s->sh.ring;

            if (first > n) first = n;
            if (params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                memset( ring + pos * fb, 0, first * fb );
                memset( ring, 0, (n - first) * fb );
            }
            else
            {
                memcpy( ring + pos * fb, s->scratch, first * fb );
                memcpy( ring, s->scratch + first * fb, (n - first) * fb );
            }
            /* published after the frames are in: the mixer never reads half-written frames */
            __atomic_store_n( &s->sh.written, w + n, __ATOMIC_RELEASE );
        }
        __atomic_add_fetch( &s->writes, 1, __ATOMIC_RELEASE );
        s->handed_out = 0;
        params->result = S_OK;
    }
    pthread_mutex_unlock( &lock );
    if (!s) log_stale( params->stream, "release_render_buffer" );
    return STATUS_SUCCESS;
}

static NTSTATUS drv_get_capture_buffer( void *args )
{
    struct get_capture_buffer_params *params = args;

    if (params->data) *params->data = NULL;
    if (params->frames) *params->frames = 0;
    if (params->flags) *params->flags = 0;
    if (params->devpos) *params->devpos = 0;
    if (params->qpcpos) *params->qpcpos = 0;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS drv_release_capture_buffer( void *args )
{
    struct release_capture_buffer_params *params = args;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- the timer thread (C10) */

struct pacing_stats
{
    uint64_t window_start_ns;
    uint64_t written_at_start;
    uint64_t missing_at_start;
    uint64_t clamped_at_start;
    uint32_t wakes;
    uint32_t pad_min, pad_max;
    uint64_t worst_late_ns;
};

static void pacing_reset( struct pacing_stats *st, const struct stream *s, uint64_t now )
{
    st->window_start_ns = now;
    st->written_at_start = __atomic_load_n( &s->sh.written, __ATOMIC_RELAXED );
    st->missing_at_start = __atomic_load_n( &s->sh.missing_frames, __ATOMIC_RELAXED );
    st->clamped_at_start = __atomic_load_n( &mixer.clamped, __ATOMIC_RELAXED );
    st->wakes = 0;
    st->pad_min = UINT32_MAX;
    st->pad_max = 0;
    st->worst_late_ns = 0;
}

static void pacing_log( struct pacing_stats *st, const struct stream *s, uint32_t lead, uint64_t now )
{
    uint32_t rate = __atomic_load_n( &s->sh.rate, __ATOMIC_RELAXED );
    uint64_t written = __atomic_load_n( &s->sh.written, __ATOMIC_RELAXED ) - st->written_at_start;
    uint64_t missing = __atomic_load_n( &s->sh.missing_frames, __ATOMIC_RELAXED ) - st->missing_at_start;
    uint64_t clamped = __atomic_load_n( &mixer.clamped, __ATOMIC_RELAXED ) - st->clamped_at_start;

    iom_log( LOG_PREFIX, "pacing stream %u (%s): %llu ms written in %llu ms, %u wakes, device short %llu ms, "
             "padding %u-%u (lead %u), worst late wake %llu us, %llu clamped samples", s->slot, s->exe,
             (unsigned long long)(rate ? written * 1000 / rate : 0),
             (unsigned long long)((now - st->window_start_ns) / MS_NS), st->wakes,
             (unsigned long long)(rate ? missing * 1000 / rate : 0), st->pad_min == UINT32_MAX ? 0 : st->pad_min,
             st->pad_max, lead, (unsigned long long)(st->worst_late_ns / 1000), (unsigned long long)clamped );
}

static NTSTATUS drv_timer_loop( void *args )
{
    struct timer_loop_params *params = args;
    struct ia_pace pace = { 0 };
    struct pacing_stats stats;
    struct stream *s;
    uint64_t now, deadline;

    if (!(s = get_stream( params->stream, "timer_loop" ))) return STATUS_SUCCESS;
    pthread_once( &config_once, read_config );
    if (lead_ms && ia_set_time_constraint( 10 * MS_NS, MS_NS / 2, 5 * MS_NS, 1 ))
        iom_log( LOG_PREFIX, "stream %u: the time-constraint policy was refused", s->slot );

    now = ia_now_ns();
    deadline = now;
    pacing_reset( &stats, s, now );

    while (!__atomic_load_n( &s->dead, __ATOMIC_ACQUIRE ))
    {
        int started = __atomic_load_n( &s->sh.started, __ATOMIC_ACQUIRE );
        HANDLE event = s->event;

        /* spec 22 Part F3: a started stream whose unit isn't running (its creation failed, or it
         * was stopped) gets the 10 ms beat, never no events */
        if (ia_plain_beat( event != NULL, started, s->null_mode, lead_ms,
                           __atomic_load_n( &unit_running, __ATOMIC_ACQUIRE ) ))
        {
            /* a plain 10 ms beat */
            deadline += 10 * MS_NS;
            now = ia_now_ns();
            if (deadline < now) deadline = now + 10 * MS_NS;
            ia_sleep_until_ns( deadline );
            if (event && __atomic_load_n( &s->sh.started, __ATOMIC_ACQUIRE ) &&
                !__atomic_load_n( &s->dead, __ATOMIC_ACQUIRE ))
                NtSetEvent( event, NULL );
            continue;
        }
        else
        {
            uint32_t rate = __atomic_load_n( &s->sh.rate, __ATOMIC_RELAXED );
            uint32_t lead = ia_lead_frames( lead_ms, rate, s->sh.capacity, rate / 100 ? rate / 100 : 1 );
            uint32_t padding = ia_padding( &s->sh );   /* played read before written */
            uint64_t writes = __atomic_load_n( &s->writes, __ATOMIC_ACQUIRE );
            struct ia_pace_decision d;

            now = ia_now_ns();
            if (now > deadline && now - deadline > stats.worst_late_ns) stats.worst_late_ns = now - deadline;
            stats.wakes++;
            if (padding < stats.pad_min) stats.pad_min = padding;
            if (padding > stats.pad_max) stats.pad_max = padding;

            ia_pace_step( &pace, padding, lead, rate, writes, now, &d );
            if (d.signal) NtSetEvent( event, NULL );
            if (now - stats.window_start_ns >= PACING_LOG_NS)
            {
                pacing_log( &stats, s, lead, now );
                pacing_reset( &stats, s, now );
            }
            deadline = now + d.sleep_ns;   /* absolute deadline */
            ia_sleep_until_ns( deadline );
        }
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- MIDI and aux (C12) */

static void log_once( volatile int *flag, const char *what )
{
    if (!__atomic_exchange_n( flag, 1, __ATOMIC_RELAXED )) iom_log( LOG_PREFIX, "%s", what );
}

static volatile int midi_init_logged, midi_out_logged, midi_in_logged, midi_wait_logged, aux_logged;

static NTSTATUS drv_midi_get_driver( void *args )
{
    return STATUS_SUCCESS;   /* nothing: mmdevapi keeps this driver as the MIDI driver */
}

static NTSTATUS drv_midi_init( void *args )
{
    struct midi_init_params *params = args;

    *params->err = DRV_FAILURE;   /* no MIDI: mmdevapi starts no notify thread */
    log_once( &midi_init_logged, "no MIDI on this port: midi_init answers DRV_FAILURE" );
    return STATUS_SUCCESS;
}

static NTSTATUS drv_midi_release( void *args )
{
    return STATUS_SUCCESS;
}

static NTSTATUS drv_midi_out_message( void *args )
{
    struct midi_out_message_params *params = args;

    *params->err = MMSYSERR_NOTSUPPORTED;
    if (params->notify) params->notify->send_notify = FALSE;
    log_once( &midi_out_logged, "MIDI out message: MMSYSERR_NOTSUPPORTED" );
    return STATUS_SUCCESS;
}

static NTSTATUS drv_midi_in_message( void *args )
{
    struct midi_in_message_params *params = args;

    *params->err = MMSYSERR_NOTSUPPORTED;
    if (params->notify) params->notify->send_notify = FALSE;
    log_once( &midi_in_logged, "MIDI in message: MMSYSERR_NOTSUPPORTED" );
    return STATUS_SUCCESS;
}

static NTSTATUS drv_midi_notify_wait( void *args )
{
    struct midi_notify_wait_params *params = args;

    /* a stub that returned without these would be a busy loop on garbage */
    *params->quit = TRUE;
    if (params->notify) params->notify->send_notify = FALSE;
    log_once( &midi_wait_logged, "MIDI notify wait: quit" );
    return STATUS_SUCCESS;
}

static NTSTATUS drv_aux_message( void *args )
{
    struct aux_message_params *params = args;

    *params->err = MMSYSERR_NOTSUPPORTED;
    log_once( &aux_logged, "aux message: MMSYSERR_NOTSUPPORTED" );
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- the 64-bit tables (C1) */

#define DRIVER_TABLE( table_name ) \
const unixlib_entry_t table_name[] EXPORTED_TABLE = \
{ \
    [process_attach] = drv_process_attach, \
    [process_detach] = drv_process_detach, \
    [main_loop] = drv_main_loop, \
    [get_endpoint_ids] = drv_get_endpoint_ids, \
    [create_stream] = drv_create_stream, \
    [release_stream] = drv_release_stream, \
    [start] = drv_start, \
    [stop] = drv_stop, \
    [reset] = drv_reset, \
    [timer_loop] = drv_timer_loop, \
    [get_render_buffer] = drv_get_render_buffer, \
    [release_render_buffer] = drv_release_render_buffer, \
    [get_capture_buffer] = drv_get_capture_buffer, \
    [release_capture_buffer] = drv_release_capture_buffer, \
    [is_format_supported] = drv_is_format_supported, \
    [get_loopback_capture_device] = drv_get_loopback_capture_device, \
    [get_mix_format] = drv_get_mix_format, \
    [get_device_period] = drv_get_device_period, \
    [get_buffer_size] = drv_get_buffer_size, \
    [get_latency] = drv_get_latency, \
    [get_current_padding] = drv_get_current_padding, \
    [get_next_packet_size] = drv_get_next_packet_size, \
    [get_frequency] = drv_get_frequency, \
    [get_position] = drv_get_position, \
    [set_volumes] = drv_set_volumes, \
    [set_event_handle] = drv_set_event_handle, \
    [set_sample_rate] = drv_set_sample_rate, \
    [test_connect] = drv_test_connect, \
    [is_started] = drv_is_started, \
    [get_prop_value] = drv_get_prop_value, \
    [midi_get_driver] = drv_midi_get_driver, \
    [midi_init] = drv_midi_init, \
    [midi_release] = drv_midi_release, \
    [midi_out_message] = drv_midi_out_message, \
    [midi_in_message] = drv_midi_in_message, \
    [midi_notify_wait] = drv_midi_notify_wait, \
    [aux_message] = drv_aux_message, \
}; \
_Static_assert( sizeof(table_name) / sizeof(table_name[0]) == funcs_count, #table_name " has 37 entries" );

DRIVER_TABLE( winecoreaudio_unix_call_funcs )
DRIVER_TABLE( wineios_unix_call_funcs )

/* ---------------------------------------------------------------- 32-bit callers (C1, A4) */

static HANDLE handle32( PTR32 value )
{
    return LongToHandle( (LONG)value );   /* handles are never converted */
}

/* the blocks shared with the 64-bit entries have no pointers */
_Static_assert( sizeof(struct start_params) == 16 && offsetof(struct start_params, result) == 8, "start" );
_Static_assert( sizeof(struct timer_loop_params) == 8, "timer_loop" );
_Static_assert( sizeof(struct release_render_buffer_params) == 24 &&
                offsetof(struct release_render_buffer_params, result) == 16, "release_render_buffer" );
_Static_assert( sizeof(struct release_capture_buffer_params) == 16, "release_capture_buffer" );
_Static_assert( sizeof(struct set_sample_rate_params) == 16 && offsetof(struct set_sample_rate_params, result) == 12,
                "set_sample_rate" );
_Static_assert( sizeof(struct is_started_params) == 16, "is_started" );

struct main_loop_params32 { PTR32 event; };
_Static_assert( sizeof(struct main_loop_params32) == 4, "main_loop32" );

static NTSTATUS wow64_main_loop( void *args )
{
    struct main_loop_params32 *params32 = args;
    struct main_loop_params params = { handle32( params32->event ) };
    return drv_main_loop( &params );
}

struct get_endpoint_ids_params32
{
    EDataFlow flow;
    PTR32 endpoints;
    unsigned int size;
    HRESULT result;
    unsigned int num;
    unsigned int default_idx;
};
_Static_assert( sizeof(struct get_endpoint_ids_params32) == 24, "get_endpoint_ids32" );
_Static_assert( offsetof(struct get_endpoint_ids_params32, result) == 12, "get_endpoint_ids32" );

static NTSTATUS wow64_get_endpoint_ids( void *args )
{
    struct get_endpoint_ids_params32 *params32 = args;
    struct get_endpoint_ids_params params;

    memset( &params, 0, sizeof(params) );
    params.flow = params32->flow;
    params.endpoints = iom_guest_ptr( params32->endpoints );
    params.size = params32->size;
    drv_get_endpoint_ids( &params );
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

struct create_stream_params32
{
    PTR32 name;
    PTR32 device;
    EDataFlow flow;
    AUDCLNT_SHAREMODE share;
    DWORD flags;
    REFERENCE_TIME duration;
    REFERENCE_TIME period;
    PTR32 fmt;
    HRESULT result;
    PTR32 channel_count;
    PTR32 stream;
};
_Static_assert( offsetof(struct create_stream_params32, duration) == 24, "create_stream32" );
_Static_assert( offsetof(struct create_stream_params32, fmt) == 40, "create_stream32" );
_Static_assert( offsetof(struct create_stream_params32, stream) == 52, "create_stream32" );
_Static_assert( sizeof(struct create_stream_params32) == 56, "create_stream32" );

static NTSTATUS wow64_create_stream( void *args )
{
    struct create_stream_params32 *params32 = args;

    params32->result = do_create_stream( iom_guest_ptr( params32->name ), params32->flow, params32->flags,
                                         params32->duration, params32->period, iom_guest_ptr( params32->fmt ),
                                         iom_guest_ptr( params32->stream ), iom_guest_ptr( params32->channel_count ) );
    return STATUS_SUCCESS;
}

struct release_stream_params32
{
    stream_handle stream;
    PTR32 timer_thread;
    HRESULT result;
};
_Static_assert( sizeof(struct release_stream_params32) == 16, "release_stream32" );

static NTSTATUS wow64_release_stream( void *args )
{
    struct release_stream_params32 *params32 = args;
    struct release_stream_params params = { params32->stream, handle32( params32->timer_thread ), S_OK };

    drv_release_stream( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

struct get_render_buffer_params32
{
    stream_handle stream;
    UINT32 frames;
    HRESULT result;
    PTR32 data;
};
_Static_assert( offsetof(struct get_render_buffer_params32, data) == 16, "get_render_buffer32" );
_Static_assert( sizeof(struct get_render_buffer_params32) == 24, "get_render_buffer32" );

static NTSTATUS wow64_get_render_buffer( void *args )
{
    struct get_render_buffer_params32 *params32 = args;
    PTR32 *slot = iom_guest_ptr( params32->data );
    BYTE *data = NULL;
    UINT32 guest = 0;

    params32->result = do_get_render_buffer( params32->stream, params32->frames, &data );
    if (SUCCEEDED(params32->result) && !iom_guest_addr( data, &guest ))
    {
        /* never truncate a pointer the program can't reach */
        cancel_render_buffer( params32->stream );
        params32->result = E_FAIL;
        guest = 0;
    }
    if (slot) *slot = guest;
    return STATUS_SUCCESS;
}

struct get_capture_buffer_params32
{
    stream_handle stream;
    HRESULT result;
    PTR32 data;
    PTR32 frames;
    PTR32 flags;
    PTR32 devpos;
    PTR32 qpcpos;
};
_Static_assert( offsetof(struct get_capture_buffer_params32, qpcpos) == 28, "get_capture_buffer32" );
_Static_assert( sizeof(struct get_capture_buffer_params32) == 32, "get_capture_buffer32" );

static NTSTATUS wow64_get_capture_buffer( void *args )
{
    struct get_capture_buffer_params32 *params32 = args;
    PTR32 *data = iom_guest_ptr( params32->data );
    UINT32 *frames = iom_guest_ptr( params32->frames ), *flags = iom_guest_ptr( params32->flags );
    UINT64 *devpos = iom_guest_ptr( params32->devpos ), *qpcpos = iom_guest_ptr( params32->qpcpos );

    if (data) *data = 0;
    if (frames) *frames = 0;
    if (flags) *flags = 0;
    if (devpos) *devpos = 0;
    if (qpcpos) *qpcpos = 0;
    params32->result = S_OK;
    return STATUS_SUCCESS;
}

struct is_format_supported_params32
{
    PTR32 device;
    EDataFlow flow;
    AUDCLNT_SHAREMODE share;
    PTR32 fmt_in;
    HRESULT result;
};
_Static_assert( sizeof(struct is_format_supported_params32) == 20, "is_format_supported32" );

static NTSTATUS wow64_is_format_supported( void *args )
{
    struct is_format_supported_params32 *params32 = args;
    params32->result = S_OK;
    return STATUS_SUCCESS;
}

struct get_loopback_capture_device_params32
{
    PTR32 name;
    PTR32 device;
    PTR32 ret_device;
    UINT32 ret_device_len;
    HRESULT result;
};
_Static_assert( sizeof(struct get_loopback_capture_device_params32) == 20, "loopback32" );

static NTSTATUS wow64_get_loopback_capture_device( void *args )
{
    struct get_loopback_capture_device_params32 *params32 = args;
    params32->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

struct get_mix_format_params32
{
    PTR32 device;
    EDataFlow flow;
    PTR32 fmt;
    HRESULT result;
};
_Static_assert( sizeof(struct get_mix_format_params32) == 16, "get_mix_format32" );

static NTSTATUS wow64_get_mix_format( void *args )
{
    struct get_mix_format_params32 *params32 = args;
    struct get_mix_format_params params;

    memset( &params, 0, sizeof(params) );
    params.flow = params32->flow;
    params.fmt = iom_guest_ptr( params32->fmt );
    drv_get_mix_format( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

struct get_device_period_params32
{
    PTR32 device;
    EDataFlow flow;
    HRESULT result;
    PTR32 def_period;
    PTR32 min_period;
};
_Static_assert( sizeof(struct get_device_period_params32) == 20, "get_device_period32" );

static NTSTATUS wow64_get_device_period( void *args )
{
    struct get_device_period_params32 *params32 = args;
    struct get_device_period_params params;

    memset( &params, 0, sizeof(params) );
    params.flow = params32->flow;
    params.def_period = iom_guest_ptr( params32->def_period );
    params.min_period = iom_guest_ptr( params32->min_period );
    drv_get_device_period( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

/* get_buffer_size, get_latency, get_current_padding, get_next_packet_size, get_frequency */
struct stream_out_params32
{
    stream_handle stream;
    HRESULT result;
    PTR32 out;
};
_Static_assert( offsetof(struct stream_out_params32, out) == 12, "stream_out32" );
_Static_assert( sizeof(struct stream_out_params32) == 16, "stream_out32" );

#define STREAM_OUT_THUNK( name, type, field ) \
static NTSTATUS wow64_##name( void *args ) \
{ \
    struct stream_out_params32 *params32 = args; \
    struct name##_params params; \
    type value = 0, *out = iom_guest_ptr( params32->out ); \
    memset( &params, 0, sizeof(params) ); \
    params.stream = params32->stream; \
    params.field = out ? out : &value; \
    drv_##name( &params ); \
    params32->result = params.result; \
    return STATUS_SUCCESS; \
}

STREAM_OUT_THUNK( get_buffer_size, UINT32, frames )
STREAM_OUT_THUNK( get_latency, REFERENCE_TIME, latency )
STREAM_OUT_THUNK( get_current_padding, UINT32, padding )
STREAM_OUT_THUNK( get_next_packet_size, UINT32, frames )
STREAM_OUT_THUNK( get_frequency, UINT64, freq )

struct get_position_params32
{
    stream_handle stream;
    BOOL device;
    HRESULT result;
    PTR32 pos;
    PTR32 qpctime;
};
_Static_assert( offsetof(struct get_position_params32, pos) == 16, "get_position32" );
_Static_assert( sizeof(struct get_position_params32) == 24, "get_position32" );

static NTSTATUS wow64_get_position( void *args )
{
    struct get_position_params32 *params32 = args;
    struct get_position_params params;
    UINT64 pos = 0, *pos_out = iom_guest_ptr( params32->pos );

    memset( &params, 0, sizeof(params) );
    params.stream = params32->stream;
    params.device = params32->device;
    params.pos = pos_out ? pos_out : &pos;
    params.qpctime = iom_guest_ptr( params32->qpctime );
    drv_get_position( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

struct set_volumes_params32
{
    stream_handle stream;
    float master_volume;
    PTR32 volumes;
    PTR32 session_volumes;
};
_Static_assert( sizeof(struct set_volumes_params32) == 24, "set_volumes32" );

static NTSTATUS wow64_set_volumes( void *args )
{
    return STATUS_SUCCESS;   /* accepted and ignored; nothing is read */
}

struct set_event_handle_params32
{
    stream_handle stream;
    PTR32 event;
    HRESULT result;
};
_Static_assert( sizeof(struct set_event_handle_params32) == 16, "set_event_handle32" );

static NTSTATUS wow64_set_event_handle( void *args )
{
    struct set_event_handle_params32 *params32 = args;
    struct set_event_handle_params params;

    memset( &params, 0, sizeof(params) );
    params.stream = params32->stream;
    params.event = handle32( params32->event );
    drv_set_event_handle( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

struct test_connect_params32
{
    PTR32 name;
    enum driver_priority priority;
};
_Static_assert( sizeof(struct test_connect_params32) == 8, "test_connect32" );

static NTSTATUS wow64_test_connect( void *args )
{
    struct test_connect_params32 *params32 = args;
    params32->priority = Priority_Preferred;
    return STATUS_SUCCESS;
}

struct get_prop_value_params32
{
    PTR32 device;
    EDataFlow flow;
    PTR32 guid;
    PTR32 prop;
    HRESULT result;
    PTR32 value;
    PTR32 buffer;
    PTR32 buffer_size;
};
_Static_assert( offsetof(struct get_prop_value_params32, result) == 16, "get_prop_value32" );
_Static_assert( sizeof(struct get_prop_value_params32) == 32, "get_prop_value32" );

static NTSTATUS wow64_get_prop_value( void *args )
{
    struct get_prop_value_params32 *params32 = args;
    params32->result = E_FAIL;   /* never writes a PROPVARIANT */
    return STATUS_SUCCESS;
}

struct midi_init_params32 { PTR32 err; };
_Static_assert( sizeof(struct midi_init_params32) == 4, "midi_init32" );

static NTSTATUS wow64_midi_init( void *args )
{
    struct midi_init_params32 *params32 = args;
    UINT err = 0, *out = iom_guest_ptr( params32->err );
    struct midi_init_params params = { out ? out : &err };
    return drv_midi_init( &params );
}

struct notify_context32
{
    BOOL send_notify;
    WORD dev_id;
    WORD msg;
    UINT param_1;
    UINT param_2;
    UINT callback;
    UINT flags;
    PTR32 device;
    UINT instance;
};
_Static_assert( sizeof(struct notify_context32) == 32, "notify_context32" );

struct midi_message_params32   /* midi_out_message, midi_in_message */
{
    UINT dev_id;
    UINT msg;
    UINT user;
    UINT param_1;
    UINT param_2;
    PTR32 err;
    PTR32 notify;
};
_Static_assert( offsetof(struct midi_message_params32, err) == 20, "midi_message32" );
_Static_assert( sizeof(struct midi_message_params32) == 28, "midi_message32" );

static NTSTATUS wow64_midi_message( void *args, int out )
{
    struct midi_message_params32 *params32 = args;
    struct notify_context32 *notify32 = iom_guest_ptr( params32->notify );
    UINT *err = iom_guest_ptr( params32->err );

    if (err) *err = MMSYSERR_NOTSUPPORTED;
    if (notify32) notify32->send_notify = FALSE;
    log_once( out ? &midi_out_logged : &midi_in_logged,
              out ? "MIDI out message: MMSYSERR_NOTSUPPORTED" : "MIDI in message: MMSYSERR_NOTSUPPORTED" );
    return STATUS_SUCCESS;
}

static NTSTATUS wow64_midi_out_message( void *args )
{
    return wow64_midi_message( args, 1 );
}

static NTSTATUS wow64_midi_in_message( void *args )
{
    return wow64_midi_message( args, 0 );
}

struct midi_notify_wait_params32
{
    PTR32 quit;
    PTR32 notify;
};
_Static_assert( sizeof(struct midi_notify_wait_params32) == 8, "midi_notify_wait32" );

static NTSTATUS wow64_midi_notify_wait( void *args )
{
    struct midi_notify_wait_params32 *params32 = args;
    struct notify_context32 *notify32 = iom_guest_ptr( params32->notify );
    BOOL *quit = iom_guest_ptr( params32->quit );

    if (notify32) notify32->send_notify = FALSE;
    if (quit) *quit = TRUE;
    log_once( &midi_wait_logged, "MIDI notify wait: quit" );
    return STATUS_SUCCESS;
}

struct aux_message_params32
{
    UINT dev_id;
    UINT msg;
    UINT user;
    UINT param_1;
    UINT param_2;
    PTR32 err;
};
_Static_assert( sizeof(struct aux_message_params32) == 24, "aux_message32" );

static NTSTATUS wow64_aux_message( void *args )
{
    struct aux_message_params32 *params32 = args;
    UINT *err = iom_guest_ptr( params32->err );

    if (err) *err = MMSYSERR_NOTSUPPORTED;
    log_once( &aux_logged, "aux message: MMSYSERR_NOTSUPPORTED" );
    return STATUS_SUCCESS;
}

#define DRIVER_WOW64_TABLE( table_name ) \
const unixlib_entry_t table_name[] EXPORTED_TABLE = \
{ \
    [process_attach] = drv_process_attach, \
    [process_detach] = drv_process_detach, \
    [main_loop] = wow64_main_loop, \
    [get_endpoint_ids] = wow64_get_endpoint_ids, \
    [create_stream] = wow64_create_stream, \
    [release_stream] = wow64_release_stream, \
    [start] = drv_start, \
    [stop] = drv_stop, \
    [reset] = drv_reset, \
    [timer_loop] = drv_timer_loop, \
    [get_render_buffer] = wow64_get_render_buffer, \
    [release_render_buffer] = drv_release_render_buffer, \
    [get_capture_buffer] = wow64_get_capture_buffer, \
    [release_capture_buffer] = drv_release_capture_buffer, \
    [is_format_supported] = wow64_is_format_supported, \
    [get_loopback_capture_device] = wow64_get_loopback_capture_device, \
    [get_mix_format] = wow64_get_mix_format, \
    [get_device_period] = wow64_get_device_period, \
    [get_buffer_size] = wow64_get_buffer_size, \
    [get_latency] = wow64_get_latency, \
    [get_current_padding] = wow64_get_current_padding, \
    [get_next_packet_size] = wow64_get_next_packet_size, \
    [get_frequency] = wow64_get_frequency, \
    [get_position] = wow64_get_position, \
    [set_volumes] = wow64_set_volumes, \
    [set_event_handle] = wow64_set_event_handle, \
    [set_sample_rate] = drv_set_sample_rate, \
    [test_connect] = wow64_test_connect, \
    [is_started] = drv_is_started, \
    [get_prop_value] = wow64_get_prop_value, \
    [midi_get_driver] = drv_midi_get_driver, \
    [midi_init] = wow64_midi_init, \
    [midi_release] = drv_midi_release, \
    [midi_out_message] = wow64_midi_out_message, \
    [midi_in_message] = wow64_midi_in_message, \
    [midi_notify_wait] = wow64_midi_notify_wait, \
    [aux_message] = wow64_aux_message, \
}; \
_Static_assert( sizeof(table_name) / sizeof(table_name[0]) == funcs_count, #table_name " has 37 entries" );

DRIVER_WOW64_TABLE( winecoreaudio_unix_call_wow64_funcs )
DRIVER_WOW64_TABLE( wineios_unix_call_wow64_funcs )
