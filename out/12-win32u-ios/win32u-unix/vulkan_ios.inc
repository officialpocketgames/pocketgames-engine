/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 34 Part D: the iOS code of win32u's vulkan.c, included at its end
 * (patches/S34-D-vulkan.patch), so it sees the file's statics (vulkan_funcs, struct instance,
 * struct surface, struct swapchain, get_surface_rect, extents_equals, find_next_struct,
 * nulldrv_vulkan_surface_create, free_debug_*). The rules without Wine types are in
 * w32ios_vk_pure.c.
 *   D1  the host driver is the layer of part 34, linked: no dlopen
 *   D2  the iOS user driver's Vulkan functions: a VK_EXT_metal_surface on the app's layer
 *   D3  swapchains: always stretch-scaled; a suboptimal result of a layer-size mismatch hidden
 *   D4  the device LUID DXGI reports
 *   D7  every host device, surface and instance by its pseudo-process, released when it ends
 */

#include <stdarg.h>
#include <stdio.h>
#include "w32ios.h"
#include "w32ios_metal.h"

static void vk_log( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));
static void vk_log( const char *fmt, ... )
{
    char buffer[512];
    va_list args;
    int len = snprintf( buffer, sizeof(buffer), "[vk-ios] " );

    va_start( args, fmt );
    len += vsnprintf( buffer + len, sizeof(buffer) - len - 1, fmt, args );
    va_end( args );
    if (len > (int)sizeof(buffer) - 2) len = sizeof(buffer) - 2;
    buffer[len++] = '\n';
    buffer[len] = 0;
    __wine_dbg_output( buffer );
}

static unsigned int vk_pid( void )
{
    return HandleToULong( NtCurrentTeb()->ClientId.UniqueProcess );
}

/* ---------------------------------------------------------------- D1 */

static pthread_once_t vk_switch_once = PTHREAD_ONCE_INIT;
static enum w32ios_vk_switch vk_switch_value;
static char vk_token;   /* the "library handle" */

static void vk_read_switch( void )
{
    vk_switch_value = w32ios_vk_switch_from( getenv( "PG_VULKAN" ) );
    vk_log( "%s", w32ios_vk_switch_line( vk_switch_value ) );
}

void *w32ios_vk_dlopen( const char *name, int flags )
{
    pthread_once( &vk_switch_once, vk_read_switch );
    if (!w32ios_vk_open_ok( name, vk_switch_value )) return NULL;
    if (!ios_vk_host_get_instance_proc_addr || !ios_vk_host_get_device_proc_addr)
    {
        vk_log( "the Vulkan layer isn't linked into the app: no Vulkan" );
        return NULL;
    }
    if (ios_vk_layer_set_process_id_func) ios_vk_layer_set_process_id_func( vk_pid );
    return &vk_token;
}

void *w32ios_vk_dlsym( void *handle, const char *name )
{
    if (handle != &vk_token) return NULL;
    switch (w32ios_vk_symbol( name ))
    {
    case W32IOS_VK_SYM_GET_INSTANCE_PROC_ADDR: return (void *)ios_vk_host_get_instance_proc_addr;
    case W32IOS_VK_SYM_GET_DEVICE_PROC_ADDR: return (void *)ios_vk_host_get_device_proc_addr;
    default: return NULL;
    }
}

int w32ios_vk_dlclose( void *handle )
{
    return 0;
}

/* ---------------------------------------------------------------- D2 */

/* the app's window table, which DXMT's unix half also uses; weak: absent, every surface is
 * headless */
extern struct w32ios_macdrv_functions macdrv_functions __attribute__((weak));

struct ios_vk_client
{
    struct client_surface  client;
    struct w32ios_vk_metal metal;
};

static void ios_vk_client_destroy( struct client_surface *client )
{
    struct ios_vk_client *surface = CONTAINING_RECORD( client, struct ios_vk_client, client );
    w32ios_vk_metal_release( &macdrv_functions, &surface->metal );
}

