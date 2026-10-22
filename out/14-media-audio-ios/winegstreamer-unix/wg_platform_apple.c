// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B4: the platform decoders. H.264 and HEVC on VideoToolbox, AAC on AudioToolbox's
// AudioConverter. This object includes no Wine header (CoreFoundation and winnt.h define clashing
// names); the parser reaches it through wgp_ops (wg_platform.h). Written from Apple's public
// documentation of VideoToolbox, CoreMedia, CoreVideo and AudioToolbox.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include "wg_platform.h"

/* ---------------------------------------------------------------- video */

struct wgp_video
{
    enum wgp_codec codec;
    CMFormatDescriptionRef desc;
    VTDecompressionSessionRef session;
    uint32_t width, height;
};

struct decode_call
{
    wgp_picture_cb cb;
    void *context;
    int64_t pts;
};

static void output_callback( void *refcon, void *frame_refcon, OSStatus status, VTDecodeInfoFlags flags,
                             CVImageBufferRef image, CMTime pts, CMTime duration )
{
    struct decode_call *call = frame_refcon;

    if (status != noErr || !image || !call) return;
    if (flags & kVTDecodeInfo_FrameDropped) return;
    CVPixelBufferRetain( image );
    call->cb( call->context, image, call->pts );
}

static int make_session( struct wgp_video *v )
{
    VTDecompressionOutputCallbackRecord cb = { output_callback, v };
    CFMutableDictionaryRef attrs;
    SInt32 format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;   /* ask for 4:2:0 bi-planar, video range */
    CFNumberRef number;
    OSStatus status;

    if (!(attrs = CFDictionaryCreateMutable( kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks ))) return WGP_E_NOMEM;
    number = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &format );
    if (number)
    {
        CFDictionarySetValue( attrs, kCVPixelBufferPixelFormatTypeKey, number );
        CFRelease( number );
    }
    status = VTDecompressionSessionCreate( kCFAllocatorDefault, v->desc, NULL, attrs, &cb, &v->session );
    CFRelease( attrs );
    if (status != noErr)
    {
        v->session = NULL;
        return WGP_E_FAIL;
    }
    return WGP_OK;
}

static void drop_session( struct wgp_video *v )
{
    if (!v->session) return;
    VTDecompressionSessionWaitForAsynchronousFrames( v->session );
    VTDecompressionSessionInvalidate( v->session );
    CFRelease( v->session );
    v->session = NULL;
}

static int video_create( enum wgp_codec codec, const uint8_t *config, size_t config_size, uint32_t width,
                         uint32_t height, struct wgp_video **out, char *why, size_t why_size )
{
    struct wgp_param_sets sets;
    struct wgp_video *v;
    const char *reason = NULL;
    OSStatus status;
    int ret;

    *out = NULL;
    if (codec == WGP_CODEC_H264) ret = wgp_parse_avcc( config, config_size, &sets, &reason );
    else ret = wgp_parse_hvcc( config, config_size, &sets, &reason );
    if (ret)
    {
        snprintf( why, why_size, "%s", reason );
        return WGP_E_FAIL;
    }
    if (codec == WGP_CODEC_HEVC && !VTIsHardwareDecodeSupported( kCMVideoCodecType_HEVC ))
    {
        snprintf( why, why_size, "no hardware HEVC decoder" );
        return WGP_E_FAIL;
    }
    if (!(v = calloc( 1, sizeof(*v) ))) return WGP_E_NOMEM;
    v->codec = codec;
    v->width = width;
    v->height = height;

    if (codec == WGP_CODEC_H264)
        status = CMVideoFormatDescriptionCreateFromH264ParameterSets( kCFAllocatorDefault, sets.count,
                (const uint8_t * const *)sets.data, sets.size, sets.nal_length_size, &v->desc );
    else
        status = CMVideoFormatDescriptionCreateFromHEVCParameterSets( kCFAllocatorDefault, sets.count,
                (const uint8_t * const *)sets.data, sets.size, sets.nal_length_size, NULL, &v->desc );
    if (status != noErr)
    {
        snprintf( why, why_size, "the parameter sets were refused (CoreMedia error %d)", (int)status );
        free( v );
        return WGP_E_FAIL;
    }
    if ((ret = make_session( v )))
    {
        snprintf( why, why_size, "VTDecompressionSessionCreate failed" );
        CFRelease( v->desc );
        free( v );
        return ret;
    }
    *out = v;
    return WGP_OK;
}

