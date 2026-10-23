// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F, audio through the driver's tables (audio-unix/ios_audio.c) with a fake output
// unit and a virtual clock (stubs_audio.c, stubs_wine.c): the endpoint ids and their too-small
// answer, the mix format, the MIDI and aux answers, streams (render buffers zeroed, too large,
// silent flag, mixing through the unit's render callback, positions), null mode's wall clock,
// the 16-stream limit, stale handles, the timer thread, process detach, and the 32-bit thunks.

#if 0
#pragma makedep unix
#endif

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#include "unixlib.h"

#include "check.h"
#include "ios_audio_pure.h"
#include "test_stubs.h"

extern const unixlib_entry_t winecoreaudio_unix_call_funcs[];
extern const unixlib_entry_t winecoreaudio_unix_call_wow64_funcs[];
extern const unixlib_entry_t wineios_unix_call_funcs[];

void ios_audio_process_ended( void *peb );

#define CALL( func, args ) winecoreaudio_unix_call_funcs[func]( args )
#define CALL32( func, args ) winecoreaudio_unix_call_wow64_funcs[func]( args )

static void float_format( WAVEFORMATEXTENSIBLE *f, int rate, int channels )
{
    static const GUID ieee = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

    memset( f, 0, sizeof(*f) );
    f->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f->Format.nChannels = channels;
    f->Format.nSamplesPerSec = rate;
    f->Format.wBitsPerSample = 32;
    f->Format.nBlockAlign = channels * 4;
    f->Format.nAvgBytesPerSec = rate * channels * 4;
    f->Format.cbSize = sizeof(*f) - sizeof(WAVEFORMATEX);
    f->Samples.wValidBitsPerSample = 32;
    f->dwChannelMask = channels == 2 ? 3 : 0;
    f->SubFormat = ieee;
}

static HRESULT create( const WAVEFORMATEX *fmt, REFERENCE_TIME duration, DWORD flags, stream_handle *h )
{
    struct create_stream_params p;
    UINT32 channels = 0;

    memset( &p, 0, sizeof(p) );
    p.name = (const WCHAR *)u"C:\\games\\test.exe";
    p.device = "ios-null";
    p.flow = eRender;
    p.share = AUDCLNT_SHAREMODE_SHARED;
    p.flags = flags;
    p.duration = duration;
    p.period = 100000;
    p.fmt = fmt;
    p.channel_count = &channels;
    p.stream = h;
    CHECK_INT( CALL( create_stream, &p ), STATUS_SUCCESS );
    if (p.result == S_OK) CHECK_INT( channels, fmt->nChannels );
    return p.result;
}

static HRESULT release( stream_handle h, HANDLE timer )
{
    struct release_stream_params p = { h, timer, 0 };
    CALL( release_stream, &p );
    return p.result;
}

static HRESULT simple( unsigned int func, stream_handle h )
{
    struct start_params p = { h, 0 };   /* start, stop, reset, is_started share this layout */
    CALL( func, &p );
    return p.result;
}

static UINT32 padding( stream_handle h, HRESULT *hr )
{
    UINT32 pad = 12345;
    struct get_current_padding_params p = { h, 0, &pad };
    CALL( get_current_padding, &p );
    if (hr) *hr = p.result;
    return pad;
}

static UINT64 position( stream_handle h )
{
    UINT64 pos = 0, qpc = 0;
    struct get_position_params p = { h, FALSE, 0, &pos, &qpc };
    CALL( get_position, &p );
    CHECK_INT( p.result, S_OK );
    CHECK_INT( qpc, test_now / 100 );
    return pos;
}

static HRESULT get_buffer( stream_handle h, UINT32 frames, BYTE **data )
{
    struct get_render_buffer_params p = { h, frames, 0, data };
    CALL( get_render_buffer, &p );
    return p.result;
}

static HRESULT release_buffer( stream_handle h, UINT32 frames, UINT flags )
{
    struct release_render_buffer_params p = { h, frames, flags, 0 };
    CALL( release_render_buffer, &p );
    return p.result;
}

