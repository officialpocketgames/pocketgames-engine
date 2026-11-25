// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 4: openal32.dll, the Windows side. Every OpenAL 1.1, ALC, EFX and supported
// extension export packs its arguments into struct openal32_call_params and goes through one
// unix call to OpenAL Soft (openal-unix/). Strings come back in two steps (size, then copy into
// a buffer this side owns) and are kept, so the returned pointers stay valid. The
// *GetProcAddress functions answer from this DLL's own exports. Calling convention: cdecl, as
// OpenAL32.dll on Windows.

#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wine/unixlib.h"

#include "openal32_funcs.h"
#include "unixlib.h"

typedef char ALboolean;
typedef char ALchar;
typedef int ALint;
typedef unsigned int ALuint;
typedef int ALsizei;
typedef int ALenum;
typedef float ALfloat;
typedef double ALdouble;
typedef void ALvoid;
typedef INT64 ALint64SOFT;
typedef char ALCboolean;
typedef char ALCchar;
typedef int ALCint;
typedef unsigned int ALCuint;
typedef int ALCsizei;
typedef int ALCenum;
typedef void ALCvoid;
typedef struct ALCdevice ALCdevice;
typedef struct ALCcontext ALCcontext;

#define AL_API CDECL

static HMODULE openal32_module;

static UINT64 oa_call( UINT32 func, UINT64 a0, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4, UINT64 a5 )
{
    struct openal32_call_params params;

    params.func = func;
    params.pad = 0;
    params.args[0] = a0;
    params.args[1] = a1;
    params.args[2] = a2;
    params.args[3] = a3;
    params.args[4] = a4;
    params.args[5] = a5;
    params.ret = 0;
    WINE_UNIX_CALL( unix_openal32_call, &params );
    return params.ret;
}

#define C0(f)                   oa_call( OAF_##f, 0, 0, 0, 0, 0, 0 )
#define C1(f, a)                oa_call( OAF_##f, a, 0, 0, 0, 0, 0 )
#define C2(f, a, b)             oa_call( OAF_##f, a, b, 0, 0, 0, 0 )
#define C3(f, a, b, c)          oa_call( OAF_##f, a, b, c, 0, 0, 0 )
#define C4(f, a, b, c, d)       oa_call( OAF_##f, a, b, c, d, 0, 0 )
#define C5(f, a, b, c, d, e)    oa_call( OAF_##f, a, b, c, d, e, 0 )

static inline UINT64 I( INT64 v ) { return (UINT64)v; }
static inline UINT64 P( const void *p ) { return (UINT64)(ULONG_PTR)p; }
static inline UINT64 F( float f ) { UINT32 u; memcpy( &u, &f, 4 ); return u; }
static inline UINT64 D( double d ) { UINT64 u; memcpy( &u, &d, 8 ); return u; }
static inline float RF( UINT64 r ) { UINT32 u = (UINT32)r; float f; memcpy( &f, &u, 4 ); return f; }
static inline double RD( UINT64 r ) { double d; memcpy( &d, &r, 8 ); return d; }
#define H(h) ((UINT64)(ULONG_PTR)(h))

/* ---------------------------------------------------------------- strings, in two steps */

struct kept_string
{
    struct kept_string *next;
    UINT32 func;
    UINT64 key[3];
    UINT32 size;
    char data[1];
};

static struct kept_string *kept_strings;
static CRITICAL_SECTION strings_cs;
static CRITICAL_SECTION_DEBUG strings_cs_debug =
{
    0, 0, &strings_cs,
    { &strings_cs_debug.ProcessLocksList, &strings_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": strings_cs") }
};
static CRITICAL_SECTION strings_cs = { &strings_cs_debug, -1, 0, 0, 0, 0 };