static CMTime to_cmtime( int64_t value, int32_t num, int32_t den )
{
    if (num <= 0 || den <= 0) return kCMTimeInvalid;
    return CMTimeMake( value * num, den );
}

static int video_decode( struct wgp_video *v, const uint8_t *data, size_t size, int64_t pts, int64_t duration,
                         int32_t tb_num, int32_t tb_den, int keyframe, wgp_picture_cb cb, void *context )
{
    struct decode_call call = { cb, context, pts };
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    CMSampleTimingInfo timing;
    VTDecodeInfoFlags info = 0;
    OSStatus status;
    int ret = WGP_OK;

    if (!size) return WGP_OK;
    if (!v->session && (ret = make_session( v ))) return ret;   /* recreated after a loss */

    status = CMBlockBufferCreateWithMemoryBlock( kCFAllocatorDefault, NULL, size, kCFAllocatorDefault, NULL,
                                                 0, size, kCMBlockBufferAssureMemoryNowFlag, &block );
    if (status != noErr) return WGP_E_NOMEM;
    if (CMBlockBufferReplaceDataBytes( data, block, 0, size ) != noErr)
    {
        CFRelease( block );
        return WGP_E_FAIL;
    }
    timing.duration = duration > 0 ? to_cmtime( duration, tb_num, tb_den ) : kCMTimeInvalid;
    timing.presentationTimeStamp = pts == WGP_NO_PTS ? kCMTimeInvalid : to_cmtime( pts, tb_num, tb_den );
    timing.decodeTimeStamp = kCMTimeInvalid;
    status = CMSampleBufferCreateReady( kCFAllocatorDefault, block, v->desc, 1, 1, &timing, 1, &size, &sample );
    CFRelease( block );
    if (status != noErr) return WGP_E_FAIL;

    /* flags 0: synchronous; every picture this packet finishes goes through the callback */
    status = VTDecompressionSessionDecodeFrame( v->session, sample, 0, &call, &info );
    if (status == noErr) VTDecompressionSessionWaitForAsynchronousFrames( v->session );
    CFRelease( sample );

    if (status == kVTInvalidSessionErr)
    {
        /* iOS tears hardware sessions down in the background: the next packet makes a new one,
         * and the picture recovers at the next keyframe */
        VTDecompressionSessionInvalidate( v->session );
        CFRelease( v->session );
        v->session = NULL;
        return WGP_E_SESSION_LOST;
    }
    (void)keyframe;
    return status == noErr ? WGP_OK : WGP_E_FAIL;
}

static void video_flush( struct wgp_video *v )
{
    drop_session( v );   /* the next packet (a keyframe after the seek) makes a new one */
}

static void video_destroy( struct wgp_video *v )
{
    if (!v) return;
    drop_session( v );
    if (v->desc) CFRelease( v->desc );
    free( v );
}

static int picture_lock( void *picture, struct wgp_picture *out )
{
    CVPixelBufferRef pb = picture;
    OSType format = CVPixelBufferGetPixelFormatType( pb );

    memset( out, 0, sizeof(*out) );
    if (format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
        format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) return WGP_E_FAIL;
    if (CVPixelBufferGetPlaneCount( pb ) != 2) return WGP_E_FAIL;
    if (CVPixelBufferLockBaseAddress( pb, kCVPixelBufferLock_ReadOnly ) != kCVReturnSuccess) return WGP_E_FAIL;
    out->width = (uint32_t)CVPixelBufferGetWidthOfPlane( pb, 0 );
    out->height = (uint32_t)CVPixelBufferGetHeightOfPlane( pb, 0 );
    out->full_range = format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    out->y = CVPixelBufferGetBaseAddressOfPlane( pb, 0 );
    out->y_stride = CVPixelBufferGetBytesPerRowOfPlane( pb, 0 );
    out->uv = CVPixelBufferGetBaseAddressOfPlane( pb, 1 );
    out->uv_stride = CVPixelBufferGetBytesPerRowOfPlane( pb, 1 );
    if (!out->y || !out->uv)
    {
        CVPixelBufferUnlockBaseAddress( pb, kCVPixelBufferLock_ReadOnly );
        return WGP_E_FAIL;
    }
    return WGP_OK;
}

