// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 4: every export of openal32.dll, in one list. The PE side (openal32_main.c),
// the unix side (openal-unix/) and the export check (tests/check-openal-exports.sh) all use it;
// its order is the function index passed through the unix call.
//
// OA(name, flags, ptrmask)
//   flags   OA_CTX    an al* call: runs with the calling pseudo-process's current context
//           OA_STR    returns a string: two-step, the caller passes a buffer and its size as
//                     the two arguments after the function's own
//           OA_UNSUP  exported but not forwarded: logged once ("[openal] call <name>
//                     unsupported"), answers a neutral value
//           OA_PE     answered by the PE side alone (the *GetProcAddress functions)
//   ptrmask bit n set: argument n is a pointer to the caller's memory (a guest address for a
//           32-bit caller, converted by rule A4). Device and context handles are not pointers.

#ifndef OPENAL32_FUNCS_H
#define OPENAL32_FUNCS_H

#define OA_CTX   0x1
#define OA_STR   0x2
#define OA_UNSUP 0x4
#define OA_PE    0x8

#define OPENAL32_FUNCS \
    /* OpenAL 1.1 */ \
    OA(alEnable,               OA_CTX, 0x00) \
    OA(alDisable,              OA_CTX, 0x00) \
    OA(alIsEnabled,            OA_CTX, 0x00) \
    OA(alGetString,            OA_CTX | OA_STR, 0x02) \
    OA(alGetBooleanv,          OA_CTX, 0x02) \
    OA(alGetIntegerv,          OA_CTX, 0x02) \
    OA(alGetFloatv,            OA_CTX, 0x02) \
    OA(alGetDoublev,           OA_CTX, 0x02) \
    OA(alGetBoolean,           OA_CTX, 0x00) \
    OA(alGetInteger,           OA_CTX, 0x00) \
    OA(alGetFloat,             OA_CTX, 0x00) \
    OA(alGetDouble,            OA_CTX, 0x00) \
    OA(alGetError,             OA_CTX, 0x00) \
    OA(alIsExtensionPresent,   OA_CTX, 0x01) \
    OA(alGetProcAddress,       OA_PE,  0x00) \
    OA(alGetEnumValue,         OA_CTX, 0x01) \
    OA(alListenerf,            OA_CTX, 0x00) \
    OA(alListener3f,           OA_CTX, 0x00) \
    OA(alListenerfv,           OA_CTX, 0x02) \
    OA(alListeneri,            OA_CTX, 0x00) \
    OA(alListener3i,           OA_CTX, 0x00) \
    OA(alListeneriv,           OA_CTX, 0x02) \
    OA(alGetListenerf,         OA_CTX, 0x02) \
    OA(alGetListener3f,        OA_CTX, 0x0e) \
    OA(alGetListenerfv,        OA_CTX, 0x02) \
    OA(alGetListeneri,         OA_CTX, 0x02) \
    OA(alGetListener3i,        OA_CTX, 0x0e) \
    OA(alGetListeneriv,        OA_CTX, 0x02) \
    OA(alGenSources,           OA_CTX, 0x02) \
    OA(alDeleteSources,        OA_CTX, 0x02) \
    OA(alIsSource,             OA_CTX, 0x00) \
    OA(alSourcef,              OA_CTX, 0x00) \
    OA(alSource3f,             OA_CTX, 0x00) \
    OA(alSourcefv,             OA_CTX, 0x04) \
    OA(alSourcei,              OA_CTX, 0x00) \
    OA(alSource3i,             OA_CTX, 0x00) \
    OA(alSourceiv,             OA_CTX, 0x04) \
    OA(alGetSourcef,           OA_CTX, 0x04) \
    OA(alGetSource3f,          OA_CTX, 0x1c) \
    OA(alGetSourcefv,          OA_CTX, 0x04) \
    OA(alGetSourcei,           OA_CTX, 0x04) \
    OA(alGetSource3i,          OA_CTX, 0x1c) \
    OA(alGetSourceiv,          OA_CTX, 0x04) \
    OA(alSourcePlayv,          OA_CTX, 0x02) \
    OA(alSourceStopv,          OA_CTX, 0x02) \
    OA(alSourceRewindv,        OA_CTX, 0x02) \
    OA(alSourcePausev,         OA_CTX, 0x02) \
    OA(alSourcePlay,           OA_CTX, 0x00) \
    OA(alSourceStop,           OA_CTX, 0x00) \
    OA(alSourceRewind,         OA_CTX, 0x00) \
    OA(alSourcePause,          OA_CTX, 0x00) \
    OA(alSourceQueueBuffers,   OA_CTX, 0x04) \
    OA(alSourceUnqueueBuffers, OA_CTX, 0x04) \
    OA(alGenBuffers,           OA_CTX, 0x02) \
    OA(alDeleteBuffers,        OA_CTX, 0x02) \
    OA(alIsBuffer,             OA_CTX, 0x00) \
    OA(alBufferData,           OA_CTX, 0x04) \
    OA(alBufferf,              OA_CTX, 0x00) \
    OA(alBuffer3f,             OA_CTX, 0x00) \
    OA(alBufferfv,             OA_CTX, 0x04) \
    OA(alBufferi,              OA_CTX, 0x00) \
    OA(alBuffer3i,             OA_CTX, 0x00) \
    OA(alBufferiv,             OA_CTX, 0x04) \
    OA(alGetBufferf,           OA_CTX, 0x04) \
    OA(alGetBuffer3f,          OA_CTX, 0x1c) \
    OA(alGetBufferfv,          OA_CTX, 0x04) \
    OA(alGetBufferi,           OA_CTX, 0x04) \
    OA(alGetBuffer3i,          OA_CTX, 0x1c) \
    OA(alGetBufferiv,          OA_CTX, 0x04) \
    OA(alDopplerFactor,        OA_CTX, 0x00) \
    OA(alDopplerVelocity,      OA_CTX, 0x00) \
    OA(alSpeedOfSound,         OA_CTX, 0x00) \
    OA(alDistanceModel,        OA_CTX, 0x00) \
    /* ALC 1.1 */ \
    OA(alcCreateContext,       0,      0x02) \
    OA(alcMakeContextCurrent,  0,      0x00) \
    OA(alcProcessContext,      0,      0x00) \
    OA(alcSuspendContext,      0,      0x00) \
    OA(alcDestroyContext,      0,      0x00) \
    OA(alcGetCurrentContext,   0,      0x00) \
    OA(alcGetContextsDevice,   0,      0x00) \
    OA(alcOpenDevice,          0,      0x01) \
    OA(alcCloseDevice,         0,      0x00) \
    OA(alcGetError,            0,      0x00) \
    OA(alcIsExtensionPresent,  0,      0x02) \
    OA(alcGetProcAddress,      OA_PE,  0x00) \
    OA(alcGetEnumValue,        0,      0x02) \
    OA(alcGetString,           OA_STR, 0x04) \
    OA(alcGetIntegerv,         0,      0x08) \
    OA(alcCaptureOpenDevice,   OA_UNSUP, 0x00) \
    OA(alcCaptureCloseDevice,  OA_UNSUP, 0x00) \
    OA(alcCaptureStart,        OA_UNSUP, 0x00) \
    OA(alcCaptureStop,         OA_UNSUP, 0x00) \
    OA(alcCaptureSamples,      OA_UNSUP, 0x00) \
    /* EFX */ \
    OA(alGenEffects,           OA_CTX, 0x02) \
    OA(alDeleteEffects,        OA_CTX, 0x02) \
    OA(alIsEffect,             OA_CTX, 0x00) \
    OA(alEffecti,              OA_CTX, 0x00) \
    OA(alEffectiv,             OA_CTX, 0x04) \
    OA(alEffectf,              OA_CTX, 0x00) \
    OA(alEffectfv,             OA_CTX, 0x04) \
    OA(alGetEffecti,           OA_CTX, 0x04) \
    OA(alGetEffectiv,          OA_CTX, 0x04) \
    OA(alGetEffectf,           OA_CTX, 0x04) \
    OA(alGetEffectfv,          OA_CTX, 0x04) \
    OA(alGenFilters,           OA_CTX, 0x02) \
    OA(alDeleteFilters,        OA_CTX, 0x02) \
    OA(alIsFilter,             OA_CTX, 0x00) \
    OA(alFilteri,              OA_CTX, 0x00) \
    OA(alFilteriv,             OA_CTX, 0x04) \
    OA(alFilterf,              OA_CTX, 0x00) \
    OA(alFilterfv,             OA_CTX, 0x04) \
    OA(alGetFilteri,           OA_CTX, 0x04) \
    OA(alGetFilteriv,          OA_CTX, 0x04) \
    OA(alGetFilterf,           OA_CTX, 0x04) \
    OA(alGetFilterfv,          OA_CTX, 0x04) \
    OA(alGenAuxiliaryEffectSlots,    OA_CTX, 0x02) \
    OA(alDeleteAuxiliaryEffectSlots, OA_CTX, 0x02) \
    OA(alIsAuxiliaryEffectSlot,      OA_CTX, 0x00) \
    OA(alAuxiliaryEffectSloti,       OA_CTX, 0x00) \
    OA(alAuxiliaryEffectSlotiv,      OA_CTX, 0x04) \
    OA(alAuxiliaryEffectSlotf,       OA_CTX, 0x00) \
    OA(alAuxiliaryEffectSlotfv,      OA_CTX, 0x04) \
    OA(alGetAuxiliaryEffectSloti,    OA_CTX, 0x04) \
    OA(alGetAuxiliaryEffectSlotiv,   OA_CTX, 0x04) \
    OA(alGetAuxiliaryEffectSlotf,    OA_CTX, 0x04) \
    OA(alGetAuxiliaryEffectSlotfv,   OA_CTX, 0x04) \
    /* ALC_EXT_thread_local_context, ALC_SOFT_pause_device, ALC_SOFT_HRTF */ \
    OA(alcSetThreadContext,    0,      0x00) \
    OA(alcGetThreadContext,    0,      0x00) \
    OA(alcDevicePauseSOFT,     0,      0x00) \
    OA(alcDeviceResumeSOFT,    0,      0x00) \
    OA(alcGetStringiSOFT,      OA_STR, 0x08) \
    OA(alcResetDeviceSOFT,     0,      0x02) \
    /* AL_SOFT_deferred_updates, AL_SOFT_source_resampler, AL_SOFT_source_latency */ \
    OA(alDeferUpdatesSOFT,     OA_CTX, 0x00) \
    OA(alProcessUpdatesSOFT,   OA_CTX, 0x00) \
    OA(alGetStringiSOFT,       OA_CTX | OA_STR, 0x04) \
    OA(alSourcedSOFT,          OA_CTX, 0x00) \
    OA(alSource3dSOFT,         OA_CTX, 0x00) \
    OA(alSourcedvSOFT,         OA_CTX, 0x04) \
    OA(alGetSourcedSOFT,       OA_CTX, 0x04) \
    OA(alGetSource3dSOFT,      OA_CTX, 0x1c) \
    OA(alGetSourcedvSOFT,      OA_CTX, 0x04) \
    OA(alSourcei64SOFT,        OA_CTX, 0x00) \
    OA(alSource3i64SOFT,       OA_CTX, 0x00) \
    OA(alSourcei64vSOFT,       OA_CTX, 0x04) \
    OA(alGetSourcei64SOFT,     OA_CTX, 0x04) \
    OA(alGetSource3i64SOFT,    OA_CTX, 0x1c) \
    OA(alGetSourcei64vSOFT,    OA_CTX, 0x04)

enum openal32_func
{
#define OA(name, flags, ptrmask) OAF_##name,
    OPENAL32_FUNCS
#undef OA
    OAF_COUNT
};

#endif /* OPENAL32_FUNCS_H */
