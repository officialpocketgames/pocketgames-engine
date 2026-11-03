// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part M2, in spec 12's harness: win32u's Vulkan rules on iOS (w32ios_vk_pure.c): the
// PG_VULKAN switch and the "library" lookups (D1), the window's Metal layer through a fake app
// table and each fallback to a headless surface (D2), the suboptimal rule (D3), the LUID (D4),
// D3DKMT creating no instance (D5) and the owners of ios_vk_release_process (D7).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w32ios_pure.h"
#include "w32ios_vk_pure.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while (0)

/* ---------------------------------------------------------------- a fake app table */

static char window_view, metal_device, metal_view, metal_layer;
static struct w32ios_macdrv_win_data win_data;
static int data_gets, data_releases, devices_made, devices_released, views_made, views_released;
static void *view_made_on, *view_device;
static int no_data, no_view, no_device, no_metal_view, no_layer;

static struct w32ios_macdrv_win_data *fake_get_win_data( void *hwnd )
{
    data_gets++;
    if (no_data) return NULL;
    win_data.hwnd = hwnd;
    win_data.client_cocoa_view = no_view ? NULL : &window_view;
    return &win_data;
}
static void fake_release_win_data( struct w32ios_macdrv_win_data *data ) { (void)data; data_releases++; }
static void *fake_create_device( void ) { if (no_device) return NULL; devices_made++; return &metal_device; }
static void fake_release_device( void *device ) { CHECK( device == &metal_device ); devices_released++; }
static void *fake_create_view( void *view, void *device )
{
    view_made_on = view;
    view_device = device;
    if (no_metal_view) return NULL;
    views_made++;
    return &metal_view;
}
static void *fake_get_layer( void *view ) { CHECK( view == &metal_view ); return no_layer ? NULL : &metal_layer; }
static void fake_release_view( void *view ) { CHECK( view == &metal_view ); views_released++; }

static struct w32ios_macdrv_functions app =
{
    .get_win_data = fake_get_win_data,
    .release_win_data = fake_release_win_data,
    .macdrv_create_metal_device = fake_create_device,
    .macdrv_release_metal_device = fake_release_device,
    .macdrv_view_create_metal_view = fake_create_view,
    .macdrv_view_get_metal_layer = fake_get_layer,
    .macdrv_view_release_metal_view = fake_release_view,
};

static void reset( void )
{
    data_gets = data_releases = devices_made = devices_released = views_made = views_released = 0;
    no_data = no_view = no_device = no_metal_view = no_layer = 0;
    view_made_on = view_device = NULL;
}

static void test_switch( void )
{
    CHECK( w32ios_vk_switch_from( NULL ) == W32IOS_VK_OFF_UNSET );    /* store builds: off */
    CHECK( w32ios_vk_switch_from( "" ) == W32IOS_VK_OFF_UNSET );
    CHECK( w32ios_vk_switch_from( "0" ) == W32IOS_VK_OFF_ZERO );
    CHECK( w32ios_vk_switch_from( "1" ) == W32IOS_VK_ON );            /* test builds */
    CHECK( w32ios_vk_switch_from( "yes" ) == W32IOS_VK_ON );
    CHECK( !strcmp( w32ios_vk_switch_line( W32IOS_VK_ON ), "Vulkan on" ) );
    CHECK( !strcmp( w32ios_vk_switch_line( W32IOS_VK_OFF_ZERO ), "Vulkan off (PG_VULKAN=0)" ) );
    CHECK( w32ios_vk_open_ok( "libvulkan-ios", W32IOS_VK_ON ) );
    CHECK( !w32ios_vk_open_ok( "libvulkan-ios", W32IOS_VK_OFF_ZERO ) );    /* PG_VULKAN=0 */
    CHECK( !w32ios_vk_open_ok( "libvulkan-ios", W32IOS_VK_OFF_UNSET ) );
    CHECK( !w32ios_vk_open_ok( "libvulkan.1.dylib", W32IOS_VK_ON ) );
    CHECK( !w32ios_vk_open_ok( "libMoltenVK.dylib", W32IOS_VK_ON ) );
    CHECK( !w32ios_vk_open_ok( NULL, W32IOS_VK_ON ) );
    CHECK( w32ios_vk_symbol( "vkGetInstanceProcAddr" ) == W32IOS_VK_SYM_GET_INSTANCE_PROC_ADDR );
    CHECK( w32ios_vk_symbol( "vkGetDeviceProcAddr" ) == W32IOS_VK_SYM_GET_DEVICE_PROC_ADDR );
    CHECK( w32ios_vk_symbol( "vkCreateInstance" ) == W32IOS_VK_SYM_NONE );
    CHECK( w32ios_vk_symbol( NULL ) == W32IOS_VK_SYM_NONE );
}

