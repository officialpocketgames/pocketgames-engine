// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B1 and A3: winegstreamer's unix call tables on iOS, under the names spec 09's
// loader looks up (winegstreamer_unix_call_funcs / winegstreamer_unix_call_wow64_funcs), and the
// gate that keeps the 64-bit table from 64-bit callers unless PG_WG_64BIT=1.
//
// Both tables follow dlls/winegstreamer/unixlib.h's enum unix_funcs entry for entry: an index
// that drifted would dispatch a call to another function's argument block.

#if 0
#pragma makedep unix
#endif

#include "wg_ios.h"
#include "ios_media_wow.h"
#include "media_log.h"

#define EXPORTED_TABLE __attribute__((used, visibility("default")))

static NTSTATUS wg_ios_init_gstreamer( void *args )
{
    /* nothing to initialise; the PE side refuses to load the DLL unless this succeeds */
    return STATUS_SUCCESS;
}

/* the seven wg_muxer_* entries: callers handle a missing component */
static NTSTATUS wg_ios_muxer_not_implemented( void *args )
{
    return STATUS_NOT_IMPLEMENTED;
}

const unixlib_entry_t winegstreamer_unix_call_funcs[] EXPORTED_TABLE =
{
    [unix_wg_init_gstreamer] = wg_ios_init_gstreamer,

    [unix_wg_parser_create] = wg_ios_parser_create,
    [unix_wg_parser_destroy] = wg_ios_parser_destroy,

    [unix_wg_parser_connect] = wg_ios_parser_connect,
    [unix_wg_parser_disconnect] = wg_ios_parser_disconnect,

    [unix_wg_parser_get_next_read_offset] = wg_ios_parser_get_next_read_offset,
    [unix_wg_parser_push_data] = wg_ios_parser_push_data,

    [unix_wg_parser_get_stream_count] = wg_ios_parser_get_stream_count,
    [unix_wg_parser_get_stream] = wg_ios_parser_get_stream,

    [unix_wg_parser_stream_get_current_format] = wg_ios_parser_stream_get_current_format,
    [unix_wg_parser_stream_get_codec_format] = wg_ios_parser_stream_get_codec_format,
    [unix_wg_parser_stream_enable] = wg_ios_parser_stream_enable,
    [unix_wg_parser_stream_disable] = wg_ios_parser_stream_disable,

    [unix_wg_parser_stream_get_buffer] = wg_ios_parser_stream_get_buffer,
    [unix_wg_parser_stream_copy_buffer] = wg_ios_parser_stream_copy_buffer,
    [unix_wg_parser_stream_release_buffer] = wg_ios_parser_stream_release_buffer,
    [unix_wg_parser_stream_notify_qos] = wg_ios_parser_stream_notify_qos,

    [unix_wg_parser_stream_get_duration] = wg_ios_parser_stream_get_duration,
    [unix_wg_parser_stream_get_tag] = wg_ios_parser_stream_get_tag,
    [unix_wg_parser_stream_seek] = wg_ios_parser_stream_seek,

    [unix_wg_transform_create] = wg_ios_transform_create,
    [unix_wg_transform_destroy] = wg_ios_transform_destroy,
    [unix_wg_transform_get_output_type] = wg_ios_transform_get_output_type,
    [unix_wg_transform_set_output_type] = wg_ios_transform_set_output_type,

    [unix_wg_transform_push_data] = wg_ios_transform_push_data,
    [unix_wg_transform_read_data] = wg_ios_transform_read_data,
    [unix_wg_transform_get_status] = wg_ios_transform_get_status,
    [unix_wg_transform_drain] = wg_ios_transform_drain,
    [unix_wg_transform_flush] = wg_ios_transform_flush,
    [unix_wg_transform_notify_qos] = wg_ios_transform_notify_qos,

    [unix_wg_muxer_create] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_destroy] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_add_stream] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_start] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_push_sample] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_read_data] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_finalize] = wg_ios_muxer_not_implemented,
};
_Static_assert( sizeof(winegstreamer_unix_call_funcs) / sizeof(winegstreamer_unix_call_funcs[0]) == unix_wg_funcs_count,
                "winegstreamer_unix_call_funcs must have one entry per enum unix_funcs value" );

/* 32-bit callers: the entries whose blocks carry pointers have their own thunks (B1, A4); the
 * others take only handles and fixed-width scalars and share the 64-bit entry. */
