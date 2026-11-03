// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part M6: vktest.exe, a Windows test program for Vulkan on the device (never on the
// owner's PC: no GPU tests there). Built in CI with llvm-mingw for x86_64 and i686
// (build-vktest.sh). It:
//   - prints the instance, the device, its features and limits;
//   - tries a device with geometryShader when the device lacks it (A12, A13), and expects a clean
//     refusal; where it has it (A14 and later, clarification 1), draws with a geometry shader and
//     captures transform feedback (tests/common/vkgx.c, offscreen);
//   - opens a window, makes a swapchain and draws a textured spinning cube, with a BC1 and a BC7
//     texture uploaded by vkCmdCopyBufferToImage (where the device offers BC; otherwise RGBA
//     stand-ins, said so);
//   - reports fps every second;
//   - runs smoke tests of tessellation (if offered), descriptor indexing, timeline semaphores,
//     dynamic rendering and mapped memory;
//   - exits with a code whose bits say what failed (VKT_FAIL_* below).
// Usage: vktest.exe [--seconds N | --forever] [--no-window]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "vktest_shaders.h"   /* SPIR-V of shaders/, made by build-vktest.sh */
#include "vkgx.h"             /* the geometry-shader draw and transform-feedback capture */

enum
{
    VKT_FAIL_INSTANCE    = 1 << 0,
    VKT_FAIL_DEVICE      = 1 << 1,
    VKT_FAIL_RENDER      = 1 << 2,
    VKT_FAIL_GEOMETRY    = 1 << 3,   /* geometryShader asked of a device without it: no clean refusal */
    VKT_FAIL_TESSELLATION = 1 << 4,
    VKT_FAIL_DESCRIPTOR_INDEXING = 1 << 5,
    VKT_FAIL_TIMELINE    = 1 << 6,
    VKT_FAIL_DYNAMIC_RENDERING = 1 << 7,
    VKT_FAIL_MAPPED_MEMORY = 1 << 8,
    VKT_FAIL_TEXTURES    = 1 << 9,
    VKT_FAIL_GEOMETRY_DRAW = 1 << 10,  /* geometryShader offered, but the draw came out wrong */
    VKT_FAIL_XFB         = 1 << 11,    /* transform feedback offered, but the capture came out wrong */
};

static unsigned int result;
#define FAIL(bit, ...) do { printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); result |= (bit); } while (0)
#define CHECK_VK(bit, call) check_vk( (bit), (call), #call )
static int check_vk( unsigned int bit, VkResult res, const char *what )
{
    if (res >= 0) return 1;
    FAIL( bit, "%s: %d", what, (int)res );
    return 0;
}

/* ---------------------------------------------------------------- functions */

#define GLOBAL_FUNCS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceVersion)
#define INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)
#define DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkQueuePresentKHR) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateImage) X(vkDestroyImage) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetImageMemoryRequirements) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkUnmapMemory) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateSampler) \
    X(vkDestroySampler) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDrawIndexed) X(vkCmdDraw) X(vkCmdSetViewport) \
    X(vkCmdSetScissor) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyBuffer) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkSignalSemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering)

#define X(name) static PFN_##name p_##name;
GLOBAL_FUNCS(X) INSTANCE_FUNCS(X) DEVICE_FUNCS(X)
#undef X
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;

/* ---------------------------------------------------------------- state */

static VkInstance instance;
static VkPhysicalDevice physical;
static VkPhysicalDeviceProperties props;
static VkPhysicalDeviceMemoryProperties memprops;
static VkDevice device;
static VkQueue queue;
static uint32_t family;
static VkCommandPool cmdpool;
static int have_tessellation, have_timeline, have_dynamic_rendering, have_indexing;
static struct vkgx_caps geometry_caps;
static VkSurfaceKHR surface;
static HWND window;
static volatile LONG resized;

static VkSwapchainKHR swapchain;
static VkFormat swap_format;
static VkExtent2D extent;
static uint32_t image_count;
static VkImage images[8];
static VkImageView views[8];
static VkFramebuffer framebuffers[8];
static VkCommandBuffer cmdbufs[8];
static VkImage depth_image;
static VkDeviceMemory depth_memory;
static VkImageView depth_view;
static VkRenderPass render_pass;
static VkPipelineLayout pipeline_layout;
static VkPipeline pipeline;
static VkDescriptorSetLayout set_layout;
static VkDescriptorPool descriptor_pool;
static VkDescriptorSet descriptor_set;

/* ---------------------------------------------------------------- helpers */

