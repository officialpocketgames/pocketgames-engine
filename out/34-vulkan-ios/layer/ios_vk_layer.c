// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part E: the iOS Vulkan layer. win32u's vulkan.c reaches the host driver (MoltenVK,
// linked statically) only through ios_vk_host_get_instance_proc_addr and
// ios_vk_host_get_device_proc_addr (E1). The layer wraps no handle: every host handle passes
// through unchanged, and command-buffer recording isn't intercepted, so draws cost nothing extra.
// What it adds:
//   E2  driver calls that can autorelease Metal objects run inside an autorelease pool
//   E3  MoltenVK's configuration, the driver line, the portability subset, refusal logs
//   E4  DEVICE_LOCAL heaps and their budgets clamped to the video memory budget
//   E5  frame pacing at the present
//   E6  the app's present count
//   E7  long waits cut into 250 ms slices, ended early for a killed pseudo-process
//   E8  the live handles of each device, for its release when the program is gone

#include <stdlib.h>
#include <string.h>
#include "ios_vk_internal.h"

_Static_assert( IVK_SUCCESS == VK_SUCCESS && IVK_NOT_READY == VK_NOT_READY && IVK_TIMEOUT == VK_TIMEOUT &&
                IVK_ERROR_DEVICE_LOST == VK_ERROR_DEVICE_LOST, "VkResult values of ios_vk_pure.h" );

/* ---------------------------------------------------------------- E1: tables */

static pthread_mutex_t tables_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ivk_map instances, physicals, devices, queues;

static _Atomic(unsigned int (*)( void )) process_id_fn;

void ios_vk_layer_set_process_id_func( unsigned int (*fn)( void ) )
{
    atomic_store( &process_id_fn, fn );
}

unsigned int ivk_process_id( void )
{
    unsigned int (*fn)( void ) = atomic_load( &process_id_fn );
    return fn ? fn() : 0;
}

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* once per pseudo-process, for one kind of line */
static int first_for_process( struct ivk_pidset *set )
{
    unsigned int pid = ivk_process_id();
    int first;

    pthread_mutex_lock( &log_lock );
    first = ivk_pid_first( set, pid );
    pthread_mutex_unlock( &log_lock );
    return first;
}

struct ivk_instance *ivk_instance_get( VkInstance handle )
{
    return handle ? ivk_map_get( &instances, IVK_H64(handle) ) : NULL;
}

struct ivk_physical *ivk_physical_get( VkPhysicalDevice handle )
{
    return handle ? ivk_map_get( &physicals, IVK_H64(handle) ) : NULL;
}

struct ivk_device *ivk_device_get( VkDevice handle )
{
    return handle ? ivk_map_get( &devices, IVK_H64(handle) ) : NULL;
}

static struct ivk_queue *queue_get( VkQueue handle )
{
    return handle ? ivk_map_get( &queues, IVK_H64(handle) ) : NULL;
}

static void table_full( const char *what )
{
    static _Atomic int logged;
    if (!atomic_exchange( &logged, 1 )) ivk_log( "the layer's %s table is full (%u entries)", what, IVK_MAP_CAP );
}

static void drop_queues_of( struct ivk_device *device )   /* holding tables_lock */
{
    unsigned int i;
    uint64_t key;

    for (i = 0; i < IVK_MAP_CAP; i++)
    {
        struct ivk_queue *queue = ivk_map_slot( &queues, i, &key );
        if (!queue || queue->device != device) continue;
        ivk_map_del( &queues, key );
        free( queue );
    }
}

struct ivk_device *ivk_device_take( VkDevice handle )
{
    struct ivk_device *device;

    if (!handle) return NULL;
    pthread_mutex_lock( &tables_lock );
    if ((device = ivk_map_del( &devices, IVK_H64(handle) ))) drop_queues_of( device );
    pthread_mutex_unlock( &tables_lock );
    return device;
}

void ivk_device_free( struct ivk_device *device )
{
    int kind;

    for (kind = 0; kind < IVK_KIND_COUNT; kind++) ivk_set_free( &device->objects[kind] );
    pthread_mutex_destroy( &device->objects_lock );
    pthread_mutex_destroy( &device->pace_lock );
    free( device );
}

void ivk_instance_free( struct ivk_instance *instance )
{
    free( instance );
}

/* ---------------------------------------------------------------- E2: pooled calls */

/* Each pooled driver call packs its arguments and result into a struct and runs a thunk inside
 * ios_vk_pooled. A few calls a frame, never per draw. */

struct present_args { PFN_vkQueuePresentKHR fn; VkQueue queue; const VkPresentInfoKHR *info; VkResult res; };
static void present_thunk( void *p ) { struct present_args *a = p; a->res = a->fn( a->queue, a->info ); }

struct submit_args { PFN_vkQueueSubmit fn; VkQueue queue; uint32_t count; const VkSubmitInfo *submits; VkFence fence; VkResult res; };
static void submit_thunk( void *p ) { struct submit_args *a = p; a->res = a->fn( a->queue, a->count, a->submits, a->fence ); }

struct submit2_args { PFN_vkQueueSubmit2 fn; VkQueue queue; uint32_t count; const VkSubmitInfo2 *submits; VkFence fence; VkResult res; };
static void submit2_thunk( void *p ) { struct submit2_args *a = p; a->res = a->fn( a->queue, a->count, a->submits, a->fence ); }

struct queue_idle_args { PFN_vkQueueWaitIdle fn; VkQueue queue; VkResult res; };
static void queue_idle_thunk( void *p ) { struct queue_idle_args *a = p; a->res = a->fn( a->queue ); }

struct device_idle_args { PFN_vkDeviceWaitIdle fn; VkDevice device; VkResult res; };
static void device_idle_thunk( void *p ) { struct device_idle_args *a = p; a->res = a->fn( a->device ); }

struct create_args   /* every vkCreate*(device, info, allocator, handle) of E2 */
{
    VkResult (VKAPI_PTR *fn)( VkDevice, const void *, const VkAllocationCallbacks *, void * );
    VkDevice device; const void *info; const VkAllocationCallbacks *alloc; void *out; VkResult res;
};
static void create_thunk( void *p ) { struct create_args *a = p; a->res = a->fn( a->device, a->info, a->alloc, a->out ); }

struct pipelines_args
{
    VkResult (VKAPI_PTR *fn)( VkDevice, VkPipelineCache, uint32_t, const void *, const VkAllocationCallbacks *, VkPipeline * );
    VkDevice device; VkPipelineCache cache; uint32_t count; const void *infos;
    const VkAllocationCallbacks *alloc; VkPipeline *out; VkResult res;
};
static void pipelines_thunk( void *p )
{
    struct pipelines_args *a = p;
    a->res = a->fn( a->device, a->cache, a->count, a->infos, a->alloc, a->out );
}

struct destroy_swapchain_args { PFN_vkDestroySwapchainKHR fn; VkDevice device; VkSwapchainKHR swapchain; const VkAllocationCallbacks *alloc; };
static void destroy_swapchain_thunk( void *p ) { struct destroy_swapchain_args *a = p; a->fn( a->device, a->swapchain, a->alloc ); }