static void test_surface( void )
{
    struct w32ios_vk_metal metal;
    struct w32ios_macdrv_functions partial = app;
    void *hwnd = (void *)0x10024;

    /* the layer of a Metal view on the window's client view; the window data released */
    reset();
    CHECK( w32ios_vk_metal_acquire( &app, hwnd, &metal ) == NULL );
    CHECK( metal.layer == &metal_layer && metal.view == &metal_view && metal.device == &metal_device );
    CHECK( view_made_on == &window_view && view_device == &metal_device );
    CHECK( data_gets == 1 && data_releases == 1 );
    CHECK( views_released == 0 && devices_released == 0 );   /* the surface keeps them */
    /* the surface's destroy releases the view, then the device */
    w32ios_vk_metal_release( &app, &metal );
    CHECK( views_released == 1 && devices_released == 1 );
    CHECK( metal.view == NULL && metal.device == NULL );

    /* each fallback to headless, with what was made released again */
    reset();
    CHECK( w32ios_vk_metal_acquire( NULL, hwnd, &metal ) != NULL );          /* no table */
    CHECK( data_gets == 0 && !metal.layer );
    partial.macdrv_view_get_metal_layer = NULL;
    CHECK( w32ios_vk_metal_acquire( &partial, hwnd, &metal ) != NULL );      /* a table without the entries */
    CHECK( data_gets == 0 );
    reset(); no_data = 1;
    CHECK( w32ios_vk_metal_acquire( &app, hwnd, &metal ) != NULL );          /* no window data */
    CHECK( data_releases == 0 && devices_made == 0 );
    reset(); no_view = 1;
    CHECK( w32ios_vk_metal_acquire( &app, hwnd, &metal ) != NULL );          /* no view */
    CHECK( data_releases == 1 && devices_made == 0 );
    reset(); no_device = 1;
    CHECK( w32ios_vk_metal_acquire( &app, hwnd, &metal ) != NULL );          /* no Metal device */
    CHECK( data_releases == 1 && views_made == 0 );
    reset(); no_metal_view = 1;
    CHECK( w32ios_vk_metal_acquire( &app, hwnd, &metal ) != NULL );          /* the app returns no view */
    CHECK( data_releases == 1 && devices_made == 1 && devices_released == 1 );
    reset(); no_layer = 1;
    CHECK( w32ios_vk_metal_acquire( &app, hwnd, &metal ) != NULL );          /* ... or no layer */
    CHECK( data_releases == 1 && views_released == 1 && devices_released == 1 );
    CHECK( !metal.view && !metal.device && !metal.layer );

    /* logged once per window */
    {
        struct w32ios_vk_windows set = { { 0 }, 0, 0 };
        CHECK( w32ios_vk_window_first( &set, hwnd ) );
        CHECK( !w32ios_vk_window_first( &set, hwnd ) );
        CHECK( w32ios_vk_window_first( &set, (void *)0x10026 ) );
    }
}