static uint32_t memory_type( uint32_t bits, VkMemoryPropertyFlags want )
{
    uint32_t i;
    for (i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int make_buffer( VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                        VkBuffer *buffer, VkDeviceMemory *memory )
{
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;

    if (p_vkCreateBuffer( device, &info, NULL, buffer ) != VK_SUCCESS) return 0;
    p_vkGetBufferMemoryRequirements( device, *buffer, &req );
    alloc.allocationSize = req.size;
    if ((alloc.memoryTypeIndex = memory_type( req.memoryTypeBits, want )) == UINT32_MAX) return 0;
    if (p_vkAllocateMemory( device, &alloc, NULL, memory ) != VK_SUCCESS) return 0;
    return p_vkBindBufferMemory( device, *buffer, *memory, 0 ) == VK_SUCCESS;
}

static int make_image( VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                       VkImage *image, VkDeviceMemory *memory, VkImageView *view )
{
    VkImageCreateInfo info = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImageViewCreateInfo vinfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format, .subresourceRange = { aspect, 0, 1, 0, 1 } };
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;

    if (p_vkCreateImage( device, &info, NULL, image ) != VK_SUCCESS) return 0;
    p_vkGetImageMemoryRequirements( device, *image, &req );
    alloc.allocationSize = req.size;
    if ((alloc.memoryTypeIndex = memory_type( req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT )) == UINT32_MAX) return 0;
    if (p_vkAllocateMemory( device, &alloc, NULL, memory ) != VK_SUCCESS) return 0;
    if (p_vkBindImageMemory( device, *image, *memory, 0 ) != VK_SUCCESS) return 0;
    vinfo.image = *image;
    return p_vkCreateImageView( device, &vinfo, NULL, view ) == VK_SUCCESS;
}

static VkShaderModule shader( const uint32_t *code, size_t size )
{
    VkShaderModuleCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule module = VK_NULL_HANDLE;
    if (p_vkCreateShaderModule( device, &info, NULL, &module ) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

/* records, submits and waits for one-time work */
static VkCommandBuffer begin_once( void )
{
    VkCommandBufferAllocateInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkCommandBuffer cb = VK_NULL_HANDLE;

    if (p_vkAllocateCommandBuffers( device, &info, &cb ) != VK_SUCCESS) return VK_NULL_HANDLE;
    p_vkBeginCommandBuffer( cb, &begin );
    return cb;
}

static VkResult end_once( VkCommandBuffer cb )
{
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VkResult res;

    p_vkEndCommandBuffer( cb );
    if ((res = p_vkQueueSubmit( queue, 1, &submit, VK_NULL_HANDLE )) != VK_SUCCESS) return res;
    return p_vkQueueWaitIdle( queue );
}

static void barrier( VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                     VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src, VkPipelineStageFlags dst )
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src_access,
        .dstAccessMask = dst_access, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    p_vkCmdPipelineBarrier( cb, src, dst, 0, 0, NULL, 0, NULL, 1, &b );
}

/* ---------------------------------------------------------------- 1. instance and device facts */

static int create_instance( void )
{
    const char *exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vktest",
        .applicationVersion = 1, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts };
    HMODULE vulkan = LoadLibraryA( "vulkan-1.dll" );
    uint32_t version = VK_API_VERSION_1_0;

    if (!vulkan || !(p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress( vulkan, "vkGetInstanceProcAddr" )))
    {
        FAIL( VKT_FAIL_INSTANCE, "no vulkan-1.dll or no vkGetInstanceProcAddr in it" );
        return 0;
    }
#define X(name) p_##name = (PFN_##name)p_vkGetInstanceProcAddr( NULL, #name );
    GLOBAL_FUNCS(X)
#undef X
    if (p_vkEnumerateInstanceVersion) p_vkEnumerateInstanceVersion( &version );
    printf( "instance: Vulkan %u.%u.%u offered\n", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
            VK_API_VERSION_PATCH(version) );
    if (!CHECK_VK( VKT_FAIL_INSTANCE, p_vkCreateInstance( &info, NULL, &instance ) )) return 0;
#define X(name) p_##name = (PFN_##name)p_vkGetInstanceProcAddr( instance, #name );
    INSTANCE_FUNCS(X)
#undef X
    return 1;
}

static void print_device( void )
{
    VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceIDProperties id = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = &driver };
    VkPhysicalDeviceProperties2 props2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id };
    VkPhysicalDeviceFeatures f;
    const VkPhysicalDeviceLimits *l = &props.limits;
    VkFormatProperties fp;
    uint32_t i, n = 0;

    p_vkGetPhysicalDeviceProperties( physical, &props );
    if (p_vkGetPhysicalDeviceProperties2) p_vkGetPhysicalDeviceProperties2( physical, &props2 );
    p_vkGetPhysicalDeviceMemoryProperties( physical, &memprops );
    p_vkGetPhysicalDeviceFeatures( physical, &f );
    printf( "device: %s, Vulkan %u.%u.%u, vendor %04x device %04x, driver %s %s (0x%x)\n", props.deviceName,
            VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion), props.vendorID, props.deviceID, driver.driverName,
            driver.driverInfo, props.driverVersion );
    printf( "device: LUID valid %u, %02x%02x%02x%02x%02x%02x%02x%02x, node mask %u\n", id.deviceLUIDValid,
            id.deviceLUID[0], id.deviceLUID[1], id.deviceLUID[2], id.deviceLUID[3], id.deviceLUID[4],
            id.deviceLUID[5], id.deviceLUID[6], id.deviceLUID[7], id.deviceNodeMask );
    printf( "features: geometryShader %u tessellationShader %u shaderFloat64 %u logicOp %u textureCompressionBC %u "
            "textureCompressionASTC_LDR %u samplerAnisotropy %u multiDrawIndirect %u depthClamp %u fillModeNonSolid %u "
            "wideLines %u robustBufferAccess %u\n", f.geometryShader, f.tessellationShader, f.shaderFloat64, f.logicOp,
            f.textureCompressionBC, f.textureCompressionASTC_LDR, f.samplerAnisotropy, f.multiDrawIndirect,
            f.depthClamp, f.fillModeNonSolid, f.wideLines, f.robustBufferAccess );
    printf( "limits: image2D %u, image3D %u, layers %u, push constants %u, bound sets %u, per-stage samplers %u, "
            "per-stage sampled images %u, per-stage storage buffers %u, UBO range %u, viewports %u, "
            "workgroup %u x %u x %u (%u invocations), anisotropy %.0f, timestampPeriod %.2f\n",
            l->maxImageDimension2D, l->maxImageDimension3D, l->maxImageArrayLayers, l->maxPushConstantsSize,
            l->maxBoundDescriptorSets, l->maxPerStageDescriptorSamplers, l->maxPerStageDescriptorSampledImages,
            l->maxPerStageDescriptorStorageBuffers, l->maxUniformBufferRange, l->maxViewports,
            l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupSize[1], l->maxComputeWorkGroupSize[2],
            l->maxComputeWorkGroupInvocations, (double)l->maxSamplerAnisotropy, (double)l->timestampPeriod );
    for (i = 0; i < memprops.memoryHeapCount; i++)
        printf( "heap %u: %llu MB%s\n", i, (unsigned long long)(memprops.memoryHeaps[i].size >> 20),
                memprops.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? ", device local" : "" );
    p_vkEnumerateDeviceExtensionProperties( physical, NULL, &n, NULL );
    printf( "device extensions: %u\n", n );
    p_vkGetPhysicalDeviceFormatProperties( physical, VK_FORMAT_D24_UNORM_S8_UINT, &fp );
    printf( "formats: D24S8 depth attachment %s\n",
            fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? "yes" : "no (programs fall back)" );
    have_tessellation = f.tessellationShader;
}

