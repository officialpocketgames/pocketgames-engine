// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 clarification 1, item 6: the CI's Vulkan render test, on the macOS runner only (its
// Metal device), never on the owner's PC. MoltenVK (UTM's fork with the adopted patches, built for
// macOS by build-moltenvk.sh --platform macos) is linked directly, with no loader and no Wine.
// It prints the device, then runs tests/common/vkgx.c: a geometry-shader draw and a
// transform-feedback capture, offscreen, read back and compared.
//   - no Metal device, or one without geometry shaders (Metal mesh shaders): SKIP, exit 0;
//   - otherwise exit 0 when both pass, else the VKGX_FAIL_* bits (and 8 when setup fails).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "vkgx.h"

/* MoltenVK's entry point, linked statically (VKAPI_ATTR is empty outside 32-bit Android) */
extern PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr( VkInstance instance, const char *name );

#define SETUP_FAILED 8

static int has_extension( const VkExtensionProperties *exts, uint32_t n, const char *name )
{
    uint32_t i;
    for (i = 0; i < n; i++) if (!strcmp( exts[i].extensionName, name )) return 1;
    return 0;
}

int main( void )
{
    PFN_vkGetInstanceProcAddr gipa = vkGetInstanceProcAddr;
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)gipa( NULL, "vkCreateInstance" );
    PFN_vkEnumerateInstanceExtensionProperties enum_instance_ext =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa( NULL, "vkEnumerateInstanceExtensionProperties" );
    PFN_vkEnumeratePhysicalDevices enum_physical;
    PFN_vkGetPhysicalDeviceProperties get_props;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_families;
    PFN_vkEnumerateDeviceExtensionProperties enum_device_ext;
    PFN_vkCreateDevice create_device;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkGetDeviceProcAddr gdpa;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk-render-test",
        .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo iinfo = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    const char *instance_exts[1];
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props;
    VkQueueFamilyProperties families[16];
    VkExtensionProperties *exts = NULL;
    uint32_t n = 0, count = 1, nfam = 16, family;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
        .pQueuePriorities = &priority };
    VkPhysicalDeviceFeatures features;
    const char *device_exts[2];
    VkDeviceCreateInfo dinfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &q, .pEnabledFeatures = &features, .ppEnabledExtensionNames = device_exts };
    struct vkgx_caps caps;
    struct vkgx_env env;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue;
    const char *xfb;
    unsigned int fails;
    VkResult res;

    setvbuf( stdout, NULL, _IONBF, 0 );
    printf( "vk-render-test (spec 34 clarification 1): MoltenVK linked directly\n" );
    if (!create_instance || !enum_instance_ext) { printf( "FAIL: no vkCreateInstance\n" ); return SETUP_FAILED; }

    /* the portability enumeration, where offered, as a program with a loader would ask for it */
    if (enum_instance_ext( NULL, &n, NULL ) == VK_SUCCESS && n && (exts = calloc( n, sizeof(*exts) )))
    {
        if (enum_instance_ext( NULL, &n, exts ) >= 0 && has_extension( exts, n, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME ))
        {
            instance_exts[iinfo.enabledExtensionCount++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            iinfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        free( exts );
        exts = NULL;
    }
    iinfo.ppEnabledExtensionNames = instance_exts;
    if ((res = create_instance( &iinfo, NULL, &instance )) != VK_SUCCESS)
    {
        /* MoltenVK gives VK_ERROR_INCOMPATIBLE_DRIVER without a Metal device */
        printf( "SKIP: no Vulkan instance (%d): no Metal device on this runner?\n", (int)res );
        return 0;
    }
    enum_physical = (PFN_vkEnumeratePhysicalDevices)gipa( instance, "vkEnumeratePhysicalDevices" );
    get_props = (PFN_vkGetPhysicalDeviceProperties)gipa( instance, "vkGetPhysicalDeviceProperties" );
    get_families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa( instance, "vkGetPhysicalDeviceQueueFamilyProperties" );
    enum_device_ext = (PFN_vkEnumerateDeviceExtensionProperties)gipa( instance, "vkEnumerateDeviceExtensionProperties" );
    create_device = (PFN_vkCreateDevice)gipa( instance, "vkCreateDevice" );
    destroy_instance = (PFN_vkDestroyInstance)gipa( instance, "vkDestroyInstance" );
    gdpa = (PFN_vkGetDeviceProcAddr)gipa( instance, "vkGetDeviceProcAddr" );
    if (!enum_physical || !get_props || !get_families || !enum_device_ext || !create_device || !destroy_instance || !gdpa)
    {
        printf( "FAIL: instance functions missing\n" );
        return SETUP_FAILED;
    }
    res = enum_physical( instance, &count, &physical );
    if (res < 0 || !count || !physical)
    {
        printf( "SKIP: no physical device (%d): no Metal device on this runner?\n", (int)res );
        destroy_instance( instance, NULL );
        return 0;
    }
    get_props( physical, &props );
    printf( "device: %s, Vulkan %u.%u.%u, vendor %04x device %04x\n", props.deviceName,
            VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion), props.vendorID, props.deviceID );
    vkgx_query( gipa, instance, physical, &caps );
    if (!caps.geometry)
    {
        printf( "SKIP: this GPU has no Metal mesh shaders, so no geometry shaders or transform feedback\n" );
        destroy_instance( instance, NULL );
        return 0;
    }

    get_families( physical, &nfam, families );
    for (family = 0; family < nfam; family++) if (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    if (family == nfam) { printf( "FAIL: no graphics queue\n" ); destroy_instance( instance, NULL ); return SETUP_FAILED; }
    q.queueFamilyIndex = family;

    memset( &features, 0, sizeof(features) );
    /* the Vulkan specification requires the portability subset when the device offers it */
    if (enum_device_ext( physical, NULL, &n, NULL ) == VK_SUCCESS && n && (exts = calloc( n, sizeof(*exts) )))
    {
        if (enum_device_ext( physical, NULL, &n, exts ) >= 0 && has_extension( exts, n, "VK_KHR_portability_subset" ))
            device_exts[dinfo.enabledExtensionCount++] = "VK_KHR_portability_subset";
        free( exts );
    }
    if ((xfb = vkgx_device_chain( &caps, &features, &dinfo.pNext ))) device_exts[dinfo.enabledExtensionCount++] = xfb;
    if ((res = create_device( physical, &dinfo, NULL, &device )) != VK_SUCCESS)
    {
        printf( "FAIL: vkCreateDevice with geometryShader%s: %d\n", xfb ? " and transform feedback" : "", (int)res );
        destroy_instance( instance, NULL );
        return SETUP_FAILED;
    }
    ((PFN_vkGetDeviceQueue)gdpa( device, "vkGetDeviceQueue" ))( device, family, 0, &queue );

    env.gipa = gipa;
    env.instance = instance;
    env.physical = physical;
    env.device = device;
    env.queue = queue;
    env.family = family;
    fails = vkgx_run( &env, &caps );

    ((PFN_vkDeviceWaitIdle)gdpa( device, "vkDeviceWaitIdle" ))( device );
    ((PFN_vkDestroyDevice)gdpa( device, "vkDestroyDevice" ))( device, NULL );
    destroy_instance( instance, NULL );
    printf( "vk-render-test: result 0x%x%s\n", fails, fails ? " (VKGX_FAIL_* bits)" : "" );
    return (int)fails;
}
