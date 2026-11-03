/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Included first in every file of winevulkan's unix side on iOS (build-ios.sh: -include). Spec 34
 * Part C2: the build's config.h, then the link-time renames.
 */

#ifndef WINEVULKAN_IOS_CONFIG_H
#define WINEVULKAN_IOS_CONFIG_H

#include "config.h"

/* Part C3: Part G (32-bit programs) isn't built yet */
#ifndef IOS_VK_WOW64
#define IOS_VK_WOW64 0
#endif

/* C2: upstream's tables stay as they are under internal names; winevulkan_ios.c exports
 * winevulkan_unix_call_funcs and winevulkan_unix_call_wow64_funcs (spec 09 A), whose entries count
 * the calls (C5) and go on to these */
#define __wine_unix_call_funcs       winevulkan_upstream_funcs
#define __wine_unix_call_wow64_funcs winevulkan_upstream_wow64_funcs

/* C2: global names of winevulkan's unix side that other unix libraries of the app could have too */
#define vk_funcs          winevulkan_vk_funcs
#define init_vulkan       winevulkan_init_vulkan
#define wow64_init_vulkan winevulkan_wow64_init_vulkan

#endif /* WINEVULKAN_IOS_CONFIG_H */