static void test_endpoints_and_formats( void )
{
    struct get_endpoint_ids_params ids;
    struct get_mix_format_params mix;
    struct test_connect_params connect;
    struct get_prop_value_params prop;
    struct get_device_period_params period;
    WAVEFORMATEXTENSIBLE fmt;
    REFERENCE_TIME def = 0, min = 0;
    unsigned char buf[64];

    CHECK_INT( CALL( process_attach, NULL ), STATUS_SUCCESS );
    memset( &ids, 0, sizeof(ids) );
    ids.flow = eRender;
    ids.endpoints = (struct endpoint *)buf;
    ids.size = 8;
    CALL( get_endpoint_ids, &ids );
    CHECK_INT( ids.result, HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER ) );
    CHECK_INT( ids.size, 36 );
    ids.size = sizeof(buf);
    CALL( get_endpoint_ids, &ids );
    CHECK_INT( ids.result, S_OK );
    CHECK_INT( ids.num, 1 );
    CHECK_INT( ids.default_idx, 0 );
    CHECK( !memcmp( buf + ids.endpoints[0].device, "ios-null", 9 ) );
    CHECK( !memcmp( buf + ids.endpoints[0].name, u"iOS Null", 18 ) );
    ids.flow = eCapture;
    CALL( get_endpoint_ids, &ids );
    CHECK_INT( ids.result, S_OK );
    CHECK_INT( ids.num, 0 );

    memset( &mix, 0, sizeof(mix) );
    mix.flow = eRender;
    mix.fmt = &fmt;
    CALL( get_mix_format, &mix );
    CHECK_INT( mix.result, S_OK );
    CHECK_INT( fmt.Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE );
    CHECK_INT( fmt.Format.nSamplesPerSec, 48000 );
    CHECK_INT( fmt.Format.nChannels, 2 );
    CHECK_INT( fmt.Format.wBitsPerSample, 32 );
    CHECK_INT( fmt.dwChannelMask, 3 );
    CHECK_INT( fmt.SubFormat.Data1, 3 );

    memset( &period, 0, sizeof(period) );
    period.def_period = &def;
    period.min_period = &min;
    CALL( get_device_period, &period );
    CHECK_INT( def, 100000 );
    CHECK_INT( min, 50000 );

    connect.name = (const WCHAR *)u"test.exe";
    connect.priority = Priority_Unavailable;
    CALL( test_connect, &connect );
    CHECK_INT( connect.priority, Priority_Preferred );
    memset( &prop, 0, sizeof(prop) );
    CALL( get_prop_value, &prop );
    CHECK_INT( prop.result, E_FAIL );

    /* the alias table has the same entries */
    CHECK( wineios_unix_call_funcs[create_stream] == winecoreaudio_unix_call_funcs[create_stream] );
    {
        unsigned int i;
        for (i = 0; i < funcs_count; i++)
        {
            CHECK( winecoreaudio_unix_call_funcs[i] != NULL );
            CHECK( winecoreaudio_unix_call_wow64_funcs[i] != NULL );
        }
    }
}

static void test_midi( void )
{
    struct midi_out_message_params out;
    struct midi_in_message_params in;
    struct midi_notify_wait_params wait;
    struct aux_message_params aux;
    struct midi_init_params init;
    struct notify_context notify;
    UINT err;
    BOOL quit;

    test_log_clear();
    err = 12345;
    init.err = &err;
    CALL( midi_init, &init );
    CHECK_INT( err, DRV_FAILURE );
    CALL( midi_init, &init );
    CHECK_INT( test_log_count( "midi_init answers DRV_FAILURE" ), 1 );   /* each kind logged once */

    memset( &out, 0, sizeof(out) );
    notify.send_notify = TRUE;
    out.err = &err;
    out.notify = &notify;
    CALL( midi_out_message, &out );
    CHECK_INT( err, MMSYSERR_NOTSUPPORTED );
    CHECK_INT( notify.send_notify, FALSE );

    memset( &in, 0, sizeof(in) );
    notify.send_notify = TRUE;
    in.err = &err;
    in.notify = &notify;
    CALL( midi_in_message, &in );
    CHECK_INT( err, MMSYSERR_NOTSUPPORTED );
    CHECK_INT( notify.send_notify, FALSE );

    quit = FALSE;
    notify.send_notify = TRUE;
    wait.quit = &quit;
    wait.notify = &notify;
    CALL( midi_notify_wait, &wait );
    CHECK_INT( quit, TRUE );
    CHECK_INT( notify.send_notify, FALSE );

    memset( &aux, 0, sizeof(aux) );
    aux.err = &err;
    err = 0;
    CALL( aux_message, &aux );
    CHECK_INT( err, MMSYSERR_NOTSUPPORTED );
    CHECK_INT( CALL( midi_release, NULL ), STATUS_SUCCESS );
}

