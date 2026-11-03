// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part D: see w32ios_vk_pure.h. No Wine and no Vulkan header.

#include <stdlib.h>
#include <string.h>
#include "w32ios_vk_pure.h"

/* ---------------------------------------------------------------- D1 */

enum w32ios_vk_switch w32ios_vk_switch_from( const char *value )
{
    if (!value || !*value) return W32IOS_VK_OFF_UNSET;
    if (!strcmp( value, "0" )) return W32IOS_VK_OFF_ZERO;
    return W32IOS_VK_ON;
}

const char *w32ios_vk_switch_line( enum w32ios_vk_switch value )
{
    switch (value)
    {
    case W32IOS_VK_ON: return "Vulkan on";
    case W32IOS_VK_OFF_ZERO: return "Vulkan off (PG_VULKAN=0)";
    default: return "Vulkan off (PG_VULKAN unset)";
    }
}

int w32ios_vk_open_ok( const char *name, enum w32ios_vk_switch value )
{
    return value == W32IOS_VK_ON && name && !strcmp( name, W32IOS_VK_SONAME );
}

enum w32ios_vk_symbol w32ios_vk_symbol( const char *name )
{
    if (!name) return W32IOS_VK_SYM_NONE;
    if (!strcmp( name, "vkGetInstanceProcAddr" )) return W32IOS_VK_SYM_GET_INSTANCE_PROC_ADDR;
    if (!strcmp( name, "vkGetDeviceProcAddr" )) return W32IOS_VK_SYM_GET_DEVICE_PROC_ADDR;
    return W32IOS_VK_SYM_NONE;
}

/* ---------------------------------------------------------------- D2 */

const char *w32ios_vk_metal_acquire( const struct w32ios_macdrv_functions *app, void *hwnd,
                                     struct w32ios_vk_metal *out )
{
    struct w32ios_macdrv_win_data *data;
    void *view;

    memset( out, 0, sizeof(*out) );
    if (!app || !app->get_win_data || !app->release_win_data || !app->macdrv_create_metal_device ||
        !app->macdrv_release_metal_device || !app->macdrv_view_create_metal_view ||
        !app->macdrv_view_get_metal_layer || !app->macdrv_view_release_metal_view)
        return "the app has no window table";
    if (!(data = app->get_win_data( hwnd ))) return "the window has no data";
    view = data->client_cocoa_view;
    if (!view)
    {
        app->release_win_data( data );
        return "the window has no view";
    }
    if (!(out->device = app->macdrv_create_metal_device()))
    {
        app->release_win_data( data );
        return "no Metal device";
    }
    out->view = app->macdrv_view_create_metal_view( view, out->device );
    app->release_win_data( data );
    if (!out->view)
    {
        app->macdrv_release_metal_device( out->device );
        out->device = NULL;
        return "the app made no Metal view";
    }
    if (!(out->layer = app->macdrv_view_get_metal_layer( out->view )))
    {
        w32ios_vk_metal_release( app, out );
        return "the app's view has no layer";
    }
    return NULL;
}

void w32ios_vk_metal_release( const struct w32ios_macdrv_functions *app, struct w32ios_vk_metal *metal )
{
    if (metal->view) app->macdrv_view_release_metal_view( metal->view );
    if (metal->device) app->macdrv_release_metal_device( metal->device );
    memset( metal, 0, sizeof(*metal) );
}

int w32ios_vk_window_first( struct w32ios_vk_windows *set, void *hwnd )
{
    unsigned int i, n = sizeof(set->hwnds) / sizeof(set->hwnds[0]);

    for (i = 0; i < set->used; i++) if (set->hwnds[i] == hwnd) return 0;
    set->hwnds[set->next] = hwnd;
    set->next = (set->next + 1) % n;
    if (set->used < n) set->used++;
    return 1;
}

/* ---------------------------------------------------------------- D3 */

int w32ios_vk_acquire_result( int result, int extent_matches )
{
    return result == W32IOS_VK_SUBOPTIMAL && extent_matches ? W32IOS_VK_SUCCESS : result;
}

int w32ios_vk_present_result( int result, int *per, const unsigned char *matches, unsigned int count,
                              unsigned int *converted )
{
    unsigned int i, all = 1;
    int any_suboptimal = 0;

    for (i = 0; i < count; i++) if (!matches[i]) all = 0;
    if (per)
    {
        for (i = 0; i < count; i++)
        {
            if (per[i] == W32IOS_VK_SUBOPTIMAL && matches[i])
            {
                per[i] = W32IOS_VK_SUCCESS;
                (*converted)++;
            }
            if (per[i] == W32IOS_VK_SUBOPTIMAL) any_suboptimal = 1;
        }
        /* a suboptimal call has no failed swapchain: its result is the worst one left */
        if (result == W32IOS_VK_SUBOPTIMAL && !any_suboptimal) result = W32IOS_VK_SUCCESS;
        return result;
    }
    /* without pResults the swapchain isn't known: only when every one matches */
    if (result == W32IOS_VK_SUBOPTIMAL && all)
    {
        (*converted)++;
        result = W32IOS_VK_SUCCESS;
    }
    return result;
}

/* ---------------------------------------------------------------- D5 */

int w32ios_vk_d3dkmt_wants_instance( void )
{
    return 0;
}

/* ---------------------------------------------------------------- D7 */

int w32ios_vk_owners_add( struct w32ios_vk_owners *owners, unsigned int pid, int kind, void *object )
{
    if (owners->count == owners->cap)
    {
        unsigned int cap = owners->cap ? owners->cap * 2 : 16;
        struct w32ios_vk_owner *items = realloc( owners->items, cap * sizeof(*items) );
        if (!items) return 0;
        owners->items = items;
        owners->cap = cap;
    }
    owners->items[owners->count].pid = pid;
    owners->items[owners->count].kind = kind;
    owners->items[owners->count].object = object;
    owners->count++;
    return 1;
}

void w32ios_vk_owners_remove( struct w32ios_vk_owners *owners, void *object )
{
    unsigned int i;

    for (i = 0; i < owners->count; i++)
    {
        if (owners->items[i].object != object) continue;
        owners->items[i] = owners->items[--owners->count];
        return;
    }
}

unsigned int w32ios_vk_owners_take( struct w32ios_vk_owners *owners, unsigned int pid, struct w32ios_vk_owner *out )
{
    static const int order[] = { W32IOS_VK_DEVICE, W32IOS_VK_SURFACE, W32IOS_VK_INSTANCE };
    unsigned int i, k, n = 0;

    for (k = 0; k < sizeof(order) / sizeof(order[0]); k++)
        for (i = 0; i < owners->count; i++)
            if (owners->items[i].pid == pid && owners->items[i].kind == order[k]) out[n++] = owners->items[i];
    /* then the table keeps only the other processes' records */
    for (i = k = 0; i < owners->count; i++)
        if (owners->items[i].pid != pid) owners->items[k++] = owners->items[i];
    owners->count = k;
    return n;
}
