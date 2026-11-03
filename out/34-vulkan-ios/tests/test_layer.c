// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part M1: host tests of the iOS Vulkan layer (out/34-vulkan-ios/layer/). No GPU and no
// real Vulkan driver: a fake driver below (bound as IOS_VK_DRIVER_GIPA) records what the layer
// calls, and a fake platform gives the clock, the memory readings and the log. Built by
// tests/run-host-tests.sh against Wine 11.4's include/wine/vulkan.h.

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ios_vk_internal.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while (0)

/* ================================================================ fake platform */

static _Atomic uint64_t fake_now = 1000000000ull;
static uint64_t last_sleep_until;
static unsigned int sleeps;
static uint64_t fake_footprint_value, fake_available_value, fake_memsize_value;
static unsigned int footprint_reads;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char logbuf[16384];

uint64_t ivk_now_ns( void ) { return atomic_load( &fake_now ); }
void ivk_sleep_until_ns( uint64_t deadline )
{
    last_sleep_until = deadline;
    sleeps++;
    if (deadline > atomic_load( &fake_now )) atomic_store( &fake_now, deadline );
}
uint64_t ivk_footprint( void ) { footprint_reads++; return fake_footprint_value; }
uint64_t ivk_available_memory( void ) { return fake_available_value; }
uint64_t ivk_memsize( void ) { return fake_memsize_value; }

void ivk_log( const char *fmt, ... )
{
    char line[700];
    va_list args;

    va_start( args, fmt );
    vsnprintf( line, sizeof(line), fmt, args );
    va_end( args );
    printf( "  log: [vk-ios] %s\n", line );
    pthread_mutex_lock( &log_mutex );
    if (strlen( logbuf ) + strlen( line ) + 2 < sizeof(logbuf)) { strcat( logbuf, line ); strcat( logbuf, "\n" ); }
    pthread_mutex_unlock( &log_mutex );
}

static int log_has( const char *text )
{
    int found;
    pthread_mutex_lock( &log_mutex );
    found = strstr( logbuf, text ) != NULL;
    pthread_mutex_unlock( &log_mutex );
    return found;
}

static void log_clear( void )
{
    pthread_mutex_lock( &log_mutex );
    logbuf[0] = 0;
    pthread_mutex_unlock( &log_mutex );
}

static unsigned int pooled_calls;
void ios_vk_pooled( void (*fn)( void *ctx ), void *ctx ) { pooled_calls++; fn( ctx ); }

/* the other parts' hooks, defined here so the layer's weak references find them */
static int vsync_mode;
static unsigned int external_presents;
static int kill_flag;
int pg_get_vsync_locked( void ) { return vsync_mode; }
void pg_note_external_present( void ) { external_presents++; }
int ios_thread_kill_pending( void ) { return kill_flag; }

static unsigned int current_pid = 0x20;
static unsigned int test_pid( void ) { return current_pid; }

/* ================================================================ fake driver */

/* every instance its own handle and physical device, as a real driver gives them */
static char instance_objs[64], physical_objs[64], device_obj, device2_obj, queue_obj;
static unsigned int ninstances;
static VkPhysicalDevice physical_of( VkInstance instance )
{
    return (VkPhysicalDevice)(void *)&physical_objs[(char *)(void *)instance - instance_objs];
}
static VkDevice next_device = (VkDevice)(void *)&device_obj;
#define FAKE_QUEUE ((VkQueue)(void *)&queue_obj)

/* what the fake saw */
static int saw_log_setting, saw_layer_settings_ext, saw_portability, device_ext_count;
static int offer_portability = 1;
/* a GPU with Metal mesh shaders: geometryShader, and VK_EXT_transform_feedback with its feature */
static int mesh_gpu = 0, saw_xfb_unoffered = 0;
static uint64_t next_handle = 0x1000;

struct event { char what[32]; uint64_t handle; };
static struct event events[256];
static unsigned int nevents;
static pthread_mutex_t events_mutex = PTHREAD_MUTEX_INITIALIZER;

static void event( const char *what, uint64_t handle )
{
    pthread_mutex_lock( &events_mutex );
    if (nevents < 256)
    {
        snprintf( events[nevents].what, sizeof(events[nevents].what), "%s", what );
        events[nevents].handle = handle;
        nevents++;
    }
    pthread_mutex_unlock( &events_mutex );
}

static const char *const instance_exts[] = { "VK_KHR_surface", "VK_EXT_metal_surface", "VK_EXT_layer_settings" };

