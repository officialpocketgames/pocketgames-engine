// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// iOS-only parts of winemetal's unix side (spec 05). Objective-C with manual
// reference counting, like the rest of the unix side.

#include "ios_support.h"
#include "app_active.h" /* spec 05 clarification 54 */
#define WINEMETAL_API
#include "../winemetal.h"
#include "ios_wow64.h"
#include "ios_wow64_objects.h"
#include "winemetal_proc_objects.h" /* spec 05 item 89 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/proc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

/* ---- common ------------------------------------------------------------- */

bool
dxmt_ios_switch(const char *name, bool default_on, _Atomic int *cache) {
  int v = atomic_load_explicit(cache, memory_order_relaxed);
  if (v < 0) {
    const char *s = getenv(name);
    if (!s || !*s)
      v = default_on ? 1 : 0;
    else
      v = strcmp(s, "0") != 0;
    atomic_store_explicit(cache, v, memory_order_relaxed);
  }
  return v != 0;
}

/* Spec 06 A. The cache holds 0 (off for everyone), 1 (on for everyone) or
   2 (unset: decided by the caller's bitness). */
bool
dxmt_ios_switch_for_caller(const char *name, _Atomic int *cache) {
  int v = atomic_load_explicit(cache, memory_order_relaxed);
  if (v < 0) {
    const char *s = getenv(name);
    if (!s || !*s)
      v = 2;
    else
      v = strcmp(s, "0") != 0;
    atomic_store_explicit(cache, v, memory_order_relaxed);
  }
  if (v == 2)
    return ios_wow_base() != 0;
  return v != 0;
}

void
dxmt_ios_log_rare(_Atomic uint64_t *counter, const char *fmt, ...) {
  uint64_t n = atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
  if (n % 256 != 0)
    return;
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  fprintf(stderr, "dxmt: %s (occurrence %llu)\n", line, (unsigned long long)(n + 1));
}

/* ---- Part A: devices ---------------------------------------------------- */

NSArray *
dxmt_ios_copy_all_devices(void) {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device)
    return [[NSArray alloc] init];
  NSArray *array = [[NSArray alloc] initWithObjects:device, nil];
  [device release];
  return array;
}

/* ---- Part B: resource options, upload pitch ----------------------------- */

/* The storage mode is the two-bit field at MTLResourceStorageModeShift (4);
   Managed is 1 there. MTLStorageModeManaged itself can't be named on iOS. */
#define DXMT_STORAGE_MASK (0x3ul << 4)
#define DXMT_STORAGE_MANAGED (0x1ul << 4)

MTLResourceOptions
dxmt_ios_storage_options(MTLResourceOptions options) {
  if ((options & DXMT_STORAGE_MASK) == DXMT_STORAGE_MANAGED)
    options = (options & ~DXMT_STORAGE_MASK) | MTLResourceStorageModeShared;
  return options;
}

MTLResourceOptions
dxmt_ios_buffer_options(MTLResourceOptions options) {
  static _Atomic int keep_write_combined = -1;
  options = dxmt_ios_storage_options(options);
  if (!dxmt_ios_switch("DXMT_WRITE_COMBINED", false, &keep_write_combined))
    options &= ~(MTLResourceOptions)MTLResourceCPUCacheModeWriteCombined;
  return options;
}