static void test_stream( void )
{
    WAVEFORMATEXTENSIBLE fmt;
    stream_handle h = 0;
    BYTE *data = NULL;
    float out[512 * 2];
    UINT32 i, size = 0;
    HRESULT hr;

    float_format( &fmt, 48000, 2 );
    CHECK_INT( create( &fmt.Format, 10000000, 0, &h ), S_OK );
    CHECK_INT( test_unit_creates, 1 );   /* the one output unit, from the first mixable stream */
    CHECK_INT( test_unit_rate, 48000 );
    CHECK_INT( test_unit_channels, 2 );
    {
        struct get_buffer_size_params p = { h, 0, &size };
        CALL( get_buffer_size, &p );
        CHECK_INT( size, 48000 );
    }

    /* zeroed every time it is handed out */
    CHECK_INT( get_buffer( h, 480, &data ), S_OK );
    CHECK( data != NULL );
    for (i = 0; i < 480 * 2; i++) CHECK( ((float *)data)[i] == 0.0f );
    memset( data, 0x55, 480 * 8 );
    CHECK_INT( get_buffer( h, 10, &data ), AUDCLNT_E_OUT_OF_ORDER );
    CHECK_INT( release_buffer( h, 0, 0 ), S_OK );
    CHECK_INT( get_buffer( h, 480, &data ), S_OK );
    CHECK( ((float *)data)[7] == 0.0f );
    for (i = 0; i < 480; i++)
    {
        ((float *)data)[2 * i] = 0.25f;
        ((float *)data)[2 * i + 1] = -0.25f;
    }
    CHECK_INT( release_buffer( h, 480, 0 ), S_OK );
    CHECK_INT( padding( h, NULL ), 480 );

    /* too large: frames + padding > capacity */
    data = (BYTE *)1;
    CHECK_INT( get_buffer( h, 48000 - 479, &data ), AUDCLNT_E_BUFFER_TOO_LARGE );
    CHECK( data == NULL );

    CHECK_INT( simple( start, h ), S_OK );
    CHECK_INT( simple( start, h ), AUDCLNT_E_NOT_STOPPED );
    CHECK_INT( simple( is_started, h ), S_OK );
    CHECK( test_unit_running );
    test_unit_render( out, 256, sizeof(out) );
    CHECK_NEAR( out[0], 0.25, 1e-6 );
    CHECK_NEAR( out[1], -0.25, 1e-6 );
    CHECK_INT( padding( h, NULL ), 224 );
    CHECK_INT( position( h ), 256 );

    /* the silent flag writes zeros */
    CHECK_INT( get_buffer( h, 100, &data ), S_OK );
    memset( data, 0x55, 100 * 8 );
    CHECK_INT( release_buffer( h, 100, AUDCLNT_BUFFERFLAGS_SILENT ), S_OK );
    test_unit_render( out, 224, sizeof(out) );
    test_unit_render( out, 100, sizeof(out) );
    CHECK_NEAR( out[0], 0.0, 0 );
    CHECK_NEAR( out[199], 0.0, 0 );

    CHECK_INT( simple( reset, h ), AUDCLNT_E_NOT_STOPPED );
    CHECK_INT( simple( stop, h ), S_OK );
    CHECK_INT( simple( stop, h ), S_FALSE );
    CHECK( !test_unit_running );   /* stopped when no stream is started */
    CHECK_INT( simple( is_started, h ), S_FALSE );
    CHECK_INT( simple( reset, h ), S_OK );
    CHECK_INT( position( h ), 0 );
    CHECK_INT( padding( h, NULL ), 0 );

    CHECK_INT( release( h, NULL ), S_OK );
    test_log_clear();
    padding( h, &hr );
    CHECK_INT( hr, AUDCLNT_E_NOT_INITIALIZED );   /* stale */
    CHECK_INT( release( h, NULL ), AUDCLNT_E_NOT_INITIALIZED );
    CHECK( test_log_count( "stale or foreign stream handle" ) >= 1 );
}

