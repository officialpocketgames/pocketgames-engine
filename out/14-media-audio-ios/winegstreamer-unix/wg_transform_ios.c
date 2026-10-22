// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B2: wg_transform_* on iOS: the WMA decoder only, as wma_decoder.c (the WMA
// decoder MFT / DMO) drives it. This file translates media types and samples; the decoding is
// wma_core.c.

#if 0
#pragma makedep unix
#endif

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wg_ios.h"
#include "ios_media_wow.h"
#include "wg_handles.h"
#include "wma_core.h"
#include "wma_params.h"

#define WAVE_TAG_PCM 0x0001
#define WAVE_TAG_FLOAT 0x0003
#define WAVE_TAG_EXTENSIBLE 0xfffe

const GUID wg_ios_mf_audio = { 0x73647561, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
const GUID wg_ios_mf_video = { 0x73646976, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

struct transform
{
    pthread_mutex_t lock;
    struct wma_core *core;
    uint32_t rate;               /* the input's rate: the output must keep it */
    WAVEFORMATEX *out_format;    /* the exact format last set */
    UINT32 out_size;
};

/* the {0000XXXX-0000-0010-8000-00AA00389B71} family */
static BOOL guid_is_wave_family( const GUID *g )
{
    static const BYTE tail[8] = { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
    return g->Data1 <= 0xffff && !g->Data2 && g->Data3 == 0x0010 && !memcmp( g->Data4, tail, 8 );
}

struct audio_info
{
    const WAVEFORMATEX *wfx;
    UINT32 size;
    UINT16 tag;          /* effective tag */
    UINT32 mask;
    const BYTE *priv;
    UINT32 priv_size;
};

/* An audio media type with a WAVEFORMATEX. FALSE for anything else. */
static BOOL parse_audio( const struct wg_media_type *mt, struct audio_info *info )
{
    const WAVEFORMATEX *wfx = mt->u.audio;

    memset( info, 0, sizeof(*info) );
    if (memcmp( &mt->major, &wg_ios_mf_audio, sizeof(GUID) )) return FALSE;
    if (!wfx || mt->format_size < sizeof(WAVEFORMATEX)) return FALSE;
    info->wfx = wfx;
    info->size = mt->format_size;
    info->tag = wfx->wFormatTag;
    if (wfx->wFormatTag == WAVE_TAG_EXTENSIBLE && mt->format_size >= sizeof(WAVEFORMATEXTENSIBLE) &&
        wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wfx;
        GUID sub;

        memcpy( &sub, &ext->SubFormat, sizeof(sub) );
        if (guid_is_wave_family( &sub )) info->tag = (UINT16)sub.Data1;
        info->mask = ext->dwChannelMask;
    }
    info->priv = (const BYTE *)wfx + sizeof(WAVEFORMATEX);
    info->priv_size = wfx->cbSize;
    if (info->priv_size > mt->format_size - sizeof(WAVEFORMATEX))
        info->priv_size = mt->format_size - sizeof(WAVEFORMATEX);
    return TRUE;
}

static BOOL is_wma_family( UINT16 tag )
{
    return tag == WMA_TAG_V1 || tag == WMA_TAG_V2 || tag == WMA_TAG_PRO || tag == WMA_TAG_LOSSLESS ||
           tag == WMA_TAG_XMA1 || tag == WMA_TAG_XMA2;
}

/* 16-bit PCM or 32-bit float, at `rate`, non-zero channels and block align */
static BOOL parse_output( const struct wg_media_type *mt, uint32_t rate, struct wma_core_output *out,
                          struct audio_info *info )
{
    const WAVEFORMATEX *wfx;

    if (!parse_audio( mt, info )) return FALSE;
    wfx = info->wfx;
    if (!wfx->nChannels || !wfx->nBlockAlign || wfx->nSamplesPerSec != rate) return FALSE;
    if (info->tag == WAVE_TAG_PCM && wfx->wBitsPerSample == 16) out->is_float = 0;
    else if (info->tag == WAVE_TAG_FLOAT && wfx->wBitsPerSample == 32) out->is_float = 1;
    else return FALSE;
    out->channels = wfx->nChannels;
    out->channel_mask = info->mask;
    return TRUE;
}

static void describe( const struct wg_media_type *mt, char *buf, size_t size )
{
    if (!memcmp( &mt->major, &wg_ios_mf_video, sizeof(GUID) ))
    {
        const MFVIDEOFORMAT *v = mt->u.video;
        if (v && mt->format_size >= sizeof(MFVIDEOFORMAT))
            snprintf( buf, size, "video %08x", (unsigned int)v->guidFormat.Data1 );
        else snprintf( buf, size, "video" );
    }
    else if (!memcmp( &mt->major, &wg_ios_mf_audio, sizeof(GUID) ) && mt->u.audio &&
             mt->format_size >= sizeof(WAVEFORMATEX))
    {
        struct audio_info info;
        parse_audio( mt, &info );
        snprintf( buf, size, "audio tag %#x %u Hz %u ch %u bits", info.tag,
                  (unsigned int)info.wfx->nSamplesPerSec, info.wfx->nChannels, info.wfx->wBitsPerSample );
    }
    else snprintf( buf, size, "major %08x", (unsigned int)mt->major.Data1 );
}

static NTSTATUS create( struct wg_transform_create_params *params )
{
    struct audio_info in, out_info;
    struct wma_core_format fmt;
    struct wma_core_output out;
    struct transform *t;
    char a[96], b[96];
    int ret;

    params->transform = 0;
    if (!parse_audio( &params->input_type, &in ) || !is_wma_family( in.tag ) ||
        !in.wfx->nChannels || !in.wfx->nSamplesPerSec || !in.wfx->nBlockAlign ||
        !parse_output( &params->output_type, in.wfx->nSamplesPerSec, &out, &out_info ))
    {
        describe( &params->input_type, a, sizeof(a) );
        describe( &params->output_type, b, sizeof(b) );
        wma_log( "create refused: %s -> %s (only WMA/XMA to 16-bit PCM or float at the input rate)", a, b );
        return STATUS_NOT_SUPPORTED;
    }

    fmt.tag = in.tag;
    fmt.rate = in.wfx->nSamplesPerSec;
    fmt.channels = in.wfx->nChannels;
    fmt.block_align = in.wfx->nBlockAlign;
    fmt.avg_bytes = in.wfx->nAvgBytesPerSec;
    fmt.bits_per_sample = in.wfx->wBitsPerSample;
    fmt.channel_mask = in.mask;
    fmt.priv = in.priv;
    fmt.priv_size = in.priv_size;

    if (!(t = calloc( 1, sizeof(*t) ))) return STATUS_NO_MEMORY;
    if (!(t->out_format = malloc( params->output_type.format_size )))
    {
        free( t );
        return STATUS_NO_MEMORY;
    }
    memcpy( t->out_format, params->output_type.u.format, params->output_type.format_size );
    t->out_size = params->output_type.format_size;
    t->rate = fmt.rate;
    pthread_mutex_init( &t->lock, NULL );

    if ((ret = wma_core_create( &fmt, &out, &t->core )))
    {
        pthread_mutex_destroy( &t->lock );
        free( t->out_format );
        free( t );
        return ret == WMA_CORE_E_NOMEM ? STATUS_NO_MEMORY : STATUS_NOT_SUPPORTED;
    }
    if (!(params->transform = wgh_alloc( WG_HANDLE_TRANSFORM, t )))
    {
        wma_core_destroy( t->core );
        pthread_mutex_destroy( &t->lock );
        free( t->out_format );
        free( t );
        return STATUS_NO_MEMORY;
    }
    return STATUS_SUCCESS;
}

NTSTATUS wg_ios_transform_create( void *args )
{
    return create( args );
}

NTSTATUS wg_ios_transform_destroy( void *args )
{
    wg_transform_t handle = *(wg_transform_t *)args;
    struct transform *t;

    if (!(t = wgh_free( handle, WG_HANDLE_TRANSFORM ))) return STATUS_INVALID_HANDLE;
    wma_core_destroy( t->core );
    pthread_mutex_destroy( &t->lock );
    free( t->out_format );
    free( t );
    return STATUS_SUCCESS;
}

static NTSTATUS get_output_type( wg_transform_t handle, GUID *major, UINT32 *format_size, void *format )
{
    struct transform *t;
    NTSTATUS status = STATUS_SUCCESS;

    if (!(t = wgh_get( handle, WG_HANDLE_TRANSFORM ))) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &t->lock );
    *major = wg_ios_mf_audio;
    if (*format_size < t->out_size) status = STATUS_BUFFER_TOO_SMALL;
    else if (!format) status = STATUS_INVALID_PARAMETER;
    else memcpy( format, t->out_format, t->out_size );
    *format_size = t->out_size;
    pthread_mutex_unlock( &t->lock );
    return status;
}

NTSTATUS wg_ios_transform_get_output_type( void *args )
{
    struct wg_transform_get_output_type_params *params = args;
    return get_output_type( params->transform, &params->media_type.major, &params->media_type.format_size,
                            params->media_type.u.format );
}

static NTSTATUS set_output_type( wg_transform_t handle, const struct wg_media_type *mt )
{
    struct wma_core_output out;
    struct audio_info info;
    struct transform *t;
    WAVEFORMATEX *copy;
    char a[96];

    if (!(t = wgh_get( handle, WG_HANDLE_TRANSFORM ))) return STATUS_INVALID_HANDLE;
    if (!parse_output( mt, t->rate, &out, &info ))
    {
        describe( mt, a, sizeof(a) );
        wma_log( "set output refused: %s (16-bit PCM or float at %u Hz only)", a, t->rate );
        return STATUS_NOT_SUPPORTED;
    }
    if (!(copy = malloc( mt->format_size ))) return STATUS_NO_MEMORY;
    memcpy( copy, mt->u.format, mt->format_size );

    pthread_mutex_lock( &t->lock );
    free( t->out_format );
    t->out_format = copy;
    t->out_size = mt->format_size;
    wma_core_set_output( t->core, &out );
    pthread_mutex_unlock( &t->lock );
    return STATUS_SUCCESS;
}

NTSTATUS wg_ios_transform_set_output_type( void *args )
{
    struct wg_transform_set_output_type_params *params = args;
    return set_output_type( params->transform, &params->media_type );
}

/* rule 10; `data` is the sample's bytes as a host pointer */
static NTSTATUS push_data( wg_transform_t handle, struct wg_sample *sample, const BYTE *data, HRESULT *result )
{
    struct transform *t;
    int ret;

    if (!(t = wgh_get( handle, WG_HANDLE_TRANSFORM ))) return STATUS_INVALID_HANDLE;
    if (!sample) return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock( &t->lock );
    if (wma_core_pending( t->core ))
    {
        /* input and output alternate: read first */
        pthread_mutex_unlock( &t->lock );
        *result = MF_E_NOTACCEPTING;
        return STATUS_SUCCESS;
    }
    if (sample->size && !data)
    {
        pthread_mutex_unlock( &t->lock );
        return STATUS_INVALID_PARAMETER;
    }
    ret = wma_core_push( t->core, data, sample->size, !!(sample->flags & WG_SAMPLE_FLAG_HAS_PTS),
                         sample->pts, !!(sample->flags & WG_SAMPLE_FLAG_DISCONTINUITY) );
    pthread_mutex_unlock( &t->lock );
    if (ret == WMA_CORE_E_NOMEM) return STATUS_NO_MEMORY;
    *result = S_OK;
    return STATUS_SUCCESS;
}

NTSTATUS wg_ios_transform_push_data( void *args )
{
    struct wg_transform_push_data_params *params = args;
    struct wg_sample *sample = params->sample;

    return push_data( params->transform, sample, sample ? (const BYTE *)(UINT_PTR)sample->data : NULL,
                      &params->result );
}

/* rule 11 */
static NTSTATUS read_data( wg_transform_t handle, struct wg_sample *sample, BYTE *data, HRESULT *result )
{
    struct wma_core_read_info info;
    struct transform *t;
    UINT32 flags;

    if (!(t = wgh_get( handle, WG_HANDLE_TRANSFORM ))) return STATUS_INVALID_HANDLE;
    if (!sample) return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock( &t->lock );
    /* no PCM, or no room for one whole frame (Clarification 2): the PCM stays queued */
    if (!wma_core_pending( t->core ) || sample->max_size < wma_core_frame_bytes( t->core ))
    {
        pthread_mutex_unlock( &t->lock );
        sample->size = 0;
        *result = MF_E_TRANSFORM_NEED_MORE_INPUT;
        return STATUS_SUCCESS;
    }
    if (sample->max_size && !data)
    {
        pthread_mutex_unlock( &t->lock );
        return STATUS_INVALID_PARAMETER;
    }
    wma_core_read( t->core, data, sample->max_size, &info );
    pthread_mutex_unlock( &t->lock );

    flags = WG_SAMPLE_FLAG_SYNC_POINT | WG_SAMPLE_FLAG_HAS_DURATION;
    if (info.has_pts) flags |= WG_SAMPLE_FLAG_HAS_PTS;
    if (info.discontinuity) flags |= WG_SAMPLE_FLAG_DISCONTINUITY;
    if (info.incomplete) flags |= WG_SAMPLE_FLAG_INCOMPLETE;
    sample->flags = flags;
    sample->size = info.bytes;
    sample->pts = info.pts;
    sample->duration = info.duration;
    *result = S_OK;
    return STATUS_SUCCESS;
}

NTSTATUS wg_ios_transform_read_data( void *args )
{
    struct wg_transform_read_data_params *params = args;
    struct wg_sample *sample = params->sample;

    return read_data( params->transform, sample, sample ? (BYTE *)(UINT_PTR)sample->data : NULL,
                      &params->result );
}

NTSTATUS wg_ios_transform_get_status( void *args )
{
    struct wg_transform_get_status_params *params = args;
    struct transform *t;

    if (!(t = wgh_get( params->transform, WG_HANDLE_TRANSFORM ))) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &t->lock );
    params->accepts_input = !wma_core_pending( t->core );
    pthread_mutex_unlock( &t->lock );
    return STATUS_SUCCESS;
}

NTSTATUS wg_ios_transform_drain( void *args )
{
    struct transform *t;

    if (!(t = wgh_get( *(wg_transform_t *)args, WG_HANDLE_TRANSFORM ))) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &t->lock );
    wma_core_drain( t->core );
    pthread_mutex_unlock( &t->lock );
    return STATUS_SUCCESS;
}

