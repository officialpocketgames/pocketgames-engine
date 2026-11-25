// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 4: the unix side of openal32.dll (openal-unix/openal_unix.c) against a fake
// OpenAL Soft: the argument translation of alBufferData, alSourcefv and alcCreateContext's
// attribute list for 64- and 32-bit callers, device and context ids, the per-process current
// context, the two-step strings and extension filtering, the "unsupported" and "device opened"
// logs, the app-inactive pause, and the pseudo-process end hook.

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "openal_api.h"
#include "test_stubs.h"
#include "unixlib.h"

typedef int32_t (*entry)( void *args );
extern const entry openal32_unix_call_funcs[];
extern const entry openal32_unix_call_wow64_funcs[];
void pg_openal_set_app_active( int active );

/* ---------------------------------------------------------------- the fake OpenAL Soft */

static struct { int n; } devices[4];
static struct { int n; } contexts[4];
static int opened, closed, destroyed, pauses[4], resumes[4];
static ALCdevice *ctx_device;
static ALCint attrs_seen[16];
static ALuint buffer_id;
static ALenum buffer_format;
static const ALvoid *buffer_data;
static ALsizei buffer_size, buffer_freq;
static uint8_t buffer_first[4];
static ALfloat source_v[3];
static ALCcontext *thread_ctx, *ctx_at_call;

static int dev_index( ALCdevice *d ) { return (int)((uintptr_t)d - (uintptr_t)devices) / (int)sizeof(devices[0]); }

static ALCdevice *f_alcOpenDevice( const ALCchar *name ) { return (ALCdevice *)&devices[opened++ % 4]; }
static ALCboolean f_alcCloseDevice( ALCdevice *d ) { if (!d) return ALC_FALSE; closed++; return ALC_TRUE; }
static ALCcontext *f_alcCreateContext( ALCdevice *d, const ALCint *attrs )
{
    int i;
    static int n;
    ctx_device = d;
    memset( attrs_seen, 0, sizeof(attrs_seen) );
    for (i = 0; attrs && i < 15 && attrs[i]; i++) attrs_seen[i] = attrs[i];
    return (ALCcontext *)&contexts[n++ % 4];
}
static void f_alcDestroyContext( ALCcontext *c ) { destroyed++; }
static ALCboolean f_alcSetThreadContext( ALCcontext *c ) { thread_ctx = c; return ALC_TRUE; }
static const ALCchar *f_alcGetString( ALCdevice *d, ALCenum p )
{
    if (p == ALC_ALL_DEVICES_SPECIFIER && d) return "Fake Device";
    if (p == ALC_DEVICE_SPECIFIER && !d) return "A\0B\0";
    if (p == ALC_EXTENSIONS) return "ALC_ENUMERATE_ALL_EXT ALC_EXT_CAPTURE ALC_SOFT_pause_device";
    return "x";
}
static void f_alcGetIntegerv( ALCdevice *d, ALCenum p, ALCsizei n, ALCint *v ) { if (p == ALC_FREQUENCY && n >= 1) *v = 48000; }
static ALCboolean f_alcIsExtensionPresent( ALCdevice *d, const ALCchar *name ) { return ALC_TRUE; }
static void f_alcDevicePauseSOFT( ALCdevice *d ) { pauses[dev_index( d )]++; }
static void f_alcDeviceResumeSOFT( ALCdevice *d ) { resumes[dev_index( d )]++; }
static const ALchar *f_alGetString( ALenum p )
{
    return p == AL_EXTENSIONS ? "AL_EXT_FLOAT32 AL_SOFT_callback_buffer AL_SOFT_source_latency" : "1.1 fake";
}
static void f_alBufferData( ALuint id, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq )
{
    buffer_id = id;
    buffer_format = format;
    buffer_data = data;
    buffer_size = size;
    buffer_freq = freq;
    memcpy( buffer_first, data, 4 );
    ctx_at_call = thread_ctx;
}
static void f_alSourcefv( ALuint id, ALenum p, const ALfloat *v ) { memcpy( source_v, v, sizeof(source_v) ); ctx_at_call = thread_ctx; }
static void f_alSourcef( ALuint id, ALenum p, ALfloat v ) { source_v[0] = v; ctx_at_call = thread_ctx; }

static struct oa_api fake =
{
    .alcOpenDevice = f_alcOpenDevice, .alcCloseDevice = f_alcCloseDevice,
    .alcCreateContext = f_alcCreateContext, .alcDestroyContext = f_alcDestroyContext,
    .alcSetThreadContext = f_alcSetThreadContext, .alcGetString = f_alcGetString,
    .alcGetIntegerv = f_alcGetIntegerv, .alcIsExtensionPresent = f_alcIsExtensionPresent,
    .alcDevicePauseSOFT = f_alcDevicePauseSOFT, .alcDeviceResumeSOFT = f_alcDeviceResumeSOFT,
    .alGetString = f_alGetString, .alBufferData = f_alBufferData, .alSourcefv = f_alSourcefv,
    .alSourcef = f_alSourcef,
};
const struct oa_api *oa_api = &fake;

