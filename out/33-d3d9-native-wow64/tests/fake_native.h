/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 J3: the fakes the dispatch runs against on Linux: the app's Section A, Wine's window
 * base and log, session N's allocator, and a small native frontend (fake_native.c). */

#ifndef FAKE_NATIVE_H
#define FAKE_NATIVE_H

#include "d3d9w_dispatch.h"

/* ---- the guest window (a 4 GB-aligned mapping) ---- */
#define G_STACK     0x00100000u    /* 64 KB, the i386 stack */
#define G_DATA      0x00200000u    /* 1 MB, what the "program" passes */
#define G_REFUSED   0x00300000u    /* one page ios_wow_native_read/_write refuse (J3) */
#define G_ARENA     0x01000000u    /* 64 MB, ios_wow_window_alloc's memory */

extern uint8_t *fake_window;                 /* host address of guest 0 */
void fake_window_init(void);
void *gptr(uint32_t g);                      /* host address of guest g */
uint32_t galloc(uint32_t n);                 /* fresh guest memory in G_DATA (zeroed) */
void fake_set_base_override(unsigned long base);   /* another "process" (0: the real one) */

extern int fake_reads, fake_writes;          /* ios_wow_native_read/_write calls */
extern ios_wow_native_fn fake_registered;    /* what ios_wow_native_register got */
extern int fake_log_lines;
extern char fake_last_log[1024];

/* ---- the fake native frontend ---- */
struct fake_calls
{
    int set_sampler; uint32_t sampler_args[3];
    int set_vs_constf; UINT constf_start, constf_count; float constf_first[4], constf_last[4];
    int clear; DWORD clear_count; D3DRECT clear_rects[4]; float clear_z;
    int create_vs; UINT vs_tokens;
    int draw_up; uint8_t draw_up_bytes[256]; size_t draw_up_len;
    int draw_iup; uint16_t iup_indices[16]; uint8_t iup_vertices[256];
    int present; RECT present_src; int present_has_rgn; DWORD rgn_count; RECT rgn_rects[4];
    int get_raster;
    int set_texture; void *set_texture_native;
    int reset; HWND reset_window;
    int create_device; HWND create_device_window; UINT create_device_bb_count;
    int set_private;
    int residency; UINT residency_count; void *residency_first;
    int unlocks;
    int get_data;
};
extern struct fake_calls fc;

/* objects that exist right now (created minus destroyed), and each kind's destroy count */
extern int fake_live_objects;
extern int fake_process_gone_calls;   /* d3d9w_native_process_gone (clarification 13) */
extern int fake_textures_destroyed;

/* the D3D9 object's GetNPatchMode answer and what a surface lock hands out */
extern float fake_npatch;
extern int fake_lock_outside;                /* LockRect returns memory outside the window */
extern int fake_query_ready;                 /* Query::GetData: S_OK (1) or S_FALSE (0) */

#endif
