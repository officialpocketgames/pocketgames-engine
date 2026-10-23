// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part C5 and C10: the app's one RemoteIO output unit, the monotonic clock, absolute
// sleeps and the Mach time-constraint policy. No Wine header (CoreFoundation and winnt.h
// clash); the driver reaches this through ios_audio_pure.h. Written from Apple's documentation
// of Audio Unit (RemoteIO), AudioToolbox and Mach thread policies. The app activates the
// AVAudioSession (Playback) before boot (spec 02).

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

#include "ios_audio_pure.h"

static AudioComponentInstance unit;
static ia_render_fn render_fn;

static OSStatus render_callback( void *refcon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time,
                                 UInt32 bus, UInt32 frames, AudioBufferList *data )
{
    AudioBuffer *buffer;

    if (!data || !data->mNumberBuffers) return noErr;
    buffer = &data->mBuffers[0];
    if (!buffer->mData) return noErr;
    render_fn( (float *)buffer->mData, frames, buffer->mDataByteSize );
    return noErr;
}

int ia_unit_create( uint32_t rate, uint32_t channels, ia_render_fn render, char *why, size_t why_size )
{
    AudioComponentDescription desc;
    AudioStreamBasicDescription format;
    AURenderCallbackStruct callback;
    AudioComponent component;
    OSStatus status;

    memset( &desc, 0, sizeof(desc) );
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    if (!(component = AudioComponentFindNext( NULL, &desc )))
    {
        snprintf( why, why_size, "no RemoteIO component" );
        return -1;
    }
    if ((status = AudioComponentInstanceNew( component, &unit )) != noErr)
    {
        snprintf( why, why_size, "AudioComponentInstanceNew failed (%d)", (int)status );
        unit = NULL;
        return -1;
    }

    /* float32, packed, interleaved */
    memset( &format, 0, sizeof(format) );
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 32;
    format.mBytesPerFrame = 4 * channels;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = format.mBytesPerFrame;
    if ((status = AudioUnitSetProperty( unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                        &format, sizeof(format) )) != noErr)
    {
        snprintf( why, why_size, "setting the stream format failed (%d)", (int)status );
        goto fail;
    }

    render_fn = render;
    callback.inputProc = render_callback;
    callback.inputProcRefCon = NULL;
    if ((status = AudioUnitSetProperty( unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                        &callback, sizeof(callback) )) != noErr)
    {
        snprintf( why, why_size, "setting the render callback failed (%d)", (int)status );
        goto fail;
    }
    if ((status = AudioUnitInitialize( unit )) != noErr)
    {
        snprintf( why, why_size, "AudioUnitInitialize failed (%d)", (int)status );
        goto fail;
    }
    return 0;

fail:
    AudioComponentInstanceDispose( unit );
    unit = NULL;
    return -1;
}

int ia_unit_start( void )
{
    return unit && AudioOutputUnitStart( unit ) == noErr ? 0 : -1;
}

int ia_unit_stop( void )
{
    return unit && AudioOutputUnitStop( unit ) == noErr ? 0 : -1;
}

/* ---------------------------------------------------------------- time */

static mach_timebase_info_data_t timebase;
static pthread_once_t timebase_once = PTHREAD_ONCE_INIT;

static void init_timebase( void )
{
    mach_timebase_info( &timebase );
    if (!timebase.numer || !timebase.denom) timebase.numer = timebase.denom = 1;
}

static uint64_t abs_to_ns( uint64_t t )
{
    pthread_once( &timebase_once, init_timebase );
    return t / timebase.denom * timebase.numer + (t % timebase.denom) * timebase.numer / timebase.denom;
}

static uint64_t ns_to_abs( uint64_t ns )
{
    pthread_once( &timebase_once, init_timebase );
    return ns / timebase.numer * timebase.denom + (ns % timebase.numer) * timebase.denom / timebase.numer;
}

uint64_t ia_now_ns( void )
{
    return abs_to_ns( mach_absolute_time() );
}

void ia_sleep_until_ns( uint64_t deadline_ns )
{
    mach_wait_until( ns_to_abs( deadline_ns ) );
}

int ia_set_time_constraint( uint64_t period_ns, uint64_t computation_ns, uint64_t constraint_ns, int preemptible )
{
    thread_time_constraint_policy_data_t policy;

    policy.period = (uint32_t)ns_to_abs( period_ns );
    policy.computation = (uint32_t)ns_to_abs( computation_ns );
    policy.constraint = (uint32_t)ns_to_abs( constraint_ns );
    policy.preemptible = preemptible ? TRUE : FALSE;
    return thread_policy_set( pthread_mach_thread_np( pthread_self() ), THREAD_TIME_CONSTRAINT_POLICY,
                              (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT ) == KERN_SUCCESS ? 0 : -1;
}