/* the device lacks geometryShader: asking for it must fail cleanly, nothing else */
static void test_geometry_refusal( void )
{
    VkPhysicalDeviceFeatures f, want;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
        .queueCount = 1, .pQueuePriorities = &priority };
    VkDeviceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &q, .pEnabledFeatures = &want };
    VkDevice d = VK_NULL_HANDLE;
    VkResult res;

    p_vkGetPhysicalDeviceFeatures( physical, &f );
    if (f.geometryShader)
    {
        printf( "geometry: offered (no refusal to test)\n" );
        return;
    }
    memset( &want, 0, sizeof(want) );
    want.geometryShader = VK_TRUE;
    res = p_vkCreateDevice( physical, &info, NULL, &d );
    if (res == VK_ERROR_FEATURE_NOT_PRESENT) printf( "geometry: refused cleanly (VK_ERROR_FEATURE_NOT_PRESENT)\n" );
    else
    {
        FAIL( VKT_FAIL_GEOMETRY, "a device with geometryShader gave %d", (int)res );
        if (res == VK_SUCCESS)
        {
            PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)p_vkGetDeviceProcAddr( d, "vkDestroyDevice" );
            if (destroy) destroy( d, NULL );
        }
    }
}

static int create_device( void )
{
    VkPhysicalDeviceVulkan13Features v13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13 };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12 };
    VkPhysicalDeviceVulkan13Features want13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features want12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &want13 };
    VkPhysicalDeviceFeatures2 want = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &want12 };
    const char *exts[2] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, NULL };
    VkQueueFamilyProperties families[16];
    uint32_t count = 1, i, nfam = 16;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
        .pQueuePriorities = &priority };
    VkDeviceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &want,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &q, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts };
    VkCommandPoolCreateInfo pool = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };

    if (!CHECK_VK( VKT_FAIL_DEVICE, p_vkEnumeratePhysicalDevices( instance, &count, &physical ) ) || !physical)
    {
        FAIL( VKT_FAIL_DEVICE, "no physical device" );
        return 0;
    }
    print_device();
    test_geometry_refusal();
    vkgx_query( p_vkGetInstanceProcAddr, instance, physical, &geometry_caps );

    p_vkGetPhysicalDeviceQueueFamilyProperties( physical, &nfam, families );
    for (i = 0; i < nfam; i++) if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    if (i == nfam) { FAIL( VKT_FAIL_DEVICE, "no graphics queue" ); return 0; }
    family = q.queueFamilyIndex = i;

    /* the 1.2 and 1.3 structs only where the device has that version */
    if (props.apiVersion < VK_API_VERSION_1_3) { v12.pNext = NULL; want12.pNext = NULL; }
    if (props.apiVersion < VK_API_VERSION_1_2) { f2.pNext = NULL; want.pNext = NULL; }
    if (p_vkGetPhysicalDeviceFeatures2) p_vkGetPhysicalDeviceFeatures2( physical, &f2 );
    else
    {
        p_vkGetPhysicalDeviceFeatures( physical, &f2.features );
        info.pNext = NULL;
        info.pEnabledFeatures = &want.features;
    }
    want.features.tessellationShader = f2.features.tessellationShader;
    want.features.textureCompressionBC = f2.features.textureCompressionBC;
    want12.timelineSemaphore = have_timeline = v12.timelineSemaphore;
    have_indexing = v12.descriptorBindingPartiallyBound && v12.descriptorBindingVariableDescriptorCount &&
                    v12.runtimeDescriptorArray;
    want12.descriptorBindingPartiallyBound = want12.descriptorBindingVariableDescriptorCount =
        want12.runtimeDescriptorArray = have_indexing;
    want13.dynamicRendering = have_dynamic_rendering = v13.dynamicRendering;
    printf( "features: timelineSemaphore %u descriptor indexing %u dynamicRendering %u\n", have_timeline,
            have_indexing, have_dynamic_rendering );
    /* geometry shaders and transform feedback, where offered (clarification 1) */
    if ((exts[1] = vkgx_device_chain( &geometry_caps, &want.features, &info.pNext ))) info.enabledExtensionCount = 2;

    if (!CHECK_VK( VKT_FAIL_DEVICE, p_vkCreateDevice( physical, &info, NULL, &device ) )) return 0;
#define X(name) p_##name = (PFN_##name)p_vkGetDeviceProcAddr( device, #name );
    DEVICE_FUNCS(X)
#undef X
    p_vkGetDeviceQueue( device, family, 0, &queue );
    pool.queueFamilyIndex = family;
    return CHECK_VK( VKT_FAIL_DEVICE, p_vkCreateCommandPool( device, &pool, NULL, &cmdpool ) );
}

/* ---------------------------------------------------------------- 2. textures (BC1, BC7) */

/* 64 x 64 checkers of 4 x 4 blocks: BC1 red/blue (colour0 > colour1: opaque, all indices 0 or 1) */
static void bc1_blocks( uint8_t *out )
{
    int bx, by;
    for (by = 0; by < 16; by++)
        for (bx = 0; bx < 16; bx++, out += 8)
        {
            out[0] = 0x00; out[1] = 0xf8;   /* colour0 red (RGB565 0xf800) */
            out[2] = 0x1f; out[3] = 0x00;   /* colour1 blue (0x001f) */
            memset( out + 4, (bx ^ by) & 1 ? 0x55 : 0x00, 4 );   /* indices: all 1 (blue) or all 0 (red) */
        }
}

static void put_bits( uint8_t *block, unsigned int *pos, uint32_t value, unsigned int count )
{
    unsigned int i;
    for (i = 0; i < count; i++, (*pos)++)
        if (value & (1u << i)) block[*pos / 8] |= (uint8_t)(1u << (*pos % 8));
}

/* BC7 mode 6 (one subset, 7-bit RGBA endpoints with a p-bit each, 4-bit indices): solid green or
 * yellow blocks, endpoint 0 = endpoint 1, every index 0 */
