// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 4: the OpenAL Soft functions the unix side of openal32.dll calls, declared from
// the OpenAL 1.1 specification, the Effects Extension guide and OpenAL Soft's extension
// documents (types, signatures and enum values only). Plain C, no Wine headers. The calls go
// through `oa_api` (a table of these functions): openal_link.c fills it with the real ones from
// libopenal.1.dylib, the host tests with fakes.

#ifndef OPENAL_API_H
#define OPENAL_API_H

#include <stdint.h>

#include "openal32_funcs.h"

typedef char ALboolean;
typedef char ALchar;
typedef int ALint;
typedef unsigned int ALuint;
typedef int ALsizei;
typedef int ALenum;
typedef float ALfloat;
typedef double ALdouble;
typedef void ALvoid;
typedef int64_t ALint64SOFT;
typedef char ALCboolean;
typedef char ALCchar;
typedef int ALCint;
typedef unsigned int ALCuint;
typedef int ALCsizei;
typedef int ALCenum;
typedef void ALCvoid;
typedef struct ALCdevice ALCdevice;
typedef struct ALCcontext ALCcontext;

#define AL_EXTENSIONS                        0xB004
#define ALC_FALSE                            0
#define ALC_TRUE                             1
#define ALC_DEFAULT_DEVICE_SPECIFIER         0x1004
#define ALC_DEVICE_SPECIFIER                 0x1005
#define ALC_EXTENSIONS                       0x1006
#define ALC_FREQUENCY                        0x1007
#define ALC_DEFAULT_ALL_DEVICES_SPECIFIER    0x1012
#define ALC_ALL_DEVICES_SPECIFIER            0x1013
#define ALC_CAPTURE_DEVICE_SPECIFIER         0x0310
#define ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER 0x0311

/* OpenAL 1.1 */
void alEnable( ALenum cap );
void alDisable( ALenum cap );
ALboolean alIsEnabled( ALenum cap );
const ALchar *alGetString( ALenum param );
void alGetBooleanv( ALenum param, ALboolean *values );
void alGetIntegerv( ALenum param, ALint *values );
void alGetFloatv( ALenum param, ALfloat *values );
void alGetDoublev( ALenum param, ALdouble *values );
ALboolean alGetBoolean( ALenum param );
ALint alGetInteger( ALenum param );
ALfloat alGetFloat( ALenum param );
ALdouble alGetDouble( ALenum param );
ALenum alGetError( void );
ALboolean alIsExtensionPresent( const ALchar *name );
void *alGetProcAddress( const ALchar *name );
ALenum alGetEnumValue( const ALchar *name );
void alListenerf( ALenum param, ALfloat v );
void alListener3f( ALenum param, ALfloat v1, ALfloat v2, ALfloat v3 );
void alListenerfv( ALenum param, const ALfloat *v );
void alListeneri( ALenum param, ALint v );
void alListener3i( ALenum param, ALint v1, ALint v2, ALint v3 );
void alListeneriv( ALenum param, const ALint *v );
void alGetListenerf( ALenum param, ALfloat *v );
void alGetListener3f( ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3 );
void alGetListenerfv( ALenum param, ALfloat *v );
void alGetListeneri( ALenum param, ALint *v );
void alGetListener3i( ALenum param, ALint *v1, ALint *v2, ALint *v3 );
void alGetListeneriv( ALenum param, ALint *v );
void alGenSources( ALsizei n, ALuint *ids );
void alDeleteSources( ALsizei n, const ALuint *ids );
ALboolean alIsSource( ALuint id );
void alSourcef( ALuint id, ALenum param, ALfloat v );
void alSource3f( ALuint id, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3 );
void alSourcefv( ALuint id, ALenum param, const ALfloat *v );
void alSourcei( ALuint id, ALenum param, ALint v );
void alSource3i( ALuint id, ALenum param, ALint v1, ALint v2, ALint v3 );
void alSourceiv( ALuint id, ALenum param, const ALint *v );
void alGetSourcef( ALuint id, ALenum param, ALfloat *v );
void alGetSource3f( ALuint id, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3 );
void alGetSourcefv( ALuint id, ALenum param, ALfloat *v );
void alGetSourcei( ALuint id, ALenum param, ALint *v );
void alGetSource3i( ALuint id, ALenum param, ALint *v1, ALint *v2, ALint *v3 );
void alGetSourceiv( ALuint id, ALenum param, ALint *v );
void alSourcePlayv( ALsizei n, const ALuint *ids );
void alSourceStopv( ALsizei n, const ALuint *ids );
void alSourceRewindv( ALsizei n, const ALuint *ids );
void alSourcePausev( ALsizei n, const ALuint *ids );
void alSourcePlay( ALuint id );
void alSourceStop( ALuint id );
void alSourceRewind( ALuint id );
void alSourcePause( ALuint id );
void alSourceQueueBuffers( ALuint id, ALsizei n, const ALuint *buffers );
void alSourceUnqueueBuffers( ALuint id, ALsizei n, ALuint *buffers );
void alGenBuffers( ALsizei n, ALuint *ids );
void alDeleteBuffers( ALsizei n, const ALuint *ids );
ALboolean alIsBuffer( ALuint id );
void alBufferData( ALuint id, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq );
void alBufferf( ALuint id, ALenum param, ALfloat v );
void alBuffer3f( ALuint id, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3 );
void alBufferfv( ALuint id, ALenum param, const ALfloat *v );
void alBufferi( ALuint id, ALenum param, ALint v );
void alBuffer3i( ALuint id, ALenum param, ALint v1, ALint v2, ALint v3 );
void alBufferiv( ALuint id, ALenum param, const ALint *v );
void alGetBufferf( ALuint id, ALenum param, ALfloat *v );
void alGetBuffer3f( ALuint id, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3 );
void alGetBufferfv( ALuint id, ALenum param, ALfloat *v );
void alGetBufferi( ALuint id, ALenum param, ALint *v );
void alGetBuffer3i( ALuint id, ALenum param, ALint *v1, ALint *v2, ALint *v3 );
void alGetBufferiv( ALuint id, ALenum param, ALint *v );
void alDopplerFactor( ALfloat v );
void alDopplerVelocity( ALfloat v );
void alSpeedOfSound( ALfloat v );
void alDistanceModel( ALenum model );