/* nargs: the function's own arguments (a0..a2); the buffer and its size follow them */
static const char *oa_string( UINT32 func, int nargs, UINT64 a0, UINT64 a1, UINT64 a2 )
{
    UINT64 args[OPENAL32_MAX_ARGS] = { a0, a1, a2, 0, 0, 0 };
    char local[512], *buf = local;
    UINT64 need;
    struct kept_string *s;
    const char *ret = NULL;

    args[nargs] = P( local );
    args[nargs + 1] = sizeof(local);
    need = oa_call( func, args[0], args[1], args[2], args[3], args[4], args[5] );
    if (!need) return NULL;
    if (need > sizeof(local))
    {
        if (!(buf = HeapAlloc( GetProcessHeap(), 0, need ))) return NULL;
        args[nargs] = P( buf );
        args[nargs + 1] = need;
        need = oa_call( func, args[0], args[1], args[2], args[3], args[4], args[5] );
        if (!need || need > args[nargs + 1])
        {
            HeapFree( GetProcessHeap(), 0, buf );
            return NULL;
        }
    }

    EnterCriticalSection( &strings_cs );
    for (s = kept_strings; s; s = s->next)
        if (s->func == func && s->key[0] == a0 && s->key[1] == a1 && s->key[2] == a2 &&
            s->size == need && !memcmp( s->data, buf, need )) break;
    if (!s && (s = HeapAlloc( GetProcessHeap(), 0, offsetof(struct kept_string, data[need]) )))
    {
        s->func = func;
        s->key[0] = a0;
        s->key[1] = a1;
        s->key[2] = a2;
        s->size = (UINT32)need;
        memcpy( s->data, buf, need );
        s->next = kept_strings;
        kept_strings = s;
    }
    if (s) ret = s->data;
    LeaveCriticalSection( &strings_cs );
    if (buf != local) HeapFree( GetProcessHeap(), 0, buf );
    return ret;
}

/* ---------------------------------------------------------------- OpenAL 1.1 */

void AL_API alEnable( ALenum cap ) { C1( alEnable, I(cap) ); }
void AL_API alDisable( ALenum cap ) { C1( alDisable, I(cap) ); }
ALboolean AL_API alIsEnabled( ALenum cap ) { return (ALboolean)C1( alIsEnabled, I(cap) ); }
const ALchar * AL_API alGetString( ALenum param ) { return oa_string( OAF_alGetString, 1, I(param), 0, 0 ); }
void AL_API alGetBooleanv( ALenum param, ALboolean *values ) { C2( alGetBooleanv, I(param), P(values) ); }
void AL_API alGetIntegerv( ALenum param, ALint *values ) { C2( alGetIntegerv, I(param), P(values) ); }
void AL_API alGetFloatv( ALenum param, ALfloat *values ) { C2( alGetFloatv, I(param), P(values) ); }
void AL_API alGetDoublev( ALenum param, ALdouble *values ) { C2( alGetDoublev, I(param), P(values) ); }
ALboolean AL_API alGetBoolean( ALenum param ) { return (ALboolean)C1( alGetBoolean, I(param) ); }
ALint AL_API alGetInteger( ALenum param ) { return (ALint)C1( alGetInteger, I(param) ); }
ALfloat AL_API alGetFloat( ALenum param ) { return RF( C1( alGetFloat, I(param) ) ); }
ALdouble AL_API alGetDouble( ALenum param ) { return RD( C1( alGetDouble, I(param) ) ); }
ALenum AL_API alGetError( void ) { return (ALenum)C0( alGetError ); }
ALboolean AL_API alIsExtensionPresent( const ALchar *name ) { return (ALboolean)C1( alIsExtensionPresent, P(name) ); }
void * AL_API alGetProcAddress( const ALchar *name ) { return name ? (void *)GetProcAddress( openal32_module, name ) : NULL; }
ALenum AL_API alGetEnumValue( const ALchar *name ) { return (ALenum)C1( alGetEnumValue, P(name) ); }