static void bc7_blocks( uint8_t *out )
{
    int bx, by, c;
    for (by = 0; by < 16; by++)
        for (bx = 0; bx < 16; bx++, out += 16)
        {
            unsigned int pos = 0;
            uint32_t rgba[4];
            int yellow = (bx ^ by) & 1;
            rgba[0] = yellow ? 127 : 0; rgba[1] = 127; rgba[2] = 0; rgba[3] = 127;
            memset( out, 0, 16 );
            put_bits( out, &pos, 1u << 6, 7 );   /* mode 6 */
            for (c = 0; c < 4; c++) { put_bits( out, &pos, rgba[c], 7 ); put_bits( out, &pos, rgba[c], 7 ); }
            put_bits( out, &pos, 1, 1 );   /* p-bits: 7-bit 127 with p 1 = 255 */
            put_bits( out, &pos, 1, 1 );
            /* the 63 index bits stay 0 */
        }
}

/* the same pictures as RGBA8, for a device without BC */
static void rgba_pixels( uint8_t *out, int bc7 )
{
    int x, y;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++, out += 4)
        {
            int odd = ((x / 4) ^ (y / 4)) & 1;
            if (!bc7) { out[0] = odd ? 0 : 255; out[1] = 0; out[2] = odd ? 255 : 0; }
            else { out[0] = odd ? 255 : 0; out[1] = 255; out[2] = 0; }
            out[3] = 255;
        }
}

static VkImage tex_images[2];
static VkDeviceMemory tex_memory[2];
static VkImageView tex_views[2];
static VkSampler sampler;

static int bc_offered( VkFormat format )
{
    VkFormatProperties fp;
    p_vkGetPhysicalDeviceFormatProperties( physical, format, &fp );
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

static int upload_texture( int index, int bc7 )
{
    VkFormat bc = bc7 ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    int native = bc_offered( bc );
    VkFormat format = native ? bc : VK_FORMAT_R8G8B8A8_UNORM;
    VkDeviceSize size = native ? (bc7 ? 16 * 256 : 8 * 256) : 64 * 64 * 4;
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 64, 64, 1 } };
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkCommandBuffer cb;
    void *data;
    int ok;

    printf( "texture %s: %s\n", bc7 ? "BC7" : "BC1", native ? "the device offers it" : "not offered: an RGBA8 stand-in" );
    if (!make_image( format, 64, 64, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, &tex_images[index], &tex_memory[index], &tex_views[index] ) ||
        !make_buffer( size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging, &staging_memory ) ||
        p_vkMapMemory( device, staging_memory, 0, size, 0, &data ) != VK_SUCCESS)
    {
        FAIL( VKT_FAIL_TEXTURES, "texture %d: no image or staging buffer", index );
        return 0;
    }
    if (!native) rgba_pixels( data, bc7 );
    else if (bc7) bc7_blocks( data );
    else bc1_blocks( data );
    p_vkUnmapMemory( device, staging_memory );

    cb = begin_once();
    barrier( cb, tex_images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );
    p_vkCmdCopyBufferToImage( cb, staging, tex_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
    barrier( cb, tex_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );
    ok = CHECK_VK( VKT_FAIL_TEXTURES, end_once( cb ) );
    p_vkDestroyBuffer( device, staging, NULL );
    p_vkFreeMemory( device, staging_memory, NULL );
    return ok;
}

/* ---------------------------------------------------------------- 3. the cube */

struct vertex { float pos[3], uv[2], tex; };
static VkBuffer vertex_buffer, index_buffer;
static VkDeviceMemory vertex_memory, index_memory;

static int make_cube( void )
{
    static const float faces[6][4][3] =
    {
        { {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1} }, { { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1} },
        { { 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1} }, { {-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1} },
        { {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1} }, { {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1} },
    };
    static const float uvs[4][2] = { {0, 1}, {1, 1}, {1, 0}, {0, 0} };
    struct vertex *v;
    uint16_t *idx;
    int f, k;

    if (!make_buffer( sizeof(struct vertex) * 24, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vertex_buffer, &vertex_memory ) ||
        !make_buffer( sizeof(uint16_t) * 36, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &index_buffer, &index_memory ))
        return 0;
    p_vkMapMemory( device, vertex_memory, 0, VK_WHOLE_SIZE, 0, (void **)&v );
    p_vkMapMemory( device, index_memory, 0, VK_WHOLE_SIZE, 0, (void **)&idx );
    for (f = 0; f < 6; f++)
    {
        for (k = 0; k < 4; k++)
        {
            memcpy( v[f * 4 + k].pos, faces[f][k], sizeof(v->pos) );
            memcpy( v[f * 4 + k].uv, uvs[k], sizeof(v->uv) );
            v[f * 4 + k].tex = f < 3 ? 0.0f : 1.0f;
        }
        idx[f * 6 + 0] = (uint16_t)(f * 4); idx[f * 6 + 1] = (uint16_t)(f * 4 + 1); idx[f * 6 + 2] = (uint16_t)(f * 4 + 2);
        idx[f * 6 + 3] = (uint16_t)(f * 4); idx[f * 6 + 4] = (uint16_t)(f * 4 + 2); idx[f * 6 + 5] = (uint16_t)(f * 4 + 3);
    }
    p_vkUnmapMemory( device, vertex_memory );
    p_vkUnmapMemory( device, index_memory );
    return 1;
}

static int make_render_pass( void )
{
    VkAttachmentDescription att[2] =
    {
        { 0, swap_format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
          VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR },
        { 0, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
          VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
    };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
        .pColorAttachments = &color, .pDepthStencilAttachment = &depth };
    VkSubpassDependency dep = { VK_SUBPASS_EXTERNAL, 0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0 };
    VkRenderPassCreateInfo info = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2,
        .pAttachments = att, .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
    return CHECK_VK( VKT_FAIL_RENDER, p_vkCreateRenderPass( device, &info, NULL, &render_pass ) );
}