/* ALC 1.1 */
ALCcontext *alcCreateContext( ALCdevice *device, const ALCint *attrs );
ALCboolean alcMakeContextCurrent( ALCcontext *context );
void alcProcessContext( ALCcontext *context );
void alcSuspendContext( ALCcontext *context );
void alcDestroyContext( ALCcontext *context );
ALCcontext *alcGetCurrentContext( void );
ALCdevice *alcGetContextsDevice( ALCcontext *context );
ALCdevice *alcOpenDevice( const ALCchar *name );
ALCboolean alcCloseDevice( ALCdevice *device );
ALCenum alcGetError( ALCdevice *device );
ALCboolean alcIsExtensionPresent( ALCdevice *device, const ALCchar *name );
void *alcGetProcAddress( ALCdevice *device, const ALCchar *name );
ALCenum alcGetEnumValue( ALCdevice *device, const ALCchar *name );
const ALCchar *alcGetString( ALCdevice *device, ALCenum param );
void alcGetIntegerv( ALCdevice *device, ALCenum param, ALCsizei size, ALCint *values );
ALCdevice *alcCaptureOpenDevice( const ALCchar *name, ALCuint freq, ALCenum format, ALCsizei size );
ALCboolean alcCaptureCloseDevice( ALCdevice *device );
void alcCaptureStart( ALCdevice *device );
void alcCaptureStop( ALCdevice *device );
void alcCaptureSamples( ALCdevice *device, ALCvoid *buffer, ALCsizei samples );

/* EFX */
#define OA_EFX_PROTOS(Obj, Objs) \
void alGen##Objs( ALsizei n, ALuint *ids ); \
void alDelete##Objs( ALsizei n, const ALuint *ids ); \
ALboolean alIs##Obj( ALuint id ); \
void al##Obj##i( ALuint id, ALenum param, ALint v ); \
void al##Obj##iv( ALuint id, ALenum param, const ALint *v ); \
void al##Obj##f( ALuint id, ALenum param, ALfloat v ); \
void al##Obj##fv( ALuint id, ALenum param, const ALfloat *v ); \
void alGet##Obj##i( ALuint id, ALenum param, ALint *v ); \
void alGet##Obj##iv( ALuint id, ALenum param, ALint *v ); \
void alGet##Obj##f( ALuint id, ALenum param, ALfloat *v ); \
void alGet##Obj##fv( ALuint id, ALenum param, ALfloat *v );
OA_EFX_PROTOS(Effect, Effects)
OA_EFX_PROTOS(Filter, Filters)
OA_EFX_PROTOS(AuxiliaryEffectSlot, AuxiliaryEffectSlots)

/* extensions */
ALCboolean alcSetThreadContext( ALCcontext *context );
ALCcontext *alcGetThreadContext( void );
void alcDevicePauseSOFT( ALCdevice *device );
void alcDeviceResumeSOFT( ALCdevice *device );
const ALCchar *alcGetStringiSOFT( ALCdevice *device, ALCenum param, ALCsizei index );
ALCboolean alcResetDeviceSOFT( ALCdevice *device, const ALCint *attrs );
void alDeferUpdatesSOFT( void );
void alProcessUpdatesSOFT( void );
const ALchar *alGetStringiSOFT( ALenum param, ALsizei index );
void alSourcedSOFT( ALuint id, ALenum param, ALdouble v );
void alSource3dSOFT( ALuint id, ALenum param, ALdouble v1, ALdouble v2, ALdouble v3 );
void alSourcedvSOFT( ALuint id, ALenum param, const ALdouble *v );
void alGetSourcedSOFT( ALuint id, ALenum param, ALdouble *v );
void alGetSource3dSOFT( ALuint id, ALenum param, ALdouble *v1, ALdouble *v2, ALdouble *v3 );
void alGetSourcedvSOFT( ALuint id, ALenum param, ALdouble *v );
void alSourcei64SOFT( ALuint id, ALenum param, ALint64SOFT v );
void alSource3i64SOFT( ALuint id, ALenum param, ALint64SOFT v1, ALint64SOFT v2, ALint64SOFT v3 );
void alSourcei64vSOFT( ALuint id, ALenum param, const ALint64SOFT *v );
void alGetSourcei64SOFT( ALuint id, ALenum param, ALint64SOFT *v );
void alGetSource3i64SOFT( ALuint id, ALenum param, ALint64SOFT *v1, ALint64SOFT *v2, ALint64SOFT *v3 );
void alGetSourcei64vSOFT( ALuint id, ALenum param, ALint64SOFT *v );

/* one member per export of the list, typed as the function above */
struct oa_api
{
#define OA(name, flags, ptrmask) __typeof__(&name) name;
    OPENAL32_FUNCS
#undef OA
};

extern const struct oa_api *oa_api;   /* openal_link.c (the app), or a test's fakes */

#endif /* OPENAL_API_H */
