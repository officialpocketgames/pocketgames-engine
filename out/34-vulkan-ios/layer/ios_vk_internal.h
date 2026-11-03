// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part E: what the layer's files share: the per-object records (E1), the driver
// functions each record keeps, the platform calls (ios_vk_platform.c on iOS; the host tests give
// fakes), the autorelease pool helper (E2) and the release thread (E8).

#ifndef IOS_VK_INTERNAL_H
#define IOS_VK_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include "ios_vk_layer.h"
#include "ios_vk_pure.h"

/* ---------------------------------------------------------------- E1: the host driver */

/* The driver's vkGetInstanceProcAddr, bound at link time. The one place the driver is named:
 * a later driver (spec 34 Part L) is a different value of this macro. */
#ifndef IOS_VK_DRIVER_GIPA
#define IOS_VK_DRIVER_GIPA vkGetInstanceProcAddr
#endif
extern PFN_vkVoidFunction VKAPI_CALL IOS_VK_DRIVER_GIPA( VkInstance instance, const char *name );

/* Instance-level driver functions an instance record keeps (X(name) for vk<name>). */
#define IVK_INSTANCE_FUNCS(X) \
    X(DestroyInstance) \
    X(EnumeratePhysicalDevices) \
    X(EnumeratePhysicalDeviceGroups) \
    X(EnumeratePhysicalDeviceGroupsKHR) \
    X(CreateDevice) \
    X(EnumerateDeviceExtensionProperties) \
    X(GetPhysicalDeviceFeatures) \
    X(GetPhysicalDeviceFeatures2) \
    X(GetPhysicalDeviceFeatures2KHR) \
    X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceProperties2) \
    X(GetPhysicalDeviceProperties2KHR) \
    X(GetPhysicalDeviceMemoryProperties) \
    X(GetPhysicalDeviceMemoryProperties2) \
    X(GetPhysicalDeviceMemoryProperties2KHR) \
    X(DestroySurfaceKHR) \
    X(GetDeviceProcAddr)

/* Device-level driver functions a device record keeps: the intercepted ones (E2-E8). */
#define IVK_DEVICE_FUNCS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) \
    X(QueueSubmit) X(QueueSubmit2) X(QueueSubmit2KHR) X(QueueWaitIdle) X(DeviceWaitIdle) \
    X(QueuePresentKHR) X(AcquireNextImageKHR) X(AcquireNextImage2KHR) \
    X(WaitForFences) X(WaitSemaphores) X(WaitSemaphoresKHR) \
    X(AllocateMemory) X(FreeMemory) \
    X(CreateImage) X(DestroyImage) X(CreateBuffer) X(DestroyBuffer) \
    X(CreateImageView) X(DestroyImageView) X(CreateBufferView) X(DestroyBufferView) \
    X(CreateSampler) X(DestroySampler) \
    X(CreateGraphicsPipelines) X(CreateComputePipelines) X(DestroyPipeline) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreatePipelineCache) X(DestroyPipelineCache) \
    X(CreateShaderModule) X(DestroyShaderModule) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreateCommandPool) X(DestroyCommandPool) \
    X(CreateQueryPool) X(DestroyQueryPool) \
    X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence) \
    X(CreateEvent) X(DestroyEvent) \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) \
    X(CreateRenderPass) X(CreateRenderPass2) X(CreateRenderPass2KHR) X(DestroyRenderPass) \
    X(CreateFramebuffer) X(DestroyFramebuffer)

struct ivk_instance_fns
{
#define X(name) PFN_vk##name name;
    IVK_INSTANCE_FUNCS(X)
#undef X
};

struct ivk_device_fns
{
#define X(name) PFN_vk##name name;
    IVK_DEVICE_FUNCS(X)
#undef X
};

/* ---------------------------------------------------------------- E1: the records */

struct ivk_instance
{
    VkInstance              handle;
    struct ivk_instance_fns fn;
    /* E8: devices of this instance handed to the release thread and not yet released. While
     * there are any, its surfaces and the instance itself are destroyed by the release thread,
     * after those devices (a swapchain must go before its surface, a device before its instance) */
    _Atomic unsigned int    releasing;
    /* E8: a device of this instance was left hanging in its wait: the instance and its surfaces
     * are left too */
    _Atomic int             poisoned;
};