static VkResult fill_exts( const char *const *names, uint32_t n, uint32_t *count, VkExtensionProperties *props )
{
    uint32_t i;
    if (!props) { *count = n; return VK_SUCCESS; }
    for (i = 0; i < n && i < *count; i++)
    {
        memset( &props[i], 0, sizeof(props[i]) );
        snprintf( props[i].extensionName, sizeof(props[i].extensionName), "%s", names[i] );
    }
    *count = i;
    return i < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_EnumerateInstanceExtensionProperties( const char *layer, uint32_t *count, VkExtensionProperties *props )
{
    (void)layer;
    return fill_exts( instance_exts, 3, count, props );
}

static VkResult VKAPI_CALL fake_CreateInstance( const VkInstanceCreateInfo *info, const VkAllocationCallbacks *alloc, VkInstance *out )
{
    const VkBaseInStructure *s;
    uint32_t i, j;

    (void)alloc;
    saw_log_setting = saw_layer_settings_ext = 0;
    for (s = info->pNext; s; s = s->pNext)
    {
        if (s->sType != VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT) continue;
        const VkLayerSettingsCreateInfoEXT *ls = (const VkLayerSettingsCreateInfoEXT *)s;
        for (i = 0; i < ls->settingCount; i++)
            if (!strcmp( ls->pSettings[i].pSettingName, "MVK_CONFIG_LOG_LEVEL" ) &&
                ls->pSettings[i].type == VK_LAYER_SETTING_TYPE_INT32_EXT &&
                *(const int32_t *)ls->pSettings[i].pValues == 1) saw_log_setting = 1;
    }
    for (i = 0; i < info->enabledExtensionCount; i++)
    {
        if (!strcmp( info->ppEnabledExtensionNames[i], "VK_EXT_layer_settings" )) saw_layer_settings_ext = 1;
        for (j = 0; j < 3; j++) if (!strcmp( info->ppEnabledExtensionNames[i], instance_exts[j] )) break;
        if (j == 3) return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    *out = (VkInstance)(void *)&instance_objs[ninstances++ % 64];
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_DestroyInstance( VkInstance instance, const VkAllocationCallbacks *alloc )
{
    (void)alloc;
    event( "instance", IVK_H64(instance) );
}

static VkResult VKAPI_CALL fake_EnumeratePhysicalDevices( VkInstance instance, uint32_t *count, VkPhysicalDevice *out )
{
    if (!out) { *count = 1; return VK_SUCCESS; }
    if (*count < 1) return VK_INCOMPLETE;
    out[0] = physical_of( instance );
    *count = 1;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_EnumeratePhysicalDeviceGroups( VkInstance instance, uint32_t *count, VkPhysicalDeviceGroupProperties *props )
{
    if (!props) { *count = 1; return VK_SUCCESS; }
    props[0].physicalDeviceCount = 1;
    props[0].physicalDevices[0] = physical_of( instance );
    *count = 1;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_EnumerateDeviceExtensionProperties( VkPhysicalDevice physical, const char *layer,
                                                                    uint32_t *count, VkExtensionProperties *props )
{
    const char *exts[3];
    uint32_t n = 0;
    (void)physical; (void)layer;
    exts[n++] = "VK_KHR_swapchain";
    if (offer_portability) exts[n++] = "VK_KHR_portability_subset";
    if (mesh_gpu) exts[n++] = "VK_EXT_transform_feedback";
    return fill_exts( exts, n, count, props );
}

static void VKAPI_CALL fake_GetPhysicalDeviceFeatures( VkPhysicalDevice physical, VkPhysicalDeviceFeatures *features )
{
    uint32_t *f = (uint32_t *)features;
    size_t i;
    (void)physical;
    for (i = 0; i < sizeof(*features) / 4; i++) f[i] = VK_TRUE;
    features->geometryShader = mesh_gpu ? VK_TRUE : VK_FALSE;
}

static void VKAPI_CALL fake_GetPhysicalDeviceFeatures2( VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *features )
{
    VkBaseOutStructure *s;
    fake_GetPhysicalDeviceFeatures( physical, &features->features );
    for (s = features->pNext; s; s = s->pNext)
    {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT)
        {
            VkPhysicalDeviceTransformFeedbackFeaturesEXT *xfb = (VkPhysicalDeviceTransformFeedbackFeaturesEXT *)s;
            if (!mesh_gpu) saw_xfb_unoffered = 1;
            xfb->transformFeedback = xfb->geometryStreams = mesh_gpu ? VK_TRUE : VK_FALSE;
        }
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        {
            /* everything of Vulkan 1.2 but drawIndirectCount */
            VkPhysicalDeviceVulkan12Features *v12 = (VkPhysicalDeviceVulkan12Features *)s;
            VkBool32 *b = &v12->samplerMirrorClampToEdge;
            size_t i, n = (sizeof(*v12) - offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / 4;
            for (i = 0; i < n; i++) b[i] = VK_TRUE;
            v12->drawIndirectCount = VK_FALSE;
        }
    }
}

static int device_wanted_bad( const VkDeviceCreateInfo *info )
{
    const VkBaseInStructure *s;
    const VkPhysicalDeviceFeatures *core = info->pEnabledFeatures;

    for (s = info->pNext; s; s = s->pNext)
    {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) core = &((const VkPhysicalDeviceFeatures2 *)s)->features;
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES &&
            ((const VkPhysicalDeviceVulkan12Features *)s)->drawIndirectCount) return 1;
    }
    return core && core->geometryShader;
}

static VkResult VKAPI_CALL fake_CreateDevice( VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                              const VkAllocationCallbacks *alloc, VkDevice *out )
{
    uint32_t i;
    (void)physical; (void)alloc;
    saw_portability = 0;
    device_ext_count = (int)info->enabledExtensionCount;
    for (i = 0; i < info->enabledExtensionCount; i++)
    {
        const char *n = info->ppEnabledExtensionNames[i];
        if (!strcmp( n, "VK_KHR_portability_subset" )) { saw_portability++; continue; }
        if (strcmp( n, "VK_KHR_swapchain" )) return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (device_wanted_bad( info )) return VK_ERROR_FEATURE_NOT_PRESENT;
    *out = next_device;
    return VK_SUCCESS;
}

static void VKAPI_CALL fake_GetPhysicalDeviceProperties( VkPhysicalDevice physical, VkPhysicalDeviceProperties *props )
{
    (void)physical;
    memset( props, 0, sizeof(*props) );
    props->apiVersion = VK_MAKE_API_VERSION( 0, 1, 3, 7 );
    snprintf( props->deviceName, sizeof(props->deviceName), "Fake GPU" );
}

static void VKAPI_CALL fake_GetPhysicalDeviceProperties2( VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *props )
{
    VkBaseOutStructure *s;
    fake_GetPhysicalDeviceProperties( physical, &props->properties );
    for (s = props->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES)
        {
            VkPhysicalDeviceDriverProperties *d = (VkPhysicalDeviceDriverProperties *)s;
            snprintf( d->driverName, sizeof(d->driverName), "FakeVK" );
            snprintf( d->driverInfo, sizeof(d->driverInfo), "9.9.1" );
        }
}

static uint64_t heap0_size = 8192 * IVK_MB, heap0_budget = 7168 * IVK_MB;

static void VKAPI_CALL fake_GetPhysicalDeviceMemoryProperties( VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties *props )
{
    (void)physical;
    memset( props, 0, sizeof(*props) );
    props->memoryHeapCount = 2;
    props->memoryHeaps[0].size = heap0_size;
    props->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    props->memoryHeaps[1].size = 4096 * IVK_MB;
    props->memoryHeaps[1].flags = 0;
}

static void VKAPI_CALL fake_GetPhysicalDeviceMemoryProperties2( VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties2 *props )
{
    VkBaseOutStructure *s;
    fake_GetPhysicalDeviceMemoryProperties( physical, &props->memoryProperties );
    for (s = props->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
        {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT *b = (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)s;
            b->heapBudget[0] = heap0_budget;
            b->heapUsage[0] = 100 * IVK_MB;
            b->heapBudget[1] = 3000 * IVK_MB;
            b->heapUsage[1] = 200 * IVK_MB;
        }
}

static void VKAPI_CALL fake_DestroySurfaceKHR( VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *alloc )
{
    (void)instance; (void)alloc;
    event( "surface", IVK_H64(surface) );
}

/* ---- device level */

static void VKAPI_CALL fake_DestroyDevice( VkDevice device, const VkAllocationCallbacks *alloc )
{
    (void)alloc;
    event( "device", IVK_H64(device) );
}

static void VKAPI_CALL fake_GetDeviceQueue( VkDevice device, uint32_t family, uint32_t index, VkQueue *out )
{
    (void)device; (void)family; (void)index;
    *out = FAKE_QUEUE;
}

static void VKAPI_CALL fake_GetDeviceQueue2( VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *out )
{
    (void)device; (void)info;
    *out = FAKE_QUEUE;
}

static unsigned int submits;
static VkResult VKAPI_CALL fake_QueueSubmit( VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f )
{ (void)q; (void)n; (void)s; (void)f; submits++; return VK_SUCCESS; }
static VkResult VKAPI_CALL fake_QueueSubmit2( VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f )
{ (void)q; (void)n; (void)s; (void)f; submits++; return VK_SUCCESS; }
static VkResult VKAPI_CALL fake_QueueWaitIdle( VkQueue q ) { (void)q; return VK_SUCCESS; }

/* vkDeviceWaitIdle can be told to hang until the test lets it go */
static pthread_mutex_t hang_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hang_cond = PTHREAD_COND_INITIALIZER;
static int hang_idle, idle_returned;
static VkResult VKAPI_CALL fake_DeviceWaitIdle( VkDevice d )
{
    (void)d;
    pthread_mutex_lock( &hang_mutex );
    while (hang_idle) pthread_cond_wait( &hang_cond, &hang_mutex );
    idle_returned++;
    pthread_mutex_unlock( &hang_mutex );
    event( "idle", IVK_H64(d) );
    return VK_SUCCESS;
}

static VkResult present_result = VK_SUCCESS;
static unsigned int driver_presents;
static VkResult VKAPI_CALL fake_QueuePresentKHR( VkQueue q, const VkPresentInfoKHR *info )
{ (void)q; (void)info; driver_presents++; return present_result; }

/* waits: each call advances the fake clock by its timeout and answers from a script */
static uint64_t slices[64];
static unsigned int nslices;
static int succeed_on_slice;   /* 1-based, 0 = never */
static int32_t slice_answer( uint64_t timeout, int32_t timeout_result )
{
    if (nslices < 64) slices[nslices] = timeout;
    nslices++;
    if (succeed_on_slice && (int)nslices == succeed_on_slice) return VK_SUCCESS;
    atomic_fetch_add( &fake_now, timeout );
    return timeout_result;
}
static VkResult VKAPI_CALL fake_WaitForFences( VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t )
{ (void)d; (void)n; (void)f; (void)all; return (VkResult)slice_answer( t, VK_TIMEOUT ); }
static VkResult VKAPI_CALL fake_WaitSemaphores( VkDevice d, const VkSemaphoreWaitInfo *i, uint64_t t )
{ (void)d; (void)i; return (VkResult)slice_answer( t, VK_TIMEOUT ); }
static VkResult VKAPI_CALL fake_AcquireNextImageKHR( VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore sem, VkFence f, uint32_t *idx )
{ (void)d; (void)s; (void)sem; (void)f; *idx = 0; return (VkResult)slice_answer( t, t ? VK_TIMEOUT : VK_NOT_READY ); }
static VkResult VKAPI_CALL fake_AcquireNextImage2KHR( VkDevice d, const VkAcquireNextImageInfoKHR *i, uint32_t *idx )
{ (void)d; *idx = 0; return (VkResult)slice_answer( i->timeout, i->timeout ? VK_TIMEOUT : VK_NOT_READY ); }

/* creates and destroys of every recorded kind */
#define FAKE_CREATE(name, info_type, type) \
static VkResult VKAPI_CALL fake_Create##name( VkDevice d, const info_type *i, const VkAllocationCallbacks *a, type *out ) \
{ (void)d; (void)i; (void)a; *out = IVK_HANDLE(type, next_handle++); return VK_SUCCESS; }
#define FAKE_DESTROY(name, type, what) \
static void VKAPI_CALL fake_Destroy##name( VkDevice d, type h, const VkAllocationCallbacks *a ) \
{ (void)d; (void)a; event( what, IVK_H64(h) ); }

FAKE_CREATE(Framebuffer, VkFramebufferCreateInfo, VkFramebuffer) FAKE_DESTROY(Framebuffer, VkFramebuffer, "framebuffer")
FAKE_CREATE(RenderPass, VkRenderPassCreateInfo, VkRenderPass)
FAKE_CREATE(RenderPass2, VkRenderPassCreateInfo2, VkRenderPass) FAKE_DESTROY(RenderPass, VkRenderPass, "render pass")
FAKE_DESTROY(Pipeline, VkPipeline, "pipeline")
FAKE_CREATE(PipelineCache, VkPipelineCacheCreateInfo, VkPipelineCache) FAKE_DESTROY(PipelineCache, VkPipelineCache, "pipeline cache")
FAKE_CREATE(ShaderModule, VkShaderModuleCreateInfo, VkShaderModule) FAKE_DESTROY(ShaderModule, VkShaderModule, "shader module")
FAKE_CREATE(PipelineLayout, VkPipelineLayoutCreateInfo, VkPipelineLayout) FAKE_DESTROY(PipelineLayout, VkPipelineLayout, "pipeline layout")
FAKE_CREATE(DescriptorPool, VkDescriptorPoolCreateInfo, VkDescriptorPool) FAKE_DESTROY(DescriptorPool, VkDescriptorPool, "descriptor pool")
FAKE_CREATE(DescriptorSetLayout, VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout) FAKE_DESTROY(DescriptorSetLayout, VkDescriptorSetLayout, "set layout")
FAKE_CREATE(CommandPool, VkCommandPoolCreateInfo, VkCommandPool) FAKE_DESTROY(CommandPool, VkCommandPool, "command pool")
FAKE_CREATE(QueryPool, VkQueryPoolCreateInfo, VkQueryPool) FAKE_DESTROY(QueryPool, VkQueryPool, "query pool")
FAKE_CREATE(Event, VkEventCreateInfo, VkEvent) FAKE_DESTROY(Event, VkEvent, "event")
FAKE_CREATE(Fence, VkFenceCreateInfo, VkFence) FAKE_DESTROY(Fence, VkFence, "fence")
FAKE_CREATE(Semaphore, VkSemaphoreCreateInfo, VkSemaphore) FAKE_DESTROY(Semaphore, VkSemaphore, "semaphore")
FAKE_CREATE(BufferView, VkBufferViewCreateInfo, VkBufferView) FAKE_DESTROY(BufferView, VkBufferView, "buffer view")
FAKE_CREATE(ImageView, VkImageViewCreateInfo, VkImageView) FAKE_DESTROY(ImageView, VkImageView, "image view")
FAKE_CREATE(Sampler, VkSamplerCreateInfo, VkSampler) FAKE_DESTROY(Sampler, VkSampler, "sampler")
FAKE_CREATE(Image, VkImageCreateInfo, VkImage) FAKE_DESTROY(Image, VkImage, "image")
FAKE_CREATE(Buffer, VkBufferCreateInfo, VkBuffer) FAKE_DESTROY(Buffer, VkBuffer, "buffer")
FAKE_CREATE(SwapchainKHR, VkSwapchainCreateInfoKHR, VkSwapchainKHR) FAKE_DESTROY(SwapchainKHR, VkSwapchainKHR, "swapchain")

static VkResult VKAPI_CALL fake_AllocateMemory( VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a, VkDeviceMemory *out )
{ (void)d; (void)i; (void)a; *out = IVK_HANDLE(VkDeviceMemory, next_handle++); return VK_SUCCESS; }
static void VKAPI_CALL fake_FreeMemory( VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a )
{ (void)d; (void)a; event( "memory", IVK_H64(m) ); }

static VkResult VKAPI_CALL fake_CreateGraphicsPipelines( VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *i,
                                                         const VkAllocationCallbacks *a, VkPipeline *out )
{
    uint32_t k;
    (void)d; (void)c; (void)i; (void)a;
    for (k = 0; k < n; k++) out[k] = IVK_HANDLE(VkPipeline, next_handle++);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_CreateComputePipelines( VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *i,
                                                        const VkAllocationCallbacks *a, VkPipeline *out )
{
    uint32_t k;
    (void)d; (void)c; (void)i; (void)a;
    /* the second of a batch fails: its handle is VK_NULL_HANDLE */
    for (k = 0; k < n; k++) out[k] = k == 1 ? VK_NULL_HANDLE : IVK_HANDLE(VkPipeline, next_handle++);
    return n > 1 ? VK_PIPELINE_COMPILE_REQUIRED : VK_SUCCESS;
}

static void VKAPI_CALL fake_CmdDraw( VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d )
{ (void)cb; (void)a; (void)b; (void)c; (void)d; }

static PFN_vkVoidFunction VKAPI_CALL fake_GetDeviceProcAddr( VkDevice device, const char *name );

#define NAMED(fn) { "vk" #fn, (PFN_vkVoidFunction)fake_##fn }
struct named { const char *name; PFN_vkVoidFunction fn; };

static const struct named global_fns[] =
{
    NAMED(CreateInstance),
    NAMED(EnumerateInstanceExtensionProperties),
};

static const struct named instance_fns[] =
{
    NAMED(DestroyInstance), NAMED(EnumeratePhysicalDevices),
    { "vkEnumeratePhysicalDeviceGroups", (PFN_vkVoidFunction)fake_EnumeratePhysicalDeviceGroups },
    { "vkEnumeratePhysicalDeviceGroupsKHR", (PFN_vkVoidFunction)fake_EnumeratePhysicalDeviceGroups },
    NAMED(CreateDevice), NAMED(EnumerateDeviceExtensionProperties), NAMED(GetPhysicalDeviceFeatures),
    NAMED(GetPhysicalDeviceFeatures2),
    { "vkGetPhysicalDeviceFeatures2KHR", (PFN_vkVoidFunction)fake_GetPhysicalDeviceFeatures2 },
    NAMED(GetPhysicalDeviceProperties), NAMED(GetPhysicalDeviceProperties2),
    { "vkGetPhysicalDeviceProperties2KHR", (PFN_vkVoidFunction)fake_GetPhysicalDeviceProperties2 },
    NAMED(GetPhysicalDeviceMemoryProperties), NAMED(GetPhysicalDeviceMemoryProperties2),
    { "vkGetPhysicalDeviceMemoryProperties2KHR", (PFN_vkVoidFunction)fake_GetPhysicalDeviceMemoryProperties2 },
    NAMED(DestroySurfaceKHR), NAMED(GetDeviceProcAddr),
};

static const struct named device_fns[] =
{
    NAMED(DestroyDevice), NAMED(GetDeviceQueue), NAMED(GetDeviceQueue2), NAMED(QueueSubmit), NAMED(QueueSubmit2),
    { "vkQueueSubmit2KHR", (PFN_vkVoidFunction)fake_QueueSubmit2 },
    NAMED(QueueWaitIdle), NAMED(DeviceWaitIdle), NAMED(QueuePresentKHR), NAMED(AcquireNextImageKHR),
    NAMED(AcquireNextImage2KHR), NAMED(WaitForFences), NAMED(WaitSemaphores),
    /* vkWaitSemaphoresKHR: this fake driver doesn't have it */
    NAMED(AllocateMemory), NAMED(FreeMemory), NAMED(CreateImage), NAMED(DestroyImage), NAMED(CreateBuffer),
    NAMED(DestroyBuffer), NAMED(CreateImageView), NAMED(DestroyImageView), NAMED(CreateBufferView),
    NAMED(DestroyBufferView), NAMED(CreateSampler), NAMED(DestroySampler), NAMED(CreateGraphicsPipelines),
    NAMED(CreateComputePipelines), NAMED(DestroyPipeline), NAMED(CreatePipelineLayout), NAMED(DestroyPipelineLayout),
    NAMED(CreatePipelineCache), NAMED(DestroyPipelineCache), NAMED(CreateShaderModule), NAMED(DestroyShaderModule),
    NAMED(CreateDescriptorPool), NAMED(DestroyDescriptorPool), NAMED(CreateDescriptorSetLayout),
    NAMED(DestroyDescriptorSetLayout), NAMED(CreateCommandPool), NAMED(DestroyCommandPool), NAMED(CreateQueryPool),
    NAMED(DestroyQueryPool), NAMED(CreateSemaphore), NAMED(DestroySemaphore), NAMED(CreateFence), NAMED(DestroyFence),
    NAMED(CreateEvent), NAMED(DestroyEvent), NAMED(CreateSwapchainKHR), NAMED(DestroySwapchainKHR),
    NAMED(CreateRenderPass), NAMED(CreateRenderPass2),
    { "vkCreateRenderPass2KHR", (PFN_vkVoidFunction)fake_CreateRenderPass2 },
    NAMED(DestroyRenderPass), NAMED(CreateFramebuffer), NAMED(DestroyFramebuffer), NAMED(CmdDraw),
};

static PFN_vkVoidFunction lookup( const struct named *list, size_t n, const char *name )
{
    size_t i;
    for (i = 0; i < n; i++) if (!strcmp( list[i].name, name )) return list[i].fn;
    return NULL;
}
#define LOOKUP(list, name) lookup( list, sizeof(list) / sizeof(list[0]), name )

PFN_vkVoidFunction VKAPI_CALL fake_vkGetInstanceProcAddr( VkInstance instance, const char *name )
{
    PFN_vkVoidFunction fn;
    if ((fn = LOOKUP( global_fns, name ))) return fn;
    if (!instance) return NULL;
    if ((fn = LOOKUP( instance_fns, name ))) return fn;
    return LOOKUP( device_fns, name );
}

static PFN_vkVoidFunction VKAPI_CALL fake_GetDeviceProcAddr( VkDevice device, const char *name )
{
    (void)device;
    return LOOKUP( device_fns, name );
}

/* ================================================================ helpers */

#define GIPA ios_vk_host_get_instance_proc_addr
#define GDPA ios_vk_host_get_device_proc_addr

static VkInstance make_instance( void )
{
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)GIPA( NULL, "vkCreateInstance" );
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *exts[] = { "VK_KHR_surface", "VK_EXT_metal_surface" };
    VkInstance instance = VK_NULL_HANDLE;

    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = exts;
    CHECK( create( &info, NULL, &instance ) == VK_SUCCESS );
    return instance;
}

static VkPhysicalDevice first_physical( VkInstance instance )
{
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)GIPA( instance, "vkEnumeratePhysicalDevices" );
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t count = 1;
    CHECK( enumerate( instance, &count, &physical ) == VK_SUCCESS );
    return physical;
}

static VkDevice make_device( VkInstance instance, VkDevice handle )
{
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)GIPA( instance, "vkCreateDevice" );
    VkDeviceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    const char *exts[] = { "VK_KHR_swapchain" };
    VkDevice device = VK_NULL_HANDLE;

    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = exts;
    next_device = handle;
    CHECK( create( first_physical( instance ), &info, NULL, &device ) == VK_SUCCESS );
    CHECK( device == handle );
    return device;
}

#define DFN(device, type, name) ((PFN_##name)GDPA( device, #name ))

/* ================================================================ tests */

static void test_pure_heaps( void )
{
    /* limit = min(footprint + available, memsize - 1536 MB) */
    CHECK( ivk_mem_limit( 1024 * IVK_MB, 12288 * IVK_MB, 16384 * IVK_MB ) == 13312 * IVK_MB );
    CHECK( ivk_mem_limit( 1024 * IVK_MB, 20000 * IVK_MB, 16384 * IVK_MB ) == 14848 * IVK_MB );
    CHECK( ivk_mem_limit( 1024 * IVK_MB, 0, 16384 * IVK_MB ) == 14848 * IVK_MB );   /* no reading: the cap */
    CHECK( ivk_mem_limit( 0, 0, 0 ) == UINT64_MAX );
    /* clamp: limit - 6656 MB, floor 1024 MB, ceiling the driver's size */
    CHECK( ivk_heap_clamp( 13312 * IVK_MB, 8192 * IVK_MB ) == 6656 * IVK_MB );
    CHECK( ivk_heap_clamp( 6144 * IVK_MB, 8192 * IVK_MB ) == 1024 * IVK_MB );      /* floor */
    CHECK( ivk_heap_clamp( 13312 * IVK_MB, 4096 * IVK_MB ) == 4096 * IVK_MB );     /* ceiling */
    CHECK( ivk_heap_clamp( 13312 * IVK_MB, 512 * IVK_MB ) == 512 * IVK_MB );       /* ceiling beats floor */
    CHECK( ivk_heap_clamp( UINT64_MAX, 8192 * IVK_MB ) == 8192 * IVK_MB );
    /* trim: footprint above limit - 1536 MB comes off, floor 768 MB */
    CHECK( ivk_budget_trim( 6656 * IVK_MB, 13312 * IVK_MB, 11000 * IVK_MB ) == 6656 * IVK_MB );
    CHECK( ivk_budget_trim( 6656 * IVK_MB, 13312 * IVK_MB, 12800 * IVK_MB ) == 5632 * IVK_MB );
    CHECK( ivk_budget_trim( 6656 * IVK_MB, 13312 * IVK_MB, 20000 * IVK_MB ) == 768 * IVK_MB );
    CHECK( ivk_budget_trim( 512 * IVK_MB, 13312 * IVK_MB, 20000 * IVK_MB ) == 512 * IVK_MB );
    CHECK( ivk_budget_due( 0, 0, 0 ) );
    CHECK( !ivk_budget_due( 1249 * 1000000ull, 1000 * 1000000ull, 1 ) );
    CHECK( ivk_budget_due( 1250 * 1000000ull, 1000 * 1000000ull, 1 ) );
}

static void test_heaps( VkInstance instance )
{
    VkPhysicalDevice physical = first_physical( instance );
    PFN_vkGetPhysicalDeviceMemoryProperties get = (PFN_vkGetPhysicalDeviceMemoryProperties)
        GIPA( instance, "vkGetPhysicalDeviceMemoryProperties" );
    PFN_vkGetPhysicalDeviceMemoryProperties2 get2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)
        GIPA( instance, "vkGetPhysicalDeviceMemoryProperties2KHR" );
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
    VkPhysicalDeviceMemoryProperties2 props2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &budget };
    VkPhysicalDeviceMemoryProperties props;
    unsigned int reads;

    CHECK( get != (PFN_vkGetPhysicalDeviceMemoryProperties)fake_GetPhysicalDeviceMemoryProperties );
    current_pid = 0x111;   /* a new process: the limit is worked out again */
    fake_memsize_value = 16384 * IVK_MB;
    fake_footprint_value = 1024 * IVK_MB;
    fake_available_value = 12288 * IVK_MB;
    get( physical, &props );
    CHECK( props.memoryHeaps[0].size == 6656 * IVK_MB );
    CHECK( props.memoryHeaps[1].size == 4096 * IVK_MB );   /* not DEVICE_LOCAL: as the driver says */

    get2( physical, &props2 );
    CHECK( props2.memoryProperties.memoryHeaps[0].size == 6656 * IVK_MB );
    CHECK( budget.heapBudget[0] == 6656 * IVK_MB );        /* min(driver's 7168 MB, clamp) */
    CHECK( budget.heapUsage[0] == 100 * IVK_MB );          /* usage as the driver reports it */
    CHECK( budget.heapBudget[1] == 3000 * IVK_MB );

    /* memory gets tight: the budget drops, but the reading is at most every 250 ms */
    fake_footprint_value = 12800 * IVK_MB;
    reads = footprint_reads;
    get2( physical, &props2 );
    CHECK( footprint_reads == reads );                      /* too soon: not re-read */
    CHECK( budget.heapBudget[0] == 6656 * IVK_MB );
    atomic_fetch_add( &fake_now, 300 * 1000000ull );
    get2( physical, &props2 );
    CHECK( footprint_reads == reads + 1 );
    CHECK( budget.heapBudget[0] == 5632 * IVK_MB );
    CHECK( props2.memoryProperties.memoryHeaps[0].size == 6656 * IVK_MB );   /* the heap size stays */
    heap0_size = 4096 * IVK_MB;
    get( physical, &props );
    CHECK( props.memoryHeaps[0].size == 4096 * IVK_MB );    /* never above the driver's size */
    heap0_size = 8192 * IVK_MB;
    current_pid = 0x20;
}

static void test_pure_pacing( void )
{
    struct ivk_pacer p = { 0 };
    const uint64_t P = 1000000000ull / 60, t0 = 5000000000ull;

    CHECK( ivk_pace_period_ns( 0 ) == 0 && ivk_pace_period_ns( 2 ) == 0 );
    CHECK( ivk_pace_period_ns( 1 ) == P && ivk_pace_period_ns( 3 ) == 1000000000ull / 30 &&
           ivk_pace_period_ns( 4 ) == 25000000ull );
    CHECK( ivk_pace( &p, 1, t0 ) == 0 );                    /* first frame: from now */
    CHECK( ivk_pace( &p, 1, t0 + 5000000 ) == t0 + P );     /* on time: hold until the deadline */
    CHECK( ivk_pace( &p, 1, t0 + 2 * P + 3000000 ) == 0 );  /* late by less than a period: no sleep */
    CHECK( p.deadline == t0 + 2 * P );                      /* ... and the cadence is kept */
    CHECK( ivk_pace( &p, 1, t0 + 2 * P + 4000000 ) == t0 + 3 * P );
    CHECK( ivk_pace( &p, 1, t0 + 6 * P ) == 0 );            /* more than a period late: from now */
    CHECK( p.deadline == t0 + 6 * P );
    CHECK( ivk_pace( &p, 1, t0 + 6 * P + 1 ) == t0 + 7 * P );
    CHECK( ivk_pace( &p, 3, t0 + 7 * P ) == 0 );            /* mode change: from now */
    CHECK( ivk_pace( &p, 3, t0 + 7 * P + 1 ) == t0 + 7 * P + 1000000000ull / 30 );
    CHECK( ivk_pace( &p, 0, t0 + 8 * P ) == 0 && !p.primed );   /* mode 0: no hold */
    CHECK( ivk_pace( &p, 2, t0 + 9 * P ) == 0 );
}

static void test_present( VkDevice device )
{
    PFN_vkGetDeviceQueue get_queue = DFN(device, x, vkGetDeviceQueue);
    PFN_vkQueuePresentKHR present = DFN(device, x, vkQueuePresentKHR);
    VkPresentInfoKHR info = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 2 };
    VkQueue queue;
    unsigned int pooled, before;

    get_queue( device, 0, 0, &queue );
    CHECK( queue == FAKE_QUEUE );
    CHECK( present != (PFN_vkQueuePresentKHR)fake_QueuePresentKHR );

    /* E6: one count per call, whatever the swapchain count */
    before = external_presents; pooled = pooled_calls; vsync_mode = 0; sleeps = 0;
    CHECK( present( queue, &info ) == VK_SUCCESS );
    CHECK( external_presents == before + 1 );
    CHECK( pooled_calls == pooled + 1 );                    /* E2 */
    CHECK( sleeps == 0 );                                   /* mode 0: no hold */
    present_result = VK_SUBOPTIMAL_KHR;
    CHECK( present( queue, &info ) == VK_SUBOPTIMAL_KHR );
    CHECK( external_presents == before + 2 );
    present_result = VK_ERROR_OUT_OF_DATE_KHR;
    CHECK( present( queue, &info ) == VK_ERROR_OUT_OF_DATE_KHR );
    CHECK( external_presents == before + 2 );               /* a failed present isn't counted */
    present_result = VK_SUCCESS;
    CHECK( log_has( "GPU meter" ) );

    /* E5: 40 fps holds the thread until the deadline */
    vsync_mode = 4;
    present( queue, &info );                                /* primes the pacer */
    sleeps = 0;
    {
        uint64_t t = atomic_load( &fake_now );
        present( queue, &info );
        CHECK( sleeps == 1 && last_sleep_until == t + 25000000ull );
    }
    vsync_mode = 0;
}