struct create_device_args { PFN_vkCreateDevice fn; VkPhysicalDevice physical; const VkDeviceCreateInfo *info; const VkAllocationCallbacks *alloc; VkDevice *out; VkResult res; };
static void create_device_thunk( void *p ) { struct create_device_args *a = p; a->res = a->fn( a->physical, a->info, a->alloc, a->out ); }

struct destroy_device_args { PFN_vkDestroyDevice fn; VkDevice device; const VkAllocationCallbacks *alloc; };
static void destroy_device_thunk( void *p ) { struct destroy_device_args *a = p; a->fn( a->device, a->alloc ); }

/* a vkCreate* of the create_args shape, pooled */
static VkResult pooled_create( void *fn, VkDevice device, const void *info, const VkAllocationCallbacks *alloc, void *out )
{
    struct create_args a;

    a.fn = (VkResult (VKAPI_PTR *)( VkDevice, const void *, const VkAllocationCallbacks *, void * ))fn;
    a.device = device; a.info = info; a.alloc = alloc; a.out = out; a.res = VK_ERROR_INITIALIZATION_FAILED;
    ios_vk_pooled( create_thunk, &a );
    return a.res;
}

/* ---------------------------------------------------------------- E8: recording */

static void track( struct ivk_device *device, enum ivk_kind kind, uint64_t handle )
{
    static _Atomic int warned;
    int ok;

    if (!handle) return;
    pthread_mutex_lock( &device->objects_lock );
    ok = ivk_set_add( &device->objects[kind], handle );
    pthread_mutex_unlock( &device->objects_lock );
    if (!ok && !atomic_exchange( &warned, 1 ))
        ivk_log( "out of memory recording a handle: it won't be released for an ended program" );
}

static void untrack( struct ivk_device *device, enum ivk_kind kind, uint64_t handle )
{
    if (!handle) return;
    pthread_mutex_lock( &device->objects_lock );
    ivk_set_del( &device->objects[kind], handle );
    pthread_mutex_unlock( &device->objects_lock );
}

unsigned int ios_vk_layer_device_objects( VkDevice handle )
{
    struct ivk_device *device = ivk_device_get( handle );
    unsigned int total = 0;
    int kind;

    if (!device) return 0;
    pthread_mutex_lock( &device->objects_lock );
    for (kind = 0; kind < IVK_KIND_COUNT; kind++) total += device->objects[kind].count;
    pthread_mutex_unlock( &device->objects_lock );
    return total;
}

void ios_vk_layer_release_device( VkDevice handle )
{
    struct ivk_device *device = ivk_device_take( handle );
    if (device) ivk_release_queue_device( device );
}

/* ---------------------------------------------------------------- E3: instances */

static pthread_once_t host_exts_once = PTHREAD_ONCE_INIT;
static int host_has_layer_settings;

static void read_host_exts( void )
{
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)IOS_VK_DRIVER_GIPA( NULL, "vkEnumerateInstanceExtensionProperties" );
    VkExtensionProperties *props;
    uint32_t count = 0, i;

    if (!enumerate || enumerate( NULL, &count, NULL ) != VK_SUCCESS || !count) return;
    if (!(props = calloc( count, sizeof(*props) ))) return;
    if (enumerate( NULL, &count, props ) >= 0)
        for (i = 0; i < count; i++)
            if (!strcmp( props[i].extensionName, IVK_LAYER_SETTINGS_NAME )) host_has_layer_settings = 1;
    free( props );
}

static const void *find_struct( const void *chain, VkStructureType type )
{
    const VkBaseInStructure *s;
    for (s = chain; s; s = s->pNext) if (s->sType == type) return s;
    return NULL;
}

static int has_name( const char *const *names, uint32_t count, const char *name )
{
    uint32_t i;
    for (i = 0; i < count; i++) if (names[i] && !strcmp( names[i], name )) return 1;
    return 0;
}

/* B5: MoltenVK logs errors only, unless MVK_CONFIG_LOG_LEVEL is set in the environment */
static int want_log_setting( const VkInstanceCreateInfo *info )
{
    const char *env = getenv( "MVK_CONFIG_LOG_LEVEL" );

    if (env && *env) return 0;
    pthread_once( &host_exts_once, read_host_exts );
    if (!host_has_layer_settings) return 0;
    /* the program brought settings of its own: leave its chain as it is */
    return !find_struct( info->pNext, VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT );
}

static PFN_vkCreateInstance driver_create_instance( void )
{
    static _Atomic(PFN_vkCreateInstance) fn;
    PFN_vkCreateInstance f = atomic_load( &fn );

    if (!f)
    {
        f = (PFN_vkCreateInstance)IOS_VK_DRIVER_GIPA( NULL, "vkCreateInstance" );
        atomic_store( &fn, f );
    }
    return f;
}

static void load_instance_fns( struct ivk_instance *instance )
{
#define X(name) instance->fn.name = (PFN_vk##name)IOS_VK_DRIVER_GIPA( instance->handle, "vk" #name );
    IVK_INSTANCE_FUNCS(X)
#undef X
}

/* whether the physical device offers a device extension */
static int offers_device_extension( struct ivk_instance *instance, VkPhysicalDevice physical, const char *name )
{
    VkExtensionProperties *props;
    uint32_t n = 0, i;
    int offered = 0;

    if (!instance->fn.EnumerateDeviceExtensionProperties ||
        instance->fn.EnumerateDeviceExtensionProperties( physical, NULL, &n, NULL ) != VK_SUCCESS || !n) return 0;
    if (!(props = calloc( n, sizeof(*props) ))) return 0;
    if (instance->fn.EnumerateDeviceExtensionProperties( physical, NULL, &n, props ) >= 0)
        for (i = 0; i < n; i++) if (!strcmp( props[i].extensionName, name )) offered = 1;
    free( props );
    return offered;
}

/* E3 and clarification 1: the driver line, with the two features the mesh-shader path gives
 * (geometry shaders, transform feedback), so a log says at once whether the GPU has them */
