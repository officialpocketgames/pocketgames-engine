// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part D: the rules of win32u's Vulkan on iOS that need no Wine and no Vulkan header,
// host-tested by tests/test_w32ios_vk.c. vulkan_ios.inc (compiled into vulkan.c) uses them.

#ifndef W32IOS_VK_PURE_H
#define W32IOS_VK_PURE_H

#include <stdint.h>
#include "w32ios_macdrv.h"

/* ---------------------------------------------------------------- D1: Vulkan on or off */

/* the host driver's library name in this build (w32ios_config.h's SONAME_LIBVULKAN) */
#define W32IOS_VK_SONAME "libvulkan-ios"

/* PG_VULKAN, read once. The owner's decision of 2026-11-03 (spec 34, "Owner's decisions" 1):
 * builds without APPSTORE set PG_VULKAN=1, store builds leave it unset, which is off. So: unset or
 * empty is off, "0" is off, anything else is on. */
enum w32ios_vk_switch
{
    W32IOS_VK_OFF_UNSET,
    W32IOS_VK_OFF_ZERO,
    W32IOS_VK_ON,
};
enum w32ios_vk_switch w32ios_vk_switch_from( const char *value );
/* the log line of a switch, after "[vk-ios] " */
const char *w32ios_vk_switch_line( enum w32ios_vk_switch value );

/* "open" succeeds for exactly W32IOS_VK_SONAME while Vulkan is on */
int w32ios_vk_open_ok( const char *name, enum w32ios_vk_switch value );

/* "symbol": which of the layer's two entry points a name stands for */
enum w32ios_vk_symbol
{
    W32IOS_VK_SYM_NONE,
    W32IOS_VK_SYM_GET_INSTANCE_PROC_ADDR,
    W32IOS_VK_SYM_GET_DEVICE_PROC_ADDR,
};
enum w32ios_vk_symbol w32ios_vk_symbol( const char *name );

/* ---------------------------------------------------------------- D2: the window's layer */

/* What a Vulkan surface keeps of the app's window table: the Metal device and view it made. */
struct w32ios_vk_metal
{
    void *device;
    void *view;
    void *layer;
};

/* D2 steps 1-2: with the app's table (NULL when the app doesn't link one), gets the window's data,
 * makes a Metal device and a Metal view on the window's client view, gets its layer, releases the
 * window data. Returns NULL with *out filled, or the reason for the headless fallback (step 5),
 * with everything it made released again. */
const char *w32ios_vk_metal_acquire( const struct w32ios_macdrv_functions *app, void *hwnd,
                                     struct w32ios_vk_metal *out );
/* D2 step 4: the surface's destroy: releases the view, then the device */
void w32ios_vk_metal_release( const struct w32ios_macdrv_functions *app, struct w32ios_vk_metal *metal );

/* "log once per window": remembers the last 64 windows it said yes for */
struct w32ios_vk_windows
{
    void *hwnds[64];
    unsigned int next, used;
};
int w32ios_vk_window_first( struct w32ios_vk_windows *set, void *hwnd );

/* ---------------------------------------------------------------- D3: suboptimal results */

#define W32IOS_VK_SUCCESS    0
#define W32IOS_VK_SUBOPTIMAL 1000001003

/* vkAcquireNextImage(2)KHR: a host VK_SUBOPTIMAL_KHR is VK_SUCCESS when the swapchain's extent
 * equals the window's client rectangle (extent_matches) */
int w32ios_vk_acquire_result( int result, int extent_matches );

/* vkQueuePresentKHR: the same per swapchain. per (may be NULL) is the program's pResults, rewritten
 * in place; matches[i] says whether swapchain i's extent equals its window's client rectangle.
 * Returns the call's result. *converted counts the results that became VK_SUCCESS. */
int w32ios_vk_present_result( int result, int *per, const unsigned char *matches, unsigned int count,
                              unsigned int *converted );

/* the first 8 such cases are logged */
#define W32IOS_VK_SUBOPTIMAL_LOGS 8

/* ---------------------------------------------------------------- D5: D3DKMT */

/* d3dkmt_init_vulkan creates no instance on iOS: spec 12 Parts C and D answer every D3DKMT query
 * as before spec 34 */
int w32ios_vk_d3dkmt_wants_instance( void );

/* ---------------------------------------------------------------- D7: owners */

enum w32ios_vk_kind { W32IOS_VK_DEVICE, W32IOS_VK_SURFACE, W32IOS_VK_INSTANCE };

struct w32ios_vk_owner
{
    unsigned int pid;      /* the creating pseudo-process */
    int          kind;     /* enum w32ios_vk_kind */
    void        *object;   /* win32u's wrapper */
};

/* Every host device, surface and instance win32u made, with its process. Not thread-safe: the
 * caller's lock guards it. */
struct w32ios_vk_owners
{
    struct w32ios_vk_owner *items;
    unsigned int count, cap;
};

/* 0 on allocation failure (the object then isn't released for its process) */
int w32ios_vk_owners_add( struct w32ios_vk_owners *owners, unsigned int pid, int kind, void *object );
/* the object's record goes (its program destroyed it) */
void w32ios_vk_owners_remove( struct w32ios_vk_owners *owners, void *object );
/* takes every record of pid out of the table into out (room for owners->count entries), devices
 * first, then surfaces, then instances; returns how many */
unsigned int w32ios_vk_owners_take( struct w32ios_vk_owners *owners, unsigned int pid, struct w32ios_vk_owner *out );

#endif /* W32IOS_VK_PURE_H */
