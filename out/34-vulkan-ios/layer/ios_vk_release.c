// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part E8: the release thread. A program that ends without destroying its device (normal
// for ExitProcess, always for a force quit) would keep its Metal memory for the rest of the
// session. win32u hands each such device here (ios_vk_layer_release_device, from Part D7's
// ios_vk_release_process); one native thread then, per device:
//   1. waits for it to go idle (vkDeviceWaitIdle on a helper thread; after 2 s the device is
//      logged and left: it leaks, the app keeps running);
//   2. destroys every handle the layer recorded for it, kinds in ivk_kind's order;
//   3. destroys the device and logs one line with the counts.
// The program's surfaces and instances whose devices are still queued are destroyed by the same
// thread after those devices (ivk_release_queue_surface / _instance), so a swapchain never
// outlives its surface and a device never outlives its instance. A handle the program destroyed
// itself was dropped from the record, so it is never destroyed twice.

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ios_vk_internal.h"

enum job_type { JOB_DEVICE, JOB_SURFACE, JOB_INSTANCE };

struct job
{
    struct job          *next;
    enum job_type        type;
    struct ivk_device   *device;
    struct ivk_instance *instance;
    VkSurfaceKHR         surface;
};

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;   /* a job came, or the thread went idle */
static struct job *head, *tail;
static int busy, started;