NTSTATUS wg_ios_transform_flush( void *args )
{
    struct transform *t;

    if (!(t = wgh_get( *(wg_transform_t *)args, WG_HANDLE_TRANSFORM ))) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &t->lock );
    wma_core_flush( t->core );
    pthread_mutex_unlock( &t->lock );
    return STATUS_SUCCESS;
}

NTSTATUS wg_ios_transform_notify_qos( void *args )
{
    struct wg_transform_notify_qos_params *params = args;

    if (!wgh_get( params->transform, WG_HANDLE_TRANSFORM )) return STATUS_INVALID_HANDLE;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- 32-bit callers (A4) */

struct wg_transform_create_params32
{
    wg_transform_t transform;
    struct wg_media_type32 input_type;
    struct wg_media_type32 output_type;
    struct wg_transform_attrs attrs;
};
_Static_assert( sizeof(struct wg_transform_attrs) == 24, "attrs" );
_Static_assert( offsetof(struct wg_transform_create_params32, input_type) == 8, "create32" );
_Static_assert( offsetof(struct wg_transform_create_params32, output_type) == 32, "create32" );
_Static_assert( offsetof(struct wg_transform_create_params32, attrs) == 56, "create32" );
_Static_assert( sizeof(struct wg_transform_create_params32) == 80, "create32" );

struct wg_transform_media_type_params32
{
    wg_transform_t transform;
    struct wg_media_type32 media_type;
};
_Static_assert( offsetof(struct wg_transform_media_type_params32, media_type) == 8, "media_type32" );
_Static_assert( sizeof(struct wg_transform_media_type_params32) == 32, "media_type32" );

struct wg_transform_data_params32
{
    wg_transform_t transform;
    PTR32 sample;
    HRESULT result;
};
_Static_assert( offsetof(struct wg_transform_data_params32, sample) == 8, "data32" );
_Static_assert( offsetof(struct wg_transform_data_params32, result) == 12, "data32" );
_Static_assert( sizeof(struct wg_transform_data_params32) == 16, "data32" );

static void media_type_from32( struct wg_media_type *mt, const struct wg_media_type32 *mt32 )
{
    mt->major = mt32->major;
    mt->format_size = mt32->format_size;
    mt->u.format = iom_guest_ptr( mt32->format );
}

NTSTATUS wg_ios_wow64_transform_create( void *args )
{
    struct wg_transform_create_params32 *params32 = args;
    struct wg_transform_create_params params;
    NTSTATUS status;

    memset( &params, 0, sizeof(params) );
    media_type_from32( &params.input_type, &params32->input_type );
    media_type_from32( &params.output_type, &params32->output_type );
    params.attrs = params32->attrs;
    status = create( &params );
    params32->transform = params.transform;
    return status;
}

NTSTATUS wg_ios_wow64_transform_get_output_type( void *args )
{
    struct wg_transform_media_type_params32 *params32 = args;

    return get_output_type( params32->transform, &params32->media_type.major,
                            &params32->media_type.format_size, iom_guest_ptr( params32->media_type.format ) );
}

NTSTATUS wg_ios_wow64_transform_set_output_type( void *args )
{
    struct wg_transform_media_type_params32 *params32 = args;
    struct wg_media_type mt;

    media_type_from32( &mt, &params32->media_type );
    return set_output_type( params32->transform, &mt );
}

/* struct wg_sample has the same layout for 32-bit callers; its data field holds a guest address */
static BYTE *sample_data32( const struct wg_sample *sample )
{
    return sample ? iom_guest_ptr( (UINT32)sample->data ) : NULL;
}

NTSTATUS wg_ios_wow64_transform_push_data( void *args )
{
    struct wg_transform_data_params32 *params32 = args;
    struct wg_sample *sample = iom_guest_ptr( params32->sample );
    HRESULT result = S_OK;
    NTSTATUS status;

    status = push_data( params32->transform, sample, sample_data32( sample ), &result );
    params32->result = result;
    return status;
}

NTSTATUS wg_ios_wow64_transform_read_data( void *args )
{
    struct wg_transform_data_params32 *params32 = args;
    struct wg_sample *sample = iom_guest_ptr( params32->sample );
    HRESULT result = S_OK;
    NTSTATUS status;

    status = read_data( params32->transform, sample, sample_data32( sample ), &result );
    params32->result = result;
    return status;
}
