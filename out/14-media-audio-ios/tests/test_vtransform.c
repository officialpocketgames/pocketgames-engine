// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 5 item 6: raw video conversion through the wg_transform entries (64-bit and
// WoW64 tables): the video processor's I420 -> NV12 test conversion is accepted, a size change
// and RGB input are refused, NV12 -> BGRx bottom-up through push and read, and push, read,
// drain and flush follow the one-held-frame rules.

#if 0
#pragma makedep unix
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wg_ios.h"
#include "check.h"
#include "test_stubs.h"

extern const unixlib_entry_t winegstreamer_unix_call_funcs[];
extern const unixlib_entry_t winegstreamer_unix_call_wow64_funcs[];

#define CALL( func, args ) winegstreamer_unix_call_funcs[func]( args )
#define CALL32( func, args ) winegstreamer_unix_call_wow64_funcs[func]( args )

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define SUB_NV12 FOURCC('N','V','1','2')
#define SUB_I420 FOURCC('I','4','2','0')
#define SUB_YUY2 FOURCC('Y','U','Y','2')
#define SUB_RGB32 22

static void video_format( MFVIDEOFORMAT *f, uint32_t subtype, UINT32 w, UINT32 h, int bottom_up )
{
    static const BYTE tail[8] = { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };

    memset( f, 0, sizeof(*f) );
    f->dwSize = sizeof(*f);
    f->videoInfo.dwWidth = w;
    f->videoInfo.dwHeight = h;
    if (bottom_up) f->videoInfo.VideoFlags = MFVideoFlag_BottomUpLinearRep;
    f->guidFormat.Data1 = subtype;
    f->guidFormat.Data3 = 0x0010;
    memcpy( f->guidFormat.Data4, tail, 8 );
}

static NTSTATUS create( MFVIDEOFORMAT *in, MFVIDEOFORMAT *out, wg_transform_t *t )
{
    struct wg_transform_create_params p;
    NTSTATUS status;

    memset( &p, 0, sizeof(p) );
    p.input_type.major = wg_ios_mf_video;
    p.input_type.format_size = sizeof(*in);
    p.input_type.u.video = in;
    p.output_type.major = wg_ios_mf_video;
    p.output_type.format_size = sizeof(*out);
    p.output_type.u.video = out;
    status = CALL( unix_wg_transform_create, &p );
    *t = p.transform;
    return status;
}

static HRESULT push( wg_transform_t t, void *data, UINT32 size, INT64 pts, NTSTATUS *status )
{
    struct wg_transform_push_data_params p;
    struct wg_sample s;

    memset( &s, 0, sizeof(s) );
    s.size = s.max_size = size;
    s.data = (UINT_PTR)data;
    s.pts = pts;
    s.duration = 333333;
    s.flags = WG_SAMPLE_FLAG_HAS_PTS | WG_SAMPLE_FLAG_HAS_DURATION;
    p.transform = t;
    p.sample = &s;
    p.result = E_FAIL;
    *status = CALL( unix_wg_transform_push_data, &p );
    return p.result;
}

static HRESULT read_frame( wg_transform_t t, void *data, UINT32 max, UINT32 stride, struct wg_sample *s, NTSTATUS *status )
{
    struct wg_transform_read_data_params p;

    memset( s, 0, sizeof(*s) );
    s->max_size = max;
    s->stride = stride;
    s->data = (UINT_PTR)data;
    p.transform = t;
    p.sample = s;
    p.result = E_FAIL;
    *status = CALL( unix_wg_transform_read_data, &p );
    return p.result;
}

static void test_create( void )
{
    MFVIDEOFORMAT in, out;
    wg_transform_t t;
    struct wg_transform_get_output_type_params got;
    MFVIDEOFORMAT back;

    test_log_clear();
    /* the video processor's test conversion */
    video_format( &in, SUB_I420, 1920, 1080, 0 );
    video_format( &out, SUB_NV12, 1920, 1080, 0 );
    CHECK_INT( create( &in, &out, &t ), STATUS_SUCCESS );
    CHECK( t != 0 );
    CHECK_INT( test_log_count( "[wg-transform] video convert I420 1920x1080 -> NV12" ), 1 );
    memset( &got, 0, sizeof(got) );
    got.transform = t;
    got.media_type.format_size = sizeof(back);
    got.media_type.u.video = &back;
    CHECK_INT( CALL( unix_wg_transform_get_output_type, &got ), STATUS_SUCCESS );
    CHECK( !memcmp( &got.media_type.major, &wg_ios_mf_video, sizeof(GUID) ) );
    CHECK_INT( back.guidFormat.Data1, SUB_NV12 );
    CHECK_INT( CALL( unix_wg_transform_destroy, &t ), STATUS_SUCCESS );

    /* a size change, RGB input, an unknown subtype */
    video_format( &out, SUB_NV12, 1280, 720, 0 );
    CHECK_INT( create( &in, &out, &t ), STATUS_NOT_SUPPORTED );
    CHECK_INT( test_log_count( "a size change; no scaling" ), 1 );
    video_format( &in, SUB_RGB32, 64, 64, 0 );
    video_format( &out, SUB_NV12, 64, 64, 0 );
    CHECK_INT( create( &in, &out, &t ), STATUS_NOT_SUPPORTED );
    video_format( &in, FOURCC('H','2','6','4'), 64, 64, 0 );
    CHECK_INT( create( &in, &out, &t ), STATUS_NOT_SUPPORTED );
    /* RGB output without the bottom-up flag: the D3D-aware line, once */
    video_format( &in, SUB_NV12, 64, 64, 0 );
    video_format( &out, SUB_RGB32, 64, 64, 0 );
    CHECK_INT( create( &in, &out, &t ), STATUS_SUCCESS );
    CHECK_INT( CALL( unix_wg_transform_destroy, &t ), STATUS_SUCCESS );
    CHECK_INT( create( &in, &out, &t ), STATUS_SUCCESS );
    CHECK_INT( CALL( unix_wg_transform_destroy, &t ), STATUS_SUCCESS );
    CHECK_INT( test_log_count( "D3D-aware output asked; using system memory" ), 1 );
}