void AL_API alListenerf( ALenum param, ALfloat v ) { C2( alListenerf, I(param), F(v) ); }
void AL_API alListener3f( ALenum param, ALfloat v1, ALfloat v2, ALfloat v3 ) { C4( alListener3f, I(param), F(v1), F(v2), F(v3) ); }
void AL_API alListenerfv( ALenum param, const ALfloat *v ) { C2( alListenerfv, I(param), P(v) ); }
void AL_API alListeneri( ALenum param, ALint v ) { C2( alListeneri, I(param), I(v) ); }
void AL_API alListener3i( ALenum param, ALint v1, ALint v2, ALint v3 ) { C4( alListener3i, I(param), I(v1), I(v2), I(v3) ); }
void AL_API alListeneriv( ALenum param, const ALint *v ) { C2( alListeneriv, I(param), P(v) ); }
void AL_API alGetListenerf( ALenum param, ALfloat *v ) { C2( alGetListenerf, I(param), P(v) ); }
void AL_API alGetListener3f( ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3 ) { C4( alGetListener3f, I(param), P(v1), P(v2), P(v3) ); }
void AL_API alGetListenerfv( ALenum param, ALfloat *v ) { C2( alGetListenerfv, I(param), P(v) ); }
void AL_API alGetListeneri( ALenum param, ALint *v ) { C2( alGetListeneri, I(param), P(v) ); }
void AL_API alGetListener3i( ALenum param, ALint *v1, ALint *v2, ALint *v3 ) { C4( alGetListener3i, I(param), P(v1), P(v2), P(v3) ); }
void AL_API alGetListeneriv( ALenum param, ALint *v ) { C2( alGetListeneriv, I(param), P(v) ); }

void AL_API alGenSources( ALsizei n, ALuint *ids ) { C2( alGenSources, I(n), P(ids) ); }
void AL_API alDeleteSources( ALsizei n, const ALuint *ids ) { C2( alDeleteSources, I(n), P(ids) ); }
ALboolean AL_API alIsSource( ALuint id ) { return (ALboolean)C1( alIsSource, id ); }
void AL_API alSourcef( ALuint id, ALenum param, ALfloat v ) { C3( alSourcef, id, I(param), F(v) ); }
void AL_API alSource3f( ALuint id, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3 ) { C5( alSource3f, id, I(param), F(v1), F(v2), F(v3) ); }
void AL_API alSourcefv( ALuint id, ALenum param, const ALfloat *v ) { C3( alSourcefv, id, I(param), P(v) ); }
void AL_API alSourcei( ALuint id, ALenum param, ALint v ) { C3( alSourcei, id, I(param), I(v) ); }
void AL_API alSource3i( ALuint id, ALenum param, ALint v1, ALint v2, ALint v3 ) { C5( alSource3i, id, I(param), I(v1), I(v2), I(v3) ); }
void AL_API alSourceiv( ALuint id, ALenum param, const ALint *v ) { C3( alSourceiv, id, I(param), P(v) ); }
void AL_API alGetSourcef( ALuint id, ALenum param, ALfloat *v ) { C3( alGetSourcef, id, I(param), P(v) ); }
void AL_API alGetSource3f( ALuint id, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3 ) { C5( alGetSource3f, id, I(param), P(v1), P(v2), P(v3) ); }
void AL_API alGetSourcefv( ALuint id, ALenum param, ALfloat *v ) { C3( alGetSourcefv, id, I(param), P(v) ); }
void AL_API alGetSourcei( ALuint id, ALenum param, ALint *v ) { C3( alGetSourcei, id, I(param), P(v) ); }
void AL_API alGetSource3i( ALuint id, ALenum param, ALint *v1, ALint *v2, ALint *v3 ) { C5( alGetSource3i, id, I(param), P(v1), P(v2), P(v3) ); }
void AL_API alGetSourceiv( ALuint id, ALenum param, ALint *v ) { C3( alGetSourceiv, id, I(param), P(v) ); }
void AL_API alSourcePlayv( ALsizei n, const ALuint *ids ) { C2( alSourcePlayv, I(n), P(ids) ); }
void AL_API alSourceStopv( ALsizei n, const ALuint *ids ) { C2( alSourceStopv, I(n), P(ids) ); }
void AL_API alSourceRewindv( ALsizei n, const ALuint *ids ) { C2( alSourceRewindv, I(n), P(ids) ); }
void AL_API alSourcePausev( ALsizei n, const ALuint *ids ) { C2( alSourcePausev, I(n), P(ids) ); }
void AL_API alSourcePlay( ALuint id ) { C1( alSourcePlay, id ); }
void AL_API alSourceStop( ALuint id ) { C1( alSourceStop, id ); }
void AL_API alSourceRewind( ALuint id ) { C1( alSourceRewind, id ); }
void AL_API alSourcePause( ALuint id ) { C1( alSourcePause, id ); }
void AL_API alSourceQueueBuffers( ALuint id, ALsizei n, const ALuint *buffers ) { C3( alSourceQueueBuffers, id, I(n), P(buffers) ); }
void AL_API alSourceUnqueueBuffers( ALuint id, ALsizei n, ALuint *buffers ) { C3( alSourceUnqueueBuffers, id, I(n), P(buffers) ); }