static void test_pure_waits( void );

static uint64_t env_now( void ) { return atomic_load( &fake_now ); }
static int env_kill( void ) { return kill_flag; }
static int32_t pure_call( void *ctx, uint64_t t ) { (void)ctx; return slice_answer( t, VK_TIMEOUT ); }
static int32_t not_ready_call( void *ctx, uint64_t t ) { (void)ctx; return slice_answer( t, VK_NOT_READY ); }

static void test_pure_waits( void )
{
    const struct ivk_wait_env env = { env_now, env_kill };
    const uint64_t ms = 1000000ull;
    uint64_t t0;

    nslices = 0; succeed_on_slice = 0; kill_flag = 0;
    CHECK( ivk_sliced_wait( 200 * ms, pure_call, NULL, &env, 0 ) == VK_TIMEOUT );
    CHECK( nslices == 1 && slices[0] == 200 * ms );         /* short: one call as given */
    nslices = 0;
    CHECK( ivk_sliced_wait( 0, pure_call, NULL, &env, 0 ) == VK_TIMEOUT && nslices == 1 && slices[0] == 0 );

    nslices = 0; t0 = atomic_load( &fake_now );
    CHECK( ivk_sliced_wait( 1000 * ms + 7, pure_call, NULL, &env, 0 ) == VK_TIMEOUT );
    CHECK( nslices == 5 );
    CHECK( slices[0] == 250 * ms && slices[3] == 250 * ms && slices[4] == 7 );
    CHECK( atomic_load( &fake_now ) == t0 + 1000 * ms + 7 );   /* returned exactly at the deadline */

    nslices = 0; succeed_on_slice = 3;
    CHECK( ivk_sliced_wait( 5000 * ms, pure_call, NULL, &env, 0 ) == VK_SUCCESS && nslices == 3 );
    succeed_on_slice = 0;

    /* a killed pseudo-process: device lost between slices, also for an endless wait */
    nslices = 0; kill_flag = 1;
    CHECK( ivk_sliced_wait( UINT64_MAX, pure_call, NULL, &env, 0 ) == VK_ERROR_DEVICE_LOST && nslices == 1 );
    kill_flag = 0;

    /* an acquire's slice that answers VK_NOT_READY is a timeout of the caller's wait */
    nslices = 0;
    CHECK( ivk_sliced_wait( 600 * ms, not_ready_call, NULL, &env, 1 ) == VK_TIMEOUT && nslices == 3 );
    nslices = 0;
    CHECK( ivk_sliced_wait( 600 * ms, not_ready_call, NULL, &env, 0 ) == VK_NOT_READY && nslices == 1 );
}

