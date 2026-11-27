// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 6 item 7: the video processor's output samples and failure lines (vp_output.h).

#include <stdio.h>

#include "vp_output.h"

#define VPO_FAILED(hr) ((hr) < 0)
#define VPO_NEED_MORE_INPUT ((vpo_hr)0xc00d6d72)   /* MF_E_TRANSFORM_NEED_MORE_INPUT */
#define VPO_STREAM_CHANGE ((vpo_hr)0xc00d6d61)     /* MF_E_TRANSFORM_STREAM_CHANGE */

vpo_hr vpo_output_sample( struct vpo_state *s, const struct vpo_ops *ops, void *ctx, const struct vpo_info *info,
                          void **sample )
{
    char line[200];
    vpo_hr hr;

    *sample = NULL;
    if (!s->fallback)
    {
        if (!VPO_FAILED( hr = ops->d3d_sample( ctx, sample ) )) return hr;
        if (!s->alloc_logged)
        {
            s->alloc_logged = 1;
            snprintf( line, sizeof(line), "[wg-transform] video processor: output sample allocation failed 0x%08x "
                      "(D3D device manager %s, shared %s, samples %u)", (unsigned int)hr,
                      info->device_manager ? "yes" : "no", info->shared ? "yes" : "no", info->samples );
            ops->log( ctx, line );
        }
        s->fallback = 1;
        if (!s->fallback_logged)
        {
            s->fallback_logged = 1;
            ops->log( ctx, "[wg-transform] video processor: D3D output samples failed; using system memory" );
        }
        *sample = NULL;
    }
    return ops->system_sample( ctx, sample );
}

void vpo_output_result( struct vpo_state *s, const struct vpo_ops *ops, void *ctx, const char *who, vpo_hr hr )
{
    char line[160];

    if (!VPO_FAILED( hr ) || hr == VPO_NEED_MORE_INPUT || hr == VPO_STREAM_CHANGE || s->output_logged) return;
    s->output_logged = 1;
    snprintf( line, sizeof(line), "[wg-transform] %s: ProcessOutput failed 0x%08x", who, (unsigned int)hr );
    ops->log( ctx, line );
}
