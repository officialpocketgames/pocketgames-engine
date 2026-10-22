// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part D1: MTLDevice.registryID of MTLCreateSystemDefaultDevice(), reached through dlsym
// and the Objective-C runtime so the file stays C (Apple: Metal's MTLCreateSystemDefaultDevice
// and MTLDevice.registryID; objc/message.h's objc_msgSend, which must be called through a cast
// to the method's exact type; sel_registerName). All three are public SDK symbols, looked up in
// the app's own image (RTLD_DEFAULT); nothing is dlopened. No Wine header here.
//
// Also the settings file of spec 02 Part G (<Documents>/madeira.cfg) for spec 05's vram-mb.

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w32ios_pure.h"
#include "w32ios_metal.h"

typedef void *(*create_device_fn)( void );
typedef void *(*register_selector_fn)( const char *name );
typedef uint64_t (*msg_u64_fn)( void *self, void *sel );
typedef void (*msg_void_fn)( void *self, void *sel );

int w32ios_metal_registry_id( uint64_t *id )
{
    create_device_fn create = (create_device_fn)dlsym( RTLD_DEFAULT, "MTLCreateSystemDefaultDevice" );
    register_selector_fn selector = (register_selector_fn)dlsym( RTLD_DEFAULT, "sel_registerName" );
    void *msg_send = dlsym( RTLD_DEFAULT, "objc_msgSend" );
    void *device;

    /* The app links Metal.framework (spec 00 D2: never dlopened by path). If the symbol isn't
     * there, there is no adapter (the caller logs it). */
    if (!create || !selector || !msg_send) return 0;
    if (!(device = create())) return 0;

    *id = ((msg_u64_fn)msg_send)( device, selector( "registryID" ) );
    ((msg_void_fn)msg_send)( device, selector( "release" ) );   /* the create rule: we own it */
    return 1;
}

int w32ios_settings_get( const char *key, char *value, size_t size )
{
    const char *docs = getenv( "MADEIRA_DOCS_DIR" ), *home = getenv( "HOME" );
    char path[1024], *text;
    size_t len;
    long file_size;
    FILE *file;
    int found;

    if (docs && *docs) snprintf( path, sizeof(path), "%s/madeira.cfg", docs );
    else if (home && *home) snprintf( path, sizeof(path), "%s/Documents/madeira.cfg", home );
    else return 0;

    if (!(file = fopen( path, "rb" ))) return 0;
    if (fseek( file, 0, SEEK_END ) || (file_size = ftell( file )) < 0 || file_size > (1 << 20) ||
        fseek( file, 0, SEEK_SET ) || !(text = malloc( file_size + 1 )))
    {
        fclose( file );
        return 0;
    }
    len = fread( text, 1, file_size, file );
    fclose( file );
    found = w32ios_cfg_find( text, len, key, value, size );
    free( text );
    return found;
}