static void test_waits( VkDevice device )
{
    PFN_vkWaitForFences wait_fences = DFN(device, x, vkWaitForFences);
    PFN_vkWaitSemaphores wait_sems = DFN(device, x, vkWaitSemaphores);
    PFN_vkAcquireNextImageKHR acquire = DFN(device, x, vkAcquireNextImageKHR);
    PFN_vkAcquireNextImage2KHR acquire2 = DFN(device, x, vkAcquireNextImage2KHR);
    VkSemaphoreWaitInfo winfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    VkAcquireNextImageInfoKHR ainfo = { .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR };
    const uint64_t ms = 1000000ull;
    VkFence fence = IVK_HANDLE(VkFence, 0x77);
    uint32_t index;
    uint64_t t0;

    /* the driver has no vkWaitSemaphoresKHR: neither has the layer */
    CHECK( GDPA( device, "vkWaitSemaphoresKHR" ) == NULL );

    nslices = 0; t0 = atomic_load( &fake_now );
    CHECK( wait_fences( device, 1, &fence, VK_TRUE, 600 * ms ) == VK_TIMEOUT );
    CHECK( nslices == 3 && slices[2] == 100 * ms && atomic_load( &fake_now ) == t0 + 600 * ms );

    nslices = 0; kill_flag = 1;
    CHECK( wait_sems( device, &winfo, 10000 * ms ) == VK_ERROR_DEVICE_LOST && nslices == 1 );
    kill_flag = 0;

    /* an acquire's last slice may be 0 long: its VK_NOT_READY is the caller's VK_TIMEOUT */
    nslices = 0;
    CHECK( acquire( device, IVK_HANDLE(VkSwapchainKHR, 0x55), 500 * ms, VK_NULL_HANDLE, VK_NULL_HANDLE, &index ) == VK_TIMEOUT );
    CHECK( nslices == 2 );
    nslices = 0;
    CHECK( acquire( device, IVK_HANDLE(VkSwapchainKHR, 0x55), 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &index ) == VK_NOT_READY );
    nslices = 0; succeed_on_slice = 2; ainfo.timeout = UINT64_MAX;
    CHECK( acquire2( device, &ainfo, &index ) == VK_SUCCESS && nslices == 2 && slices[0] == 250 * ms );
    succeed_on_slice = 0;
}