/* 16x16 NV12 white over black (rows 0-7 black, 8-15 white) -> BGRx bottom-up */
static void test_push_read( void )
{
    MFVIDEOFORMAT in, out;
    struct wg_transform_get_status_params st;
    struct wg_sample s;
    wg_transform_t t;
    NTSTATUS status;
    uint8_t frame[16 * 16 * 3 / 2], rgb[16 * 16 * 4], wide[32 * 16];
    unsigned int y;

    memset( frame, 16, 16 * 8 );
    memset( frame + 16 * 8, 235, 16 * 8 );
    memset( frame + 16 * 16, 128, 16 * 8 );
    video_format( &in, SUB_NV12, 16, 16, 0 );
    video_format( &out, SUB_RGB32, 16, 16, 1 );
    test_log_clear();
    CHECK_INT( create( &in, &out, &t ), STATUS_SUCCESS );
    CHECK_INT( test_log_count( "-> RGB32 bottom-up" ), 1 );

    /* nothing held: need more input */
    CHECK_INT( read_frame( t, rgb, sizeof(rgb), 0, &s, &status ), MF_E_TRANSFORM_NEED_MORE_INPUT );
    CHECK_INT( status, STATUS_SUCCESS );
    CHECK_INT( s.size, 0 );
    /* one frame in; a second isn't accepted */
    CHECK_INT( push( t, frame, sizeof(frame), 1000, &status ), S_OK );
    CHECK_INT( push( t, frame, sizeof(frame), 2000, &status ), MF_E_NOTACCEPTING );
    st.transform = t;
    CHECK_INT( CALL( unix_wg_transform_get_status, &st ), STATUS_SUCCESS );
    CHECK_INT( st.accepts_input, 0 );
    /* too small: the frame stays */
    read_frame( t, rgb, 100, 0, &s, &status );
    CHECK_INT( status, STATUS_BUFFER_TOO_SMALL );
    CHECK_INT( read_frame( t, rgb, sizeof(rgb), 0, &s, &status ), S_OK );
    CHECK_INT( s.size, sizeof(rgb) );
    CHECK_INT( s.pts, 1000 );
    CHECK_INT( s.duration, 333333 );
    CHECK( (s.flags & (WG_SAMPLE_FLAG_HAS_PTS | WG_SAMPLE_FLAG_HAS_DURATION)) == (WG_SAMPLE_FLAG_HAS_PTS | WG_SAMPLE_FLAG_HAS_DURATION) );
    /* bottom-up: the first rows written are the picture's bottom (white) */
    for (y = 0; y < 16; y++)
    {
        uint8_t want = y < 8 ? 255 : 0;
        CHECK( abs( rgb[y * 64] - want ) <= 2 && abs( rgb[y * 64 + 2] - want ) <= 2 && rgb[y * 64 + 3] == 255 );
    }
    CHECK_INT( CALL( unix_wg_transform_get_status, &st ), STATUS_SUCCESS );
    CHECK_INT( st.accepts_input, 1 );

    /* a 2D output buffer (stride 128 bytes) */
    CHECK_INT( push( t, frame, sizeof(frame), 3000, &status ), S_OK );
    {
        uint8_t *big = calloc( 1, 128 * 16 );
        CHECK_INT( read_frame( t, big, 128 * 16, 128, &s, &status ), S_OK );
        CHECK_INT( s.size, 128 * 16 );
        CHECK( abs( big[15 * 128] - 0 ) <= 2 && abs( big[0] - 255 ) <= 2 && big[64] == 0 );   /* padding untouched */
        free( big );
    }

    /* drain and flush drop the held frame */
    CHECK_INT( push( t, frame, sizeof(frame), 4000, &status ), S_OK );
    CHECK_INT( CALL( unix_wg_transform_drain, &t ), STATUS_SUCCESS );
    CHECK_INT( read_frame( t, rgb, sizeof(rgb), 0, &s, &status ), MF_E_TRANSFORM_NEED_MORE_INPUT );
    CHECK_INT( push( t, frame, sizeof(frame), 5000, &status ), S_OK );
    CHECK_INT( CALL( unix_wg_transform_flush, &t ), STATUS_SUCCESS );
    CHECK_INT( read_frame( t, rgb, sizeof(rgb), 0, &s, &status ), MF_E_TRANSFORM_NEED_MORE_INPUT );

    /* a short input frame is refused */
    push( t, frame, 100, 6000, &status );
    CHECK_INT( status, STATUS_INVALID_PARAMETER );

    /* set_output_type: another listed format at the same size; a size change refused */
    {
        struct wg_transform_set_output_type_params so;
        MFVIDEOFORMAT yuy2;

        video_format( &yuy2, SUB_YUY2, 16, 16, 0 );
        memset( &so, 0, sizeof(so) );
        so.transform = t;
        so.media_type.major = wg_ios_mf_video;
        so.media_type.format_size = sizeof(yuy2);
        so.media_type.u.video = &yuy2;
        CHECK_INT( CALL( unix_wg_transform_set_output_type, &so ), STATUS_SUCCESS );
        CHECK_INT( push( t, frame, sizeof(frame), 7000, &status ), S_OK );
        CHECK_INT( read_frame( t, wide, sizeof(wide), 0, &s, &status ), S_OK );
        CHECK_INT( s.size, 16 * 2 * 16 );
        CHECK( wide[0] == 16 && wide[1] == 128 && wide[15 * 32] == 235 );
        video_format( &yuy2, SUB_YUY2, 32, 16, 0 );
        CHECK_INT( CALL( unix_wg_transform_set_output_type, &so ), STATUS_NOT_SUPPORTED );
    }
    CHECK_INT( CALL( unix_wg_transform_destroy, &t ), STATUS_SUCCESS );
}