/* a pipeline of the cube (tess: the tessellation smoke test's, on the same render pass) */
static VkPipeline make_pipeline( int tess )
{
    VkShaderModule vs = tess ? shader( solid_vert, sizeof(solid_vert) ) : shader( cube_vert, sizeof(cube_vert) );
    VkShaderModule fs = tess ? shader( solid_frag, sizeof(solid_frag) ) : shader( cube_frag, sizeof(cube_frag) );
    VkShaderModule tcs = tess ? shader( tess_tesc, sizeof(tess_tesc) ) : VK_NULL_HANDLE;
    VkShaderModule tes = tess ? shader( tess_tese, sizeof(tess_tese) ) : VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[4] =
    {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tcs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, tes, "main", NULL },
    };
    VkVertexInputBindingDescription binding = { 0, sizeof(struct vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[3] =
    {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32_SFLOAT, 12 }, { 2, 0, VK_FORMAT_R32_SFLOAT, 20 },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = tess ? 0 : 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = tess ? 0 : 3, .pVertexAttributeDescriptions = attrs };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = tess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineTessellationStateCreateInfo ts = { .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .patchControlPoints = 3 };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS };
    VkPipelineColorBlendAttachmentState blend = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo info = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = tess ? 4 : 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pTessellationState = tess ? &ts : NULL, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = pipeline_layout, .renderPass = render_pass };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;

    if (vs && fs && (!tess || (tcs && tes))) res = p_vkCreateGraphicsPipelines( device, VK_NULL_HANDLE, 1, &info, NULL, &p );
    if (res != VK_SUCCESS) p = VK_NULL_HANDLE;
    if (vs) p_vkDestroyShaderModule( device, vs, NULL );
    if (fs) p_vkDestroyShaderModule( device, fs, NULL );
    if (tcs) p_vkDestroyShaderModule( device, tcs, NULL );
    if (tes) p_vkDestroyShaderModule( device, tes, NULL );
    return p;
}

static int make_descriptors( void )
{
    VkDescriptorSetLayoutBinding b[2] =
    {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo linfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = b };
    VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo pinfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
        .poolSizeCount = 1, .pPoolSizes = &size };
    VkDescriptorSetAllocateInfo ainfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1 };
    VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 };
    VkPipelineLayoutCreateInfo plinfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
        .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
    VkSamplerCreateInfo sinfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = 1.0f };
    VkDescriptorImageInfo images_info[2];
    VkWriteDescriptorSet writes[2];
    int i;

    if (!CHECK_VK( VKT_FAIL_RENDER, p_vkCreateDescriptorSetLayout( device, &linfo, NULL, &set_layout ) ) ||
        !CHECK_VK( VKT_FAIL_RENDER, p_vkCreateDescriptorPool( device, &pinfo, NULL, &descriptor_pool ) ) ||
        !CHECK_VK( VKT_FAIL_RENDER, p_vkCreatePipelineLayout( device, &plinfo, NULL, &pipeline_layout ) ) ||
        !CHECK_VK( VKT_FAIL_RENDER, p_vkCreateSampler( device, &sinfo, NULL, &sampler ) ))
        return 0;
    ainfo.descriptorPool = descriptor_pool;
    ainfo.pSetLayouts = &set_layout;
    if (!CHECK_VK( VKT_FAIL_RENDER, p_vkAllocateDescriptorSets( device, &ainfo, &descriptor_set ) )) return 0;
    for (i = 0; i < 2; i++)
    {
        images_info[i].sampler = sampler;
        images_info[i].imageView = tex_views[i];
        images_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset( &writes[i], 0, sizeof(writes[i]) );
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptor_set;
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images_info[i];
    }
    p_vkUpdateDescriptorSets( device, 2, writes, 0, NULL );
    return 1;
}

static void destroy_swapchain( void )
{
    uint32_t i;
    for (i = 0; i < image_count; i++)
    {
        if (framebuffers[i]) p_vkDestroyFramebuffer( device, framebuffers[i], NULL );
        if (views[i]) p_vkDestroyImageView( device, views[i], NULL );
        framebuffers[i] = VK_NULL_HANDLE;
        views[i] = VK_NULL_HANDLE;
    }
    if (depth_view) p_vkDestroyImageView( device, depth_view, NULL );
    if (depth_image) p_vkDestroyImage( device, depth_image, NULL );
    if (depth_memory) p_vkFreeMemory( device, depth_memory, NULL );
    depth_view = VK_NULL_HANDLE; depth_image = VK_NULL_HANDLE; depth_memory = VK_NULL_HANDLE;
}

static int make_swapchain( void )
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[32];
    uint32_t nformats = 32, i;
    VkSwapchainKHR old = swapchain;
    VkSwapchainCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    VkCommandBufferAllocateInfo cinfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY };

    if (!CHECK_VK( VKT_FAIL_RENDER, p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical, surface, &caps ) )) return 0;
    if (!CHECK_VK( VKT_FAIL_RENDER, p_vkGetPhysicalDeviceSurfaceFormatsKHR( physical, surface, &nformats, formats ) ))
        return 0;
    swap_format = formats[0].format;
    for (i = 0; i < nformats; i++) if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) swap_format = formats[i].format;
    extent = caps.currentExtent;
    if (!extent.width || !extent.height) return 1;   /* minimised: nothing to draw */
    info.surface = surface;
    info.minImageCount = caps.minImageCount < 3 && (!caps.maxImageCount || caps.maxImageCount >= 3) ? 3 : caps.minImageCount;
    info.imageFormat = swap_format;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent;
    info.oldSwapchain = old;
    if (!CHECK_VK( VKT_FAIL_RENDER, p_vkCreateSwapchainKHR( device, &info, NULL, &swapchain ) )) return 0;
    if (old)
    {
        destroy_swapchain();
        p_vkDestroySwapchainKHR( device, old, NULL );
    }
    if (!render_pass && !make_render_pass()) return 0;
    image_count = 8;
    p_vkGetSwapchainImagesKHR( device, swapchain, &image_count, images );
    if (!make_image( VK_FORMAT_D32_SFLOAT, extent.width, extent.height, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT, &depth_image, &depth_memory, &depth_view ))
    {
        FAIL( VKT_FAIL_RENDER, "no depth buffer" );
        return 0;
    }
    for (i = 0; i < image_count; i++)
    {
        VkImageViewCreateInfo vinfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = swap_format, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VkImageView att[2];
        VkFramebufferCreateInfo finfo = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = render_pass,
            .attachmentCount = 2, .pAttachments = att, .width = extent.width, .height = extent.height, .layers = 1 };
        if (!CHECK_VK( VKT_FAIL_RENDER, p_vkCreateImageView( device, &vinfo, NULL, &views[i] ) )) return 0;
        att[0] = views[i];
        att[1] = depth_view;
        if (!CHECK_VK( VKT_FAIL_RENDER, p_vkCreateFramebuffer( device, &finfo, NULL, &framebuffers[i] ) )) return 0;
    }
    if (!cmdbufs[0])
    {
        cinfo.commandPool = cmdpool;
        cinfo.commandBufferCount = 8;
        if (!CHECK_VK( VKT_FAIL_RENDER, p_vkAllocateCommandBuffers( device, &cinfo, cmdbufs ) )) return 0;
    }
    printf( "swapchain: %u x %u, %u images, format %d\n", extent.width, extent.height, image_count, (int)swap_format );
    return 1;
}