static void test_routing( VkInstance instance, VkDevice device )
{
    VkPhysicalDevice physical = first_physical( instance );

    CHECK( GIPA( NULL, "vkGetInstanceProcAddr" ) == (PFN_vkVoidFunction)ios_vk_host_get_instance_proc_addr );
    CHECK( GIPA( instance, "vkGetDeviceProcAddr" ) == (PFN_vkVoidFunction)ios_vk_host_get_device_proc_addr );
    CHECK( GIPA( NULL, "vkCreateInstance" ) != (PFN_vkVoidFunction)fake_CreateInstance );
    CHECK( GIPA( NULL, "vkEnumerateInstanceExtensionProperties" ) ==
           (PFN_vkVoidFunction)fake_EnumerateInstanceExtensionProperties );   /* not intercepted */
    CHECK( GIPA( instance, "vkCreateDevice" ) != (PFN_vkVoidFunction)fake_CreateDevice );
    CHECK( GIPA( instance, "vkGetPhysicalDeviceFeatures" ) == (PFN_vkVoidFunction)fake_GetPhysicalDeviceFeatures );
    CHECK( GIPA( instance, "vkCmdDraw" ) == (PFN_vkVoidFunction)fake_CmdDraw );
    CHECK( GIPA( instance, "vkNoSuchFunction" ) == NULL );
    CHECK( GDPA( device, "vkCmdDraw" ) == (PFN_vkVoidFunction)fake_CmdDraw );   /* draws pay nothing */
    CHECK( GDPA( device, "vkQueueSubmit" ) != NULL && GDPA( device, "vkQueueSubmit" ) != (PFN_vkVoidFunction)fake_QueueSubmit );
    CHECK( GDPA( device, "vkCreateImage" ) != (PFN_vkVoidFunction)fake_CreateImage );
    CHECK( GDPA( device, "vkGetDeviceProcAddr" ) == (PFN_vkVoidFunction)ios_vk_host_get_device_proc_addr );
    CHECK( GDPA( device, "vkCreateDevice" ) == NULL );      /* not a device function */
    CHECK( GDPA( device, "vkNoSuchFunction" ) == NULL );
    CHECK( GDPA( (VkDevice)(void *)&queue_obj, "vkCmdDraw" ) == NULL );   /* unknown device */
    /* per-object tables */
    CHECK( ivk_instance_get( instance ) != NULL );
    CHECK( ivk_physical_get( physical ) && ivk_physical_get( physical )->instance == ivk_instance_get( instance ) );
    CHECK( ivk_device_get( device ) && ivk_device_get( device )->instance == ivk_instance_get( instance ) );
    {
        unsigned int before = pooled_calls, n = submits;
        PFN_vkQueueSubmit submit = DFN(device, x, vkQueueSubmit);
        PFN_vkGetDeviceQueue2 get_queue2 = DFN(device, x, vkGetDeviceQueue2);
        VkDeviceQueueInfo2 qinfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2 };
        VkQueue queue;
        get_queue2( device, &qinfo, &queue );
        CHECK( submit( queue, 0, NULL, VK_NULL_HANDLE ) == VK_SUCCESS && submits == n + 1 );
        CHECK( pooled_calls == before + 1 );
    }
}