void AL_API alGenBuffers( ALsizei n, ALuint *ids ) { C2( alGenBuffers, I(n), P(ids) ); }
void AL_API alDeleteBuffers( ALsizei n, const ALuint *ids ) { C2( alDeleteBuffers, I(n), P(ids) ); }
ALboolean AL_API alIsBuffer( ALuint id ) { return (ALboolean)C1( alIsBuffer, id ); }
void AL_API alBufferData( ALuint id, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq ) { C5( alBufferData, id, I(format), P(data), I(size), I(freq) ); }
void AL_API alBufferf( ALuint id, ALenum param, ALfloat v ) { C3( alBufferf, id, I(param), F(v) ); }
void AL_API alBuffer3f( ALuint id, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3 ) { C5( alBuffer3f, id, I(param), F(v1), F(v2), F(v3) ); }
void AL_API alBufferfv( ALuint id, ALenum param, const ALfloat *v ) { C3( alBufferfv, id, I(param), P(v) ); }
void AL_API alBufferi( ALuint id, ALenum param, ALint v ) { C3( alBufferi, id, I(param), I(v) ); }
void AL_API alBuffer3i( ALuint id, ALenum param, ALint v1, ALint v2, ALint v3 ) { C5( alBuffer3i, id, I(param), I(v1), I(v2), I(v3) ); }
void AL_API alBufferiv( ALuint id, ALenum param, const ALint *v ) { C3( alBufferiv, id, I(param), P(v) ); }
void AL_API alGetBufferf( ALuint id, ALenum param, ALfloat *v ) { C3( alGetBufferf, id, I(param), P(v) ); }
void AL_API alGetBuffer3f( ALuint id, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3 ) { C5( alGetBuffer3f, id, I(param), P(v1), P(v2), P(v3) ); }
void AL_API alGetBufferfv( ALuint id, ALenum param, ALfloat *v ) { C3( alGetBufferfv, id, I(param), P(v) ); }
void AL_API alGetBufferi( ALuint id, ALenum param, ALint *v ) { C3( alGetBufferi, id, I(param), P(v) ); }
void AL_API alGetBuffer3i( ALuint id, ALenum param, ALint *v1, ALint *v2, ALint *v3 ) { C5( alGetBuffer3i, id, I(param), P(v1), P(v2), P(v3) ); }
void AL_API alGetBufferiv( ALuint id, ALenum param, ALint *v ) { C3( alGetBufferiv, id, I(param), P(v) ); }

void AL_API alDopplerFactor( ALfloat v ) { C1( alDopplerFactor, F(v) ); }
void AL_API alDopplerVelocity( ALfloat v ) { C1( alDopplerVelocity, F(v) ); }
void AL_API alSpeedOfSound( ALfloat v ) { C1( alSpeedOfSound, F(v) ); }
void AL_API alDistanceModel( ALenum model ) { C1( alDistanceModel, I(model) ); }

/* ---------------------------------------------------------------- ALC 1.1 */