uint32_t
dxmt_ios_bytes_per_pixel(MTLPixelFormat format, MTLBlitOption option) {
  switch (format) {
  case MTLPixelFormatA8Unorm:
  case MTLPixelFormatR8Unorm:
  case MTLPixelFormatR8Unorm_sRGB:
  case MTLPixelFormatR8Snorm:
  case MTLPixelFormatR8Uint:
  case MTLPixelFormatR8Sint:
  case MTLPixelFormatStencil8:
  case MTLPixelFormatX32_Stencil8:
    return 1;
  case MTLPixelFormatR16Unorm:
  case MTLPixelFormatR16Snorm:
  case MTLPixelFormatR16Uint:
  case MTLPixelFormatR16Sint:
  case MTLPixelFormatR16Float:
  case MTLPixelFormatRG8Unorm:
  case MTLPixelFormatRG8Unorm_sRGB:
  case MTLPixelFormatRG8Snorm:
  case MTLPixelFormatRG8Uint:
  case MTLPixelFormatRG8Sint:
  case MTLPixelFormatB5G6R5Unorm:
  case MTLPixelFormatA1BGR5Unorm:
  case MTLPixelFormatABGR4Unorm:
  case MTLPixelFormatBGR5A1Unorm:
  case MTLPixelFormatDepth16Unorm:
    return 2;
  case MTLPixelFormatR32Uint:
  case MTLPixelFormatR32Sint:
  case MTLPixelFormatR32Float:
  case MTLPixelFormatRG16Unorm:
  case MTLPixelFormatRG16Snorm:
  case MTLPixelFormatRG16Uint:
  case MTLPixelFormatRG16Sint:
  case MTLPixelFormatRG16Float:
  case MTLPixelFormatRGBA8Unorm:
  case MTLPixelFormatRGBA8Unorm_sRGB:
  case MTLPixelFormatRGBA8Snorm:
  case MTLPixelFormatRGBA8Uint:
  case MTLPixelFormatRGBA8Sint:
  case MTLPixelFormatBGRA8Unorm:
  case MTLPixelFormatBGRA8Unorm_sRGB:
  case MTLPixelFormatRGB10A2Unorm:
  case MTLPixelFormatRGB10A2Uint:
  case MTLPixelFormatRG11B10Float:
  case MTLPixelFormatRGB9E5Float:
  case MTLPixelFormatBGR10A2Unorm:
  case MTLPixelFormatBGR10_XR:
  case MTLPixelFormatBGR10_XR_sRGB:
  case MTLPixelFormatDepth32Float:
    return 4;
  case MTLPixelFormatRG32Uint:
  case MTLPixelFormatRG32Sint:
  case MTLPixelFormatRG32Float:
  case MTLPixelFormatRGBA16Unorm:
  case MTLPixelFormatRGBA16Snorm:
  case MTLPixelFormatRGBA16Uint:
  case MTLPixelFormatRGBA16Sint:
  case MTLPixelFormatRGBA16Float:
  case MTLPixelFormatBGRA10_XR:
  case MTLPixelFormatBGRA10_XR_sRGB:
    return 8;
  case MTLPixelFormatRGBA32Uint:
  case MTLPixelFormatRGBA32Sint:
  case MTLPixelFormatRGBA32Float:
    return 16;
  case MTLPixelFormatDepth32Float_Stencil8:
    if (option & MTLBlitOptionDepthFromDepthStencil)
      return 4;
    if (option & MTLBlitOptionStencilFromDepthStencil)
      return 1;
    return 0;
  default:
    return 0; /* compressed, packed 4:2:2 or unknown: no check */
  }
}

bool
dxmt_ios_upload_pitch_ok(
    id<MTLTexture> texture, MTLBlitOption option, uint64_t width, uint64_t bytes_per_row, const char *what
) {
  static _Atomic uint64_t skipped = 0;
  if (!texture)
    return true;
  if (bytes_per_row == 0 &&
      (texture.textureType == MTLTextureType1D || texture.textureType == MTLTextureType1DArray))
    return true; /* 1D uploads may leave the pitch to Metal */
  uint32_t bpp = dxmt_ios_bytes_per_pixel(texture.pixelFormat, option);
  if (!bpp || bytes_per_row >= width * bpp)
    return true;
  dxmt_ios_log_rare(
      &skipped, "skipped %s: row pitch %llu < width %llu x %u bytes (Metal format %lu)", what,
      (unsigned long long)bytes_per_row, (unsigned long long)width, bpp, (unsigned long)texture.pixelFormat
  );
  return false;
}

/* ---- Part C: physical format of BC textures ------------------------------ */

bool
dxmt_ios_bc_supported(void) {
  /* -1 unknown, 0 no, 1 yes */
  static _Atomic int supported = -1;
  int v = atomic_load_explicit(&supported, memory_order_acquire);
  if (v < 0) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    v = device && [device supportsBCTextureCompression] ? 1 : 0;
    [device release];
    atomic_store_explicit(&supported, v, memory_order_release);
    fprintf(stderr, "dxmt: BC texture compression %s\n", v ? "supported" : "not supported, BC textures are decoded");
  }
  return v != 0;
}

MTLPixelFormat
dxmt_ios_physical_format(MTLPixelFormat format) {
  switch (format) {
  case MTLPixelFormatBC1_RGBA:
  case MTLPixelFormatBC2_RGBA:
  case MTLPixelFormatBC3_RGBA:
  case MTLPixelFormatBC7_RGBAUnorm:
    return dxmt_ios_bc_supported() ? format : MTLPixelFormatRGBA8Unorm;
  case MTLPixelFormatBC1_RGBA_sRGB:
  case MTLPixelFormatBC2_RGBA_sRGB:
  case MTLPixelFormatBC3_RGBA_sRGB:
  case MTLPixelFormatBC7_RGBAUnorm_sRGB:
    return dxmt_ios_bc_supported() ? format : MTLPixelFormatRGBA8Unorm_sRGB;
  case MTLPixelFormatBC4_RUnorm:
    return dxmt_ios_bc_supported() ? format : MTLPixelFormatR8Unorm;
  case MTLPixelFormatBC4_RSnorm:
    return dxmt_ios_bc_supported() ? format : MTLPixelFormatR8Snorm;
  case MTLPixelFormatBC5_RGUnorm:
    return dxmt_ios_bc_supported() ? format : MTLPixelFormatRG8Unorm;
  case MTLPixelFormatBC5_RGSnorm:
    return dxmt_ios_bc_supported() ? format : MTLPixelFormatRG8Snorm;
  case MTLPixelFormatBC6H_RGBFloat:
  case MTLPixelFormatBC6H_RGBUfloat:
    return dxmt_ios_bc_supported() ? format : MTLPixelFormatRGBA16Float;
  default:
    return format;
  }
}

