// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part A, "Unix libraries by name". There are no .so files on iOS: every DLL's unix
// half is linked into the app binary and exports its call tables under a name derived from
// the DLL name (ios_unixlib_symbol). virtual.c's MemoryWineLoadUnixLib* handlers call this file
// instead of dlopen().

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "unix_private.h"
#include "ios_pure.h"
#include "ios_dylib.h"   /* round 6 item 6: the calling module's name */

#ifdef WINE_IOS

static pthread_mutex_t logged_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *logged_names[64];
static unsigned int logged_count;

/* log "no table" once per DLL name */
static void log_missing_once( const char *dll_name, const char *symbol )
{
    unsigned int i;
    BOOL seen = FALSE;

    pthread_mutex_lock( &logged_mutex );
    for (i = 0; i < logged_count; i++)
        if (!strcmp( logged_names[i], symbol )) seen = TRUE;
    if (!seen && logged_count < ARRAY_SIZE(logged_names))
        logged_names[logged_count++] = strdup( symbol );
    pthread_mutex_unlock( &logged_mutex );

    if (!seen) ios_log( "unixlib: %s has no table %s linked into the app: STATUS_DLL_NOT_FOUND\n",
                        dll_name, symbol );
}

/***********************************************************************
 *           ios_unixlib_table
 *
 * The call table of the unix library of dll_name: the 32-bit (wow64) table for a 32-bit caller,
 * the 64-bit one otherwise. As upstream calls a library's __wine_unix_lib_init each time it
 * binds it, the library's optional "<name>_unix_lib_init" runs then, on the loading thread
 * (win32u registers its system calls there, spec 12).
 */
/* ---------- spec 09 round 6 item 6: tables with their sizes ----------
 * A table's size is known when its library defines "<table symbol>_count" (an unsigned int);
 * Wine's libraries don't, so their calls are only checked for a missing table. Append-only,
 * read without a lock (the count of entries is published with release order). */
#define IOS_TABLES 64

struct bound_table
{
    const void  *funcs;
    unsigned int count;   /* 0: not known */
};

static struct bound_table bound_tables[IOS_TABLES];
static unsigned int bound_count;

static void bind_table( const void *funcs, const char *symbol )
{
    char count_symbol[300];
    const unsigned int *count;
    unsigned int i, n;

    snprintf( count_symbol, sizeof(count_symbol), "%s_count", symbol );
    count = dlsym( RTLD_DEFAULT, count_symbol );
    pthread_mutex_lock( &logged_mutex );
    n = __atomic_load_n( &bound_count, __ATOMIC_RELAXED );
    for (i = 0; i < n; i++) if (bound_tables[i].funcs == funcs) break;
    if (i == n && n < IOS_TABLES)
    {
        bound_tables[n].funcs = funcs;
        bound_tables[n].count = count ? *count : 0;
        __atomic_store_n( &bound_count, n + 1, __ATOMIC_RELEASE );
    }
    pthread_mutex_unlock( &logged_mutex );
}

static unsigned int table_count( const void *funcs )
{
    unsigned int i, n = __atomic_load_n( &bound_count, __ATOMIC_ACQUIRE );

    for (i = 0; i < n; i++) if (bound_tables[i].funcs == funcs) return bound_tables[i].count;
    return 0;
}

/* once per calling module (its name from its signed image, else its address) */
static void log_bad_call_once( const void *caller, unsigned int status, unsigned int code, unsigned int count )
{
    const struct ios_dylib_image *img = ios_dylib_find( caller );
    char name[80];

    if (img) snprintf( name, sizeof(name), "%s", img->name );
    else snprintf( name, sizeof(name), "module at %p", caller );
    {
        unsigned int i;
        BOOL seen = FALSE;
        char key[96];

        snprintf( key, sizeof(key), "call:%s", name );
        pthread_mutex_lock( &logged_mutex );
        for (i = 0; i < logged_count; i++) if (!strcmp( logged_names[i], key )) seen = TRUE;
        if (!seen && logged_count < ARRAY_SIZE(logged_names)) logged_names[logged_count++] = strdup( key );
        pthread_mutex_unlock( &logged_mutex );
        if (seen) return;
    }
    if (status == IOS_UNIX_NO_TABLE)
        ios_log( "[unix] %s: no unix table; calls fail with STATUS_DLL_NOT_FOUND\n", name );
    else
        ios_log( "[unix] %s: call %u past its table's %u entries; calls like it fail with "
                 "STATUS_INVALID_PARAMETER\n", name, code, count );
}