static void test_null_mode( void )
{
    WAVEFORMATEX fmt;
    stream_handle h = 0;
    struct set_sample_rate_params rate;

    memset( &fmt, 0, sizeof(fmt) );
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48000;
    fmt.wBitsPerSample = 24;   /* not mixable */
    fmt.nBlockAlign = 6;
    fmt.nAvgBytesPerSec = 48000 * 6;
    test_log_clear();
    CHECK_INT( create( &fmt, 2000000, 0, &h ), S_OK );
    CHECK( test_log_count( "not mixable" ) == 1 );
    CHECK_INT( simple( start, h ), S_OK );
    CHECK_INT( padding( h, NULL ), 0 );              /* always hungry */
    test_now += 100000000;                           /* 100 ms */
    CHECK_INT( position( h ), 4800 );
    CHECK_INT( simple( stop, h ), S_OK );
    test_now += 100000000;
    CHECK_INT( position( h ), 4800 );                /* accumulated across stops */
    CHECK_INT( simple( start, h ), S_OK );
    test_now += 50000000;
    CHECK_INT( position( h ), 7200 );
    rate.stream = h;
    rate.rate = 24000.0f;
    CALL( set_sample_rate, &rate );
    CHECK_INT( rate.result, S_OK );
    test_now += 100000000;
    CHECK_INT( position( h ), 7200 + 2400 );
    rate.rate = 0.0f;
    CALL( set_sample_rate, &rate );
    CHECK_INT( rate.result, E_INVALIDARG );
    CHECK_INT( release( h, NULL ), S_OK );
}

static void test_limits( void )
{
    WAVEFORMATEXTENSIBLE fmt;
    stream_handle h[17];
    int i;

    float_format( &fmt, 48000, 2 );
    for (i = 0; i < 16; i++) CHECK_INT( create( &fmt.Format, 0, 0, &h[i] ), S_OK );
    CHECK_INT( create( &fmt.Format, 0, 0, &h[16] ), E_OUTOFMEMORY );
    for (i = 0; i < 16; i++) CHECK_INT( release( h[i], NULL ), S_OK );
}

static void *timer_main( void *arg )
{
    struct timer_loop_params p = { *(stream_handle *)arg };
    CALL( timer_loop, &p );
    return NULL;
}

static void test_timer( void )
{
    WAVEFORMATEXTENSIBLE fmt;
    struct set_event_handle_params ev;
    stream_handle h = 0;
    int waited = 0;

    float_format( &fmt, 48000, 2 );
    CHECK_INT( create( &fmt.Format, 1000000, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, &h ), S_OK );
    CHECK_INT( simple( start, h ), AUDCLNT_E_EVENTHANDLE_NOT_SET );
    ev.stream = h;
    ev.event = (HANDLE)(uintptr_t)0x1234;
    CALL( set_event_handle, &ev );
    CHECK_INT( ev.result, S_OK );
    CHECK_INT( simple( start, h ), S_OK );

    test_events_set = 0;
    test_timer_thread_running = 1;
    pthread_create( &test_timer_thread, NULL, timer_main, &h );
    while (__atomic_load_n( &test_events_set, __ATOMIC_RELAXED ) < 3 && waited++ < 5000)
    {
        struct timespec ts = { 0, 1000000 };
        nanosleep( &ts, NULL );
    }
    CHECK( test_events_set >= 3 );   /* below the lead: the client is woken */
    /* release joins the timer thread (the stub's wait on its handle) before freeing */
    CHECK_INT( release( h, TEST_TIMER_THREAD_HANDLE ), S_OK );
    CHECK_INT( test_timer_thread_running, 0 );
}

static void test_detach( void )
{
    WAVEFORMATEXTENSIBLE fmt;
    stream_handle a = 0, b = 0;
    void *peb_a = (void *)0x1111000, *peb_b = (void *)0x2222000, *saved = test_peb;
    HRESULT hr;

    float_format( &fmt, 48000, 2 );
    test_peb = peb_a;
    CHECK_INT( create( &fmt.Format, 0, 0, &a ), S_OK );
    test_peb = peb_b;
    CHECK_INT( create( &fmt.Format, 0, 0, &b ), S_OK );
    CHECK_INT( simple( start, b ), S_OK );
    test_peb = peb_a;
    CALL( process_detach, NULL );         /* only the calling pseudo-process's streams */
    padding( a, &hr );
    CHECK_INT( hr, AUDCLNT_E_NOT_INITIALIZED );
    padding( b, &hr );
    CHECK_INT( hr, S_OK );
    test_peb = peb_b;
    CHECK( test_unit_running );
    ios_audio_process_ended( peb_b );       /* the hook offered to spec 09 */
    padding( b, &hr );
    CHECK_INT( hr, AUDCLNT_E_NOT_INITIALIZED );
    CHECK( !test_unit_running );
    test_peb = saved;
}