static void matrix( float *m, double t )
{
    float aspect = extent.height ? (float)extent.width / (float)extent.height : 1.0f;
    float f = 1.0f / tanf( 0.5f ), znear = 0.1f, zfar = 20.0f;
    float c = cosf( (float)t ), s = sinf( (float)t ), c2 = cosf( (float)t * 0.7f ), s2 = sinf( (float)t * 0.7f );
    /* rotation about y then x, the cube 5 units away, then the projection (column-major) */
    float r[16] = { c, s2 * s, -c2 * s, 0,  0, c2, s2, 0,  s, -s2 * c, c2 * c, 0,  0, 0, -5, 1 };
    float p[16] = { f / aspect, 0, 0, 0,  0, -f, 0, 0,  0, 0, zfar / (znear - zfar), -1,
                    0, 0, znear * zfar / (znear - zfar), 0 };
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
        {
            m[i * 4 + j] = 0;
            for (k = 0; k < 4; k++) m[i * 4 + j] += p[k * 4 + j] * r[i * 4 + k];
        }
}

static void record( VkCommandBuffer cb, uint32_t image, double t )
{
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkClearValue clear[2] = { { .color = { { 0.05f, 0.05f, 0.1f, 1.0f } } }, { .depthStencil = { 1.0f, 0 } } };
    VkRenderPassBeginInfo rp = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = render_pass,
        .framebuffer = framebuffers[image], .renderArea = { { 0, 0 }, extent }, .clearValueCount = 2, .pClearValues = clear };
    VkViewport viewport = { 0, 0, (float)extent.width, (float)extent.height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, extent };
    VkDeviceSize offset = 0;
    float mvp[16];

    matrix( mvp, t );
    p_vkResetCommandBuffer( cb, 0 );
    p_vkBeginCommandBuffer( cb, &begin );
    p_vkCmdBeginRenderPass( cb, &rp, VK_SUBPASS_CONTENTS_INLINE );
    p_vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
    p_vkCmdSetViewport( cb, 0, 1, &viewport );
    p_vkCmdSetScissor( cb, 0, 1, &scissor );
    p_vkCmdBindVertexBuffers( cb, 0, 1, &vertex_buffer, &offset );
    p_vkCmdBindIndexBuffer( cb, index_buffer, 0, VK_INDEX_TYPE_UINT16 );
    p_vkCmdBindDescriptorSets( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &descriptor_set, 0, NULL );
    p_vkCmdPushConstants( cb, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), mvp );
    p_vkCmdDrawIndexed( cb, 36, 1, 0, 0, 0 );
    p_vkCmdEndRenderPass( cb );
    p_vkEndCommandBuffer( cb );
}

static LRESULT CALLBACK wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    if (msg == WM_SIZE) InterlockedExchange( &resized, 1 );
    if (msg == WM_CLOSE) { PostQuitMessage( 0 ); return 0; }
    return DefWindowProcA( hwnd, msg, wp, lp );
}

static void render( double seconds )
{
    VkSemaphoreCreateInfo sinfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo finfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VkWin32SurfaceCreateInfoKHR surf = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    WNDCLASSA wc = { .lpfnWndProc = wndproc, .lpszClassName = "vktest", .hInstance = GetModuleHandleA( NULL ) };
    VkSemaphore acquired = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    LARGE_INTEGER freq, start, now, last;
    unsigned long long frames = 0, frames_since = 0;
    VkBool32 present_ok = VK_FALSE;
    MSG msg;

    RegisterClassA( &wc );
    window = CreateWindowExA( 0, "vktest", "vktest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 1280, 720, NULL, NULL,
                              wc.hInstance, NULL );
    if (!window) { FAIL( VKT_FAIL_RENDER, "no window" ); return; }
    surf.hinstance = wc.hInstance;
    surf.hwnd = window;
    if (!CHECK_VK( VKT_FAIL_RENDER, p_vkCreateWin32SurfaceKHR( instance, &surf, NULL, &surface ) )) return;
    p_vkGetPhysicalDeviceSurfaceSupportKHR( physical, family, surface, &present_ok );
    if (!present_ok) { FAIL( VKT_FAIL_RENDER, "the queue can't present to the surface" ); return; }
    if (!make_swapchain()) return;
    if (!(pipeline = make_pipeline( 0 ))) { FAIL( VKT_FAIL_RENDER, "no cube pipeline" ); return; }
    p_vkCreateSemaphore( device, &sinfo, NULL, &acquired );
    p_vkCreateSemaphore( device, &sinfo, NULL, &done );
    p_vkCreateFence( device, &finfo, NULL, &fence );

    QueryPerformanceFrequency( &freq );
    QueryPerformanceCounter( &start );
    last = start;
    for (;;)
    {
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired,
            .pWaitDstStageMask = &stage, .commandBufferCount = 1, .signalSemaphoreCount = 1, .pSignalSemaphores = &done };
        VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
            .pWaitSemaphores = &done, .swapchainCount = 1, .pSwapchains = &swapchain };
        double t;
        uint32_t image;
        VkResult res;

        while (PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE ))
        {
            if (msg.message == WM_QUIT) goto out;
            TranslateMessage( &msg );
            DispatchMessageA( &msg );
        }
        QueryPerformanceCounter( &now );
        t = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        if (seconds > 0 && t >= seconds) break;
        if ((double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart >= 1.0)
        {
            printf( "fps: %.1f (%llu frames)\n", (double)frames_since * (double)freq.QuadPart / (double)(now.QuadPart - last.QuadPart), frames );
            fflush( stdout );
            frames_since = 0;
            last = now;
        }
        if (InterlockedExchange( &resized, 0 ))
        {
            p_vkDeviceWaitIdle( device );
            if (!make_swapchain()) break;
            printf( "resized\n" );
        }
        if (!extent.width || !extent.height) { Sleep( 16 ); continue; }

        p_vkWaitForFences( device, 1, &fence, VK_TRUE, UINT64_MAX );
        res = p_vkAcquireNextImageKHR( device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &image );
        if (res == VK_ERROR_OUT_OF_DATE_KHR) { InterlockedExchange( &resized, 1 ); continue; }
        if (res < 0) { FAIL( VKT_FAIL_RENDER, "vkAcquireNextImageKHR: %d", (int)res ); break; }
        if (res == VK_SUBOPTIMAL_KHR) printf( "acquire: suboptimal\n" );
        p_vkResetFences( device, 1, &fence );
        record( cmdbufs[image], image, t );
        submit.pCommandBuffers = &cmdbufs[image];
        if (!CHECK_VK( VKT_FAIL_RENDER, p_vkQueueSubmit( queue, 1, &submit, fence ) )) break;
        present.pImageIndices = &image;
        res = p_vkQueuePresentKHR( queue, &present );
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
        {
            if (res == VK_SUBOPTIMAL_KHR) printf( "present: suboptimal (a rebuild)\n" );
            InterlockedExchange( &resized, 1 );
        }
        else if (res < 0) { FAIL( VKT_FAIL_RENDER, "vkQueuePresentKHR: %d", (int)res ); break; }
        frames++;
        frames_since++;
    }