/* real time, not ivk_now_ns: the 2 s limit is wall-clock patience, also in the host tests */
static uint64_t mono_ns( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* waits on cond for at most until_mono (a mono_ns time); 0 when that time has passed */
static int timed_wait( pthread_cond_t *cond, pthread_mutex_t *lock, uint64_t until_mono )
{
    uint64_t now = mono_ns(), step;
    struct timespec ts;

    if (now >= until_mono) return 0;
    step = until_mono - now;
    if (step > 100000000ull) step = 100000000ull;   /* re-check the monotonic clock every 100 ms */
    clock_gettime( CLOCK_REALTIME, &ts );
    ts.tv_sec += (time_t)(step / 1000000000ull);
    ts.tv_nsec += (long)(step % 1000000000ull);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait( cond, lock, &ts );
    return 1;
}

/* ---------------------------------------------------------------- 1. the bounded wait */

struct idle_wait
{
    pthread_mutex_t    lock;
    pthread_cond_t     cond;
    int                done, abandoned;
    struct ivk_device *device;
};

static void idle_thunk( void *p )
{
    struct ivk_device *device = p;
    if (device->fn.DeviceWaitIdle) device->fn.DeviceWaitIdle( device->handle );
}

static void *waiter_main( void *p )
{
    struct idle_wait *w = p;
    int abandoned;

    ios_vk_pooled( idle_thunk, w->device );
    pthread_mutex_lock( &w->lock );
    w->done = 1;
    abandoned = w->abandoned;
    pthread_cond_signal( &w->cond );
    pthread_mutex_unlock( &w->lock );
    if (abandoned)   /* the release thread gave up on it: the wait is this thread's to free */
    {
        pthread_mutex_destroy( &w->lock );
        pthread_cond_destroy( &w->cond );
        free( w );
    }
    return NULL;
}

/* 1 when the device went idle within IVK_RELEASE_WAIT_MS */
static int wait_idle( struct ivk_device *device )
{
    uint64_t until = mono_ns() + (uint64_t)IVK_RELEASE_WAIT_MS * 1000000ull;
    struct idle_wait *w;
    pthread_attr_t attr;
    pthread_t thread;
    int done, ok;

    if (!(w = calloc( 1, sizeof(*w) ))) goto inline_wait;
    pthread_mutex_init( &w->lock, NULL );
    pthread_cond_init( &w->cond, NULL );
    w->device = device;
    pthread_attr_init( &attr );
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
    ok = !pthread_create( &thread, &attr, waiter_main, w );
    pthread_attr_destroy( &attr );
    if (!ok)
    {
        pthread_mutex_destroy( &w->lock );
        pthread_cond_destroy( &w->cond );
        free( w );
        goto inline_wait;
    }

    pthread_mutex_lock( &w->lock );
    while (!w->done && timed_wait( &w->cond, &w->lock, until )) {}
    done = w->done;
    if (!done) w->abandoned = 1;
    pthread_mutex_unlock( &w->lock );
    if (!done) return 0;   /* the waiter frees w if it ever returns */
    pthread_mutex_destroy( &w->lock );
    pthread_cond_destroy( &w->cond );
    free( w );
    return 1;

inline_wait:
    ivk_log( "release: no helper thread, waiting for device %p without a limit", (void *)device->handle );
    ios_vk_pooled( idle_thunk, device );
    return 1;
}

/* ---------------------------------------------------------------- 2, 3. destroying */

#define DESTROY(fn_name, type) \
    if (device->fn.fn_name) device->fn.fn_name( device->handle, IVK_HANDLE(type, handle), NULL ); \
    break;

static void destroy_one( void *ctx, enum ivk_kind kind, uint64_t handle )
{
    struct ivk_device *device = ctx;

    switch (kind)
    {
    case IVK_SWAPCHAIN:             DESTROY(DestroySwapchainKHR, VkSwapchainKHR)
    case IVK_FRAMEBUFFER:           DESTROY(DestroyFramebuffer, VkFramebuffer)
    case IVK_RENDER_PASS:           DESTROY(DestroyRenderPass, VkRenderPass)
    case IVK_PIPELINE:              DESTROY(DestroyPipeline, VkPipeline)
    case IVK_PIPELINE_CACHE:        DESTROY(DestroyPipelineCache, VkPipelineCache)
    case IVK_SHADER_MODULE:         DESTROY(DestroyShaderModule, VkShaderModule)
    case IVK_PIPELINE_LAYOUT:       DESTROY(DestroyPipelineLayout, VkPipelineLayout)
    case IVK_DESCRIPTOR_POOL:       DESTROY(DestroyDescriptorPool, VkDescriptorPool)
    case IVK_DESCRIPTOR_SET_LAYOUT: DESTROY(DestroyDescriptorSetLayout, VkDescriptorSetLayout)
    case IVK_COMMAND_POOL:          DESTROY(DestroyCommandPool, VkCommandPool)
    case IVK_QUERY_POOL:            DESTROY(DestroyQueryPool, VkQueryPool)
    case IVK_EVENT:                 DESTROY(DestroyEvent, VkEvent)
    case IVK_FENCE:                 DESTROY(DestroyFence, VkFence)
    case IVK_SEMAPHORE:             DESTROY(DestroySemaphore, VkSemaphore)
    case IVK_BUFFER_VIEW:           DESTROY(DestroyBufferView, VkBufferView)
    case IVK_IMAGE_VIEW:            DESTROY(DestroyImageView, VkImageView)
    case IVK_SAMPLER:               DESTROY(DestroySampler, VkSampler)
    case IVK_IMAGE:                 DESTROY(DestroyImage, VkImage)
    case IVK_BUFFER:                DESTROY(DestroyBuffer, VkBuffer)
    case IVK_MEMORY:                DESTROY(FreeMemory, VkDeviceMemory)
    case IVK_KIND_COUNT:            break;
    }
}
#undef DESTROY

static void release_device_pooled( void *p )
{
    struct ivk_device *device = p;
    unsigned int counts[IVK_KIND_COUNT], total;
    char summary[400];

    /* no program thread uses the device any more: no lock needed for its sets */
    total = ivk_release_all( device->objects, destroy_one, device, counts );
    if (device->fn.DestroyDevice) device->fn.DestroyDevice( device->handle, NULL );
    ivk_release_summary( counts, summary, sizeof(summary) );
    ivk_log( "released device %p: %s (%u objects)", (void *)device->handle, summary, total );
}

static void run_device( struct ivk_device *device )
{
    struct ivk_instance *instance = device->instance;

    if (!wait_idle( device ))
    {
        ivk_log( "device %p: vkDeviceWaitIdle didn't return within %u ms; the device is left (it leaks)",
                 (void *)device->handle, IVK_RELEASE_WAIT_MS );
        atomic_store( &instance->poisoned, 1 );
        atomic_fetch_sub( &instance->releasing, 1 );
        return;   /* the record stays with the waiter, which may still use it */
    }
    ios_vk_pooled( release_device_pooled, device );
    ivk_device_free( device );
    atomic_fetch_sub( &instance->releasing, 1 );
}

static void run_job( struct job *job )
{
    struct ivk_instance *instance = job->instance;

    switch (job->type)
    {
    case JOB_DEVICE:
        run_device( job->device );
        break;
    case JOB_SURFACE:
        if (atomic_load( &instance->poisoned ))
            ivk_log( "surface 0x%llx left: a device of its instance hung", (unsigned long long)IVK_H64(job->surface) );
        else if (instance->fn.DestroySurfaceKHR)
            instance->fn.DestroySurfaceKHR( instance->handle, job->surface, NULL );
        break;
    case JOB_INSTANCE:
        if (atomic_load( &instance->poisoned ))
        {
            ivk_log( "instance %p left: a device of it hung", (void *)instance->handle );
            break;
        }
        if (instance->fn.DestroyInstance) instance->fn.DestroyInstance( instance->handle, NULL );
        ivk_instance_free( instance );
        break;
    }
}

static void *release_main( void *arg )
{
    (void)arg;
    for (;;)
    {
        struct job *job;

        pthread_mutex_lock( &queue_lock );
        while (!head)
        {
            if (busy) { busy = 0; pthread_cond_broadcast( &queue_cond ); }
            pthread_cond_wait( &queue_cond, &queue_lock );
        }
        job = head;
        if (!(head = job->next)) tail = NULL;
        busy = 1;
        pthread_mutex_unlock( &queue_lock );

        run_job( job );
        free( job );
    }
    return NULL;
}

static void enqueue( struct job *job )
{
    pthread_attr_t attr;
    pthread_t thread;

    pthread_mutex_lock( &queue_lock );
    if (tail) tail->next = job;
    else head = job;
    tail = job;
    if (!started)
    {
        pthread_attr_init( &attr );
        pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
        started = !pthread_create( &thread, &attr, release_main, NULL );
        pthread_attr_destroy( &attr );
        if (!started) ivk_log( "release: the release thread couldn't start (%d)", errno );
    }
    pthread_cond_broadcast( &queue_cond );
    pthread_mutex_unlock( &queue_lock );
}

static struct job *new_job( enum job_type type, struct ivk_instance *instance )
{
    struct job *job = calloc( 1, sizeof(*job) );
    if (!job) return NULL;
    job->type = type;
    job->instance = instance;
    return job;
}

void ivk_release_queue_device( struct ivk_device *device )
{
    struct job *job = new_job( JOB_DEVICE, device->instance );

    if (!job)
    {
        ivk_log( "release: out of memory, device %p left", (void *)device->handle );
        return;
    }
    job->device = device;
    atomic_fetch_add( &device->instance->releasing, 1 );
    enqueue( job );
}

int ivk_release_queue_surface( struct ivk_instance *instance, VkSurfaceKHR surface )
{
    struct job *job;

    if (!atomic_load( &instance->releasing ) || !(job = new_job( JOB_SURFACE, instance ))) return 0;
    job->surface = surface;
    enqueue( job );
    return 1;
}

int ivk_release_queue_instance( struct ivk_instance *instance )
{
    struct job *job;

    if (!atomic_load( &instance->releasing ) || !(job = new_job( JOB_INSTANCE, instance ))) return 0;
    enqueue( job );
    return 1;
}

int ios_vk_layer_release_drain( unsigned int timeout_ms )
{
    uint64_t until = mono_ns() + (uint64_t)timeout_ms * 1000000ull;
    int idle;

    pthread_mutex_lock( &queue_lock );
    while ((head || busy) && started && timed_wait( &queue_cond, &queue_lock, until )) {}
    idle = !head && !busy;
    pthread_mutex_unlock( &queue_lock );
    return idle;
}