/* spec 22 Part F: kept sessions */
static void test_session_reuse( void )
{
    WAVEFORMATEXTENSIBLE fmt;
    struct set_event_handle_params ev;
    stream_handle a = 0, b = 0, c = 0;
    void *peb_a = (void *)0x3333000, *saved = test_peb;
    BYTE *data = NULL;
    HRESULT hr;
    int waited = 0;

    float_format( &fmt, 48000, 2 );

    /* F1: the process-end hook is registered once, at the first attach */
    CALL( process_attach, NULL );
    CALL( process_attach, NULL );
    CHECK_INT( test_end_hook_adds, 1 );
    CHECK( test_end_hook != NULL );

    /* F1: an ended process's streams are unpublished and stop counting (the unit stops: no
     * kept session here) */
    unsetenv( "MYIOSDECK_SESSION_REUSE" );
    test_peb = peb_a;
    CHECK_INT( create( &fmt.Format, 0, 0, &a ), S_OK );
    CHECK_INT( simple( start, a ), S_OK );
    CHECK( test_unit_running );
    test_log_clear();
    if (test_end_hook) test_end_hook( peb_a );   /* as spec 09 runs it after the threads are gone */
    padding( a, &hr );
    CHECK_INT( hr, AUDCLNT_E_NOT_INITIALIZED );
    CHECK( test_log_count( "process ended: 1 stream(s) stopped and unpublished" ) == 1 );
    CHECK( !test_unit_running );
    test_peb = saved;

    /* F2: in a kept session the unit keeps running when the last stream stops or goes */
    setenv( "MYIOSDECK_SESSION_REUSE", "1", 1 );
    CHECK_INT( create( &fmt.Format, 0, 0, &b ), S_OK );
    CHECK_INT( simple( start, b ), S_OK );
    CHECK( test_unit_running );
    CHECK_INT( simple( stop, b ), S_OK );
    CHECK( test_unit_running );                 /* silent, still running */
    CHECK_INT( release( b, NULL ), S_OK );
    CHECK( test_unit_running );
    test_peb = peb_a;
    CHECK_INT( create( &fmt.Format, 0, 0, &b ), S_OK );
    CHECK_INT( simple( start, b ), S_OK );
    if (test_end_hook) test_end_hook( peb_a );  /* a game of the kept session ends */
    CHECK( test_unit_running );
    test_peb = saved;
    /* without the setting the old rule is back: stopped when no stream plays */
    unsetenv( "MYIOSDECK_SESSION_REUSE" );
    CHECK_INT( create( &fmt.Format, 0, 0, &b ), S_OK );
    CHECK_INT( simple( start, b ), S_OK );
    CHECK_INT( simple( stop, b ), S_OK );
    CHECK( !test_unit_running );
    CHECK_INT( release( b, NULL ), S_OK );

    /* F3: a started stream whose unit doesn't run still gets the 10 ms beat; its buffer is
     * full, so the pacing (which waits for the unit to drain it) would never signal */
    test_unit_start_fail = 1;
    CHECK_INT( create( &fmt.Format, 1000000, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, &c ), S_OK );
    CHECK_INT( get_buffer( c, 4800, &data ), S_OK );
    CHECK_INT( release_buffer( c, 4800, 0 ), S_OK );
    ev.stream = c;
    ev.event = (HANDLE)(uintptr_t)0x1234;
    CALL( set_event_handle, &ev );
    CHECK_INT( ev.result, S_OK );
    CHECK_INT( simple( start, c ), S_OK );
    CHECK( !test_unit_running );
    test_events_set = 0;
    test_timer_thread_running = 1;
    pthread_create( &test_timer_thread, NULL, timer_main, &c );
    while (__atomic_load_n( &test_events_set, __ATOMIC_RELAXED ) < 3 && waited++ < 5000)
    {
        struct timespec ts = { 0, 1000000 };
        nanosleep( &ts, NULL );
    }
    CHECK( test_events_set >= 3 );
    CHECK_INT( release( c, TEST_TIMER_THREAD_HANDLE ), S_OK );
    CHECK_INT( test_timer_thread_running, 0 );
    test_unit_start_fail = 0;
}