struct ivk_physical
{
    VkPhysicalDevice     handle;
    struct ivk_instance *instance;
};

struct ivk_device
{
    VkDevice              handle;
    VkPhysicalDevice      physical;
    struct ivk_instance  *instance;
    struct ivk_device_fns fn;
    /* E8: the live handles the program created, per kind */
    pthread_mutex_t       objects_lock;
    struct ivk_set        objects[IVK_KIND_COUNT];
    /* E5 */
    pthread_mutex_t       pace_lock;
    struct ivk_pacer      pacer;
};

struct ivk_queue
{
    VkQueue            handle;
    struct ivk_device *device;
};

/* lookups (lock-free) */
struct ivk_instance *ivk_instance_get( VkInstance handle );
struct ivk_physical *ivk_physical_get( VkPhysicalDevice handle );
struct ivk_device *ivk_device_get( VkDevice handle );

/* E8: a device record taken out of the tables (the program is gone); its queues are dropped
 * too. NULL if unknown. */
struct ivk_device *ivk_device_take( VkDevice handle );
void ivk_device_free( struct ivk_device *device );
/* an instance record destroyed by the release thread (its tables entries are already gone) */
void ivk_instance_free( struct ivk_instance *instance );

/* the calling pseudo-process (ios_vk_layer_set_process_id_func), 0 when unknown */
unsigned int ivk_process_id( void );

/* ---------------------------------------------------------------- E2: autorelease pools */

/* Runs fn(ctx) inside an @autoreleasepool (ios_vk_pool.m); a plain call on other hosts
 * (ios_vk_pool.c). */
void ios_vk_pooled( void (*fn)( void *ctx ), void *ctx );

/* ---------------------------------------------------------------- E3: refused devices */

/* Logs, once per process, what the program asked of the device that the driver lacks
 * (ios_vk_features.c). */
void ivk_explain_device_refusal( struct ivk_instance *instance, VkPhysicalDevice physical,
                                 const VkDeviceCreateInfo *info, VkResult res );
/* the same for instance extensions */
void ivk_explain_instance_refusal( const VkInstanceCreateInfo *info, VkResult res );

/* ---------------------------------------------------------------- E8: the release thread */

/* ios_vk_release.c: queue a device (already taken out of the tables) */
void ivk_release_queue_device( struct ivk_device *device );
/* queue the destruction of a surface or of an instance behind the instance's pending devices;
 * 0 when nothing is pending (the caller destroys it at once) */
int ivk_release_queue_surface( struct ivk_instance *instance, VkSurfaceKHR surface );
int ivk_release_queue_instance( struct ivk_instance *instance );
/* the wait for a hanging device (2 s; the host tests keep it) */
#define IVK_RELEASE_WAIT_MS 2000u

/* ---------------------------------------------------------------- platform */

/* ios_vk_platform.c (iOS); the host tests define their own */
uint64_t ivk_now_ns( void );                    /* monotonic (mach_absolute_time) */
void ivk_sleep_until_ns( uint64_t deadline );   /* mach_wait_until */
uint64_t ivk_footprint( void );                 /* phys_footprint of TASK_VM_INFO; 0 unknown */
uint64_t ivk_available_memory( void );          /* os_proc_available_memory(); 0 unknown */
uint64_t ivk_memsize( void );                   /* hw.memsize; 0 unknown */
void ivk_log( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));   /* "[vk-ios] " ... */

/* the other parts' optional exports, referenced weak (absent: the defaults the spec gives) */
extern int pg_get_vsync_locked( void ) __attribute__((weak));        /* spec 05 E3 */
extern void pg_note_external_present( void ) __attribute__((weak));  /* spec 05, spec 34 E6 */
extern int ios_thread_kill_pending( void ) __attribute__((weak));    /* spec 09 D3, spec 34 E7 */

#endif /* IOS_VK_INTERNAL_H */
