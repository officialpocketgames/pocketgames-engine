// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 6 item 7: the video processor's output samples (pe/dlls/winegstreamer/
// vp_output.c) with a fake D3D allocator that fails: the allocation and fallback lines once,
// system-memory samples (output still produced) from then on, a reset after a new device
// manager, and the first failed ProcessOutput result.

#include <string.h>

#include "check.h"
#include "vp_output.h"

struct fake
{
    int d3d_calls, system_calls;
    vpo_hr d3d_result;
    char lines[8][200];
    int nlines;
    int d3d_sample, system_sample;   /* what the samples point at */
};

static vpo_hr fake_d3d( void *ctx, void **sample )
{
    struct fake *f = ctx;

    f->d3d_calls++;
    if (f->d3d_result < 0) return f->d3d_result;
    *sample = &f->d3d_sample;
    return 0;
}

static vpo_hr fake_system( void *ctx, void **sample )
{
    struct fake *f = ctx;

    f->system_calls++;
    *sample = &f->system_sample;
    return 0;
}

static void fake_log( void *ctx, const char *line )
{
    struct fake *f = ctx;

    if (f->nlines < 8) snprintf( f->lines[f->nlines++], sizeof(f->lines[0]), "%s", line );
}

static const struct vpo_ops ops = { fake_d3d, fake_system, fake_log };

int main( void )
{
    struct vpo_info info = { 1, 1, 6 };
    struct vpo_state s;
    struct fake f;
    void *sample;
    int i;

    /* the D3D allocator works: its samples, nothing logged */
    memset( &s, 0, sizeof(s) );
    memset( &f, 0, sizeof(f) );
    CHECK_INT( vpo_output_sample( &s, &ops, &f, &info, &sample ), 0 );
    CHECK( sample == &f.d3d_sample );
    CHECK_INT( f.nlines, 0 );

    /* it fails (shared textures refused): both lines once, system memory, and it stays so */
    memset( &s, 0, sizeof(s) );
    memset( &f, 0, sizeof(f) );
    f.d3d_result = (vpo_hr)0x80070057;   /* E_INVALIDARG */
    for (i = 0; i < 5; i++)
    {
        CHECK_INT( vpo_output_sample( &s, &ops, &f, &info, &sample ), 0 );
        CHECK( sample == &f.system_sample );   /* output still produced */
    }
    CHECK_INT( f.d3d_calls, 1 );
    CHECK_INT( f.system_calls, 5 );
    CHECK_INT( f.nlines, 2 );
    CHECK( !strcmp( f.lines[0], "[wg-transform] video processor: output sample allocation failed 0x80070057 "
                                "(D3D device manager yes, shared yes, samples 6)" ) );
    CHECK( !strcmp( f.lines[1], "[wg-transform] video processor: D3D output samples failed; using system memory" ) );

    /* a new device manager tries D3D again; the lines aren't repeated for this transform */
    vpo_reset( &s );
    CHECK_INT( vpo_output_sample( &s, &ops, &f, &info, &sample ), 0 );
    CHECK_INT( f.d3d_calls, 2 );
    CHECK( sample == &f.system_sample );
    CHECK_INT( f.nlines, 2 );

    /* another transform logs its own lines */
    memset( &s, 0, sizeof(s) );
    info.device_manager = 1;
    info.shared = 0;
    info.samples = 2;
    CHECK_INT( vpo_output_sample( &s, &ops, &f, &info, &sample ), 0 );
    CHECK_INT( f.nlines, 4 );
    CHECK( !strcmp( f.lines[2], "[wg-transform] video processor: output sample allocation failed 0x80070057 "
                                "(D3D device manager yes, shared no, samples 2)" ) );

    /* ProcessOutput results: "need more input" and "stream change" aren't failures; the first
     * failure is logged once */
    memset( &s, 0, sizeof(s) );
    f.nlines = 0;
    vpo_output_result( &s, &ops, &f, "video processor", 0 );
    vpo_output_result( &s, &ops, &f, "video processor", (vpo_hr)0xc00d6d72 );
    vpo_output_result( &s, &ops, &f, "video processor", (vpo_hr)0xc00d6d61 );
    CHECK_INT( f.nlines, 0 );
    vpo_output_result( &s, &ops, &f, "colour converter", (vpo_hr)0x8007000e );
    vpo_output_result( &s, &ops, &f, "colour converter", (vpo_hr)0x80004005 );
    CHECK_INT( f.nlines, 1 );
    CHECK( !strcmp( f.lines[0], "[wg-transform] colour converter: ProcessOutput failed 0x8007000e" ) );

    return test_report( "test_vp_output" );
}