/* ---- Part D1: memory cap, video memory budget ---------------------------- */

#define MB (1024ull * 1024ull)

/* Spec 00 Part I: the engine reads no settings file, so these are always the
   defaults: 1536 MB kept back from hw.memsize, the computed budget, and 1536 MB
   of headroom before the budget is trimmed. */
#define RAM_RESERVE_MB 1536
#define VRAM_TRIM_MB 1536

uint64_t
dxmt_ios_footprint(void) {
  task_vm_info_data_t info;
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
    return 0;
  return info.phys_footprint;
}

uint64_t
dxmt_ios_available_memory(void) {
  return (uint64_t)os_proc_available_memory();
}

static uint64_t
memory_cap(void) {
  static _Atomic uint64_t cap = UINT64_MAX; /* UINT64_MAX = not worked out yet */
  uint64_t c = atomic_load_explicit(&cap, memory_order_relaxed);
  if (c != UINT64_MAX)
    return c;
  uint64_t memsize = 0;
  size_t size = sizeof(memsize);
  if (sysctlbyname("hw.memsize", &memsize, &size, NULL, 0) != 0)
    memsize = 0;
  if (memsize == 0) {
    c = 0; /* 0 turns the cap off */
  } else {
    uint64_t reserve = (uint64_t)RAM_RESERVE_MB * MB;
    c = memsize > reserve ? memsize - reserve : 0;
  }
  atomic_store_explicit(&cap, c, memory_order_relaxed);
  fprintf(
      stderr, "dxmt: memory cap %llu MB (hw.memsize %llu MB, reserve %d MB)%s\n", (unsigned long long)(c / MB),
      (unsigned long long)(memsize / MB), RAM_RESERVE_MB, c ? "" : ", off"
  );
  return c;
}

uint64_t
dxmt_ios_memory_limit(uint64_t footprint) {
  uint64_t cap = memory_cap();
  uint64_t available = dxmt_ios_available_memory();
  uint64_t process_limit = available ? footprint + available : UINT64_MAX;
  static atomic_bool logged_above = false;
  if (cap && process_limit != UINT64_MAX && process_limit > cap && !atomic_exchange(&logged_above, true))
    fprintf(
        stderr, "dxmt: process memory limit %llu MB is above the cap %llu MB\n",
        (unsigned long long)(process_limit / MB), (unsigned long long)(cap / MB)
    );
  uint64_t limit = process_limit;
  if (cap && cap < limit)
    limit = cap;
  return limit;
}

uint64_t
dxmt_ios_vram_budget_base(uint64_t limit, uint64_t metal_value) {
  const uint64_t reserved = 4096 * MB + 2560 * MB; /* guest system memory + translator/Wine/app */
  uint64_t base = limit > reserved ? limit - reserved : 0;
  if (base < 1024 * MB)
    base = 1024 * MB;
  if (metal_value && base > metal_value)
    base = metal_value;
  return base;
}

uint64_t
dxmt_ios_vram_budget_trimmed(uint64_t base, uint64_t limit, uint64_t footprint, uint64_t trim) {
  uint64_t budget = base;
  if (trim && limit != UINT64_MAX && limit > trim && footprint > limit - trim) {
    uint64_t excess = footprint - (limit - trim);
    budget = base > excess ? base - excess : 0;
  }
  if (budget < 768 * MB)
    budget = 768 * MB;
  if (budget > base)
    budget = base;
  return budget;
}