ALCcontext * AL_API alcCreateContext( ALCdevice *device, const ALCint *attrs ) { return (ALCcontext *)(ULONG_PTR)C2( alcCreateContext, H(device), P(attrs) ); }
ALCboolean AL_API alcMakeContextCurrent( ALCcontext *context ) { return (ALCboolean)C1( alcMakeContextCurrent, H(context) ); }
void AL_API alcProcessContext( ALCcontext *context ) { C1( alcProcessContext, H(context) ); }
void AL_API alcSuspendContext( ALCcontext *context ) { C1( alcSuspendContext, H(context) ); }
void AL_API alcDestroyContext( ALCcontext *context ) { C1( alcDestroyContext, H(context) ); }
ALCcontext * AL_API alcGetCurrentContext( void ) { return (ALCcontext *)(ULONG_PTR)C0( alcGetCurrentContext ); }
ALCdevice * AL_API alcGetContextsDevice( ALCcontext *context ) { return (ALCdevice *)(ULONG_PTR)C1( alcGetContextsDevice, H(context) ); }
ALCdevice * AL_API alcOpenDevice( const ALCchar *name ) { return (ALCdevice *)(ULONG_PTR)C1( alcOpenDevice, P(name) ); }
ALCboolean AL_API alcCloseDevice( ALCdevice *device ) { return (ALCboolean)C1( alcCloseDevice, H(device) ); }
ALCenum AL_API alcGetError( ALCdevice *device ) { return (ALCenum)C1( alcGetError, H(device) ); }
ALCboolean AL_API alcIsExtensionPresent( ALCdevice *device, const ALCchar *name ) { return (ALCboolean)C2( alcIsExtensionPresent, H(device), P(name) ); }
void * AL_API alcGetProcAddress( ALCdevice *device, const ALCchar *name ) { return name ? (void *)GetProcAddress( openal32_module, name ) : NULL; }
ALCenum AL_API alcGetEnumValue( ALCdevice *device, const ALCchar *name ) { return (ALCenum)C2( alcGetEnumValue, H(device), P(name) ); }
const ALCchar * AL_API alcGetString( ALCdevice *device, ALCenum param ) { return oa_string( OAF_alcGetString, 2, H(device), I(param), 0 ); }
void AL_API alcGetIntegerv( ALCdevice *device, ALCenum param, ALCsizei size, ALCint *values ) { C4( alcGetIntegerv, H(device), I(param), I(size), P(values) ); }

/* capture is not forwarded (no microphone on this port): logged once, a neutral answer */
ALCdevice * AL_API alcCaptureOpenDevice( const ALCchar *name, ALCuint freq, ALCenum format, ALCsizei size ) { C0( alcCaptureOpenDevice ); return NULL; }
ALCboolean AL_API alcCaptureCloseDevice( ALCdevice *device ) { C0( alcCaptureCloseDevice ); return 0; }
void AL_API alcCaptureStart( ALCdevice *device ) { C0( alcCaptureStart ); }
void AL_API alcCaptureStop( ALCdevice *device ) { C0( alcCaptureStop ); }
void AL_API alcCaptureSamples( ALCdevice *device, ALCvoid *buffer, ALCsizei samples ) { C0( alcCaptureSamples ); }

/* ---------------------------------------------------------------- EFX */

#define EFX_OBJECT(Obj, Objs) \
void AL_API alGen##Objs( ALsizei n, ALuint *ids ) { C2( alGen##Objs, I(n), P(ids) ); } \
void AL_API alDelete##Objs( ALsizei n, const ALuint *ids ) { C2( alDelete##Objs, I(n), P(ids) ); } \
ALboolean AL_API alIs##Obj( ALuint id ) { return (ALboolean)C1( alIs##Obj, id ); } \
void AL_API al##Obj##i( ALuint id, ALenum param, ALint v ) { C3( al##Obj##i, id, I(param), I(v) ); } \
void AL_API al##Obj##iv( ALuint id, ALenum param, const ALint *v ) { C3( al##Obj##iv, id, I(param), P(v) ); } \
void AL_API al##Obj##f( ALuint id, ALenum param, ALfloat v ) { C3( al##Obj##f, id, I(param), F(v) ); } \
void AL_API al##Obj##fv( ALuint id, ALenum param, const ALfloat *v ) { C3( al##Obj##fv, id, I(param), P(v) ); } \
void AL_API alGet##Obj##i( ALuint id, ALenum param, ALint *v ) { C3( alGet##Obj##i, id, I(param), P(v) ); } \
void AL_API alGet##Obj##iv( ALuint id, ALenum param, ALint *v ) { C3( alGet##Obj##iv, id, I(param), P(v) ); } \
void AL_API alGet##Obj##f( ALuint id, ALenum param, ALfloat *v ) { C3( alGet##Obj##f, id, I(param), P(v) ); } \
void AL_API alGet##Obj##fv( ALuint id, ALenum param, ALfloat *v ) { C3( alGet##Obj##fv, id, I(param), P(v) ); }