static void picture_unlock( void *picture )
{
    CVPixelBufferUnlockBaseAddress( (CVPixelBufferRef)picture, kCVPixelBufferLock_ReadOnly );
}

static void picture_release( void *picture )
{
    CVPixelBufferRelease( (CVPixelBufferRef)picture );
}

/* ---------------------------------------------------------------- AAC */

#define NO_MORE_INPUT ((OSStatus)0x77676e64)   /* 'wgnd': our own "one packet per call" answer */

struct wgp_aac
{
    AudioConverterRef converter;
    uint32_t channels;
};

struct aac_input
{
    const uint8_t *data;
    UInt32 size;
    UInt32 channels;
    int consumed;
    AudioStreamPacketDescription desc;
};

static OSStatus aac_input_proc( AudioConverterRef converter, UInt32 *packets, AudioBufferList *data,
                                AudioStreamPacketDescription **desc, void *user )
{
    struct aac_input *in = user;

    if (in->consumed)
    {
        *packets = 0;
        return NO_MORE_INPUT;
    }
    data->mNumberBuffers = 1;
    data->mBuffers[0].mData = (void *)in->data;
    data->mBuffers[0].mDataByteSize = in->size;
    data->mBuffers[0].mNumberChannels = in->channels;
    in->desc.mStartOffset = 0;
    in->desc.mVariableFramesInPacket = 0;
    in->desc.mDataByteSize = in->size;
    if (desc) *desc = &in->desc;
    *packets = 1;
    in->consumed = 1;
    return noErr;
}