uint64_t
dxmt_ios_working_set_budget(uint64_t metal_value) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static uint64_t base = 0, last = 0, last_logged = 0, last_time = 0;
  static mach_timebase_info_data_t timebase;

  pthread_mutex_lock(&lock);
  uint64_t now = mach_absolute_time();
  if (!timebase.denom)
    mach_timebase_info(&timebase);
  uint64_t elapsed_ns = (now - last_time) * timebase.numer / timebase.denom;
  if (base && elapsed_ns < 250ull * 1000 * 1000) {
    uint64_t v = last;
    pthread_mutex_unlock(&lock);
    return v;
  }
  last_time = now;

  uint64_t footprint = dxmt_ios_footprint();
  uint64_t limit = dxmt_ios_memory_limit(footprint);
  if (!base) {
    base = dxmt_ios_vram_budget_base(limit, metal_value);
    last_logged = base;
    fprintf(
        stderr, "dxmt: video memory budget %llu MB (limit %llu MB, Metal %llu MB)\n", (unsigned long long)(base / MB),
        (unsigned long long)(limit == UINT64_MAX ? 0 : limit / MB), (unsigned long long)(metal_value / MB)
    );
  }
  uint64_t trim = (uint64_t)VRAM_TRIM_MB * MB;
  last = dxmt_ios_vram_budget_trimmed(base, limit, footprint, trim);
  uint64_t moved = last > last_logged ? last - last_logged : last_logged - last;
  if (moved >= 64 * MB) {
    fprintf(
        stderr, "dxmt: video memory budget now %llu MB (footprint %llu MB)\n", (unsigned long long)(last / MB),
        (unsigned long long)(footprint / MB)
    );
    last_logged = last;
  }
  uint64_t v = last;
  pthread_mutex_unlock(&lock);
  return v;
}

/* ---- Part E: presenting and frame pacing --------------------------------- */

#define DXMT_IOS_EXPORT __attribute__((visibility("default"), used))

bool
dxmt_ios_present_pixels(void) {
  static _Atomic int cache = -1;
  return dxmt_ios_switch_for_caller("DXMT_PRESENT_PIXELS", &cache);
}

static uint64_t
abs_from_ns(uint64_t ns) {
  static mach_timebase_info_data_t timebase;
  if (!timebase.denom)
    mach_timebase_info(&timebase);
  return ns * timebase.denom / timebase.numer;
}

static uint64_t
ns_from_abs(uint64_t ticks) {
  static mach_timebase_info_data_t timebase;
  if (!timebase.denom)
    mach_timebase_info(&timebase);
  return ticks * timebase.numer / timebase.denom;
}

/* Pacing mode: 0 = as fast as the display allows, 1 = 60 fps cap,
   2 = uncapped ("RAW"), 3 = 30 fps cap, 4 = 40 fps cap. */
static _Atomic int g_vsync_mode = 1;
static _Atomic uint64_t g_present_count = 0;
static _Atomic uint64_t g_last_real_acquire = 0; /* mach_absolute_time */
static _Atomic int g_panel_hz = 0, g_intent_hz = 0;

DXMT_IOS_EXPORT void
pg_set_vsync_locked(int mode) {
  if (mode < 0 || mode > 4) {
    fprintf(stderr, "dxmt: ignoring unknown pacing mode %d\n", mode);
    return;
  }
  int old = atomic_exchange(&g_vsync_mode, mode);
  if (old != mode)
    fprintf(stderr, "dxmt: pacing mode %d\n", mode);
}

DXMT_IOS_EXPORT int
pg_get_vsync_locked(void) {
  return atomic_load(&g_vsync_mode);
}

DXMT_IOS_EXPORT void
pg_set_display_max_fps(int panel_hz, int intent_hz) {
  atomic_store(&g_panel_hz, panel_hz);
  atomic_store(&g_intent_hz, intent_hz);
  fprintf(stderr, "dxmt: display max %d Hz, intended %d fps\n", panel_hz, intent_hz);
}

DXMT_IOS_EXPORT uint64_t
pg_get_present_count(void) {
  return atomic_load(&g_present_count);
}

/* spec 35 L1: the iOS GL driver (pgl) counts its presents and RAW skips here too, so the app's
   frame counter counts OpenGL frames */
DXMT_IOS_EXPORT void
pg_present_count_add(uint32_t n) {
  atomic_fetch_add(&g_present_count, n);
}

DXMT_IOS_EXPORT int
pg_dxmt_has_40_cap(void) {
  return 1;
}

/* Spec 34 (changes to spec 05): a present that didn't go through DXMT (the Vulkan layer's,
   spec 34 E6) counts too, so the app's frame-rate overlay sees it. */
DXMT_IOS_EXPORT void
pg_note_external_present(void) {
  atomic_fetch_add(&g_present_count, 1);
}

/* Precise limiter (DXMT_PRESENT_LIMITER=1): hold the encode thread until an
   absolute deadline, advanced by one period from the previous deadline; more
   than one period late restarts from now, and a late frame doesn't sleep. */
static bool
present_limiter_enabled(void) {
  static _Atomic int cache = -1;
  return dxmt_ios_switch("DXMT_PRESENT_LIMITER", false, &cache);
}

