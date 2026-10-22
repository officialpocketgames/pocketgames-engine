// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B: winegstreamer's unix side on iOS, Wine-side declarations. Included only by the
// files that include Wine headers (wg_ios_main.c, wg_transform_ios.c, wg_parser_ios.c).

#ifndef WG_IOS_H
#define WG_IOS_H

#include <stdint.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"
#include "mferror.h"
#include "unixlib.h"     /* dlls/winegstreamer/unixlib.h (upstream, unchanged) */

typedef UINT32 PTR32;

/* 32-bit layout of struct wg_media_type: the format pointer is 4 bytes (B1) */
struct wg_media_type32
{
    GUID major;
    UINT32 format_size;
    PTR32 format;
};
_Static_assert( sizeof(struct wg_media_type32) == 24, "wg_media_type32" );
_Static_assert( sizeof(struct wg_media_type) == 32, "wg_media_type (64-bit)" );
_Static_assert( sizeof(struct wg_format) == 120, "wg_format" );
_Static_assert( sizeof(struct wg_parser_buffer) == 32, "wg_parser_buffer" );
_Static_assert( sizeof(struct wg_sample) == 48, "wg_sample" );

/* the GUIDs this side compares (by value; no initguid linkage needed) */
extern const GUID wg_ios_mf_audio;   /* MFMediaType_Audio */
extern const GUID wg_ios_mf_video;   /* MFMediaType_Video */

/* wg_transform_ios.c: the WMA decoder (B2) */
NTSTATUS wg_ios_transform_create( void *args );
NTSTATUS wg_ios_transform_destroy( void *args );
NTSTATUS wg_ios_transform_get_output_type( void *args );
NTSTATUS wg_ios_transform_set_output_type( void *args );
NTSTATUS wg_ios_transform_push_data( void *args );
NTSTATUS wg_ios_transform_read_data( void *args );
NTSTATUS wg_ios_transform_get_status( void *args );
NTSTATUS wg_ios_transform_drain( void *args );
NTSTATUS wg_ios_transform_flush( void *args );
NTSTATUS wg_ios_transform_notify_qos( void *args );
NTSTATUS wg_ios_wow64_transform_create( void *args );
NTSTATUS wg_ios_wow64_transform_get_output_type( void *args );
NTSTATUS wg_ios_wow64_transform_set_output_type( void *args );
NTSTATUS wg_ios_wow64_transform_push_data( void *args );
NTSTATUS wg_ios_wow64_transform_read_data( void *args );

/* wg_parser_ios.c: the media parser (B3) */
NTSTATUS wg_ios_parser_create( void *args );
NTSTATUS wg_ios_parser_destroy( void *args );
NTSTATUS wg_ios_parser_connect( void *args );
NTSTATUS wg_ios_parser_disconnect( void *args );
NTSTATUS wg_ios_parser_get_next_read_offset( void *args );
NTSTATUS wg_ios_parser_push_data( void *args );
NTSTATUS wg_ios_parser_get_stream_count( void *args );
NTSTATUS wg_ios_parser_get_stream( void *args );
NTSTATUS wg_ios_parser_stream_get_current_format( void *args );
NTSTATUS wg_ios_parser_stream_get_codec_format( void *args );
NTSTATUS wg_ios_parser_stream_enable( void *args );
NTSTATUS wg_ios_parser_stream_disable( void *args );
NTSTATUS wg_ios_parser_stream_get_buffer( void *args );
NTSTATUS wg_ios_parser_stream_copy_buffer( void *args );
NTSTATUS wg_ios_parser_stream_release_buffer( void *args );
NTSTATUS wg_ios_parser_stream_notify_qos( void *args );
NTSTATUS wg_ios_parser_stream_get_duration( void *args );
NTSTATUS wg_ios_parser_stream_get_tag( void *args );
NTSTATUS wg_ios_parser_stream_seek( void *args );
NTSTATUS wg_ios_wow64_parser_connect( void *args );
NTSTATUS wg_ios_wow64_parser_push_data( void *args );
NTSTATUS wg_ios_wow64_parser_stream_get_current_format( void *args );
NTSTATUS wg_ios_wow64_parser_stream_get_codec_format( void *args );
NTSTATUS wg_ios_wow64_parser_stream_enable( void *args );
NTSTATUS wg_ios_wow64_parser_stream_get_buffer( void *args );
NTSTATUS wg_ios_wow64_parser_stream_copy_buffer( void *args );
NTSTATUS wg_ios_wow64_parser_stream_get_tag( void *args );

#endif /* WG_IOS_H */