/* the 32-bit thunks: guest pointers in, guest pointers out (A4) */
static void test_wow64( void )
{
    struct { UINT32 name, device; EDataFlow flow; AUDCLNT_SHAREMODE share; DWORD flags;
             REFERENCE_TIME duration, period; UINT32 fmt; HRESULT result; UINT32 channel_count, stream; } *cs;
    struct { UINT64 stream; UINT32 frames; HRESULT result; UINT32 data; } *rb;
    struct { UINT32 name; enum driver_priority priority; } *tc;
    struct { UINT32 quit, notify; } *nw;
    struct { UINT64 stream; BOOL device; HRESULT result; UINT32 pos, qpctime; } *gp;
    struct { EDataFlow flow; UINT32 endpoints; unsigned int size; HRESULT result; unsigned int num, def; } *ids;
    struct { BOOL send_notify; WORD dev_id, msg; UINT p1, p2, cb, flags; UINT32 device; UINT inst; } *notify;
    WAVEFORMATEXTENSIBLE *fmt;
    UINT32 *channels, *slot;
    UINT64 *handle, *pos;
    BOOL *quit;
    BYTE *eps;

    test_enter_window();
    cs = test_arena_alloc( sizeof(*cs) );
    rb = test_arena_alloc( sizeof(*rb) );
    tc = test_arena_alloc( sizeof(*tc) );
    nw = test_arena_alloc( sizeof(*nw) );
    gp = test_arena_alloc( sizeof(*gp) );
    ids = test_arena_alloc( sizeof(*ids) );
    notify = test_arena_alloc( sizeof(*notify) );
    fmt = test_arena_alloc( sizeof(*fmt) );
    channels = test_arena_alloc( 4 );
    slot = test_arena_alloc( 4 );
    handle = test_arena_alloc( 8 );
    pos = test_arena_alloc( 8 );
    quit = test_arena_alloc( sizeof(BOOL) );
    eps = test_arena_alloc( 64 );

    float_format( fmt, 44100, 2 );
    memset( cs, 0, sizeof(*cs) );
    cs->flow = eRender;
    cs->duration = 1000000;
    cs->fmt = test_guest( fmt );
    cs->channel_count = test_guest( channels );
    cs->stream = test_guest( handle );
    CALL32( create_stream, cs );
    CHECK_INT( cs->result, S_OK );
    CHECK_INT( *channels, 2 );
    CHECK( *handle != 0 );

    rb->stream = *handle;
    rb->frames = 441;
    rb->data = test_guest( slot );
    CALL32( get_render_buffer, rb );
    CHECK_INT( rb->result, S_OK );
    CHECK( *slot != 0 );
    CHECK( ios_wow_in_window( (void *)(uintptr_t)(test_wow_base + *slot) ) );   /* in the caller's window */
    {
        struct release_render_buffer_params p = { *handle, 441, 0, 0 };
        CALL32( release_render_buffer, &p );
        CHECK_INT( p.result, S_OK );
    }
    /* a buffer the program can't reach: 0 and E_FAIL, never truncated */
    test_window_reject = 1;
    *slot = 0xdeadbeef;
    CALL32( get_render_buffer, rb );
    CHECK_INT( rb->result, E_FAIL );
    CHECK_INT( *slot, 0 );
    test_window_reject = 0;
    CALL32( get_render_buffer, rb );
    CHECK_INT( rb->result, S_OK );   /* the failed handout was cancelled */

    gp->stream = *handle;
    gp->pos = test_guest( pos );
    gp->qpctime = 0;
    CALL32( get_position, gp );
    CHECK_INT( gp->result, S_OK );

    tc->priority = Priority_Unavailable;
    CALL32( test_connect, tc );
    CHECK_INT( tc->priority, Priority_Preferred );

    *quit = FALSE;
    notify->send_notify = TRUE;
    nw->quit = test_guest( quit );
    nw->notify = test_guest( notify );
    CALL32( midi_notify_wait, nw );
    CHECK_INT( *quit, TRUE );
    CHECK_INT( notify->send_notify, FALSE );

    ids->flow = eRender;
    ids->endpoints = test_guest( eps );
    ids->size = 64;
    CALL32( get_endpoint_ids, ids );
    CHECK_INT( ids->result, S_OK );
    CHECK_INT( ids->num, 1 );
    CHECK( !memcmp( eps + 26, "ios-null", 9 ) );

    {
        struct { UINT64 stream; UINT32 timer; HRESULT result; } rel = { *handle, 0, 0 };
        CALL32( release_stream, &rel );
        CHECK_INT( rel.result, S_OK );
    }
    test_leave_window();
}

int main( void )
{
    test_endpoints_and_formats();
    test_midi();
    test_stream();
    test_null_mode();
    test_limits();
    test_timer();
    test_detach();
    test_session_reuse();
    test_wow64();
    return test_report( "test_audio_wine" );
}