static void log_driver( struct ivk_instance *instance )
{
    static struct ivk_pidset logged;
    VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 props2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver };
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT };
    VkPhysicalDeviceFeatures2 features2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    PFN_vkGetPhysicalDeviceProperties2 get2;
    PFN_vkGetPhysicalDeviceFeatures2 features_get2;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props;
    uint32_t count = 1;
    VkResult res;

    if (!first_for_process( &logged )) return;
    if (!instance->fn.EnumeratePhysicalDevices || !instance->fn.GetPhysicalDeviceProperties) return;
    res = instance->fn.EnumeratePhysicalDevices( instance->handle, &count, &physical );
    if (res < 0 || !count || !physical)
    {
        ivk_log( "host driver: no physical device (%d)", (int)res );
        return;
    }
    memset( &props, 0, sizeof(props) );
    instance->fn.GetPhysicalDeviceProperties( physical, &props );
    get2 = instance->fn.GetPhysicalDeviceProperties2KHR ? instance->fn.GetPhysicalDeviceProperties2KHR
                                                         : instance->fn.GetPhysicalDeviceProperties2;
    if (get2) get2( physical, &props2 );
    /* the transform feedback struct only where the device offers its extension */
    if (offers_device_extension( instance, physical, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME )) features2.pNext = &xfb;
    features_get2 = instance->fn.GetPhysicalDeviceFeatures2KHR ? instance->fn.GetPhysicalDeviceFeatures2KHR
                                                               : instance->fn.GetPhysicalDeviceFeatures2;
    if (features_get2) features_get2( physical, &features2 );
    else if (instance->fn.GetPhysicalDeviceFeatures) instance->fn.GetPhysicalDeviceFeatures( physical, &features2.features );
    driver.driverName[sizeof(driver.driverName) - 1] = 0;
    driver.driverInfo[sizeof(driver.driverInfo) - 1] = 0;
    props.deviceName[sizeof(props.deviceName) - 1] = 0;
    ivk_log( "host driver %s %s, Vulkan %u.%u.%u, device %s, geometryShader %s, transformFeedback %s",
             driver.driverName[0] ? driver.driverName : "(unnamed)",
             driver.driverInfo[0] ? driver.driverInfo : "(no version)",
             VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion), props.deviceName,
             features2.features.geometryShader ? "yes" : "no", xfb.transformFeedback ? "yes" : "no" );
}

static VkResult VKAPI_CALL ivk_CreateInstance( const VkInstanceCreateInfo *create_info,
                                               const VkAllocationCallbacks *alloc, VkInstance *out )
{
    static const int32_t log_level_errors = 1;   /* MoltenVK's MVK_CONFIG_LOG_LEVEL_ERROR */
    PFN_vkCreateInstance create = driver_create_instance();
    VkInstanceCreateInfo info = *create_info;
    VkLayerSettingEXT setting;
    VkLayerSettingsCreateInfoEXT settings;
    struct ivk_instance *instance;
    const char **names = NULL;
    VkResult res;
    int put;

    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    if (want_log_setting( create_info ) &&
        (names = malloc( (info.enabledExtensionCount + 1) * sizeof(*names) )))
    {
        if (info.enabledExtensionCount)
            memcpy( names, info.ppEnabledExtensionNames, info.enabledExtensionCount * sizeof(*names) );
        if (!has_name( names, info.enabledExtensionCount, IVK_LAYER_SETTINGS_NAME ))
            names[info.enabledExtensionCount++] = IVK_LAYER_SETTINGS_NAME;
        info.ppEnabledExtensionNames = names;

        memset( &setting, 0, sizeof(setting) );
        setting.pLayerName = "MoltenVK";
        setting.pSettingName = "MVK_CONFIG_LOG_LEVEL";
        setting.type = VK_LAYER_SETTING_TYPE_INT32_EXT;
        setting.valueCount = 1;
        setting.pValues = &log_level_errors;
        memset( &settings, 0, sizeof(settings) );
        settings.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
        settings.pNext = info.pNext;
        settings.settingCount = 1;
        settings.pSettings = &setting;
        info.pNext = &settings;
    }

    res = create( &info, alloc, out );
    free( names );
    if (res != VK_SUCCESS)
    {
        ivk_explain_instance_refusal( create_info, res );
        return res;
    }

    if (!(instance = calloc( 1, sizeof(*instance) ))) goto oom;
    instance->handle = *out;
    load_instance_fns( instance );
    pthread_mutex_lock( &tables_lock );
    put = ivk_map_put( &instances, IVK_H64(*out), instance );
    pthread_mutex_unlock( &tables_lock );
    if (!put)
    {
        table_full( "instance" );
        free( instance );
        goto oom;
    }
    log_driver( instance );
    return VK_SUCCESS;

oom:
    {
        PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)IOS_VK_DRIVER_GIPA( *out, "vkDestroyInstance" );
        if (destroy) destroy( *out, alloc );
        *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

static void VKAPI_CALL ivk_DestroyInstance( VkInstance handle, const VkAllocationCallbacks *alloc )
{
    struct ivk_instance *instance;
    unsigned int i;
    uint64_t key;

    if (!handle) return;
    pthread_mutex_lock( &tables_lock );
    if ((instance = ivk_map_del( &instances, IVK_H64(handle) )))
    {
        for (i = 0; i < IVK_MAP_CAP; i++)
        {
            struct ivk_physical *physical = ivk_map_slot( &physicals, i, &key );
            if (!physical || physical->instance != instance) continue;
            ivk_map_del( &physicals, key );
            free( physical );
        }
    }
    pthread_mutex_unlock( &tables_lock );

    if (!instance)
    {
        PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)IOS_VK_DRIVER_GIPA( handle, "vkDestroyInstance" );
        if (destroy) destroy( handle, alloc );
        return;
    }
    /* E8: devices of it are still waiting for the release thread: it goes after them */
    if (ivk_release_queue_instance( instance )) return;
    instance->fn.DestroyInstance( handle, alloc );
    ivk_instance_free( instance );
}

static void record_physical( struct ivk_instance *instance, VkPhysicalDevice handle )
{
    struct ivk_physical *physical;

    if (!handle || ivk_physical_get( handle )) return;
    if (!(physical = calloc( 1, sizeof(*physical) ))) return;
    physical->handle = handle;
    physical->instance = instance;
    pthread_mutex_lock( &tables_lock );
    if (ivk_map_get( &physicals, IVK_H64(handle) ) || !ivk_map_put( &physicals, IVK_H64(handle), physical ))
    {
        if (!ivk_map_get( &physicals, IVK_H64(handle) )) table_full( "physical device" );
        free( physical );
    }
    pthread_mutex_unlock( &tables_lock );
}

static VkResult VKAPI_CALL ivk_EnumeratePhysicalDevices( VkInstance handle, uint32_t *count, VkPhysicalDevice *out )
{
    struct ivk_instance *instance = ivk_instance_get( handle );
    VkResult res;
    uint32_t i;

    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    res = instance->fn.EnumeratePhysicalDevices( handle, count, out );
    if (res >= 0 && out) for (i = 0; i < *count; i++) record_physical( instance, out[i] );
    return res;
}

static VkResult enumerate_groups( struct ivk_instance *instance, PFN_vkEnumeratePhysicalDeviceGroups fn,
                                  VkInstance handle, uint32_t *count, VkPhysicalDeviceGroupProperties *props )
{
    VkResult res;
    uint32_t i, j;

    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    res = fn( handle, count, props );
    if (res >= 0 && props)
        for (i = 0; i < *count; i++)
            for (j = 0; j < props[i].physicalDeviceCount && j < VK_MAX_DEVICE_GROUP_SIZE; j++)
                record_physical( instance, props[i].physicalDevices[j] );
    return res;
}

static VkResult VKAPI_CALL ivk_EnumeratePhysicalDeviceGroups( VkInstance handle, uint32_t *count,
                                                              VkPhysicalDeviceGroupProperties *props )
{
    struct ivk_instance *instance = ivk_instance_get( handle );
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    return enumerate_groups( instance, instance->fn.EnumeratePhysicalDeviceGroups, handle, count, props );
}