static void test_instance_config( void )
{
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)GIPA( NULL, "vkCreateInstance" );
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *bad[] = { "VK_KHR_surface", "VK_KHR_no_such_extension" };
    VkInstance instance;

    /* B5: log level "errors" through VK_EXT_layer_settings */
    log_clear();
    instance = make_instance();
    CHECK( saw_log_setting && saw_layer_settings_ext );
    CHECK( log_has( "host driver FakeVK 9.9.1, Vulkan 1.3.7, device Fake GPU, geometryShader no, transformFeedback no" ) );
    CHECK( !saw_xfb_unoffered );
    ((PFN_vkDestroyInstance)GIPA( instance, "vkDestroyInstance" ))( instance, NULL );
    /* clarification 1: a GPU with mesh shaders (A14 and later) has both, and the line says so */
    log_clear();
    mesh_gpu = 1;
    current_pid = 0x32;
    instance = make_instance();
    CHECK( log_has( "device Fake GPU, geometryShader yes, transformFeedback yes" ) );
    ((PFN_vkDestroyInstance)GIPA( instance, "vkDestroyInstance" ))( instance, NULL );
    mesh_gpu = 0;
    current_pid = 0x20;
    /* ... unless the environment sets it */
    setenv( "MVK_CONFIG_LOG_LEVEL", "3", 1 );
    instance = make_instance();
    CHECK( !saw_log_setting && !saw_layer_settings_ext );
    ((PFN_vkDestroyInstance)GIPA( instance, "vkDestroyInstance" ))( instance, NULL );
    unsetenv( "MVK_CONFIG_LOG_LEVEL" );

    /* a missing instance extension is named */
    log_clear();
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = bad;
    current_pid = 0x31;
    CHECK( create( &info, NULL, &instance ) == VK_ERROR_EXTENSION_NOT_PRESENT );
    CHECK( log_has( "instance refused: missing VK_KHR_no_such_extension" ) );
    current_pid = 0x20;
}