out:
    p_vkDeviceWaitIdle( device );
    printf( "render: %llu frames\n", frames );
    if (!frames) FAIL( VKT_FAIL_RENDER, "no frame presented" );
    if (fence) p_vkDestroyFence( device, fence, NULL );
    if (acquired) p_vkDestroySemaphore( device, acquired, NULL );
    if (done) p_vkDestroySemaphore( device, done, NULL );
}

/* ---------------------------------------------------------------- 4. smoke tests */

static void test_tessellation( void )
{
    VkPipeline p;

    if (!have_tessellation) { printf( "tessellation: not offered\n" ); return; }
    if (!render_pass) { printf( "tessellation: no render pass (no window): skipped\n" ); return; }
    if (!(p = make_pipeline( 1 ))) { FAIL( VKT_FAIL_TESSELLATION, "no tessellation pipeline" ); return; }
    printf( "tessellation: a pipeline with both stages made\n" );
    p_vkDestroyPipeline( device, p, NULL );
}

static void test_descriptor_indexing( void )
{
    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo binfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 1, .pBindingFlags = &flags };
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo linfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &binfo,
        .bindingCount = 1, .pBindings = &b };
    VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 };
    VkDescriptorPoolCreateInfo pinfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
        .poolSizeCount = 1, .pPoolSizes = &size };
    uint32_t count = 16;
    VkDescriptorSetVariableDescriptorCountAllocateInfo vinfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, .descriptorSetCount = 1,
        .pDescriptorCounts = &count };
    VkDescriptorSetAllocateInfo ainfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .pNext = &vinfo,
        .descriptorSetCount = 1 };
    VkDescriptorImageInfo image = { sampler, tex_views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstArrayElement = 3,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image };
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;

    if (!have_indexing) { printf( "descriptor indexing: not offered\n" ); return; }
    if (CHECK_VK( VKT_FAIL_DESCRIPTOR_INDEXING, p_vkCreateDescriptorSetLayout( device, &linfo, NULL, &layout ) ) &&
        CHECK_VK( VKT_FAIL_DESCRIPTOR_INDEXING, p_vkCreateDescriptorPool( device, &pinfo, NULL, &pool ) ))
    {
        ainfo.descriptorPool = pool;
        ainfo.pSetLayouts = &layout;
        if (CHECK_VK( VKT_FAIL_DESCRIPTOR_INDEXING, p_vkAllocateDescriptorSets( device, &ainfo, &set ) ) && sampler && tex_views[0])
        {
            write.dstSet = set;
            p_vkUpdateDescriptorSets( device, 1, &write, 0, NULL );
            printf( "descriptor indexing: a partly bound set of 16 of 64 written\n" );
        }
    }
    if (pool) p_vkDestroyDescriptorPool( device, pool, NULL );
    if (layout) p_vkDestroyDescriptorSetLayout( device, layout, NULL );
}

static void test_timeline( void )
{
    VkSemaphoreTypeCreateInfo type = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE };
    VkSemaphoreCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type };
    VkSemaphore sem = VK_NULL_HANDLE;
    VkSemaphoreSignalInfo signal = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .value = 1 };
    uint64_t value = 0, two = 2;
    VkSemaphoreWaitInfo wait = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pValues = &two };
    VkTimelineSemaphoreSubmitInfo tl = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &two };
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tl, .signalSemaphoreCount = 1 };

    if (!have_timeline) { printf( "timeline semaphores: not offered\n" ); return; }
    if (!CHECK_VK( VKT_FAIL_TIMELINE, p_vkCreateSemaphore( device, &info, NULL, &sem ) )) return;
    signal.semaphore = sem;
    if (CHECK_VK( VKT_FAIL_TIMELINE, p_vkSignalSemaphore( device, &signal ) ) &&
        CHECK_VK( VKT_FAIL_TIMELINE, p_vkGetSemaphoreCounterValue( device, sem, &value ) ) && value != 1)
        FAIL( VKT_FAIL_TIMELINE, "the host signal gave %llu", (unsigned long long)value );
    submit.pSignalSemaphores = &sem;
    wait.pSemaphores = &sem;
    if (CHECK_VK( VKT_FAIL_TIMELINE, p_vkQueueSubmit( queue, 1, &submit, VK_NULL_HANDLE ) ))
    {
        VkResult res = p_vkWaitSemaphores( device, &wait, 2000000000ull );
        if (res != VK_SUCCESS) FAIL( VKT_FAIL_TIMELINE, "waiting for value 2 gave %d", (int)res );
        else printf( "timeline semaphores: host signal, queue signal and wait done\n" );
    }
    p_vkQueueWaitIdle( queue );
    p_vkDestroySemaphore( device, sem, NULL );
}

