// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// iOS-only parts of winemetal's unix side (spec 05). Objective-C with manual
// reference counting, like the rest of the unix side.

#include "ios_support.h"
#define WINEMETAL_API
#include "../winemetal.h"
#include "ios_wow64.h"
#include "ios_wow64_objects.h"
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

DXMT_IOS_EXPORT int
pg_dxmt_has_40_cap(void) {
  return 1;
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

id<CAMetalDrawable>
dxmt_ios_next_drawable(CAMetalLayer *layer) {
  static _Atomic uint64_t acquires = 0;
  static _Atomic uint64_t slow = 0;
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

void
dxmt_ios_will_commit(id<MTLCommandBuffer> cmdbuf) {
  if (!atomic_load(&g_meter_enabled) || !cmdbuf)
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
  wow_retain(object);
}

void
dxmt_ios_wow_release(uint64_t object) {
  if (!object)
    return;
  pthread_mutex_lock(&g_wow_lock);
  bool last = g_wow_table_ready && dxmt_wow_objects_released(&g_wow_objects, object);
  pthread_mutex_unlock(&g_wow_lock);
  wow_release(object); /* the program's reference */
  if (last)
    wow_release(object); /* the table's own: the program holds none any more */
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