static void
present_limiter_wait(double period_seconds) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static uint64_t deadline = 0;
  uint64_t period = abs_from_ns((uint64_t)(period_seconds * 1e9));
  uint64_t now = mach_absolute_time();
  pthread_mutex_lock(&lock);
  uint64_t next = deadline ? deadline + period : now;
  if (!deadline || now > next + period)
    next = now;
  deadline = next;
  pthread_mutex_unlock(&lock);
  if (next > now)
    mach_wait_until(next);
}

void
dxmt_ios_present(id<MTLCommandBuffer> cmdbuf, id<MTLDrawable> drawable) {
  if (!cmdbuf || !drawable)
    return;
  double period = 0;
  switch (atomic_load(&g_vsync_mode)) {
  case 1: period = 1.0 / 60.0; break;
  case 3: period = 1.0 / 30.0; break;
  case 4: period = 1.0 / 40.0; break;
  default: break; /* 0 and 2: plain present */
  }
  if (period > 0 && !present_limiter_enabled()) {
    [cmdbuf presentDrawable:drawable afterMinimumDuration:period];
  } else {
    if (period > 0)
      present_limiter_wait(period);
    [cmdbuf presentDrawable:drawable];
  }
  atomic_fetch_add(&g_present_count, 1);
}

/* ---- Spec 05 clarification 54: no GPU work while the app isn't active ---- */

static void
app_gate_log(const char *line) {
  fprintf(stderr, "%s\n", line);
}

static struct dxmt_app_gate g_app_gate = DXMT_APP_GATE_INIT(app_gate_log);

DXMT_IOS_EXPORT void
pg_dxmt_set_app_active(int active) {
  dxmt_app_gate_set_active(&g_app_gate, active, ns_from_abs(mach_absolute_time()));
}

void
dxmt_ios_hold_while_inactive(void) {
  dxmt_app_gate_hold(&g_app_gate);
}

id<CAMetalDrawable>
dxmt_ios_next_drawable(CAMetalLayer *layer) {
  static _Atomic uint64_t acquires = 0;
  static _Atomic uint64_t slow = 0;
  /* clarification 54: no drawable while inactive; the presenter drops the frame as for a nil
     drawable in RAW mode */
  if (dxmt_app_gate_drop_present(&g_app_gate))
    return nil;
  if (atomic_load(&g_vsync_mode) == 2) {
    /* RAW: no drawable when less than 18 ms passed since the last real acquire;
       the frame counts as presented. */
    uint64_t last = atomic_load(&g_last_real_acquire);
    if (last && ns_from_abs(mach_absolute_time() - last) < 18ull * 1000 * 1000) {
      atomic_fetch_add(&g_present_count, 1);
      return nil;
    }
  }
  uint64_t t0 = mach_absolute_time();
  id<CAMetalDrawable> drawable = [layer nextDrawable];
  uint64_t t1 = mach_absolute_time();
  atomic_store(&g_last_real_acquire, t1);
  double ms = (double)ns_from_abs(t1 - t0) / 1e6;
  uint64_t n = atomic_fetch_add(&acquires, 1);
  if (n < 8)
    fprintf(stderr, "dxmt: nextDrawable #%llu took %.2f ms%s\n", (unsigned long long)(n + 1), ms, drawable ? "" : " (nil)");
  else if (ms > 50.0)
    dxmt_ios_log_rare(&slow, "nextDrawable blocked %.1f ms%s", ms, drawable ? "" : " (nil)");
  return drawable;
}

/* GPU meter: while enabled, every command buffer the unix half commits gets a
   completed handler adding GPUEndTime - max(GPUStartTime, previous end) to the
   busy total when positive, and counting the buffer. */
static atomic_bool g_meter_enabled = false;
static pthread_mutex_t g_meter_lock = PTHREAD_MUTEX_INITIALIZER;
static double g_meter_busy = 0, g_meter_prev_end = 0;
static uint64_t g_meter_cmdbufs = 0;

DXMT_IOS_EXPORT void
pg_gpu_meter_enable(int on) {
  atomic_store(&g_meter_enabled, on != 0);
}

DXMT_IOS_EXPORT double
pg_gpu_meter_busy_seconds(void) {
  pthread_mutex_lock(&g_meter_lock);
  double v = g_meter_busy;
  pthread_mutex_unlock(&g_meter_lock);
  return v;
}

DXMT_IOS_EXPORT uint64_t
pg_gpu_meter_cmdbufs(void) {
  pthread_mutex_lock(&g_meter_lock);
  uint64_t v = g_meter_cmdbufs;
  pthread_mutex_unlock(&g_meter_lock);
  return v;
}

