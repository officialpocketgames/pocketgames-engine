// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part E: the iOS Vulkan layer between win32u's vulkan.c and the host driver (MoltenVK,
// linked statically). What other parts call. The spec writer adds E1's, E8's and Part D7's
// declarations to interfaces/wine_unix_contract.h when spec 34 is final; until then win32u
// declares the ones it uses itself (out/12-win32u-ios/win32u-unix/vulkan_ios.h), with the same
// types.

#ifndef IOS_VK_LAYER_H
#define IOS_VK_LAYER_H

#include "ios_vk_vulkan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* E1: the host driver as win32u sees it. win32u's "dlsym" of vkGetInstanceProcAddr and
 * vkGetDeviceProcAddr returns these (Part D1). An intercepted name (E2-E8) gives the layer's
 * wrapper, any other the driver's own pointer; a name the driver doesn't know gives NULL. */
PFN_vkVoidFunction ios_vk_host_get_instance_proc_addr( VkInstance instance, const char *name );
PFN_vkVoidFunction ios_vk_host_get_device_proc_addr( VkDevice device, const char *name );

/* E8: hands a device the program will never use again (its pseudo-process ended) to the
 * layer's release thread and returns at once. That thread waits for the device to go idle
 * (giving up after 2 s: the device is then left and logged), destroys every handle the layer
 * recorded for it in E8's order, then the device. Unknown or NULL devices are ignored. */
void ios_vk_layer_release_device( VkDevice device );

/* Not in the spec's list (CHANGES.md): the number of handles the layer has recorded for a
 * device (0 for an unknown one), for Part H's per-process log line, which win32u writes. */
unsigned int ios_vk_layer_device_objects( VkDevice device );

/* Not in the spec's list (CHANGES.md): how the layer learns the calling pseudo-process's id for
 * its "once per process" log lines. win32u sets it (GetCurrentProcessId() of the unix side);
 * unset, every line counts as one process's. */
void ios_vk_layer_set_process_id_func( unsigned int (*fn)( void ) );

/* For tests and an orderly app exit: waits up to timeout_ms until the release thread has no
 * job left. Returns 1 when idle. */
int ios_vk_layer_release_drain( unsigned int timeout_ms );

#ifdef __cplusplus
}
#endif

#endif /* IOS_VK_LAYER_H */