static void test_device_creation( VkInstance instance )
{
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)GIPA( instance, "vkCreateDevice" );
    VkPhysicalDevice physical = first_physical( instance );
    VkDeviceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan12Features v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures core;
    const char *exts[] = { "VK_KHR_swapchain" };
    const char *exts_bad[] = { "VK_KHR_swapchain", "VK_EXT_transform_feedback" };
    const char *exts_port[] = { "VK_KHR_swapchain", "VK_KHR_portability_subset" };
    VkDevice device;

    /* portability subset: added when offered, once, never seen by the program */
    info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = exts;
    next_device = (VkDevice)(void *)&device2_obj;
    offer_portability = 1;
    CHECK( create( physical, &info, NULL, &device ) == VK_SUCCESS );
    CHECK( saw_portability == 1 && device_ext_count == 2 );
    CHECK( info.enabledExtensionCount == 1 && info.ppEnabledExtensionNames == exts );
    DFN(device, x, vkDestroyDevice)( device, NULL );
    info.enabledExtensionCount = 2; info.ppEnabledExtensionNames = exts_port;
    CHECK( create( physical, &info, NULL, &device ) == VK_SUCCESS && saw_portability == 1 && device_ext_count == 2 );
    DFN(device, x, vkDestroyDevice)( device, NULL );
    offer_portability = 0;
    info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = exts;
    CHECK( create( physical, &info, NULL, &device ) == VK_SUCCESS && saw_portability == 0 && device_ext_count == 1 );
    DFN(device, x, vkDestroyDevice)( device, NULL );
    offer_portability = 1;

    /* E3: a missing feature and a missing extension, named once per process */
    log_clear();
    current_pid = 0x40;
    memset( &core, 0, sizeof(core) );
    core.geometryShader = VK_TRUE;
    core.samplerAnisotropy = VK_TRUE;   /* offered */
    info.pEnabledFeatures = &core;
    info.enabledExtensionCount = 2; info.ppEnabledExtensionNames = exts_bad;
    CHECK( create( physical, &info, NULL, &device ) == VK_ERROR_EXTENSION_NOT_PRESENT );
    CHECK( log_has( "device refused: missing VK_EXT_transform_feedback, geometryShader" ) );
    CHECK( !log_has( "samplerAnisotropy" ) );
    log_clear();
    CHECK( create( physical, &info, NULL, &device ) == VK_ERROR_EXTENSION_NOT_PRESENT );
    CHECK( !log_has( "device refused" ) );                  /* once per process */

    /* the Vulkan 1.2 struct of the chain is compared too */
    current_pid = 0x41;
    info.pEnabledFeatures = NULL;
    info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = exts;
    f2.pNext = &v12;
    info.pNext = &f2;
    v12.drawIndirectCount = VK_TRUE;
    v12.timelineSemaphore = VK_TRUE;   /* offered */
    CHECK( create( physical, &info, NULL, &device ) == VK_ERROR_FEATURE_NOT_PRESENT );
    CHECK( log_has( "device refused: missing drawIndirectCount" ) );
    CHECK( !log_has( "timelineSemaphore" ) );
    current_pid = 0x20;
}

static int event_index( const char *what, uint64_t handle )
{
    unsigned int i;
    for (i = 0; i < nevents; i++) if (!strcmp( events[i].what, what ) && events[i].handle == handle) return (int)i;
    return -1;
}

static void test_release( VkInstance instance )
{
    VkDevice device = make_device( instance, (VkDevice)(void *)&device2_obj );
    VkImageCreateInfo iinfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkBufferCreateInfo binfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo minfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkSwapchainCreateInfoKHR sinfo = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkFramebufferCreateInfo finfo = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkComputePipelineCreateInfo cinfo[3];
    VkImage image1, image2;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkSwapchainKHR swapchain;
    VkFramebuffer framebuffer;
    VkPipeline pipelines[3];
    int i_swap, i_fb, i_pipe, i_img, i_buf, i_mem, i_dev, i_idle;
    unsigned int pooled;

    memset( cinfo, 0, sizeof(cinfo) );
    pooled = pooled_calls;
    CHECK( DFN(device, x, vkCreateImage)( device, &iinfo, NULL, &image1 ) == VK_SUCCESS );
    CHECK( pooled_calls == pooled + 1 );                    /* E2: vkCreateImage is pooled */
    DFN(device, x, vkCreateImage)( device, &iinfo, NULL, &image2 );
    DFN(device, x, vkCreateBuffer)( device, &binfo, NULL, &buffer );
    DFN(device, x, vkAllocateMemory)( device, &minfo, NULL, &memory );
    DFN(device, x, vkCreateSwapchainKHR)( device, &sinfo, NULL, &swapchain );
    DFN(device, x, vkCreateFramebuffer)( device, &finfo, NULL, &framebuffer );
    CHECK( DFN(device, x, vkCreateComputePipelines)( device, VK_NULL_HANDLE, 3, cinfo, NULL, pipelines ) == VK_PIPELINE_COMPILE_REQUIRED );
    CHECK( pipelines[1] == VK_NULL_HANDLE );
    /* the program destroys one image itself */
    DFN(device, x, vkDestroyImage)( device, image1, NULL );
    CHECK( ios_vk_layer_device_objects( device ) == 7 );

    nevents = 0;
    ios_vk_layer_release_device( device );
    CHECK( ivk_device_get( device ) == NULL );              /* handed over: out of the tables */
    CHECK( ios_vk_layer_release_drain( 5000 ) );
    i_idle = event_index( "idle", IVK_H64(device) );
    i_swap = event_index( "swapchain", IVK_H64(swapchain) );
    i_fb = event_index( "framebuffer", IVK_H64(framebuffer) );
    i_pipe = event_index( "pipeline", IVK_H64(pipelines[0]) );
    i_img = event_index( "image", IVK_H64(image2) );
    i_buf = event_index( "buffer", IVK_H64(buffer) );
    i_mem = event_index( "memory", IVK_H64(memory) );
    i_dev = event_index( "device", IVK_H64(device) );
    CHECK( i_idle == 0 );                                   /* vkDeviceWaitIdle first */
    CHECK( i_swap > i_idle && i_fb > i_swap && i_pipe > i_fb && i_img > i_pipe && i_buf > i_img && i_mem > i_buf );
    CHECK( event_index( "pipeline", IVK_H64(pipelines[2]) ) > i_fb );
    CHECK( i_dev == (int)nevents - 1 );                     /* the device last */
    CHECK( event_index( "image", IVK_H64(image1) ) < 0 );   /* the program's own destroy isn't repeated */
    CHECK( nevents == 9 );                                  /* idle, 7 objects, device; nothing twice */
    CHECK( log_has( "1 swapchains, 1 framebuffers, 2 pipelines, 1 images, 1 buffers, 1 memory (7 objects)" ) );
}