static void test_suboptimal( void )
{
    int per[3];
    unsigned char matches[3] = { 1, 0, 1 };
    unsigned int converted = 0;

    CHECK( w32ios_vk_acquire_result( W32IOS_VK_SUBOPTIMAL, 1 ) == W32IOS_VK_SUCCESS );
    CHECK( w32ios_vk_acquire_result( W32IOS_VK_SUBOPTIMAL, 0 ) == W32IOS_VK_SUBOPTIMAL );   /* a real mismatch */
    CHECK( w32ios_vk_acquire_result( 2, 1 ) == 2 );                                         /* others as they are */
    CHECK( w32ios_vk_acquire_result( -1000001004, 1 ) == -1000001004 );                     /* VK_ERROR_OUT_OF_DATE_KHR */

    /* per swapchain, with pResults */
    per[0] = W32IOS_VK_SUBOPTIMAL; per[1] = W32IOS_VK_SUBOPTIMAL; per[2] = 0;
    CHECK( w32ios_vk_present_result( W32IOS_VK_SUBOPTIMAL, per, matches, 3, &converted ) == W32IOS_VK_SUBOPTIMAL );
    CHECK( per[0] == 0 && per[1] == W32IOS_VK_SUBOPTIMAL && per[2] == 0 && converted == 1 );
    per[0] = W32IOS_VK_SUBOPTIMAL; per[1] = 0; per[2] = W32IOS_VK_SUBOPTIMAL; converted = 0;
    CHECK( w32ios_vk_present_result( W32IOS_VK_SUBOPTIMAL, per, matches, 3, &converted ) == 0 );
    CHECK( per[0] == 0 && per[2] == 0 && converted == 2 );
    /* without pResults: only when every swapchain matches */
    converted = 0;
    CHECK( w32ios_vk_present_result( W32IOS_VK_SUBOPTIMAL, NULL, matches, 3, &converted ) == W32IOS_VK_SUBOPTIMAL );
    matches[1] = 1;
    CHECK( w32ios_vk_present_result( W32IOS_VK_SUBOPTIMAL, NULL, matches, 3, &converted ) == 0 && converted == 1 );
    CHECK( w32ios_vk_present_result( -4, NULL, matches, 3, &converted ) == -4 );
}

static void test_luid( void )
{
    /* D4: bswap64(MTLDevice.registryID), the LUID DXGI reports (spec 12 D1's helper) */
    uint64_t value = w32ios_luid_from_registry_id( 0x0000000100000a2bull );
    CHECK( value == 0x2b0a000001000000ull );
    CHECK( (uint32_t)value == 0x01000000u && (int32_t)(value >> 32) == 0x2b0a0000 );
    CHECK( !w32ios_vk_d3dkmt_wants_instance() );   /* D5 */
}

static void test_owners( void )
{
    struct w32ios_vk_owners owners = { NULL, 0, 0 };
    struct w32ios_vk_owner out[64];
    char a_instance, a_device, a_surface, a_device2, b_instance, b_device;
    unsigned int n, i;

    CHECK( w32ios_vk_owners_add( &owners, 0x20, W32IOS_VK_INSTANCE, &a_instance ) );
    CHECK( w32ios_vk_owners_add( &owners, 0x140, W32IOS_VK_INSTANCE, &b_instance ) );
    CHECK( w32ios_vk_owners_add( &owners, 0x20, W32IOS_VK_SURFACE, &a_surface ) );
    CHECK( w32ios_vk_owners_add( &owners, 0x20, W32IOS_VK_DEVICE, &a_device ) );
    CHECK( w32ios_vk_owners_add( &owners, 0x140, W32IOS_VK_DEVICE, &b_device ) );
    CHECK( w32ios_vk_owners_add( &owners, 0x20, W32IOS_VK_DEVICE, &a_device2 ) );
    /* the program destroyed one device itself */
    w32ios_vk_owners_remove( &owners, &a_device2 );
    CHECK( owners.count == 5 );

    /* process 0x20 ends: devices, then surfaces, then instances; 0x140 untouched */
    n = w32ios_vk_owners_take( &owners, 0x20, out );
    CHECK( n == 3 );
    CHECK( out[0].object == &a_device && out[0].kind == W32IOS_VK_DEVICE );
    CHECK( out[1].object == &a_surface && out[2].object == &a_instance );
    for (i = 0; i < n; i++) CHECK( out[i].pid == 0x20 && out[i].object != &a_device2 );
    CHECK( owners.count == 2 );
    CHECK( w32ios_vk_owners_take( &owners, 0x20, out ) == 0 );   /* nothing twice */
    n = w32ios_vk_owners_take( &owners, 0x140, out );
    CHECK( n == 2 && out[0].object == &b_device && out[1].object == &b_instance && owners.count == 0 );

    for (i = 0; i < 100; i++) CHECK( w32ios_vk_owners_add( &owners, i % 3, W32IOS_VK_DEVICE, &out[i % 64] ) );
    CHECK( w32ios_vk_owners_take( &owners, 1, out ) == 33 && owners.count == 67 );
    free( owners.items );
}

int main( void )
{
    test_switch();
    test_surface();
    test_suboptimal();
    test_luid();
    test_owners();
    if (failures) { printf( "test_w32ios_vk: %d failure(s)\n", failures ); return 1; }
    printf( "test_w32ios_vk: all passed\n" );
    return 0;
}