static int aac_create( const uint8_t *asc, size_t asc_size, uint32_t container_rate, uint32_t container_channels,
                       struct wgp_aac **out, uint32_t *rate, uint32_t *channels, uint32_t *frames_per_packet,
                       char *why, size_t why_size )
{
    uint8_t cookie[WGP_ESDS_MAX];
    size_t cookie_size;
    AudioStreamBasicDescription in, pcm;
    AudioFormatInfo info;
    UInt32 size;
    OSStatus status;
    struct wgp_aac *a;

    *out = NULL;
    if (!(cookie_size = wgp_esds_cookie( asc, asc_size, cookie, sizeof(cookie) )))
    {
        snprintf( why, why_size, "AudioSpecificConfig of %zu bytes (2-64 needed)", asc_size );
        return WGP_E_FAIL;
    }

    memset( &in, 0, sizeof(in) );
    in.mFormatID = kAudioFormatMPEG4AAC;
    size = sizeof(in);
    status = AudioFormatGetProperty( kAudioFormatProperty_FormatInfo, (UInt32)cookie_size, cookie, &size, &in );
    if (status == noErr)
    {
        /* the richest layer: SBR/PS streams report their real rate (maybe twice the container's) */
        memset( &info, 0, sizeof(info) );
        info.mASBD = in;
        info.mMagicCookie = cookie;
        info.mMagicCookieSize = (UInt32)cookie_size;
        size = 0;
        if (AudioFormatGetPropertyInfo( kAudioFormatProperty_FormatList, sizeof(info), &info, &size ) == noErr &&
            size >= sizeof(AudioFormatListItem))
        {
            AudioFormatListItem *items = malloc( size );
            if (items && AudioFormatGetProperty( kAudioFormatProperty_FormatList, sizeof(info), &info, &size,
                                                 items ) == noErr && size >= sizeof(AudioFormatListItem))
                in = items[0].mASBD;
            free( items );
        }
    }
    if (status != noErr || !in.mSampleRate || !in.mChannelsPerFrame)
    {
        memset( &in, 0, sizeof(in) );
        in.mFormatID = kAudioFormatMPEG4AAC;
        in.mSampleRate = container_rate;
        in.mChannelsPerFrame = container_channels;
        in.mFramesPerPacket = 1024;
    }
    if (!in.mFramesPerPacket) in.mFramesPerPacket = 1024;

    if (!in.mChannelsPerFrame || in.mChannelsPerFrame > 8 ||
        (in.mChannelsPerFrame > 2 && in.mChannelsPerFrame != 6 && in.mChannelsPerFrame != 8))
    {
        snprintf( why, why_size, "%u channels (1, 2, 6 or 8 supported)", (unsigned int)in.mChannelsPerFrame );
        return WGP_E_FAIL;
    }
    if (!in.mSampleRate)
    {
        snprintf( why, why_size, "no sample rate" );
        return WGP_E_FAIL;
    }

    memset( &pcm, 0, sizeof(pcm) );
    pcm.mSampleRate = in.mSampleRate;
    pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    pcm.mChannelsPerFrame = in.mChannelsPerFrame;
    pcm.mBitsPerChannel = 32;
    pcm.mBytesPerFrame = 4 * in.mChannelsPerFrame;
    pcm.mFramesPerPacket = 1;
    pcm.mBytesPerPacket = pcm.mBytesPerFrame;

    if (!(a = calloc( 1, sizeof(*a) ))) return WGP_E_NOMEM;
    if ((status = AudioConverterNew( &in, &pcm, &a->converter )) != noErr)
    {
        snprintf( why, why_size, "AudioConverterNew failed (%d)", (int)status );
        free( a );
        return WGP_E_FAIL;
    }
    AudioConverterSetProperty( a->converter, kAudioConverterDecompressionMagicCookie, (UInt32)cookie_size, cookie );
    if (in.mChannelsPerFrame == 6 || in.mChannelsPerFrame == 8)
    {
        /* WAVE speaker order (AAC's own order starts with the centre) */
        AudioChannelLayout layout;

        memset( &layout, 0, sizeof(layout) );
        layout.mChannelLayoutTag = in.mChannelsPerFrame == 6 ? kAudioChannelLayoutTag_MPEG_5_1_A
                                                             : kAudioChannelLayoutTag_MPEG_7_1_C;
        status = AudioConverterSetProperty( a->converter, kAudioConverterOutputChannelLayout, sizeof(layout), &layout );
        if (status != noErr)
        {
            snprintf( why, why_size, "can't set the %u-channel output layout (%d)",
                      (unsigned int)in.mChannelsPerFrame, (int)status );
            AudioConverterDispose( a->converter );
            free( a );
            return WGP_E_FAIL;
        }
    }
    a->channels = in.mChannelsPerFrame;
    *rate = (uint32_t)in.mSampleRate;
    *channels = in.mChannelsPerFrame;
    *frames_per_packet = in.mFramesPerPacket;
    *out = a;
    return WGP_OK;
}

static int aac_decode( struct wgp_aac *a, const uint8_t *data, size_t size, float *out, uint32_t max_frames,
                       uint32_t *frames )
{
    struct aac_input in;
    AudioBufferList list;
    UInt32 n = max_frames;
    OSStatus status;

    *frames = 0;
    if (!size) return WGP_OK;
    memset( &in, 0, sizeof(in) );
    in.data = data;
    in.size = (UInt32)size;
    in.channels = a->channels;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = a->channels;
    list.mBuffers[0].mDataByteSize = max_frames * 4 * a->channels;
    list.mBuffers[0].mData = out;
    status = AudioConverterFillComplexBuffer( a->converter, aac_input_proc, &in, &n, &list, NULL );
    if (status != noErr && status != NO_MORE_INPUT) return WGP_E_FAIL;
    *frames = n;
    return WGP_OK;
}

static void aac_flush( struct wgp_aac *a )
{
    AudioConverterReset( a->converter );
}

static void aac_destroy( struct wgp_aac *a )
{
    if (!a) return;
    AudioConverterDispose( a->converter );
    free( a );
}

const struct wgp_ops wgp_apple_ops =
{
    video_create,
    video_decode,
    video_flush,
    video_destroy,
    picture_lock,
    picture_unlock,
    picture_release,
    aac_create,
    aac_decode,
    aac_flush,
    aac_destroy,
};

const struct wgp_ops *wgp_ops = &wgp_apple_ops;