/* spec 05 item 89 (the end of this file) */
static struct wmp_proc *proc_will_commit(void);
static void proc_completed(struct wmp_proc *proc);
static enum wmp_add_result proc_add(uint64_t object, enum wmp_kind kind);
static enum wmp_release_result proc_drop(uint64_t object);

void
dxmt_ios_will_commit(id<MTLCommandBuffer> cmdbuf) {
  if (!cmdbuf)
    return;
  /* clarification 54: a command buffer refused for background execution anyway (at the edge
     of a change) logs the lost line once per period, besides DXMT's device-error line. It is
     named by its label, else by its number among the commits. */
  static _Atomic uint64_t commits = 0;
  uint64_t number = atomic_fetch_add(&commits, 1) + 1;
  /* spec 05 item 89: the calling program's work in flight, which its end waits for */
  struct wmp_proc *proc = proc_will_commit();
  [cmdbuf addCompletedHandler:^(id<MTLCommandBuffer> done) {
    proc_completed(proc);
    if (done.status != MTLCommandBufferStatusError || !done.error)
      return;
    bool background = done.error.code == MTLCommandBufferErrorNotPermitted ||
                      [done.error.localizedDescription rangeOfString:@"BackgroundExecutionNotPermitted"].location !=
                          NSNotFound;
    if (!background)
      return;
    char what[96];
    if (done.label.length)
      snprintf(what, sizeof(what), "%s", done.label.UTF8String);
    else
      snprintf(what, sizeof(what), "command buffer #%llu", (unsigned long long)number);
    dxmt_app_gate_lost(&g_app_gate, what);
  }];
  if (!atomic_load(&g_meter_enabled))
    return;
  [cmdbuf addCompletedHandler:^(id<MTLCommandBuffer> done) {
    double start = done.GPUStartTime, end = done.GPUEndTime;
    pthread_mutex_lock(&g_meter_lock);
    double from = start > g_meter_prev_end ? start : g_meter_prev_end;
    if (end - from > 0)
      g_meter_busy += end - from;
    if (end > g_meter_prev_end)
      g_meter_prev_end = end;
    g_meter_cmdbufs++;
    pthread_mutex_unlock(&g_meter_lock);
  }];
}

/* ---- Spec 06 Part E: Metal objects of finished 32-bit programs ----------- */

/* 65536 records (1.5 MB), allocated on the first 32-bit creation. */
#define DXMT_WOW_TABLE_CAPACITY (1u << 16)

static pthread_mutex_t g_wow_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dxmt_wow_objects g_wow_objects;
static bool g_wow_table_ready = false;
static bool g_wow_full_logged = false;

static void
wow_retain(uint64_t object) {
  [(id)(uintptr_t)object retain];
}

static void
wow_release(uint64_t object) {
  [(id)(uintptr_t)object release];
}

static uint64_t
wow_retain_count(uint64_t object) {
  return (uint64_t)[(id)(uintptr_t)object retainCount];
}

/* Called with g_wow_lock held. */
static bool
wow_table_ready(void) {
  if (!g_wow_table_ready) {
    struct dxmt_wow_record *slots = calloc(DXMT_WOW_TABLE_CAPACITY, sizeof(*slots));
    if (!slots)
      return false;
    dxmt_wow_objects_init(&g_wow_objects, slots, DXMT_WOW_TABLE_CAPACITY, wow_retain, wow_release, wow_retain_count);
    g_wow_table_ready = true;
  }
  return true;
}

void
dxmt_ios_wow_created(uint64_t object) {
  uint64_t base = ios_wow_base();
  if (!object || !base)
    return;
  pthread_mutex_lock(&g_wow_lock);
  if (wow_table_ready() && dxmt_wow_objects_created(&g_wow_objects, object, base) == DXMT_WOW_TABLE_FULL &&
      !g_wow_full_logged) {
    g_wow_full_logged = true;
    fprintf(stderr, "dxmt: 32-bit object table is full (%u records); new objects aren't recorded\n",
            DXMT_WOW_TABLE_CAPACITY);
  }
  pthread_mutex_unlock(&g_wow_lock);
}

void
dxmt_ios_wow_retain(uint64_t object) {
  if (!object)
    return;
  pthread_mutex_lock(&g_wow_lock);
  if (g_wow_table_ready)
    dxmt_wow_objects_retained(&g_wow_objects, object);
  pthread_mutex_unlock(&g_wow_lock);
  proc_add(object, WMP_OBJECT); /* spec 05 item 89 */
  wow_retain(object);
}