static void test_dynamic_rendering( void )
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderingAttachmentInfo att = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { { 1, 0, 1, 1 } } } };
    VkRenderingInfo info = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { 64, 64 } },
        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att };
    VkCommandBuffer cb;

    if (!have_dynamic_rendering || !p_vkCmdBeginRendering) { printf( "dynamic rendering: not offered\n" ); return; }
    if (!make_image( VK_FORMAT_R8G8B8A8_UNORM, 64, 64, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                     &image, &memory, &view ))
    {
        FAIL( VKT_FAIL_DYNAMIC_RENDERING, "no image" );
        return;
    }
    att.imageView = view;
    cb = begin_once();
    barrier( cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );
    p_vkCmdBeginRendering( cb, &info );
    p_vkCmdEndRendering( cb );
    if (CHECK_VK( VKT_FAIL_DYNAMIC_RENDERING, end_once( cb ) )) printf( "dynamic rendering: a cleared pass submitted\n" );
    p_vkDestroyImageView( device, view, NULL );
    p_vkDestroyImage( device, image, NULL );
    p_vkFreeMemory( device, memory, NULL );
}

static void test_mapped_memory( void )
{
    const VkDeviceSize size = 1 << 20;
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
    VkDeviceMemory src_memory = VK_NULL_HANDLE, dst_memory = VK_NULL_HANDLE;
    VkBufferCopy region = { 0, 0, size };
    VkCommandBuffer cb;
    uint32_t *p, i, bad = 0;

    if (!make_buffer( size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, &src, &src_memory ) ||
        !make_buffer( size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host, &dst, &dst_memory ) ||
        p_vkMapMemory( device, src_memory, 0, VK_WHOLE_SIZE, 0, (void **)&p ) != VK_SUCCESS)
    {
        FAIL( VKT_FAIL_MAPPED_MEMORY, "no mapped buffer" );
        return;
    }
    printf( "mapped memory: at %p\n", (void *)p );
    for (i = 0; i < size / 4; i++) p[i] = i * 2654435761u;
    p_vkUnmapMemory( device, src_memory );
    cb = begin_once();
    p_vkCmdCopyBuffer( cb, src, dst, 1, &region );
    if (CHECK_VK( VKT_FAIL_MAPPED_MEMORY, end_once( cb ) ) &&
        p_vkMapMemory( device, dst_memory, 0, VK_WHOLE_SIZE, 0, (void **)&p ) == VK_SUCCESS)
    {
        for (i = 0; i < size / 4; i++) if (p[i] != i * 2654435761u) bad++;
        p_vkUnmapMemory( device, dst_memory );
        if (bad) FAIL( VKT_FAIL_MAPPED_MEMORY, "%u words differ after a GPU copy", bad );
        else printf( "mapped memory: 1 MB written, copied by the GPU, read back the same\n" );
    }
    p_vkDestroyBuffer( device, src, NULL );
    p_vkDestroyBuffer( device, dst, NULL );
    p_vkFreeMemory( device, src_memory, NULL );
    p_vkFreeMemory( device, dst_memory, NULL );
}

/* ---------------------------------------------------------------- main */

int main( int argc, char **argv )
{
    double seconds = 10;
    int i, with_window = 1;

    setvbuf( stdout, NULL, _IONBF, 0 );
    for (i = 1; i < argc; i++)
    {
        if (!strcmp( argv[i], "--seconds" ) && i + 1 < argc) seconds = atof( argv[++i] );
        else if (!strcmp( argv[i], "--forever" )) seconds = 0;
        else if (!strcmp( argv[i], "--no-window" )) with_window = 0;
    }
    printf( "vktest (spec 34 M6), %s\n", sizeof(void *) == 8 ? "64-bit" : "32-bit" );
    if (create_instance() && create_device())
    {
        struct vkgx_env env = { p_vkGetInstanceProcAddr, instance, physical, device, queue, family };
        unsigned int gx = vkgx_run( &env, &geometry_caps );

        if (gx & VKGX_FAIL_GEOMETRY_DRAW) result |= VKT_FAIL_GEOMETRY_DRAW;
        if (gx & VKGX_FAIL_XFB) result |= VKT_FAIL_XFB;
        if (upload_texture( 0, 0 ) && upload_texture( 1, 1 ) && make_descriptors() && make_cube())
        {
            if (with_window) render( seconds );
        }
        else FAIL( VKT_FAIL_RENDER, "no textures, descriptors or cube" );
        test_tessellation();
        test_descriptor_indexing();
        test_timeline();
        test_dynamic_rendering();
        test_mapped_memory();
        p_vkDeviceWaitIdle( device );
        if (pipeline) p_vkDestroyPipeline( device, pipeline, NULL );
        destroy_swapchain();
        if (swapchain) p_vkDestroySwapchainKHR( device, swapchain, NULL );
        if (render_pass) p_vkDestroyRenderPass( device, render_pass, NULL );
        if (pipeline_layout) p_vkDestroyPipelineLayout( device, pipeline_layout, NULL );
        if (descriptor_pool) p_vkDestroyDescriptorPool( device, descriptor_pool, NULL );
        if (set_layout) p_vkDestroyDescriptorSetLayout( device, set_layout, NULL );
        if (sampler) p_vkDestroySampler( device, sampler, NULL );
        for (i = 0; i < 2; i++)
        {
            if (tex_views[i]) p_vkDestroyImageView( device, tex_views[i], NULL );
            if (tex_images[i]) p_vkDestroyImage( device, tex_images[i], NULL );
            if (tex_memory[i]) p_vkFreeMemory( device, tex_memory[i], NULL );
        }
        if (vertex_buffer) p_vkDestroyBuffer( device, vertex_buffer, NULL );
        if (index_buffer) p_vkDestroyBuffer( device, index_buffer, NULL );
        if (vertex_memory) p_vkFreeMemory( device, vertex_memory, NULL );
        if (index_memory) p_vkFreeMemory( device, index_memory, NULL );
        p_vkDestroyCommandPool( device, cmdpool, NULL );
        p_vkDestroyDevice( device, NULL );
        if (surface) p_vkDestroySurfaceKHR( instance, surface, NULL );
        p_vkDestroyInstance( instance, NULL );
    }
    printf( "vktest: result 0x%x%s\n", result, result ? " (bits: see VKT_FAIL_* in vktest.c)" : " (all passed)" );
    return (int)result;
}
