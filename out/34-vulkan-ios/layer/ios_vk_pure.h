// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part E: the arithmetic and bookkeeping of the iOS Vulkan layer that needs no Vulkan
// driver, no Apple API and no Vulkan header, so the host tests drive it directly
// (tests/test_layer.c). ios_vk_layer.c and ios_vk_release.c call it.

#ifndef IOS_VK_PURE_H
#define IOS_VK_PURE_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define IVK_MB (1024ull * 1024ull)

/* VkResult values the pure code returns (ios_vk_layer.c checks them against the header) */
#define IVK_SUCCESS             0
#define IVK_NOT_READY           1
#define IVK_TIMEOUT             2
#define IVK_ERROR_DEVICE_LOST   (-4)

/* ---------------------------------------------------------------- E4: memory heaps */

/* The memory limit of spec 05 D1, worked out the layer's own way: min(footprint +
 * available, memsize - 1536 MB). available 0 = unknown (that term drops out), memsize 0 =
 * unknown (no cap). UINT64_MAX when neither is known. */
uint64_t ivk_mem_limit( uint64_t footprint, uint64_t available, uint64_t memsize );

/* E4: what a DEVICE_LOCAL heap reports: limit - 4096 MB - 2560 MB, at least 1024 MB, and never
 * above the driver's own size of that heap. */
uint64_t ivk_heap_clamp( uint64_t limit, uint64_t driver_size );

/* E4, D1's trim rule (spec 00 Part I: trim 1536 MB): when footprint > limit - 1536 MB, the
 * excess comes off the clamped size; never below 768 MB or above the clamped size. */
uint64_t ivk_budget_trim( uint64_t clamped, uint64_t limit, uint64_t footprint );

/* E4: the trimmed budget is worked out again at most every 250 ms */
#define IVK_BUDGET_REREAD_NS (250ull * 1000 * 1000)
int ivk_budget_due( uint64_t now_ns, uint64_t last_ns, int have_last );

/* ---------------------------------------------------------------- E5: frame pacing */

struct ivk_pacer
{
    uint64_t deadline;   /* the last frame's deadline, ns */
    int      mode;       /* the mode that deadline was set in */
    int      primed;     /* a deadline exists */
};

/* the period of a pacing mode (spec 05 E3's table): 1 = 60 fps, 3 = 30 fps, 4 = 40 fps;
 * 0 for modes without a hold (0, 2 and unknown ones) */
uint64_t ivk_pace_period_ns( int mode );

/* E5: called before a present at time now. Returns the absolute time (ns) to hold the thread
 * until, or 0 for no hold. The deadline advances by one period from the previous deadline; a
 * thread more than one period late starts again from now; a late frame doesn't sleep; the
 * first frame and a mode change start from now. */
uint64_t ivk_pace( struct ivk_pacer *pacer, int mode, uint64_t now_ns );

/* ---------------------------------------------------------------- E7: bounded waits */

#define IVK_WAIT_SLICE_NS (250ull * 1000 * 1000)

/* one call of the driver's wait with the given timeout (ns); returns a VkResult */
typedef int32_t (*ivk_wait_call)( void *ctx, uint64_t timeout_ns );

struct ivk_wait_env
{
    uint64_t (*now_ns)( void );   /* monotonic clock */
    int (*kill_pending)( void );  /* 1: the calling thread's pseudo-process is being killed */
};

/* E7: runs a wait of `timeout_ns` (UINT64_MAX: forever) as slices of at most 250 ms. A
 * timeout of 250 ms or less is one call, as the caller gave it. Between slices kill_pending is
 * asked; when it answers 1 the result is IVK_ERROR_DEVICE_LOST. The caller's deadline is kept
 * exactly: the last slice ends at it and IVK_TIMEOUT comes back then. not_ready_is_timeout: a
 * slice that answers IVK_NOT_READY (an acquire with a zero slice) counts as IVK_TIMEOUT. */
int32_t ivk_sliced_wait( uint64_t timeout_ns, ivk_wait_call call, void *ctx,
                         const struct ivk_wait_env *env, int not_ready_is_timeout );

/* ---------------------------------------------------------------- E1: per-object tables */

/* A fixed table from a dispatchable handle (a pointer value, never 0) to a record. Readers take
 * no lock: a slot's value is written before its key is published (release/acquire). Writers
 * (put, del) hold the table's lock (the caller's). A lookup racing with the deletion of the
 * same handle may see the old or no record; Vulkan forbids using a handle while it is being
 * destroyed, so that case doesn't arise in a valid program. */