static void test_release_hang( void )
{
    VkInstance instance = make_instance();
    VkDevice device = make_device( instance, (VkDevice)(void *)&device_obj );
    PFN_vkDestroySurfaceKHR destroy_surface = (PFN_vkDestroySurfaceKHR)GIPA( instance, "vkDestroySurfaceKHR" );
    PFN_vkDestroyInstance destroy_instance = (PFN_vkDestroyInstance)GIPA( instance, "vkDestroyInstance" );
    VkSurfaceKHR surface = IVK_HANDLE(VkSurfaceKHR, 0xabc);
    struct timespec a, b;
    double secs;

    nevents = 0;
    log_clear();
    pthread_mutex_lock( &hang_mutex );
    hang_idle = 1;
    pthread_mutex_unlock( &hang_mutex );

    clock_gettime( CLOCK_MONOTONIC, &a );
    ios_vk_layer_release_device( device );
    /* the surface and the instance wait for the device (Part D7's order, behind the release) */
    destroy_surface( instance, surface, NULL );
    destroy_instance( instance, NULL );
    CHECK( event_index( "surface", IVK_H64(surface) ) < 0 && event_index( "instance", IVK_H64(instance) ) < 0 );
    CHECK( ios_vk_layer_release_drain( 6000 ) );
    clock_gettime( CLOCK_MONOTONIC, &b );
    secs = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
    printf( "  the hanging wait was given up after %.2f s\n", secs );
    CHECK( secs >= 1.9 && secs < 4.0 );
    CHECK( log_has( "didn't return within 2000 ms; the device is left" ) );
    CHECK( event_index( "device", IVK_H64(device) ) < 0 );  /* left: never destroyed under a hung wait */
    CHECK( event_index( "surface", IVK_H64(surface) ) < 0 && log_has( "surface 0xabc left" ) );
    CHECK( event_index( "instance", IVK_H64(instance) ) < 0 && log_has( "left: a device of it hung" ) );

    /* the app keeps running: the waiter comes back later and nothing breaks */
    pthread_mutex_lock( &hang_mutex );
    hang_idle = 0;
    pthread_cond_broadcast( &hang_cond );
    pthread_mutex_unlock( &hang_mutex );
}

static void test_deferred_order( void )
{
    VkInstance instance = make_instance();
    VkDevice device = make_device( instance, (VkDevice)(void *)&device_obj );
    PFN_vkDestroySurfaceKHR destroy_surface = (PFN_vkDestroySurfaceKHR)GIPA( instance, "vkDestroySurfaceKHR" );
    PFN_vkDestroyInstance destroy_instance = (PFN_vkDestroyInstance)GIPA( instance, "vkDestroyInstance" );
    VkSurfaceKHR surface = IVK_HANDLE(VkSurfaceKHR, 0xdef);
    int i_dev, i_surf, i_inst;

    nevents = 0;
    ios_vk_layer_release_device( device );
    destroy_surface( instance, surface, NULL );
    destroy_instance( instance, NULL );
    CHECK( ios_vk_layer_release_drain( 5000 ) );
    i_dev = event_index( "device", IVK_H64(device) );
    i_surf = event_index( "surface", IVK_H64(surface) );
    i_inst = event_index( "instance", IVK_H64(instance) );
    CHECK( i_dev >= 0 && i_surf > i_dev && i_inst > i_surf );

    /* nothing pending: destroyed at once */
    instance = make_instance();
    nevents = 0;
    ((PFN_vkDestroySurfaceKHR)GIPA( instance, "vkDestroySurfaceKHR" ))( instance, surface, NULL );
    CHECK( event_index( "surface", IVK_H64(surface) ) == 0 );
    ((PFN_vkDestroyInstance)GIPA( instance, "vkDestroyInstance" ))( instance, NULL );
    CHECK( event_index( "instance", IVK_H64(instance) ) == 1 );
}

static void test_pure_sets( void )
{
    struct ivk_set set = { 0 };
    struct ivk_names names;
    unsigned int counts[IVK_KIND_COUNT] = { 0 };
    char buf[128];
    uint64_t i;
    int ok = 1;

    for (i = 1; i <= 1000; i++) ok &= ivk_set_add( &set, i * 0x10 );
    CHECK( ok && set.count == 1000 );
    for (i = 1; i <= 1000; i += 2) ivk_set_del( &set, i * 0x10 );
    CHECK( set.count == 500 && !ivk_set_has( &set, 0x10 ) && ivk_set_has( &set, 0x20 ) );
    for (i = 1; i <= 5000; i++) { ivk_set_add( &set, 0x100000 + i ); ivk_set_del( &set, 0x100000 + i ); }
    CHECK( set.count == 500 && ivk_set_has( &set, 0x2000 ) );
    CHECK( !ivk_set_add( &set, 0 ) );
    ivk_set_free( &set );

    counts[IVK_SWAPCHAIN] = 2; counts[IVK_MEMORY] = 11;
    ivk_release_summary( counts, buf, sizeof(buf) );
    CHECK( !strcmp( buf, "2 swapchains, 11 memory" ) );
    memset( counts, 0, sizeof(counts) );
    ivk_release_summary( counts, buf, sizeof(buf) );
    CHECK( !strcmp( buf, "no objects" ) );
    CHECK( !strcmp( ivk_kind_name( IVK_RENDER_PASS ), "render passes" ) );

    ivk_names_init( &names );
    for (i = 0; i < 200; i++) ivk_names_add( &names, "someFeatureName" );
    CHECK( names.count == 200 && names.cut && strlen( names.buf ) < sizeof(names.buf) );
    CHECK( strstr( names.buf, "..." ) != NULL );
}

static void test_pure_map( void )
{
    static struct ivk_map map;
    static int vals[2000];
    unsigned int i;

    for (i = 0; i < IVK_MAP_CAP; i++) CHECK( ivk_map_put( &map, 0x1000 + i * 8, &vals[i] ) );
    CHECK( !ivk_map_put( &map, 0x99999, &vals[0] ) );       /* full */
    CHECK( ivk_map_get( &map, 0x1000 + 17 * 8 ) == &vals[17] );
    CHECK( ivk_map_del( &map, 0x1000 + 17 * 8 ) == &vals[17] );
    CHECK( ivk_map_get( &map, 0x1000 + 17 * 8 ) == NULL );
    CHECK( ivk_map_put( &map, 0x99999, &vals[1999] ) && ivk_map_get( &map, 0x99999 ) == &vals[1999] );
    CHECK( ivk_map_get( &map, 0x1000 + 18 * 8 ) == &vals[18] );
    CHECK( !ivk_map_put( &map, 0, &vals[0] ) );
}

int main( void )
{
    VkInstance instance;
    VkDevice device;

    setvbuf( stdout, NULL, _IONBF, 0 );
    ios_vk_layer_set_process_id_func( test_pid );
    test_pure_heaps();
    test_pure_pacing();
    test_pure_waits();
    test_pure_sets();
    test_pure_map();
    test_instance_config();

    instance = make_instance();
    device = make_device( instance, (VkDevice)(void *)&device_obj );
    test_routing( instance, device );
    test_heaps( instance );
    test_present( device );
    test_waits( device );
    test_device_creation( instance );
    DFN(device, x, vkDestroyDevice)( device, NULL );
    CHECK( ivk_device_get( device ) == NULL );
    test_release( instance );
    test_deferred_order();
    test_release_hang();

    if (failures) { printf( "test_layer: %d failure(s)\n", failures ); return 1; }
    printf( "test_layer: all passed\n" );
    return 0;
}