static VkResult VKAPI_CALL ivk_EnumeratePhysicalDeviceGroupsKHR( VkInstance handle, uint32_t *count,
                                                                 VkPhysicalDeviceGroupProperties *props )
{
    struct ivk_instance *instance = ivk_instance_get( handle );
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    return enumerate_groups( instance, instance->fn.EnumeratePhysicalDeviceGroupsKHR, handle, count, props );
}

static void VKAPI_CALL ivk_DestroySurfaceKHR( VkInstance handle, VkSurfaceKHR surface, const VkAllocationCallbacks *alloc )
{
    struct ivk_instance *instance = ivk_instance_get( handle );

    if (!instance || !instance->fn.DestroySurfaceKHR) return;
    /* E8: a swapchain on it may still wait for the release thread */
    if (surface && ivk_release_queue_surface( instance, surface )) return;
    instance->fn.DestroySurfaceKHR( handle, surface, alloc );
}

/* ---------------------------------------------------------------- E4: memory heaps */

static pthread_mutex_t budget_lock = PTHREAD_MUTEX_INITIALIZER;
static struct
{
    int          have;
    unsigned int pid;          /* the limit is worked out once per process */
    uint64_t     limit;
    uint64_t     footprint;    /* re-read at most every 250 ms */
    uint64_t     read_at;
} budget;

static void read_budget( uint64_t *limit, uint64_t *footprint )
{
    unsigned int pid = ivk_process_id();
    uint64_t now = ivk_now_ns();

    pthread_mutex_lock( &budget_lock );
    if (!budget.have || budget.pid != pid)
    {
        budget.footprint = ivk_footprint();
        budget.limit = ivk_mem_limit( budget.footprint, ivk_available_memory(), ivk_memsize() );
        budget.read_at = now;
        budget.pid = pid;
        budget.have = 1;
    }
    else if (ivk_budget_due( now, budget.read_at, 1 ))
    {
        budget.footprint = ivk_footprint();
        budget.read_at = now;
    }
    *limit = budget.limit;
    *footprint = budget.footprint;
    pthread_mutex_unlock( &budget_lock );
}