#define IVK_MAP_CAP 1024u   /* a power of two */
struct ivk_map
{
    _Atomic uint64_t keys[IVK_MAP_CAP];
    void *_Atomic vals[IVK_MAP_CAP];
    unsigned int count;
};

void *ivk_map_get( struct ivk_map *map, uint64_t key );
/* 0 when the table is full */
int ivk_map_put( struct ivk_map *map, uint64_t key, void *val );
/* returns the record that was there, or NULL */
void *ivk_map_del( struct ivk_map *map, uint64_t key );
/* the i-th slot's record (NULL for an empty or deleted slot), i < IVK_MAP_CAP; for the writers'
 * walks (holding the lock) */
void *ivk_map_slot( struct ivk_map *map, unsigned int i, uint64_t *key );

/* ---------------------------------------------------------------- E8: recorded handles */

/* The kinds of handles E8 records per device, in the order E8 destroys them. */
enum ivk_kind
{
    IVK_SWAPCHAIN,
    IVK_FRAMEBUFFER,
    IVK_RENDER_PASS,
    IVK_PIPELINE,
    IVK_PIPELINE_CACHE,
    IVK_SHADER_MODULE,
    IVK_PIPELINE_LAYOUT,
    IVK_DESCRIPTOR_POOL,
    IVK_DESCRIPTOR_SET_LAYOUT,
    IVK_COMMAND_POOL,
    IVK_QUERY_POOL,
    IVK_EVENT,
    IVK_FENCE,
    IVK_SEMAPHORE,
    IVK_BUFFER_VIEW,
    IVK_IMAGE_VIEW,
    IVK_SAMPLER,
    IVK_IMAGE,
    IVK_BUFFER,
    IVK_MEMORY,
    IVK_KIND_COUNT
};

/* plural names for the release line ("swapchains", ...) */
const char *ivk_kind_name( enum ivk_kind kind );

/* A set of non-dispatchable handles (never 0). Not thread-safe: the device record's lock
 * guards it. */
struct ivk_set
{
    uint64_t *slots;     /* 0 empty, UINT64_MAX deleted */
    uint32_t  cap;       /* 0 or a power of two */
    uint32_t  count;
    uint32_t  used;      /* count + deleted slots */
};

/* 0 on allocation failure (the handle is then not recorded) */
int ivk_set_add( struct ivk_set *set, uint64_t handle );
/* 1 when it was there */
int ivk_set_del( struct ivk_set *set, uint64_t handle );
int ivk_set_has( const struct ivk_set *set, uint64_t handle );
/* calls fn for every handle, in no particular order */
void ivk_set_each( const struct ivk_set *set, void (*fn)( void *ctx, uint64_t handle ), void *ctx );
void ivk_set_free( struct ivk_set *set );

/* E8: destroys every handle of every set in ivk_kind order through destroy(ctx, kind, handle),
 * counting them into counts[] (may be NULL). Returns the total. The sets are emptied. */
unsigned int ivk_release_all( struct ivk_set sets[IVK_KIND_COUNT],
                              void (*destroy)( void *ctx, enum ivk_kind kind, uint64_t handle ),
                              void *ctx, unsigned int counts[IVK_KIND_COUNT] );

/* E8's log line: "3 swapchains, 1 memory" for the non-zero counts (or "no objects") */
void ivk_release_summary( const unsigned int counts[IVK_KIND_COUNT], char *buf, size_t size );

/* ---------------------------------------------------------------- logs */

/* "once per process": remembers the last 32 process ids it said yes for */
struct ivk_pidset
{
    unsigned int pids[32];
    unsigned int next;
    unsigned int used;
};
/* 1 the first time a pid is seen (and remembers it), 0 after; not thread-safe */
int ivk_pid_first( struct ivk_pidset *set, unsigned int pid );

/* A list of names for one log line, ", "-separated, cut with "..." when it doesn't fit. */
struct ivk_names
{
    char     buf[480];
    size_t   len;
    unsigned int count;
    int      cut;
};
void ivk_names_init( struct ivk_names *names );
void ivk_names_add( struct ivk_names *names, const char *name );

/* E3: compares two arrays of VkBool32 (uint32_t) fields: every field the program asked for
 * (want[i] != 0) that the driver lacks (have[i] == 0) adds its name, the i-th of `fields` (the
 * names one after another, each ended by a NUL). Returns how many were missing. */
unsigned int ivk_bool_diff( const uint32_t *want, const uint32_t *have, unsigned int count,
                            const char *fields, struct ivk_names *out );

#endif /* IOS_VK_PURE_H */
