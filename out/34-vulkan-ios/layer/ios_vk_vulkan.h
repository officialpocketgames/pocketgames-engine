// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part E: the Vulkan declarations the layer is compiled with.
//
// On iOS: the Khronos headers MoltenVK ships (build-moltenvk.sh's <out>/include, Vulkan-Headers
// at the revision the MoltenVK fork pins), without prototypes: the layer reaches the driver only
// through IOS_VK_DRIVER_GIPA (ios_vk_internal.h).
// Host tests (IOS_VK_WINE_HEADERS): Wine 11.4's include/wine/vulkan.h, generated from the same
// registry (vk.xml 1.4.344); WINE_UNIX_LIB gives it no prototypes and an empty VKAPI_CALL, as
// on Apple platforms. Only declarations are used.

#ifndef IOS_VK_VULKAN_H
#define IOS_VK_VULKAN_H

#ifdef IOS_VK_WINE_HEADERS
# ifndef WINE_UNIX_LIB
#  define WINE_UNIX_LIB
# endif
# include <stdarg.h>
# include <stdint.h>
# include "windef.h"
# include "winbase.h"
# include "wine/vulkan.h"
#else
# ifndef VK_NO_PROTOTYPES
#  define VK_NO_PROTOTYPES
# endif
# include <vulkan/vulkan.h>
#endif

/* names the layer uses as strings (VK_KHR_portability_subset is in vulkan_beta.h, which the
 * layer doesn't include) */
#define IVK_PORTABILITY_SUBSET_NAME "VK_KHR_portability_subset"
#define IVK_LAYER_SETTINGS_NAME     "VK_EXT_layer_settings"

/* handle values as numbers and back (non-dispatchable handles are pointers in the Khronos
 * headers and uint64_t in Wine's on 64-bit targets) */
#define IVK_H64( h )           ((uint64_t)(uintptr_t)(h))
#define IVK_HANDLE( type, v )  ((type)(uintptr_t)(v))

#endif /* IOS_VK_VULKAN_H */