static void ios_vk_client_nothing( struct client_surface *client )
{
}

static void ios_vk_client_present( struct client_surface *client, HDC hdc )
{
}

static const struct client_surface_funcs ios_vk_client_funcs =
{
    .destroy = ios_vk_client_destroy,
    .detach = ios_vk_client_nothing,
    .update = ios_vk_client_nothing,
    .present = ios_vk_client_present,
};

static VkResult ios_vk_surface_create( HWND hwnd, const struct vulkan_instance *instance, VkSurfaceKHR *handle,
                                       struct client_surface **client )
{
    static pthread_mutex_t logged_lock = PTHREAD_MUTEX_INITIALIZER;
    static struct w32ios_vk_windows logged;
    const struct w32ios_macdrv_functions *app = &macdrv_functions ? &macdrv_functions : NULL;
    VkMetalSurfaceCreateInfoEXT info = { .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT };
    struct ios_vk_client *surface;
    struct w32ios_vk_metal metal;
    const char *why;
    int first;
    VkResult res;

    if (!(why = w32ios_vk_metal_acquire( app, hwnd, &metal )))
    {
        if (!instance->p_vkCreateMetalSurfaceEXT)
        {
            w32ios_vk_metal_release( app, &metal );
            why = "the instance has no VK_EXT_metal_surface";
        }
        else if (!(surface = client_surface_create( sizeof(*surface), &ios_vk_client_funcs, hwnd )))
        {
            w32ios_vk_metal_release( app, &metal );
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        else
        {
            /* the client surface keeps the view and the device; its destroy releases them */
            surface->metal = metal;
            info.pLayer = metal.layer;
            if (!(res = instance->p_vkCreateMetalSurfaceEXT( instance->host.instance, &info, NULL, handle )))
            {
                *client = &surface->client;
                return VK_SUCCESS;
            }
            client_surface_release( &surface->client );
            why = "vkCreateMetalSurfaceEXT failed";
        }
    }

    /* D2.5: a headless surface; the program can still probe, and draws nowhere */
    pthread_mutex_lock( &logged_lock );
    first = w32ios_vk_window_first( &logged, hwnd );
    pthread_mutex_unlock( &logged_lock );
    if (first) vk_log( "window %p: %s: a headless surface (it draws nowhere)", hwnd, why );
    return nulldrv_vulkan_surface_create( hwnd, instance, handle, client );
}

static VkBool32 ios_vk_presentation_support( struct vulkan_physical_device *physical_device, uint32_t queue )
{
    return VK_TRUE;
}

/* VK_KHR_win32_surface <-> VK_EXT_metal_surface both ways, as winemac.drv does when the host has
 * the Metal surface; VK_EXT_headless_surface goes with it when the host has it, for D2.5's
 * fallback */
static void ios_vk_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    if (extensions->has_VK_KHR_win32_surface)
    {
        extensions->has_VK_EXT_metal_surface = 1;
        if (vulkan_funcs.host_extensions.has_VK_EXT_headless_surface) extensions->has_VK_EXT_headless_surface = 1;
    }
    if (extensions->has_VK_EXT_metal_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void ios_vk_map_device_extensions( struct vulkan_device_extensions *extensions )
{
}

static const struct vulkan_driver_funcs ios_vk_driver_funcs =
{
    .p_vulkan_surface_create = ios_vk_surface_create,
    .p_get_physical_device_presentation_support = ios_vk_presentation_support,
    .p_map_instance_extensions = ios_vk_map_instance_extensions,
    .p_map_device_extensions = ios_vk_map_device_extensions,
};

UINT w32ios_VulkanInit( UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs )
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR( "version mismatch, win32u wants %u but the iOS driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }
    *driver_funcs = &ios_vk_driver_funcs;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- D3 */

/* the game view's layer is almost never the swapchain's size: always stretch when the host can */
static void ios_vk_swapchain_scaling( const struct vulkan_instance *instance, const struct vulkan_device *device,
                                      VkSwapchainCreateInfoKHR *info, VkSwapchainPresentScalingCreateInfoEXT *scaling )
{
    if (!instance->extensions.has_VK_EXT_surface_maintenance1) return;
    if (!device->extensions.has_VK_EXT_swapchain_maintenance1 && !device->extensions.has_VK_KHR_swapchain_maintenance1) return;
    if (find_next_struct( info->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT )) return;
    scaling->scalingBehavior = VK_PRESENT_SCALING_STRETCH_BIT_EXT;
    scaling->pNext = (void *)info->pNext;
    info->pNext = scaling;
}

static LONG ios_vk_suboptimal_logs;

static BOOL ios_vk_extent_matches( struct swapchain *swapchain )
{
    HWND hwnd = swapchain->surface->hwnd;
    RECT client_rect;

    return get_surface_rect( hwnd, &client_rect, NtUserGetDpiForWindow( hwnd ) ) &&
           extents_equals( &swapchain->extents, &client_rect );
}

static void ios_vk_log_suboptimal( const char *where, struct swapchain *swapchain )
{
    LONG n = InterlockedIncrement( &ios_vk_suboptimal_logs );
    if (n <= W32IOS_VK_SUBOPTIMAL_LOGS)
        vk_log( "%s: host VK_SUBOPTIMAL_KHR for swapchain %p (%ux%u, the window's size) returned as VK_SUCCESS%s",
                where, swapchain, swapchain->extents.width, swapchain->extents.height,
                n == W32IOS_VK_SUBOPTIMAL_LOGS ? " (last of these lines)" : "" );
}

static VkResult ios_vk_acquire_result( VkResult res, struct swapchain *swapchain )
{
    VkResult ret;

    if (res != VK_SUBOPTIMAL_KHR) return res;
    ret = w32ios_vk_acquire_result( res, ios_vk_extent_matches( swapchain ) );
    if (ret != res) ios_vk_log_suboptimal( "acquire", swapchain );
    return ret;
}

static VkResult ios_vk_present_result( VkResult res, VkPresentInfoKHR *info, const VkSwapchainKHR *client_swapchains )
{
    unsigned char matches_buffer[16], *matches = matches_buffer;
    unsigned int i, converted = 0, count = info->swapchainCount;
    BOOL any = FALSE;

    if (res != VK_SUBOPTIMAL_KHR && info->pResults)
        for (i = 0; i < count; i++) if (info->pResults[i] == VK_SUBOPTIMAL_KHR) any = TRUE;
    if (res != VK_SUBOPTIMAL_KHR && !any) return res;
    if (count > ARRAY_SIZE(matches_buffer) && !(matches = malloc( count ))) return res;
    for (i = 0; i < count; i++) matches[i] = ios_vk_extent_matches( swapchain_from_handle( client_swapchains[i] ) );
    res = w32ios_vk_present_result( res, (int *)info->pResults, matches, count, &converted );
    if (converted) ios_vk_log_suboptimal( "present", swapchain_from_handle( client_swapchains[0] ) );
    if (matches != matches_buffer) free( matches );
    return res;
}

/* ---------------------------------------------------------------- D4 */

static pthread_once_t ios_vk_luid_once = PTHREAD_ONCE_INIT;
static BOOL ios_vk_have_luid;
static LUID ios_vk_metal_luid;

static void ios_vk_read_luid( void )
{
    uint64_t id, value;

    if (!w32ios_metal_registry_id( &id ))
    {
        vk_log( "no Metal device: the Vulkan device has no LUID" );
        return;
    }
    value = w32ios_luid_from_registry_id( id );   /* spec 12 D1: what DXGI reports */
    ios_vk_metal_luid.LowPart = (DWORD)value;
    ios_vk_metal_luid.HighPart = (LONG)(value >> 32);
    ios_vk_have_luid = TRUE;
}

/* spec 12 C1 keeps the display device lists empty, so get_gpu_info_from_uuid never finds one */
static BOOL ios_vk_luid( LUID *luid, UINT32 *node_mask )
{
    pthread_once( &ios_vk_luid_once, ios_vk_read_luid );
    if (!ios_vk_have_luid) return FALSE;
    *luid = ios_vk_metal_luid;
    *node_mask = 1;
    return TRUE;
}

/* ---------------------------------------------------------------- D7 */

static pthread_mutex_t ios_vk_owners_lock = PTHREAD_MUTEX_INITIALIZER;
static struct w32ios_vk_owners ios_vk_owners;

static void ios_vk_owner_add( int kind, void *object )
{
    int ok;

    pthread_mutex_lock( &ios_vk_owners_lock );
    ok = w32ios_vk_owners_add( &ios_vk_owners, vk_pid(), kind, object );
    pthread_mutex_unlock( &ios_vk_owners_lock );
    if (!ok) vk_log( "out of memory: object %p won't be released with its process", object );
}

static void ios_vk_owner_remove( void *object )
{
    pthread_mutex_lock( &ios_vk_owners_lock );
    w32ios_vk_owners_remove( &ios_vk_owners, object );
    pthread_mutex_unlock( &ios_vk_owners_lock );
}

static void ios_vk_release_device( struct vulkan_device *device )
{
    struct vulkan_instance *instance = device->physical_device->instance;
    unsigned int i;

    /* the layer destroys the host device and what was made on it, on its release thread */
    if (ios_vk_layer_release_device) ios_vk_layer_release_device( device->host.device );
    for (i = 0; i < device->queue_count; i++) instance->p_remove_object( instance, &device->queues[i].obj );
    instance->p_remove_object( instance, &device->obj );
    free( device );
}

static void ios_vk_release_surface( struct surface *surface )
{
    struct vulkan_instance *instance = surface->obj.instance;

    /* the layer puts this behind the release of the instance's devices (a swapchain on it may
     * still exist) */
    instance->p_vkDestroySurfaceKHR( instance->host.instance, surface->obj.host.surface, NULL );
    client_surface_release( surface->client );
    instance->p_remove_object( instance, &surface->obj.obj );
    free( surface );
}

static void ios_vk_release_instance( struct instance *instance )
{
    /* the layer destroys it after its devices */
    instance->obj.p_vkDestroyInstance( instance->obj.host.instance, NULL );
    if (instance->objects.compare) pthread_rwlock_destroy( &instance->objects_lock );
    free_debug_utils_messengers( &instance->utils_messengers );
    free_debug_report_callbacks( &instance->report_callbacks );
    free( instance );
}

void ios_vk_release_process( unsigned int pid )
{
    struct w32ios_vk_owner *taken;
    unsigned int n, i, devices = 0, surfaces = 0, instances = 0, objects = 0;

    pthread_mutex_lock( &ios_vk_owners_lock );
    if (!ios_vk_owners.count || !(taken = malloc( ios_vk_owners.count * sizeof(*taken) )))
    {
        pthread_mutex_unlock( &ios_vk_owners_lock );
        return;
    }
    n = w32ios_vk_owners_take( &ios_vk_owners, pid, taken );
    pthread_mutex_unlock( &ios_vk_owners_lock );

    /* no win32u lock is held from here: the layer may take its time */
    for (i = 0; i < n; i++)
    {
        switch (taken[i].kind)
        {
        case W32IOS_VK_DEVICE:
        {
            struct vulkan_device *device = taken[i].object;
            if (ios_vk_layer_device_objects) objects += ios_vk_layer_device_objects( device->host.device );
            ios_vk_release_device( device );
            devices++;
            break;
        }
        case W32IOS_VK_SURFACE:
            ios_vk_release_surface( taken[i].object );
            surfaces++;
            break;
        case W32IOS_VK_INSTANCE:
            ios_vk_release_instance( taken[i].object );
            instances++;
            break;
        }
    }
    free( taken );
    if (n) vk_log( "process %04x: released %u devices (%u objects), %u surfaces, %u instances",
                   pid, devices, objects, surfaces, instances );
}