void
dxmt_ios_wow_release(uint64_t object) {
  if (!object)
    return;
  pthread_mutex_lock(&g_wow_lock);
  bool last = g_wow_table_ready && dxmt_wow_objects_released(&g_wow_objects, object);
  pthread_mutex_unlock(&g_wow_lock);
  enum wmp_release_result counted = proc_drop(object); /* spec 05 item 89 */
  wow_release(object); /* the program's reference */
  if (last)
    wow_release(object); /* the table's own: the program holds none any more */
  if (counted == WMP_LAST)
    wow_release(object); /* item 89's record's own */
}

DXMT_IOS_EXPORT void
winemetal_release_window(unsigned long base) {
  struct dxmt_wow_record *records = NULL;
  uint32_t n = 0;
  pthread_mutex_lock(&g_wow_lock);
  if (g_wow_table_ready)
    n = dxmt_wow_objects_take_window(&g_wow_objects, base, &records);
  pthread_mutex_unlock(&g_wow_lock);
  if (n) {
    @autoreleasepool {
      dxmt_wow_objects_give_back(&g_wow_objects, records, n);
    }
  }
  free(records);
  fprintf(stderr, "dxmt: released %u Metal objects of the 32-bit program at window %#lx\n", n, base);
}

/* ---- Spec 05 item 89: a program's objects, released when its process ends --- */

/* Wine's iOS ntdll (spec 09 Part B1 and spec 22 C6). Weak, so that a build without them leaves
   item 89 off instead of failing to link. */
extern void *ios_current_peb(void) __attribute__((weak_import));
extern int ios_process_end_hook_add(void (*fn)(const void *peb)) __attribute__((weak_import));

/* at most this long for an ended program's command buffers before its objects go */
#define DXMT_PROC_END_WAIT_NS (2ull * 1000 * 1000 * 1000)

static void
proc_release_view(uint64_t view) {
  dxmt_ios_release_metal_view_on_main(view);
}

static const struct wmp_ops g_proc_ops = {wow_retain, wow_release, wow_retain_count, proc_release_view};
static struct wmp_state g_procs = WMP_STATE_INIT(&g_proc_ops);
static _Thread_local int t_untracked;
static _Atomic int g_proc_switch = -1;
static _Atomic int g_proc_hook = 0; /* 0 not yet, 1 registered, -1 unavailable */

static void dxmt_ios_process_ended(const void *peb);

/* The native Direct3D 9 frontend (spec 33) brackets its unix calls with these: its objects of
   an ended process stay as they are (spec 33 clarification 13), so they are never counted. */
DXMT_IOS_EXPORT void
dxmt_ios_untracked_begin(void) {
  t_untracked++;
}

DXMT_IOS_EXPORT void
dxmt_ios_untracked_end(void) {
  t_untracked--;
}

/* The calling pseudo-process, or 0: item 89 off (DXMT_RELEASE_ENDED=0, or ntdll without the
   calls), an untracked call, or a thread of no pseudo-process. The first call registers the
   process-end hook. */
static uint64_t
proc_owner(void) {
  if (t_untracked || !dxmt_ios_switch("DXMT_RELEASE_ENDED", true, &g_proc_switch))
    return 0;
  int hook = atomic_load_explicit(&g_proc_hook, memory_order_acquire);
  if (hook == 0) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_proc_hook, &expected, 2)) {
      bool ok = ios_current_peb && ios_process_end_hook_add && ios_process_end_hook_add(dxmt_ios_process_ended) == 0;
      if (!ok)
        fprintf(stderr, "dxmt: no process-end hook from ntdll: Metal objects of ended programs are not released\n");
      atomic_store_explicit(&g_proc_hook, ok ? 1 : -1, memory_order_release);
      hook = ok ? 1 : -1;
    } else {
      hook = expected;
    }
  }
  if (hook == -1)
    return 0;
  /* 2: another thread is registering right now; counting meanwhile is harmless */
  return ios_current_peb ? (uint64_t)(uintptr_t)ios_current_peb() : 0;
}

static enum wmp_add_result
proc_add(uint64_t object, enum wmp_kind kind) {
  static atomic_bool full_logged = false;
  uint64_t owner = proc_owner();
  if (!object || !owner)
    return WMP_NOT_ADDED;
  enum wmp_add_result result = wmp_add_ref(&g_procs, object, owner, kind, ios_wow_base());
  if (result == WMP_NOT_ADDED && !atomic_exchange(&full_logged, true))
    fprintf(stderr, "dxmt: no memory for the per-process object table; an object isn't counted\n");
  return result;
}

static enum wmp_release_result
proc_drop(uint64_t object) {
  uint64_t owner = proc_owner();
  return owner ? wmp_release_ref(&g_procs, object, owner) : WMP_UNCOUNTED;
}

static struct wmp_proc *
proc_will_commit(void) {
  return wmp_will_commit(&g_procs, proc_owner());
}

static void
proc_completed(struct wmp_proc *proc) {
  wmp_completed(&g_procs, proc);
}

