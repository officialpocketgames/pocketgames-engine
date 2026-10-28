// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// iOS-only parts of winemetal's unix side (spec 05). Compiled by build-ios.sh
// with DXMT_IOS=1, included only from winemetal_unix.c and cache.c.

#ifndef DXMT_IOS_SUPPORT_H
#define DXMT_IOS_SUPPORT_H

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

/* ---- common ---- */

/* A switch read once from the environment: unset or empty = default_on,
   "0" = off, anything else = on. `cache` must be a static int initialised to -1. */
bool dxmt_ios_switch(const char *name, bool default_on, _Atomic int *cache);

/* Log a line (to stderr, which the app sends to its log) the first time and
   then every 256th time `counter` is bumped. */
void dxmt_ios_log_rare(_Atomic uint64_t *counter, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- Part A ---- */

/* iOS has one Metal device. Returns a +1 NSArray (empty if none). */
NSArray *dxmt_ios_copy_all_devices(void);

/* ---- Part B ---- */

/* B1: Managed storage becomes Shared. */
MTLResourceOptions dxmt_ios_storage_options(MTLResourceOptions options);

/* B1 + B2: buffers also lose the write-combined CPU cache mode, unless
   DXMT_WRITE_COMBINED=1. */
MTLResourceOptions dxmt_ios_buffer_options(MTLResourceOptions options);

/* B10: bytes per pixel of an uncompressed format (of the plane selected by
   `option` for a combined depth-stencil format); 0 when unknown/compressed. */
uint32_t dxmt_ios_bytes_per_pixel(MTLPixelFormat format, MTLBlitOption option);

/* B10: true if `bytes_per_row` covers `width` texels of the texture's actual
   format (or the format is compressed/unknown). Logs (rarely) and returns
   false otherwise. */
bool dxmt_ios_upload_pitch_ok(
    id<MTLTexture> texture, MTLBlitOption option, uint64_t width, uint64_t bytes_per_row, const char *what
);

/* ---- Part C ---- */

/* BC support of the system default device, worked out once. */
bool dxmt_ios_bc_supported(void);

/* Without BC support, the uncompressed format a BC format is created with
   (BC1/2/3/7 -> RGBA8 [sRGB kept], BC4 -> R8, BC5 -> RG8, BC6H -> RGBA16Float);
   every other format, and every format on a GPU with BC, is returned as is. */
MTLPixelFormat dxmt_ios_physical_format(MTLPixelFormat format);

/* ---- Part D1 ---- */

/* phys_footprint from task_info(TASK_VM_INFO); 0 if unknown. */
uint64_t dxmt_ios_footprint(void);

/* os_proc_available_memory(); 0 if unknown. */
uint64_t dxmt_ios_available_memory(void);

/* min(footprint + os_proc_available_memory(), cap), cap = hw.memsize -
   1536 MB (spec 00 Part I). UINT64_MAX if neither is known. */
uint64_t dxmt_ios_memory_limit(uint64_t footprint);

/* The base budget: limit - 4096 MB - 2560 MB (spec 00 Part I), at least
   1024 MB and at most metal_value (when known). */
uint64_t dxmt_ios_vram_budget_base(uint64_t limit, uint64_t metal_value);

/* The budget lowered while memory is tight: if footprint > limit - trim, the
   excess is subtracted; never below 768 MB or above base. trim 0 = never. */
uint64_t dxmt_ios_vram_budget_trimmed(uint64_t base, uint64_t limit, uint64_t footprint, uint64_t trim);

/* What recommendedMaxWorkingSetSize returns on iOS (re-evaluated at most every
   250 ms; logs moves of 64 MB or more). */
uint64_t dxmt_ios_working_set_budget(uint64_t metal_value);

/* ---- Part E ---- */

/* E2.5: DXMT_PRESENT_PIXELS: report contents scale 1.0. Spec 06 A: decided
   per call (on by default for a 32-bit caller only). */
bool dxmt_ios_present_pixels(void);

/* E3: nextDrawable with pacing (RAW mode may return nil) and timing logs.
   Returns an autoreleased drawable or nil. */
id<CAMetalDrawable> dxmt_ios_next_drawable(CAMetalLayer *layer);

/* E3: present a drawable according to the pacing mode. */
void dxmt_ios_present(id<MTLCommandBuffer> cmdbuf, id<MTLDrawable> drawable);

/* E3: GPU meter hook, called before each commit the unix half does. */
void dxmt_ios_will_commit(id<MTLCommandBuffer> cmdbuf);

/* E3: exported for the app's frame-rate setting and performance overlay
   (names and types fixed by the spec). */
void pg_set_vsync_locked(int mode);
int pg_get_vsync_locked(void);
void pg_set_display_max_fps(int panel_hz, int intent_hz);
uint64_t pg_get_present_count(void);
int pg_dxmt_has_40_cap(void);
void pg_gpu_meter_enable(int on);
double pg_gpu_meter_busy_seconds(void);
uint64_t pg_gpu_meter_cmdbufs(void);

/* ---- Spec 06 (32-bit callers) ---- */

/* A: a switch of the unix half, decided per call because one unix half serves
   64-bit and 32-bit programs: unset or empty = on for a 32-bit caller
   (ios_wow_base() != 0) and off for a 64-bit one; "0" = off for everyone;
   anything else = on for everyone. The environment is read once into `cache`
   (a static int initialised to -1). */
bool dxmt_ios_switch_for_caller(const char *name, _Atomic int *cache);

/* E: the calling 32-bit program created `object` (a texture or buffer); a
   no-op for a 64-bit caller or a null object. */
void dxmt_ios_wow_created(uint64_t object);

/* E: the calling 32-bit program retains / releases `object`. These do the
   retain or release themselves (and adjust the count of recorded objects). */
void dxmt_ios_wow_retain(uint64_t object);
void dxmt_ios_wow_release(uint64_t object);

/* E: Wine calls this (if the symbol exists) when it tears down the window of
   a program that ended. */
void winemetal_release_window(unsigned long base);

#endif