/***********************************************************************
 *           ios_unix_call
 *
 * Spec 09 round 6 item 6: the unix-call dispatcher's call (__wine_unix_call, also for WoW64,
 * whose wow64.dll calls it), on the kernel stack: a module whose query for its table failed
 * (handle 0), or an index past a table of known size, fails without touching memory.
 * caller: the return address in the calling DLL, for the log.
 */
NTSTATUS ios_unix_call( const void *funcs, unsigned int code, void *args, const void *caller )
{
    unsigned int count = funcs ? table_count( funcs ) : 0;
    unsigned int status = ios_unix_call_check( funcs != NULL, count != 0, count, code );

    if (status)
    {
        log_bad_call_once( caller, status, code, count );
        return status;
    }
    return ((const unixlib_entry_t *)funcs)[code]( args );
}

NTSTATUS ios_unixlib_table( const char *dll_name, BOOL wow64, const void **funcs )
{
    char symbol[256];
    NTSTATUS (*init)(void);

    *funcs = NULL;
    if (ios_unixlib_symbol( dll_name, wow64, symbol, sizeof(symbol) ))
    {
        log_missing_once( dll_name, dll_name );
        return STATUS_DLL_NOT_FOUND;
    }
    if (!(*funcs = dlsym( RTLD_DEFAULT, symbol )))
    {
        log_missing_once( dll_name, symbol );
        return STATUS_DLL_NOT_FOUND;
    }
    bind_table( *funcs, symbol );   /* round 6 item 6 */
    if (!ios_unixlib_init_symbol( dll_name, symbol, sizeof(symbol) ) &&
        (init = dlsym( RTLD_DEFAULT, symbol )))
        return init();
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           ios_module_export_name
 *
 * The DLL name recorded in a mapped PE module's export directory ("winemetal.dll").
 * Works for 32-bit and 64-bit images.
 */
BOOL ios_module_export_name( const void *module, char *buf, size_t size )
{
    const BYTE *base = ios_host_ptr( module );
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_DATA_DIRECTORY *dir;
    const IMAGE_EXPORT_DIRECTORY *exports;
    const IMAGE_NT_HEADERS32 *nt32;
    const char *name;
    size_t len;

    if (!base || !size || dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    nt32 = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt32->Signature != IMAGE_NT_SIGNATURE) return FALSE;

    if (nt32->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        const IMAGE_NT_HEADERS64 *nt64 = (const IMAGE_NT_HEADERS64 *)nt32;
        if (nt64->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return FALSE;
        dir = &nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    }
    else if (nt32->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        if (nt32->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return FALSE;
        dir = &nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    }
    else return FALSE;

    if (!dir->VirtualAddress || dir->Size < sizeof(*exports)) return FALSE;
    exports = (const IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
    if (!exports->Name) return FALSE;
    name = (const char *)(base + exports->Name);
    len = strnlen( name, size );
    if (!len || len >= size) return FALSE;
    memcpy( buf, name, len + 1 );
    return TRUE;
}


/***********************************************************************
 *           ios_unicode_dll_name
 *
 * An ASCII copy of a unicode DLL name (as passed to MemoryWineLoadUnixLibByName).
 */
BOOL ios_unicode_dll_name( const UNICODE_STRING *name, char *buf, size_t size )
{
    size_t i, len = name->Length / sizeof(WCHAR);

    if (!len || len >= size) return FALSE;
    for (i = 0; i < len; i++)
    {
        if (name->Buffer[i] > 127) return FALSE;
        buf[i] = (char)name->Buffer[i];
    }
    buf[len] = 0;
    return TRUE;
}

#endif /* WINE_IOS */