void
dxmt_ios_proc_created(uint64_t object) {
  proc_add(object, WMP_OBJECT);
}

void
dxmt_ios_proc_retain(uint64_t object) {
  if (!object)
    return;
  proc_add(object, WMP_OBJECT);
  [(id)(uintptr_t)object retain];
}

void
dxmt_ios_proc_release(uint64_t object) {
  if (!object)
    return;
  enum wmp_release_result counted = proc_drop(object);
  [(id)(uintptr_t)object release]; /* the program's */
  if (counted == WMP_LAST)
    [(id)(uintptr_t)object release]; /* the record's own: the program holds none any more */
}

void
dxmt_ios_proc_view_created(uint64_t view) {
  proc_add(view, WMP_VIEW);
}

void
dxmt_ios_proc_view_released(uint64_t view) {
  proc_drop(view);
}

/* allocatedSize where Metal reports it; a texture view, or a texture made on a buffer, shares
   memory counted with its parent */
static uint64_t
proc_bytes(uint64_t object) {
  id o = (id)(uintptr_t)object;
  if (![o respondsToSelector:@selector(allocatedSize)])
    return 0;
  if ([o respondsToSelector:@selector(parentTexture)] &&
      ([(id<MTLTexture>)o parentTexture] || [(id<MTLTexture>)o buffer]))
    return 0;
  return (uint64_t)[(id<MTLResource>)o allocatedSize];
}

static int
wow_record_object_cmp(const void *a, const void *b) {
  uint64_t x = ((const struct dxmt_wow_record *)a)->object, y = ((const struct dxmt_wow_record *)b)->object;
  return x < y ? -1 : (x > y ? 1 : 0);
}

/* ntdll's process-end hook: runs with the ended pseudo-process's PEB after its threads are gone,
   before its memory (and a 32-bit program's window) goes back. */
static void
dxmt_ios_process_ended(const void *peb) {
  uint64_t t0 = mach_absolute_time();
  struct wmp_end end;
  wmp_take_ended(&g_procs, (uint64_t)(uintptr_t)peb, DXMT_PROC_END_WAIT_NS, &end);
  if (!end.known && !end.count) {
    free(end.records);
    return;
  }
  double waited_ms = (double)ns_from_abs(mach_absolute_time() - t0) / 1e6;

  /* A 32-bit program's textures and buffers are also in spec 06 Part E's table, which holds a
     reference of its own: take its records of this window now (winemetal_release_window, which
     ntdll calls next, then finds none). Objects in both give the program's references back once,
     through this ledger, then both tables' own. */
  struct dxmt_wow_record *wow = NULL;
  uint32_t wow_n = 0;
  if (end.wow_base) {
    pthread_mutex_lock(&g_wow_lock);
    if (g_wow_table_ready)
      wow_n = dxmt_wow_objects_take_window(&g_wow_objects, end.wow_base, &wow);
    pthread_mutex_unlock(&g_wow_lock);
    if (wow_n > 1)
      qsort(wow, wow_n, sizeof(*wow), wow_record_object_cmp);
  }
  bool *wow_matched = wow_n ? calloc(wow_n, sizeof(bool)) : NULL;

  uint32_t objects = 0, views = 0;
  uint64_t bytes = 0;
  @autoreleasepool {
    for (uint32_t i = 0; i < end.count; i++) {
      struct wmp_record *r = &end.records[i];
      if (r->kind == WMP_VIEW) {
        views++;
        continue;
      }
      objects++;
      bytes += proc_bytes(r->object);
      if (wow_n && wow_matched) {
        struct dxmt_wow_record key = {r->object, 0, 0};
        struct dxmt_wow_record *hit = bsearch(&key, wow, wow_n, sizeof(*wow), wow_record_object_cmp);
        if (hit) {
          r->extra_own = 1;
          wow_matched[hit - wow] = true;
        }
      }
    }
    wmp_give_back(&g_proc_ops, end.records, end.count);
    for (uint32_t i = 0; i < wow_n; i++) {
      if (wow_matched && wow_matched[i])
        wow_release(wow[i].object); /* the 32-bit table's own; the ledger gave the program's back */
      else
        dxmt_wow_objects_give_back(&g_wow_objects, &wow[i], 1); /* not in the ledger: as before */
    }
  }
  fprintf(stderr,
          "dxmt: the program's process ended: released %u Metal objects (%.1f MB) and %u views, after its GPU work %s "
          "(%.0f ms)\n",
          objects, (double)bytes / (1024.0 * 1024.0), views,
          end.drained ? "completed" : "timed out at 2 s", waited_ms);
  free(wow_matched);
  free(wow);
  free(end.records);
}
