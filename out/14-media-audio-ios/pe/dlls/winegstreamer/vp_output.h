// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 6 item 7: how the video processor gets its output samples, and the lines it
// logs when that or ProcessOutput fails. Plain C (no Windows headers), so the host tests reach
// it; video_processor.c and color_convert.c supply the allocators and the log.

#ifndef VP_OUTPUT_H
#define VP_OUTPUT_H

#include <stdint.h>

typedef int32_t vpo_hr;   /* HRESULT */

struct vpo_state
{
    int fallback;            /* the D3D allocator failed: system-memory samples from now on */
    int alloc_logged, fallback_logged, output_logged;
};

struct vpo_ops
{
    vpo_hr (*d3d_sample)( void *ctx, void **sample );      /* init the D3D allocator once, allocate */
    vpo_hr (*system_sample)( void *ctx, void **sample );   /* a system-memory sample */
    void (*log)( void *ctx, const char *line );
};

struct vpo_info
{
    int device_manager;      /* a D3D device manager was set */
    int shared;              /* MF_SA_D3D11_SHARED or MF_SA_D3D11_SHARED_WITHOUT_MUTEX */
    unsigned int samples;    /* MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT */
};

/* an output sample for ProcessOutput: from the D3D allocator, or system memory after it failed
 * (the failure and the fallback are logged once each) */
vpo_hr vpo_output_sample( struct vpo_state *s, const struct vpo_ops *ops, void *ctx, const struct vpo_info *info,
                          void **sample );
/* the first failed ProcessOutput result is logged ("need more input" and "stream change" are
 * not failures) */
void vpo_output_result( struct vpo_state *s, const struct vpo_ops *ops, void *ctx, const char *who, vpo_hr hr );
/* a new device manager: try the D3D allocator again */
static inline void vpo_reset( struct vpo_state *s ) { s->fallback = 0; }

#endif /* VP_OUTPUT_H */