EFX_OBJECT(Effect, Effects)
EFX_OBJECT(Filter, Filters)
EFX_OBJECT(AuxiliaryEffectSlot, AuxiliaryEffectSlots)

/* ---------------------------------------------------------------- extensions */

ALCboolean AL_API alcSetThreadContext( ALCcontext *context ) { return (ALCboolean)C1( alcSetThreadContext, H(context) ); }
ALCcontext * AL_API alcGetThreadContext( void ) { return (ALCcontext *)(ULONG_PTR)C0( alcGetThreadContext ); }
void AL_API alcDevicePauseSOFT( ALCdevice *device ) { C1( alcDevicePauseSOFT, H(device) ); }
void AL_API alcDeviceResumeSOFT( ALCdevice *device ) { C1( alcDeviceResumeSOFT, H(device) ); }
const ALCchar * AL_API alcGetStringiSOFT( ALCdevice *device, ALCenum param, ALCsizei index ) { return oa_string( OAF_alcGetStringiSOFT, 3, H(device), I(param), I(index) ); }
ALCboolean AL_API alcResetDeviceSOFT( ALCdevice *device, const ALCint *attrs ) { return (ALCboolean)C2( alcResetDeviceSOFT, H(device), P(attrs) ); }

void AL_API alDeferUpdatesSOFT( void ) { C0( alDeferUpdatesSOFT ); }
void AL_API alProcessUpdatesSOFT( void ) { C0( alProcessUpdatesSOFT ); }
const ALchar * AL_API alGetStringiSOFT( ALenum param, ALsizei index ) { return oa_string( OAF_alGetStringiSOFT, 2, I(param), I(index), 0 ); }
void AL_API alSourcedSOFT( ALuint id, ALenum param, ALdouble v ) { C3( alSourcedSOFT, id, I(param), D(v) ); }
void AL_API alSource3dSOFT( ALuint id, ALenum param, ALdouble v1, ALdouble v2, ALdouble v3 ) { C5( alSource3dSOFT, id, I(param), D(v1), D(v2), D(v3) ); }
void AL_API alSourcedvSOFT( ALuint id, ALenum param, const ALdouble *v ) { C3( alSourcedvSOFT, id, I(param), P(v) ); }
void AL_API alGetSourcedSOFT( ALuint id, ALenum param, ALdouble *v ) { C3( alGetSourcedSOFT, id, I(param), P(v) ); }
void AL_API alGetSource3dSOFT( ALuint id, ALenum param, ALdouble *v1, ALdouble *v2, ALdouble *v3 ) { C5( alGetSource3dSOFT, id, I(param), P(v1), P(v2), P(v3) ); }
void AL_API alGetSourcedvSOFT( ALuint id, ALenum param, ALdouble *v ) { C3( alGetSourcedvSOFT, id, I(param), P(v) ); }
void AL_API alSourcei64SOFT( ALuint id, ALenum param, ALint64SOFT v ) { C3( alSourcei64SOFT, id, I(param), I(v) ); }
void AL_API alSource3i64SOFT( ALuint id, ALenum param, ALint64SOFT v1, ALint64SOFT v2, ALint64SOFT v3 ) { C5( alSource3i64SOFT, id, I(param), I(v1), I(v2), I(v3) ); }
void AL_API alSourcei64vSOFT( ALuint id, ALenum param, const ALint64SOFT *v ) { C3( alSourcei64vSOFT, id, I(param), P(v) ); }
void AL_API alGetSourcei64SOFT( ALuint id, ALenum param, ALint64SOFT *v ) { C3( alGetSourcei64SOFT, id, I(param), P(v) ); }
void AL_API alGetSource3i64SOFT( ALuint id, ALenum param, ALint64SOFT *v1, ALint64SOFT *v2, ALint64SOFT *v3 ) { C5( alGetSource3i64SOFT, id, I(param), P(v1), P(v2), P(v3) ); }
void AL_API alGetSourcei64vSOFT( ALuint id, ALenum param, ALint64SOFT *v ) { C3( alGetSourcei64vSOFT, id, I(param), P(v) ); }

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        openal32_module = instance;
        DisableThreadLibraryCalls( instance );
        return !__wine_init_unix_call();
    }
    return TRUE;
}