static void clamp_heaps( VkPhysicalDeviceMemoryProperties *props, VkPhysicalDeviceMemoryBudgetPropertiesEXT *budgets )
{
    static struct ivk_pidset logged;
    uint64_t limit, footprint;
    uint32_t i;

    read_budget( &limit, &footprint );
    for (i = 0; i < props->memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
    {
        VkMemoryHeap *heap = &props->memoryHeaps[i];
        uint64_t clamped;

        if (!(heap->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        clamped = ivk_heap_clamp( limit, heap->size );
        if (clamped != heap->size && first_for_process( &logged ))
            ivk_log( "heap %u: %llu MB reported as %llu MB (the video memory budget)", i,
                     (unsigned long long)(heap->size / IVK_MB), (unsigned long long)(clamped / IVK_MB) );
        heap->size = clamped;
        if (budgets)
        {
            uint64_t trimmed = ivk_budget_trim( clamped, limit, footprint );
            if (budgets->heapBudget[i] > trimmed) budgets->heapBudget[i] = trimmed;
        }
    }
}

static void VKAPI_CALL ivk_GetPhysicalDeviceMemoryProperties( VkPhysicalDevice handle, VkPhysicalDeviceMemoryProperties *props )
{
    struct ivk_physical *physical = ivk_physical_get( handle );

    if (!physical) return;
    physical->instance->fn.GetPhysicalDeviceMemoryProperties( handle, props );
    clamp_heaps( props, NULL );
}

static void memory_properties2( PFN_vkGetPhysicalDeviceMemoryProperties2 fn, VkPhysicalDevice handle,
                                VkPhysicalDeviceMemoryProperties2 *props )
{
    if (!fn) return;
    fn( handle, props );
    clamp_heaps( &props->memoryProperties,
                 (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)find_struct( props->pNext,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT ) );
}

static void VKAPI_CALL ivk_GetPhysicalDeviceMemoryProperties2( VkPhysicalDevice handle, VkPhysicalDeviceMemoryProperties2 *props )
{
    struct ivk_physical *physical = ivk_physical_get( handle );
    if (physical) memory_properties2( physical->instance->fn.GetPhysicalDeviceMemoryProperties2, handle, props );
}

static void VKAPI_CALL ivk_GetPhysicalDeviceMemoryProperties2KHR( VkPhysicalDevice handle, VkPhysicalDeviceMemoryProperties2 *props )
{
    struct ivk_physical *physical = ivk_physical_get( handle );
    if (physical) memory_properties2( physical->instance->fn.GetPhysicalDeviceMemoryProperties2KHR, handle, props );
}

/* ---------------------------------------------------------------- E3: devices */

/* E3: the extensions the device is created with: the program's, plus the portability subset
 * when the physical device offers it (the Vulkan specification requires it then). NULL when
 * nothing needs adding; *count is the new count. */
static const char **device_extensions( struct ivk_instance *instance, VkPhysicalDevice physical,
                                       const VkDeviceCreateInfo *info, uint32_t *count )
{
    const char **names = NULL;

    if (has_name( info->ppEnabledExtensionNames, info->enabledExtensionCount, IVK_PORTABILITY_SUBSET_NAME )) return NULL;
    if (!offers_device_extension( instance, physical, IVK_PORTABILITY_SUBSET_NAME )) return NULL;

    if (!(names = malloc( (info->enabledExtensionCount + 1) * sizeof(*names) ))) return NULL;
    if (info->enabledExtensionCount)
        memcpy( names, info->ppEnabledExtensionNames, info->enabledExtensionCount * sizeof(*names) );
    names[info->enabledExtensionCount] = IVK_PORTABILITY_SUBSET_NAME;
    *count = info->enabledExtensionCount + 1;
    return names;
}

static VkResult VKAPI_CALL ivk_CreateDevice( VkPhysicalDevice handle, const VkDeviceCreateInfo *create_info,
                                             const VkAllocationCallbacks *alloc, VkDevice *out )
{
    struct ivk_physical *physical = ivk_physical_get( handle );
    struct create_device_args a;
    VkDeviceCreateInfo info;
    struct ivk_device *device;
    struct ivk_instance *instance;
    const char **names;
    uint32_t count = 0;
    int put, kind;

    if (!physical) return VK_ERROR_INITIALIZATION_FAILED;
    instance = physical->instance;
    info = *create_info;
    if ((names = device_extensions( instance, handle, create_info, &count )))
    {
        info.ppEnabledExtensionNames = names;
        info.enabledExtensionCount = count;
    }

    a.fn = instance->fn.CreateDevice; a.physical = handle; a.info = &info; a.alloc = alloc; a.out = out;
    a.res = VK_ERROR_INITIALIZATION_FAILED;
    ios_vk_pooled( create_device_thunk, &a );
    free( names );
    if (a.res != VK_SUCCESS)
    {
        ivk_explain_device_refusal( instance, handle, create_info, a.res );
        return a.res;
    }

    if (!(device = calloc( 1, sizeof(*device) ))) goto oom;
    device->handle = *out;
    device->physical = handle;
    device->instance = instance;
    pthread_mutex_init( &device->objects_lock, NULL );
    pthread_mutex_init( &device->pace_lock, NULL );
    for (kind = 0; kind < IVK_KIND_COUNT; kind++) memset( &device->objects[kind], 0, sizeof(device->objects[kind]) );
#define X(name) device->fn.name = (PFN_vk##name)instance->fn.GetDeviceProcAddr( *out, "vk" #name );
    IVK_DEVICE_FUNCS(X)
#undef X
    pthread_mutex_lock( &tables_lock );
    put = ivk_map_put( &devices, IVK_H64(*out), device );
    pthread_mutex_unlock( &tables_lock );
    if (!put)
    {
        table_full( "device" );
        ivk_device_free( device );
        goto oom;
    }
    return VK_SUCCESS;

oom:
    {
        PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)instance->fn.GetDeviceProcAddr( *out, "vkDestroyDevice" );
        if (destroy) destroy( *out, alloc );
        *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

static void VKAPI_CALL ivk_DestroyDevice( VkDevice handle, const VkAllocationCallbacks *alloc )
{
    struct ivk_device *device = ivk_device_take( handle );
    struct destroy_device_args a;

    if (!device) return;   /* not one of ours: nothing the layer can call */
    /* the program destroys its own objects; the record goes with the device */
    a.fn = device->fn.DestroyDevice; a.device = handle; a.alloc = alloc;
    ios_vk_pooled( destroy_device_thunk, &a );
    ivk_device_free( device );
}

static void record_queue( struct ivk_device *device, VkQueue handle )
{
    struct ivk_queue *queue;

    if (!handle || queue_get( handle )) return;
    if (!(queue = calloc( 1, sizeof(*queue) ))) return;
    queue->handle = handle;
    queue->device = device;
    pthread_mutex_lock( &tables_lock );
    if (ivk_map_get( &queues, IVK_H64(handle) ) || !ivk_map_put( &queues, IVK_H64(handle), queue ))
    {
        if (!ivk_map_get( &queues, IVK_H64(handle) )) table_full( "queue" );
        free( queue );
    }
    pthread_mutex_unlock( &tables_lock );
}

static void VKAPI_CALL ivk_GetDeviceQueue( VkDevice handle, uint32_t family, uint32_t index, VkQueue *out )
{
    struct ivk_device *device = ivk_device_get( handle );

    if (!device) { *out = VK_NULL_HANDLE; return; }
    device->fn.GetDeviceQueue( handle, family, index, out );
    record_queue( device, *out );
}

static void VKAPI_CALL ivk_GetDeviceQueue2( VkDevice handle, const VkDeviceQueueInfo2 *info, VkQueue *out )
{
    struct ivk_device *device = ivk_device_get( handle );

    if (!device || !device->fn.GetDeviceQueue2) { *out = VK_NULL_HANDLE; return; }
    device->fn.GetDeviceQueue2( handle, info, out );
    record_queue( device, *out );
}

/* ---------------------------------------------------------------- E2: queue work */

static VkResult VKAPI_CALL ivk_QueueSubmit( VkQueue handle, uint32_t count, const VkSubmitInfo *submits, VkFence fence )
{
    struct ivk_queue *queue = queue_get( handle );
    struct submit_args a;

    if (!queue) return VK_ERROR_DEVICE_LOST;
    a.fn = queue->device->fn.QueueSubmit; a.queue = handle; a.count = count; a.submits = submits; a.fence = fence;
    a.res = VK_ERROR_DEVICE_LOST;
    ios_vk_pooled( submit_thunk, &a );
    return a.res;
}

static VkResult submit2( PFN_vkQueueSubmit2 fn, VkQueue handle, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence )
{
    struct submit2_args a;

    if (!fn) return VK_ERROR_DEVICE_LOST;
    a.fn = fn; a.queue = handle; a.count = count; a.submits = submits; a.fence = fence; a.res = VK_ERROR_DEVICE_LOST;
    ios_vk_pooled( submit2_thunk, &a );
    return a.res;
}

static VkResult VKAPI_CALL ivk_QueueSubmit2( VkQueue handle, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence )
{
    struct ivk_queue *queue = queue_get( handle );
    return queue ? submit2( queue->device->fn.QueueSubmit2, handle, count, submits, fence ) : VK_ERROR_DEVICE_LOST;
}

static VkResult VKAPI_CALL ivk_QueueSubmit2KHR( VkQueue handle, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence )
{
    struct ivk_queue *queue = queue_get( handle );
    return queue ? submit2( queue->device->fn.QueueSubmit2KHR, handle, count, submits, fence ) : VK_ERROR_DEVICE_LOST;
}

static VkResult VKAPI_CALL ivk_QueueWaitIdle( VkQueue handle )
{
    struct ivk_queue *queue = queue_get( handle );
    struct queue_idle_args a;

    if (!queue) return VK_ERROR_DEVICE_LOST;
    a.fn = queue->device->fn.QueueWaitIdle; a.queue = handle; a.res = VK_ERROR_DEVICE_LOST;
    ios_vk_pooled( queue_idle_thunk, &a );
    return a.res;
}

static VkResult VKAPI_CALL ivk_DeviceWaitIdle( VkDevice handle )
{
    struct ivk_device *device = ivk_device_get( handle );
    struct device_idle_args a;

    if (!device) return VK_ERROR_DEVICE_LOST;
    a.fn = device->fn.DeviceWaitIdle; a.device = handle; a.res = VK_ERROR_DEVICE_LOST;
    ios_vk_pooled( device_idle_thunk, &a );
    return a.res;
}

/* ---------------------------------------------------------------- E5, E6: presenting */

static int pacing_enabled( void )
{
    static _Atomic int value = -1;
    int v = atomic_load( &value );

    if (v < 0)
    {
        const char *env = getenv( "PG_VK_PACING" );
        v = !(env && !strcmp( env, "0" ));
        atomic_store( &value, v );
    }
    return v;
}

static void pace( struct ivk_device *device )
{
    int mode = pg_get_vsync_locked ? pg_get_vsync_locked() : 0;
    uint64_t until;

    if (!pacing_enabled()) return;
    pthread_mutex_lock( &device->pace_lock );
    until = ivk_pace( &device->pacer, mode, ivk_now_ns() );
    pthread_mutex_unlock( &device->pace_lock );
    if (until) ivk_sleep_until_ns( until );
}

static VkResult VKAPI_CALL ivk_QueuePresentKHR( VkQueue handle, const VkPresentInfoKHR *info )
{
    static _Atomic int meter_logged;
    struct ivk_queue *queue = queue_get( handle );
    struct present_args a;

    if (!queue) return VK_ERROR_DEVICE_LOST;
    pace( queue->device );
    a.fn = queue->device->fn.QueuePresentKHR; a.queue = handle; a.info = info; a.res = VK_ERROR_DEVICE_LOST;
    ios_vk_pooled( present_thunk, &a );
    if (a.res >= 0 && pg_note_external_present) pg_note_external_present();
    if (!atomic_exchange( &meter_logged, 1 ))
        ivk_log( "the GPU meter (pg_gpu_meter_*) doesn't see Vulkan work; the present count does" );
    return a.res;
}

/* ---------------------------------------------------------------- E7: bounded waits */

static int kill_pending( void )
{
    return ios_thread_kill_pending ? ios_thread_kill_pending() : 0;
}

static const struct ivk_wait_env wait_env = { ivk_now_ns, kill_pending };

struct fences_wait
{
    PFN_vkWaitForFences fn; VkDevice device; uint32_t count; const VkFence *fences; VkBool32 all;
    uint64_t slice; VkResult res;
};
static void fences_thunk( void *p ) { struct fences_wait *a = p; a->res = a->fn( a->device, a->count, a->fences, a->all, a->slice ); }
static int32_t fences_call( void *ctx, uint64_t slice )
{
    struct fences_wait *a = ctx;
    a->slice = slice;
    ios_vk_pooled( fences_thunk, a );
    return a->res;
}

static VkResult VKAPI_CALL ivk_WaitForFences( VkDevice handle, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout )
{
    struct ivk_device *device = ivk_device_get( handle );
    struct fences_wait a;

    if (!device) return VK_ERROR_DEVICE_LOST;
    a.fn = device->fn.WaitForFences; a.device = handle; a.count = count; a.fences = fences; a.all = all;
    a.res = VK_ERROR_DEVICE_LOST;
    return (VkResult)ivk_sliced_wait( timeout, fences_call, &a, &wait_env, 0 );
}

struct semaphores_wait
{
    PFN_vkWaitSemaphores fn; VkDevice device; const VkSemaphoreWaitInfo *info; uint64_t slice; VkResult res;
};
static void semaphores_thunk( void *p ) { struct semaphores_wait *a = p; a->res = a->fn( a->device, a->info, a->slice ); }
static int32_t semaphores_call( void *ctx, uint64_t slice )
{
    struct semaphores_wait *a = ctx;
    a->slice = slice;
    ios_vk_pooled( semaphores_thunk, a );
    return a->res;
}

static VkResult wait_semaphores( PFN_vkWaitSemaphores fn, VkDevice handle, const VkSemaphoreWaitInfo *info, uint64_t timeout )
{
    struct semaphores_wait a;

    if (!fn) return VK_ERROR_DEVICE_LOST;
    a.fn = fn; a.device = handle; a.info = info; a.res = VK_ERROR_DEVICE_LOST;
    return (VkResult)ivk_sliced_wait( timeout, semaphores_call, &a, &wait_env, 0 );
}

static VkResult VKAPI_CALL ivk_WaitSemaphores( VkDevice handle, const VkSemaphoreWaitInfo *info, uint64_t timeout )
{
    struct ivk_device *device = ivk_device_get( handle );
    return device ? wait_semaphores( device->fn.WaitSemaphores, handle, info, timeout ) : VK_ERROR_DEVICE_LOST;
}

static VkResult VKAPI_CALL ivk_WaitSemaphoresKHR( VkDevice handle, const VkSemaphoreWaitInfo *info, uint64_t timeout )
{
    struct ivk_device *device = ivk_device_get( handle );
    return device ? wait_semaphores( device->fn.WaitSemaphoresKHR, handle, info, timeout ) : VK_ERROR_DEVICE_LOST;
}

struct acquire_wait
{
    PFN_vkAcquireNextImageKHR fn; VkDevice device; VkSwapchainKHR swapchain; VkSemaphore semaphore;
    VkFence fence; uint32_t *index; uint64_t slice; VkResult res;
};
static void acquire_thunk( void *p )
{
    struct acquire_wait *a = p;
    a->res = a->fn( a->device, a->swapchain, a->slice, a->semaphore, a->fence, a->index );
}
static int32_t acquire_call( void *ctx, uint64_t slice )
{
    struct acquire_wait *a = ctx;
    a->slice = slice;
    ios_vk_pooled( acquire_thunk, a );
    return a->res;
}

static VkResult VKAPI_CALL ivk_AcquireNextImageKHR( VkDevice handle, VkSwapchainKHR swapchain, uint64_t timeout,
                                                    VkSemaphore semaphore, VkFence fence, uint32_t *index )
{
    struct ivk_device *device = ivk_device_get( handle );
    struct acquire_wait a;

    if (!device) return VK_ERROR_DEVICE_LOST;
    a.fn = device->fn.AcquireNextImageKHR; a.device = handle; a.swapchain = swapchain; a.semaphore = semaphore;
    a.fence = fence; a.index = index; a.res = VK_ERROR_DEVICE_LOST;
    return (VkResult)ivk_sliced_wait( timeout, acquire_call, &a, &wait_env, 1 );
}

struct acquire2_wait
{
    PFN_vkAcquireNextImage2KHR fn; VkDevice device; VkAcquireNextImageInfoKHR info; uint32_t *index; VkResult res;
};
static void acquire2_thunk( void *p ) { struct acquire2_wait *a = p; a->res = a->fn( a->device, &a->info, a->index ); }
static int32_t acquire2_call( void *ctx, uint64_t slice )
{
    struct acquire2_wait *a = ctx;
    a->info.timeout = slice;
    ios_vk_pooled( acquire2_thunk, a );
    return a->res;
}

static VkResult VKAPI_CALL ivk_AcquireNextImage2KHR( VkDevice handle, const VkAcquireNextImageInfoKHR *info, uint32_t *index )
{
    struct ivk_device *device = ivk_device_get( handle );
    struct acquire2_wait a;

    if (!device || !device->fn.AcquireNextImage2KHR) return VK_ERROR_DEVICE_LOST;
    a.fn = device->fn.AcquireNextImage2KHR; a.device = handle; a.info = *info; a.index = index;
    a.res = VK_ERROR_DEVICE_LOST;
    return (VkResult)ivk_sliced_wait( info->timeout, acquire2_call, &a, &wait_env, 1 );
}

/* ---------------------------------------------------------------- E2, E8: objects */

/* a recorded kind made by vkCreate<name>(device, info, allocator, handle) and destroyed by
 * vkDestroy<destroy>(device, handle, allocator); pooled says whether E2 wraps the create */
#define IVK_CREATE(name, info_type, handle_type, kind, pooled) \
static VkResult VKAPI_CALL ivk_Create##name( VkDevice handle, const info_type *info, \
                                             const VkAllocationCallbacks *alloc, handle_type *out ) \
{ \
    struct ivk_device *device = ivk_device_get( handle ); \
    VkResult res; \
    if (!device) return VK_ERROR_DEVICE_LOST; \
    if (pooled) res = pooled_create( (void *)device->fn.Create##name, handle, info, alloc, out ); \
    else res = device->fn.Create##name( handle, info, alloc, out ); \
    if (res == VK_SUCCESS) track( device, kind, IVK_H64(*out) ); \
    return res; \
}

#define IVK_DESTROY(name, handle_type, kind) \
static void VKAPI_CALL ivk_Destroy##name( VkDevice handle, handle_type object, const VkAllocationCallbacks *alloc ) \
{ \
    struct ivk_device *device = ivk_device_get( handle ); \
    if (!device) return; \
    untrack( device, kind, IVK_H64(object) ); \
    device->fn.Destroy##name( handle, object, alloc ); \
}

IVK_CREATE(Framebuffer, VkFramebufferCreateInfo, VkFramebuffer, IVK_FRAMEBUFFER, 0)
IVK_DESTROY(Framebuffer, VkFramebuffer, IVK_FRAMEBUFFER)
IVK_CREATE(RenderPass, VkRenderPassCreateInfo, VkRenderPass, IVK_RENDER_PASS, 0)
IVK_CREATE(RenderPass2, VkRenderPassCreateInfo2, VkRenderPass, IVK_RENDER_PASS, 0)
IVK_CREATE(RenderPass2KHR, VkRenderPassCreateInfo2, VkRenderPass, IVK_RENDER_PASS, 0)
IVK_DESTROY(RenderPass, VkRenderPass, IVK_RENDER_PASS)
IVK_DESTROY(Pipeline, VkPipeline, IVK_PIPELINE)
IVK_CREATE(PipelineCache, VkPipelineCacheCreateInfo, VkPipelineCache, IVK_PIPELINE_CACHE, 0)
IVK_DESTROY(PipelineCache, VkPipelineCache, IVK_PIPELINE_CACHE)
IVK_CREATE(ShaderModule, VkShaderModuleCreateInfo, VkShaderModule, IVK_SHADER_MODULE, 1)
IVK_DESTROY(ShaderModule, VkShaderModule, IVK_SHADER_MODULE)
IVK_CREATE(PipelineLayout, VkPipelineLayoutCreateInfo, VkPipelineLayout, IVK_PIPELINE_LAYOUT, 0)
IVK_DESTROY(PipelineLayout, VkPipelineLayout, IVK_PIPELINE_LAYOUT)
IVK_CREATE(DescriptorPool, VkDescriptorPoolCreateInfo, VkDescriptorPool, IVK_DESCRIPTOR_POOL, 0)
IVK_DESTROY(DescriptorPool, VkDescriptorPool, IVK_DESCRIPTOR_POOL)
IVK_CREATE(DescriptorSetLayout, VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout, IVK_DESCRIPTOR_SET_LAYOUT, 0)
IVK_DESTROY(DescriptorSetLayout, VkDescriptorSetLayout, IVK_DESCRIPTOR_SET_LAYOUT)
IVK_CREATE(CommandPool, VkCommandPoolCreateInfo, VkCommandPool, IVK_COMMAND_POOL, 0)
IVK_DESTROY(CommandPool, VkCommandPool, IVK_COMMAND_POOL)
IVK_CREATE(QueryPool, VkQueryPoolCreateInfo, VkQueryPool, IVK_QUERY_POOL, 0)
IVK_DESTROY(QueryPool, VkQueryPool, IVK_QUERY_POOL)
IVK_CREATE(Event, VkEventCreateInfo, VkEvent, IVK_EVENT, 0)
IVK_DESTROY(Event, VkEvent, IVK_EVENT)
IVK_CREATE(Fence, VkFenceCreateInfo, VkFence, IVK_FENCE, 0)
IVK_DESTROY(Fence, VkFence, IVK_FENCE)
IVK_CREATE(Semaphore, VkSemaphoreCreateInfo, VkSemaphore, IVK_SEMAPHORE, 0)
IVK_DESTROY(Semaphore, VkSemaphore, IVK_SEMAPHORE)
IVK_CREATE(BufferView, VkBufferViewCreateInfo, VkBufferView, IVK_BUFFER_VIEW, 0)
IVK_DESTROY(BufferView, VkBufferView, IVK_BUFFER_VIEW)
IVK_CREATE(ImageView, VkImageViewCreateInfo, VkImageView, IVK_IMAGE_VIEW, 0)
IVK_DESTROY(ImageView, VkImageView, IVK_IMAGE_VIEW)
IVK_CREATE(Sampler, VkSamplerCreateInfo, VkSampler, IVK_SAMPLER, 0)
IVK_DESTROY(Sampler, VkSampler, IVK_SAMPLER)
IVK_CREATE(Image, VkImageCreateInfo, VkImage, IVK_IMAGE, 1)
IVK_DESTROY(Image, VkImage, IVK_IMAGE)
IVK_CREATE(Buffer, VkBufferCreateInfo, VkBuffer, IVK_BUFFER, 1)
IVK_DESTROY(Buffer, VkBuffer, IVK_BUFFER)
IVK_CREATE(SwapchainKHR, VkSwapchainCreateInfoKHR, VkSwapchainKHR, IVK_SWAPCHAIN, 1)

static void VKAPI_CALL ivk_DestroySwapchainKHR( VkDevice handle, VkSwapchainKHR swapchain, const VkAllocationCallbacks *alloc )
{
    struct ivk_device *device = ivk_device_get( handle );
    struct destroy_swapchain_args a;

    if (!device) return;
    untrack( device, IVK_SWAPCHAIN, IVK_H64(swapchain) );
    a.fn = device->fn.DestroySwapchainKHR; a.device = handle; a.swapchain = swapchain; a.alloc = alloc;
    ios_vk_pooled( destroy_swapchain_thunk, &a );
}

static VkResult VKAPI_CALL ivk_AllocateMemory( VkDevice handle, const VkMemoryAllocateInfo *info,
                                               const VkAllocationCallbacks *alloc, VkDeviceMemory *out )
{
    struct ivk_device *device = ivk_device_get( handle );
    VkResult res;

    if (!device) return VK_ERROR_DEVICE_LOST;
    res = pooled_create( (void *)device->fn.AllocateMemory, handle, info, alloc, out );
    if (res == VK_SUCCESS) track( device, IVK_MEMORY, IVK_H64(*out) );
    return res;
}

static void VKAPI_CALL ivk_FreeMemory( VkDevice handle, VkDeviceMemory memory, const VkAllocationCallbacks *alloc )
{
    struct ivk_device *device = ivk_device_get( handle );

    if (!device) return;
    untrack( device, IVK_MEMORY, IVK_H64(memory) );
    device->fn.FreeMemory( handle, memory, alloc );
}

static VkResult create_pipelines( struct ivk_device *device, void *fn, VkDevice handle, VkPipelineCache cache,
                                  uint32_t count, const void *infos, const VkAllocationCallbacks *alloc, VkPipeline *out )
{
    struct pipelines_args a;
    uint32_t i;

    a.fn = (VkResult (VKAPI_PTR *)( VkDevice, VkPipelineCache, uint32_t, const void *, const VkAllocationCallbacks *, VkPipeline * ))fn;
    a.device = handle; a.cache = cache; a.count = count; a.infos = infos; a.alloc = alloc; a.out = out;
    a.res = VK_ERROR_DEVICE_LOST;
    ios_vk_pooled( pipelines_thunk, &a );
    /* every pipeline that was made, also when another of the batch failed */
    for (i = 0; i < count; i++) track( device, IVK_PIPELINE, IVK_H64(out[i]) );
    return a.res;
}

static VkResult VKAPI_CALL ivk_CreateGraphicsPipelines( VkDevice handle, VkPipelineCache cache, uint32_t count,
                                                        const VkGraphicsPipelineCreateInfo *infos,
                                                        const VkAllocationCallbacks *alloc, VkPipeline *out )
{
    struct ivk_device *device = ivk_device_get( handle );
    if (!device) return VK_ERROR_DEVICE_LOST;
    return create_pipelines( device, (void *)device->fn.CreateGraphicsPipelines, handle, cache, count, infos, alloc, out );
}

static VkResult VKAPI_CALL ivk_CreateComputePipelines( VkDevice handle, VkPipelineCache cache, uint32_t count,
                                                       const VkComputePipelineCreateInfo *infos,
                                                       const VkAllocationCallbacks *alloc, VkPipeline *out )
{
    struct ivk_device *device = ivk_device_get( handle );
    if (!device) return VK_ERROR_DEVICE_LOST;
    return create_pipelines( device, (void *)device->fn.CreateComputePipelines, handle, cache, count, infos, alloc, out );
}

/* ---------------------------------------------------------------- E1: routing */

struct ivk_hook
{
    const char        *name;
    PFN_vkVoidFunction fn;
    int                device_level;   /* also answered by vkGetDeviceProcAddr */
};

#define HOOK(name, level) { "vk" #name, (PFN_vkVoidFunction)ivk_##name, level }
static const struct ivk_hook hooks[] =
{
    HOOK(CreateInstance, 0),
    HOOK(DestroyInstance, 0),
    HOOK(EnumeratePhysicalDevices, 0),
    HOOK(EnumeratePhysicalDeviceGroups, 0),
    HOOK(EnumeratePhysicalDeviceGroupsKHR, 0),
    HOOK(CreateDevice, 0),
    HOOK(GetPhysicalDeviceMemoryProperties, 0),
    HOOK(GetPhysicalDeviceMemoryProperties2, 0),
    HOOK(GetPhysicalDeviceMemoryProperties2KHR, 0),
    HOOK(DestroySurfaceKHR, 0),
    HOOK(DestroyDevice, 1),
    HOOK(GetDeviceQueue, 1),
    HOOK(GetDeviceQueue2, 1),
    HOOK(QueueSubmit, 1),
    HOOK(QueueSubmit2, 1),
    HOOK(QueueSubmit2KHR, 1),
    HOOK(QueueWaitIdle, 1),
    HOOK(DeviceWaitIdle, 1),
    HOOK(QueuePresentKHR, 1),
    HOOK(AcquireNextImageKHR, 1),
    HOOK(AcquireNextImage2KHR, 1),
    HOOK(WaitForFences, 1),
    HOOK(WaitSemaphores, 1),
    HOOK(WaitSemaphoresKHR, 1),
    HOOK(AllocateMemory, 1),
    HOOK(FreeMemory, 1),
    HOOK(CreateImage, 1),
    HOOK(DestroyImage, 1),
    HOOK(CreateBuffer, 1),
    HOOK(DestroyBuffer, 1),
    HOOK(CreateImageView, 1),
    HOOK(DestroyImageView, 1),
    HOOK(CreateBufferView, 1),
    HOOK(DestroyBufferView, 1),
    HOOK(CreateSampler, 1),
    HOOK(DestroySampler, 1),
    HOOK(CreateGraphicsPipelines, 1),
    HOOK(CreateComputePipelines, 1),
    HOOK(DestroyPipeline, 1),
    HOOK(CreatePipelineLayout, 1),
    HOOK(DestroyPipelineLayout, 1),
    HOOK(CreatePipelineCache, 1),
    HOOK(DestroyPipelineCache, 1),
    HOOK(CreateShaderModule, 1),
    HOOK(DestroyShaderModule, 1),
    HOOK(CreateDescriptorPool, 1),
    HOOK(DestroyDescriptorPool, 1),
    HOOK(CreateDescriptorSetLayout, 1),
    HOOK(DestroyDescriptorSetLayout, 1),
    HOOK(CreateCommandPool, 1),
    HOOK(DestroyCommandPool, 1),
    HOOK(CreateQueryPool, 1),
    HOOK(DestroyQueryPool, 1),
    HOOK(CreateSemaphore, 1),
    HOOK(DestroySemaphore, 1),
    HOOK(CreateFence, 1),
    HOOK(DestroyFence, 1),
    HOOK(CreateEvent, 1),
    HOOK(DestroyEvent, 1),
    HOOK(CreateSwapchainKHR, 1),
    HOOK(DestroySwapchainKHR, 1),
    HOOK(CreateRenderPass, 1),
    HOOK(CreateRenderPass2, 1),
    HOOK(CreateRenderPass2KHR, 1),
    HOOK(DestroyRenderPass, 1),
    HOOK(CreateFramebuffer, 1),
    HOOK(DestroyFramebuffer, 1),
};
#undef HOOK

static const struct ivk_hook *find_hook( const char *name )
{
    size_t i;

    if (name[0] != 'v' || name[1] != 'k') return NULL;
    for (i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++)
        if (!strcmp( hooks[i].name, name )) return &hooks[i];
    return NULL;
}

PFN_vkVoidFunction ios_vk_host_get_instance_proc_addr( VkInstance instance, const char *name )
{
    const struct ivk_hook *hook;
    PFN_vkVoidFunction driver;

    if (!name) return NULL;
    if (!strcmp( name, "vkGetInstanceProcAddr" )) return (PFN_vkVoidFunction)ios_vk_host_get_instance_proc_addr;
    /* a name the driver doesn't know stays unknown, also for the layer's own wrappers */
    if (!(driver = IOS_VK_DRIVER_GIPA( instance, name ))) return NULL;
    if (!strcmp( name, "vkGetDeviceProcAddr" )) return (PFN_vkVoidFunction)ios_vk_host_get_device_proc_addr;
    if ((hook = find_hook( name ))) return hook->fn;
    return driver;
}

PFN_vkVoidFunction ios_vk_host_get_device_proc_addr( VkDevice handle, const char *name )
{
    struct ivk_device *device = ivk_device_get( handle );
    const struct ivk_hook *hook;
    PFN_vkVoidFunction driver;

    if (!name || !device || !device->instance->fn.GetDeviceProcAddr) return NULL;
    if (!strcmp( name, "vkGetDeviceProcAddr" )) return (PFN_vkVoidFunction)ios_vk_host_get_device_proc_addr;
    if (!(driver = device->instance->fn.GetDeviceProcAddr( handle, name ))) return NULL;
    if ((hook = find_hook( name )) && hook->device_level) return hook->fn;
    return driver;
}