static void test_wow64( void )
{
    struct create32 { UINT64 transform; struct wg_media_type32 in, out; struct wg_transform_attrs attrs; } *c;
    struct data32 { UINT64 transform; UINT32 sample; HRESULT result; } *d;
    MFVIDEOFORMAT *in, *out;
    struct wg_sample *s;
    uint8_t *frame, *dst;

    test_enter_window();
    c = test_arena_alloc( sizeof(*c) );
    d = test_arena_alloc( sizeof(*d) );
    in = test_arena_alloc( sizeof(*in) );
    out = test_arena_alloc( sizeof(*out) );
    s = test_arena_alloc( sizeof(*s) );
    frame = test_arena_alloc( 16 * 16 * 3 / 2 );
    dst = test_arena_alloc( 16 * 16 * 3 / 2 );
    video_format( in, SUB_I420, 16, 16, 0 );
    video_format( out, SUB_NV12, 16, 16, 0 );
    memset( frame, 50, 256 );          /* Y */
    memset( frame + 256, 60, 64 );     /* U */
    memset( frame + 320, 70, 64 );     /* V */
    memset( c, 0, sizeof(*c) );
    c->in.major = wg_ios_mf_video;
    c->in.format_size = sizeof(*in);
    c->in.format = test_guest( in );
    c->out.major = wg_ios_mf_video;
    c->out.format_size = sizeof(*out);
    c->out.format = test_guest( out );
    CHECK_INT( CALL32( unix_wg_transform_create, c ), STATUS_SUCCESS );
    CHECK( c->transform != 0 );

    memset( s, 0, sizeof(*s) );
    s->size = s->max_size = 384;
    s->data = test_guest( frame );
    s->pts = 42;
    s->flags = WG_SAMPLE_FLAG_HAS_PTS;
    d->transform = c->transform;
    d->sample = test_guest( s );
    CHECK_INT( CALL32( unix_wg_transform_push_data, d ), STATUS_SUCCESS );
    CHECK_INT( d->result, S_OK );
    CHECK_INT( CALL32( unix_wg_transform_push_data, d ), STATUS_SUCCESS );
    CHECK_INT( d->result, MF_E_NOTACCEPTING );
    memset( s, 0, sizeof(*s) );
    s->max_size = 384;
    s->data = test_guest( dst );
    CHECK_INT( CALL32( unix_wg_transform_read_data, d ), STATUS_SUCCESS );
    CHECK_INT( d->result, S_OK );
    CHECK_INT( s->size, 384 );
    CHECK_INT( s->pts, 42 );
    CHECK( dst[0] == 50 && dst[256] == 60 && dst[257] == 70 && dst[383] == 70 );
    CHECK_INT( CALL32( unix_wg_transform_read_data, d ), STATUS_SUCCESS );
    CHECK_INT( d->result, MF_E_TRANSFORM_NEED_MORE_INPUT );
    CHECK_INT( CALL32( unix_wg_transform_destroy, &c->transform ), STATUS_SUCCESS );
    test_leave_window();
}

int main( void )
{
    test_create();
    test_push_read();
    test_wow64();
    return test_report( "test_vtransform" );
}
