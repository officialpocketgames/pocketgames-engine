// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part D: declarations and macros for win32u's vulkan.c on iOS, included right after its
// #includes (patches/S34-D-vulkan.patch). The code is vulkan_ios.inc, included at its end.

#ifndef W32IOS_VULKAN_IOS_H
#define W32IOS_VULKAN_IOS_H

#include "w32ios_vk_pure.h"

/* The iOS Vulkan layer (out/34-vulkan-ios/layer/ios_vk_layer.h, Part E1 and E8), with the same
 * types. Weak: without part 34 in the link, "open" fails and win32u has no Vulkan, as upstream
 * built without the library. */
extern PFN_vkVoidFunction ios_vk_host_get_instance_proc_addr( VkInstance instance, const char *name ) __attribute__((weak));
extern PFN_vkVoidFunction ios_vk_host_get_device_proc_addr( VkDevice device, const char *name ) __attribute__((weak));
extern void ios_vk_layer_release_device( VkDevice device ) __attribute__((weak));
extern unsigned int ios_vk_layer_device_objects( VkDevice device ) __attribute__((weak));
extern void ios_vk_layer_set_process_id_func( unsigned int (*fn)( void ) ) __attribute__((weak));

/* D1: vulkan.c's dlopen, dlsym and dlclose of the host driver become these, as spec 12 A4 does
 * for FreeType. "open" succeeds for exactly SONAME_LIBVULKAN while PG_VULKAN is on; "symbol"
 * gives the layer's two entry points and NULL for any other name; "close" does nothing. */
void *w32ios_vk_dlopen( const char *name, int flags );
void *w32ios_vk_dlsym( void *handle, const char *name );
int w32ios_vk_dlclose( void *handle );
#define dlopen  w32ios_vk_dlopen
#define dlsym   w32ios_vk_dlsym
#define dlclose w32ios_vk_dlclose

/* D2: the iOS user driver's pVulkanInit (w32ios_driver.c's table) */
UINT w32ios_VulkanInit( UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs );

/* D7: called by ntdll (Part H) when a pseudo-process has ended, before its memory goes back */
void ios_vk_release_process( unsigned int pid );

/* the hooks the patched functions call (vulkan_ios.inc) */
struct swapchain;
static void ios_vk_owner_add( int kind, void *object );
static void ios_vk_owner_remove( void *object );
static BOOL ios_vk_luid( LUID *luid, UINT32 *node_mask );
static void ios_vk_swapchain_scaling( const struct vulkan_instance *instance, const struct vulkan_device *device,
                                      VkSwapchainCreateInfoKHR *info, VkSwapchainPresentScalingCreateInfoEXT *scaling );
static VkResult ios_vk_acquire_result( VkResult res, struct swapchain *swapchain );
static VkResult ios_vk_present_result( VkResult res, VkPresentInfoKHR *info, const VkSwapchainKHR *client_swapchains );

#endif /* W32IOS_VULKAN_IOS_H */