const unixlib_entry_t winegstreamer_unix_call_wow64_funcs[] EXPORTED_TABLE =
{
    [unix_wg_init_gstreamer] = wg_ios_init_gstreamer,

    [unix_wg_parser_create] = wg_ios_parser_create,
    [unix_wg_parser_destroy] = wg_ios_parser_destroy,

    [unix_wg_parser_connect] = wg_ios_wow64_parser_connect,
    [unix_wg_parser_disconnect] = wg_ios_parser_disconnect,

    [unix_wg_parser_get_next_read_offset] = wg_ios_parser_get_next_read_offset,
    [unix_wg_parser_push_data] = wg_ios_wow64_parser_push_data,

    [unix_wg_parser_get_stream_count] = wg_ios_parser_get_stream_count,
    [unix_wg_parser_get_stream] = wg_ios_parser_get_stream,

    [unix_wg_parser_stream_get_current_format] = wg_ios_wow64_parser_stream_get_current_format,
    [unix_wg_parser_stream_get_codec_format] = wg_ios_wow64_parser_stream_get_codec_format,
    [unix_wg_parser_stream_enable] = wg_ios_wow64_parser_stream_enable,
    [unix_wg_parser_stream_disable] = wg_ios_parser_stream_disable,

    [unix_wg_parser_stream_get_buffer] = wg_ios_wow64_parser_stream_get_buffer,
    [unix_wg_parser_stream_copy_buffer] = wg_ios_wow64_parser_stream_copy_buffer,
    [unix_wg_parser_stream_release_buffer] = wg_ios_parser_stream_release_buffer,
    [unix_wg_parser_stream_notify_qos] = wg_ios_parser_stream_notify_qos,

    [unix_wg_parser_stream_get_duration] = wg_ios_parser_stream_get_duration,
    [unix_wg_parser_stream_get_tag] = wg_ios_wow64_parser_stream_get_tag,
    [unix_wg_parser_stream_seek] = wg_ios_parser_stream_seek,

    [unix_wg_transform_create] = wg_ios_wow64_transform_create,
    [unix_wg_transform_destroy] = wg_ios_transform_destroy,
    [unix_wg_transform_get_output_type] = wg_ios_wow64_transform_get_output_type,
    [unix_wg_transform_set_output_type] = wg_ios_wow64_transform_set_output_type,

    [unix_wg_transform_push_data] = wg_ios_wow64_transform_push_data,
    [unix_wg_transform_read_data] = wg_ios_wow64_transform_read_data,
    [unix_wg_transform_get_status] = wg_ios_transform_get_status,
    [unix_wg_transform_drain] = wg_ios_transform_drain,
    [unix_wg_transform_flush] = wg_ios_transform_flush,
    [unix_wg_transform_notify_qos] = wg_ios_transform_notify_qos,

    [unix_wg_muxer_create] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_destroy] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_add_stream] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_start] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_push_sample] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_read_data] = wg_ios_muxer_not_implemented,
    [unix_wg_muxer_finalize] = wg_ios_muxer_not_implemented,
};
_Static_assert( sizeof(winegstreamer_unix_call_wow64_funcs) / sizeof(winegstreamer_unix_call_wow64_funcs[0]) ==
                unix_wg_funcs_count, "winegstreamer_unix_call_wow64_funcs must have one entry per enum unix_funcs value" );

/* Spec 09's loader calls <name>_unix_lib_init after binding a table and returns its status.
 * A3: a 64-bit caller (no guest window) is treated as having no table unless PG_WG_64BIT=1:
 * no 64-bit winegstreamer.dll is shipped, and this keeps 64-bit behaviour as it was. */
__attribute__((used, visibility("default"))) NTSTATUS winegstreamer_unix_lib_init( void )
{
    static volatile int logged;

    if (ios_wow_base()) return STATUS_SUCCESS;
    if (iom_env_is( "PG_WG_64BIT", "1" )) return STATUS_SUCCESS;
    if (!__atomic_exchange_n( &logged, 1, __ATOMIC_RELAXED ))
        iom_log( "[winegstreamer] ", "64-bit caller: no unix library (set PG_WG_64BIT=1 to enable it)" );
    return STATUS_DLL_NOT_FOUND;
}