/* ---------------------------------------------------------------- calls */

static uint64_t call( int wow64, uint32_t func, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4 )
{
    struct openal32_call_params p = { func, 0, { a0, a1, a2, a3, a4, 0 }, 0 };
    CHECK_INT( (wow64 ? openal32_unix_call_wow64_funcs : openal32_unix_call_funcs)[unix_openal32_call]( &p ), 0 );
    return p.ret;
}
#define C64(f, ...) call( 0, OAF_##f, __VA_ARGS__ )
#define C32(f, ...) call( 1, OAF_##f, __VA_ARGS__ )
#define P(x) ((uint64_t)(uintptr_t)(x))

static uint32_t f32bits( float f ) { uint32_t u; memcpy( &u, &f, 4 ); return u; }

int main( void )
{
    void *peb_a = (void *)0x1000, *peb_b = (void *)0x2000;
    uint64_t dev, dev2, ctx_a, ctx_b, need;
    char buf[128];

    /* devices and the open log, once per pseudo-process */
    test_peb = peb_a;
    test_log_clear();
    dev = C64( alcOpenDevice, 0, 0, 0, 0, 0 );
    CHECK( dev != 0 && dev <= 0xffffffffu );
    CHECK_INT( test_log_count( "[openal] device opened: Fake Device, 48000 Hz" ), 1 );
    dev2 = C64( alcOpenDevice, 0, 0, 0, 0, 0 );
    CHECK( dev2 != 0 && dev2 != dev );
    CHECK_INT( test_log_count( "[openal] device opened:" ), 1 );
    CHECK_INT( test_end_hook_adds, 1 );

    /* alcCreateContext's attribute list, 64-bit */
    {
        ALCint attrs[] = { ALC_FREQUENCY, 44100, 0 };
        ctx_a = C64( alcCreateContext, dev, P(attrs), 0, 0, 0 );
        CHECK( ctx_a != 0 && ctx_a <= 0xffffffffu );
        CHECK( ctx_device == (ALCdevice *)&devices[0] );
        CHECK_INT( attrs_seen[0], ALC_FREQUENCY );
        CHECK_INT( attrs_seen[1], 44100 );
        CHECK_INT( attrs_seen[2], 0 );
        CHECK_INT( C64( alcGetContextsDevice, ctx_a, 0, 0, 0, 0 ), dev );
    }

    /* the current context per pseudo-process; al* calls run with it */
    CHECK_INT( C64( alcMakeContextCurrent, ctx_a, 0, 0, 0, 0 ), ALC_TRUE );
    CHECK_INT( C64( alcMakeContextCurrent, 0x77770005, 0, 0, 0, 0 ), ALC_FALSE );   /* not an id */
    test_peb = peb_b;
    ctx_b = C64( alcCreateContext, dev2, 0, 0, 0, 0 );
    CHECK_INT( C64( alcMakeContextCurrent, ctx_b, 0, 0, 0, 0 ), ALC_TRUE );
    C64( alSourcef, 1, 0x100a, f32bits( 0.5f ), 0, 0 );
    CHECK( ctx_at_call == (ALCcontext *)&contexts[1] );
    test_peb = peb_a;
    C64( alSourcef, 1, 0x100a, f32bits( 0.25f ), 0, 0 );
    CHECK( ctx_at_call == (ALCcontext *)&contexts[0] );
    CHECK_NEAR( source_v[0], 0.25, 0 );
    CHECK_INT( C64( alcGetCurrentContext, 0, 0, 0, 0, 0 ), ctx_a );

    /* alBufferData and alSourcefv, 64-bit: the caller's own pointers */
    {
        static const uint8_t pcm[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        float v[3] = { 1.0f, 2.0f, 3.0f };

        C64( alBufferData, 7, 0x1101, P(pcm), sizeof(pcm), 22050 );
        CHECK_INT( buffer_id, 7 );
        CHECK_INT( buffer_format, 0x1101 );
        CHECK( buffer_data == pcm );
        CHECK_INT( buffer_size, 8 );
        CHECK_INT( buffer_freq, 22050 );
        C64( alSourcefv, 3, 0x1004, P(v), 0, 0 );
        CHECK_NEAR( source_v[2], 3.0, 0 );
    }

    /* the same through the 32-bit table: guest addresses converted by rule A4 */
    test_enter_window();
    {
        uint8_t *pcm = test_arena_alloc( 16 );
        float *v = test_arena_alloc( 3 * sizeof(float) );
        ALCint *attrs = test_arena_alloc( 5 * sizeof(ALCint) );
        uint64_t ctx32;

        memcpy( pcm, "\x10\x20\x30\x40", 4 );
        C32( alBufferData, 9, 0x1103, test_guest( pcm ), 16, 44100 );
        CHECK( buffer_data == pcm );
        CHECK_INT( buffer_first[3], 0x40 );
        CHECK_INT( buffer_size, 16 );

        v[0] = -1.0f; v[1] = 0.5f; v[2] = 8.0f;
        C32( alSourcefv, 3, 0x1006, test_guest( v ), 0, 0 );
        CHECK_NEAR( source_v[0], -1.0, 0 );
        CHECK_NEAR( source_v[2], 8.0, 0 );

        attrs[0] = ALC_FREQUENCY; attrs[1] = 22050; attrs[2] = 0x1010; attrs[3] = 32; attrs[4] = 0;
        ctx32 = C32( alcCreateContext, dev, test_guest( attrs ), 0, 0, 0 );
        CHECK( ctx32 != 0 && ctx32 <= 0xffffffffu );
        CHECK_INT( attrs_seen[1], 22050 );
        CHECK_INT( attrs_seen[3], 32 );
        CHECK_INT( attrs_seen[4], 0 );

        /* a string into a guest buffer */
        {
            char *gbuf = test_arena_alloc( 32 );
            need = C32( alGetString, 0xB002, test_guest( gbuf ), 32, 0, 0 );
            CHECK_INT( need, 9 );
            CHECK( !strcmp( gbuf, "1.1 fake" ) );
        }
    }
    test_leave_window();

    /* two-step strings, device lists, filtered extensions */
    need = C64( alcGetString, 0, ALC_DEVICE_SPECIFIER, 0, 0, 0 );
    CHECK_INT( need, 5 );
    memset( buf, 'z', sizeof(buf) );
    CHECK_INT( C64( alcGetString, 0, ALC_DEVICE_SPECIFIER, P(buf), sizeof(buf), 0 ), 5 );
    CHECK_MEM( buf, "A\0B\0", 5 );
    C64( alcGetString, dev, ALC_EXTENSIONS, P(buf), sizeof(buf), 0 );
    CHECK( !strcmp( buf, "ALC_ENUMERATE_ALL_EXT ALC_SOFT_pause_device" ) );
    C64( alGetString, AL_EXTENSIONS, P(buf), sizeof(buf), 0, 0 );
    CHECK( !strcmp( buf, "AL_EXT_FLOAT32 AL_SOFT_source_latency" ) );
    CHECK_INT( C64( alcIsExtensionPresent, dev, P("ALC_EXT_CAPTURE"), 0, 0, 0 ), ALC_FALSE );
    CHECK_INT( C64( alcIsExtensionPresent, dev, P("ALC_SOFT_pause_device"), 0, 0, 0 ), ALC_TRUE );
    memset( buf, 'z', sizeof(buf) );
    CHECK_INT( C64( alcGetString, 0, ALC_CAPTURE_DEVICE_SPECIFIER, P(buf), sizeof(buf), 0 ), 2 );
    CHECK_MEM( buf, "\0\0", 2 );
    CHECK_INT( C64( alGetString, 0xB002, P(buf), 4, 0, 0 ), 9 );   /* too small: the size only */

    /* not forwarded: logged once per name */
    test_log_clear();
    C64( alcCaptureOpenDevice, 0, 0, 0, 0, 0 );
    C64( alcCaptureOpenDevice, 0, 0, 0, 0, 0 );
    CHECK_INT( test_log_count( "[openal] call alcCaptureOpenDevice unsupported" ), 1 );

    /* the app inactive: every device paused; a device the program paused stays paused */
    C64( alcDevicePauseSOFT, dev2, 0, 0, 0, 0 );
    CHECK_INT( pauses[1], 1 );
    pg_openal_set_app_active( 0 );
    CHECK_INT( pauses[0], 1 );
    CHECK_INT( pauses[1], 2 );
    C64( alcDeviceResumeSOFT, dev, 0, 0, 0, 0 );
    CHECK_INT( resumes[0], 0 );                    /* held while the app is inactive */
    pg_openal_set_app_active( 0 );                 /* repeating a state does nothing */
    CHECK_INT( pauses[0], 1 );
    {
        uint64_t dev3 = C64( alcOpenDevice, 0, 0, 0, 0, 0 );   /* opened while inactive: paused */
        CHECK( dev3 != 0 );
        CHECK_INT( pauses[2], 1 );
    }
    pg_openal_set_app_active( 1 );
    CHECK_INT( resumes[0], 1 );
    CHECK_INT( resumes[1], 0 );                    /* the program's own pause */
    CHECK_INT( resumes[2], 1 );

    /* a pseudo-process ends: its contexts destroyed, devices closed, ids gone */
    destroyed = closed = 0;
    CHECK( test_end_hook != NULL );
    test_end_hook( peb_a );
    CHECK( destroyed >= 2 );   /* ctx_a and the 32-bit caller's context */
    CHECK( closed >= 1 );
    CHECK_INT( C64( alcGetContextsDevice, ctx_a, 0, 0, 0, 0 ), 0 );
    test_peb = peb_b;
    CHECK_INT( C64( alcGetContextsDevice, ctx_b, 0, 0, 0, 0 ), dev2 );   /* the other process's stays */

    return test_report( "test_openal" );
}
