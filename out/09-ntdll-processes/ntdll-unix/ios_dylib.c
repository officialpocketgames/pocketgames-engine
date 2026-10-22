// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 10 Part C: images from signed dylibs. The app's build turns each Windows DLL into
// lib<name>.dylib whose signed text segment holds the image's headers and code, followed by its
// data, at the image's RVAs. This file finds and opens those dylibs, gets a fresh instance of one
// for each pseudo-process that maps it (the app's mid_pe_dylib_instance), and keeps the table of
// images (lock-free lookups by address, never freed). virtual.c registers the views.

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "unix_private.h"
#include "ios_vm_pure.h"
#include "ios_dylib.h"
#include "wine_unix_contract.h"   /* Section B: mid_pe_dylib_instance */

#ifdef WINE_IOS

/* Part C6: the byte offset, from the thread's TSD base (TPIDRRO_EL0), of the TSD slot of the TEB
 * key (spec 09 Part E2), set before any image is mapped */
int ios_teb_tls_slot_offset = -1;

#define IOS_DYLIB_MAX 4096

static struct ios_dylib_image images[IOS_DYLIB_MAX];
static unsigned int image_count;   /* published with release order, read with acquire */
static pthread_mutex_t images_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static int trace_fd = -2;


/***********************************************************************
 *           ios_dylib_trace
 *
 * Part C8: a line to stderr and to the file named by MYIOSDECK_NOJIT_TRACE (opened on first
 * use, appended), so the lines survive a crash.
 */
void ios_dylib_trace( const char *fmt, ... )
{
    char buffer[512];
    va_list args;
    int len;

    len = snprintf( buffer, sizeof(buffer), "nojit: " );
    va_start( args, fmt );
    len += vsnprintf( buffer + len, sizeof(buffer) - len, fmt, args );
    va_end( args );
    if (len >= (int)sizeof(buffer)) len = sizeof(buffer) - 1;

    write( 2, buffer, len );
    pthread_mutex_lock( &trace_mutex );
    if (trace_fd == -2)
    {
        const char *path = getenv( "MYIOSDECK_NOJIT_TRACE" );
        trace_fd = (path && *path) ? open( path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644 ) : -1;
    }
    if (trace_fd >= 0) write( trace_fd, buffer, len );
    pthread_mutex_unlock( &trace_mutex );
}


/* appends an entry, or reuses one whose instance's view went with its process (spec 22 C7: the
 * table never runs out when programs start one after another); the caller holds images_mutex.
 * A reused entry's end is NULL until the entry is complete, so lock-free readers skip it. */
static struct ios_dylib_image *add_image( const char *name, USHORT machine, void *handle,
                                          char *image, char *data, char *end, uint64_t *teb_offset )
{
    unsigned int i, count = __atomic_load_n( &image_count, __ATOMIC_RELAXED );
    struct ios_dylib_image *entry = NULL;

    for (i = 0; i < count && !entry; i++)
        if (!images[i].image && !images[i].handle) entry = &images[i];
    if (!entry)
    {
        if (count >= IOS_DYLIB_MAX) return NULL;
        entry = &images[count];
    }
    snprintf( entry->name, sizeof(entry->name), "%s", name );
    entry->machine = machine;
    entry->handle = handle;
    entry->image = image;
    entry->data = data;
    entry->teb_offset = teb_offset;
    entry->has_view = 0;
    entry->path_tail[0] = 0;
    __atomic_store_n( &entry->end, end, __ATOMIC_RELEASE );
    if (entry == &images[count]) __atomic_store_n( &image_count, count + 1, __ATOMIC_RELEASE );
    return entry;
}


/***********************************************************************
 *           ios_dylib_view_gone
 *
 * Spec 22 C7: the view over an image at base was deleted with its ended process. A fresh
 * instance's entry becomes free for a later one (the instance's data was written by that process,
 * so it is never mapped again). The dylib's first instance keeps its entry: later processes get
 * fresh instances of it, as before.
 */
void ios_dylib_view_gone( const void *base )
{
    unsigned int i, count;

    pthread_mutex_lock( &images_mutex );
    count = __atomic_load_n( &image_count, __ATOMIC_RELAXED );
    for (i = 0; i < count; i++)
    {
        struct ios_dylib_image *entry = &images[i];

        if (entry->image != (const char *)base || entry->handle) continue;
        __atomic_store_n( &entry->end, NULL, __ATOMIC_RELEASE );
        entry->image = entry->data = NULL;
        entry->has_view = 0;
        break;
    }
    pthread_mutex_unlock( &images_mutex );
}


/***********************************************************************
 *           ios_dylib_find
 *
 * Part C7: the image containing addr, lock-free (entries are complete before the count that
 * covers them is published).
 */
const struct ios_dylib_image *ios_dylib_find( const void *addr )
{
    unsigned int i, count = __atomic_load_n( &image_count, __ATOMIC_ACQUIRE );

    for (i = 0; i < count; i++)
    {
        const char *end = __atomic_load_n( &images[i].end, __ATOMIC_ACQUIRE );  /* NULL: a free entry */
        if ((const char *)addr >= images[i].image && (const char *)addr < end) return &images[i];
    }
    return NULL;
}


/***********************************************************************
 *           ios_dylib_next_start
 *
 * The lowest image start above addr, or NULL (Part B: where a protection change must split).
 */
const char *ios_dylib_next_start( const void *addr )
{
    unsigned int i, count = __atomic_load_n( &image_count, __ATOMIC_ACQUIRE );
    const char *best = NULL;

    for (i = 0; i < count; i++)
        if (images[i].image > (const char *)addr && (!best || images[i].image < best)) best = images[i].image;
    return best;
}


/* the entry of the dylib's first instance (the one dlopen mapped) */
static struct ios_dylib_image *find_first( const char *path )
{
    unsigned int i, count = __atomic_load_n( &image_count, __ATOMIC_ACQUIRE );

    for (i = 0; i < count; i++)
        if (images[i].handle && !strcmp( images[i].path_tail, path )) return &images[i];
    return NULL;
}


/***********************************************************************
 *           set_ntdll_tsd_offset
 *
 * Clarification 4 (spec 18 Part B1): both 64-bit ntdll images export `ios_teb_tsd_offset`; write
 * the TEB slot offset there, as the TEB offset of Part C2, for the first image and each instance.
 * 0 means "not known" on the PE side, so nothing is written before the offset is known.
 */
static void set_ntdll_tsd_offset( const char *name, char *image, char *end )
{
    unsigned int *slot;

    if (strcasecmp( ios_file_part( name ), "ntdll.dll" )) return;
    if (ios_teb_tls_slot_offset <= 0) return;
    if (!(slot = ios_pe_find_export( image, end - image, "ios_teb_tsd_offset" )))
    {
        ios_dylib_trace( "%s: no ios_teb_tsd_offset export\n", name );
        return;
    }
    *slot = ios_teb_tls_slot_offset;
}


/* the dylib path for nt_name and machine (clarification 9 and 10); 0 on success */
static int dylib_path_of( const UNICODE_STRING *nt_name, USHORT machine, char *path, size_t size )
{
    char name[256];
    const char *dir;
    int len;

    if (!nt_name || !nt_name->Buffer || !nt_name->Length) return -1;
    if (!(dir = ios_dylib_dir( machine, getenv( "MYIOSDECK_PE_DIR" ), getenv( "MYIOSDECK_PE_DIR_A64" ))))
        return -1;
    len = ntdll_wcstoumbs( nt_name->Buffer, nt_name->Length / sizeof(WCHAR), name, sizeof(name) - 1, FALSE );
    if (len <= 0) return -1;
    name[len] = 0;
    return ios_dylib_path( dir, name, path, size );
}


/***********************************************************************
 *           ios_dylib_exists
 *
 * Clarification 9: whether a signed dylib carries a DLL of this name and machine (a native
 * file of that name shadows one of our builtins). Opens nothing.
 */
BOOL ios_dylib_exists( const UNICODE_STRING *nt_name, USHORT machine )
{
    char path[1024];
    struct stat st;

    if (dylib_path_of( nt_name, machine, path, sizeof(path) )) return FALSE;
    return !stat( path, &st );
}


/***********************************************************************
 *           ios_dylib_instances
 *
 * Clarification 10: the image starts of the instances with a view of the dylib that carries
 * nt_name for this machine (at most max); virtual.c looks for the calling process's view among
 * them. Opens nothing.
 */
int ios_dylib_instances( const UNICODE_STRING *nt_name, USHORT machine, char **bases, int max )
{
    char path[1024];
    unsigned int i, count;
    int found = 0;

    if (dylib_path_of( nt_name, machine, path, sizeof(path) )) return 0;
    pthread_mutex_lock( &images_mutex );
    count = __atomic_load_n( &image_count, __ATOMIC_RELAXED );
    for (i = 0; i < count && found < max; i++)
        if (images[i].image && images[i].has_view && !strcmp( images[i].path_tail, path ))
            bases[found++] = images[i].image;
    pthread_mutex_unlock( &images_mutex );
    return found;
}


/***********************************************************************
 *           ios_dylib_open
 *
 * Part C1-C4: the image of the DLL nt_name for an image of this machine, from its dylib:
 * the first instance on first use, a fresh one from the app when the first already has a view.
 * Returns STATUS_SUCCESS with *ret filled, STATUS_NOT_FOUND when there is no dylib for it (map
 * the upstream way), or an error that fails the mapping.
 */
NTSTATUS ios_dylib_open( const UNICODE_STRING *nt_name, USHORT machine, struct ios_dylib_image **ret )
{
    char path[1024], name[256], err[256];
    const char *dir;
    struct ios_dylib_image *first, *entry = NULL;
    struct ios_nojit_pe_instance inst;
    struct stat st;
    int len;

    *ret = NULL;
    if (!nt_name || !nt_name->Buffer || !nt_name->Length) return STATUS_NOT_FOUND;  /* no name: upstream way */
    if (!(dir = ios_dylib_dir( machine, getenv( "MYIOSDECK_PE_DIR" ), getenv( "MYIOSDECK_PE_DIR_A64" ))))
        return STATUS_NOT_FOUND;
    len = ntdll_wcstoumbs( nt_name->Buffer, nt_name->Length / sizeof(WCHAR), name, sizeof(name) - 1, FALSE );
    if (len <= 0) return STATUS_NOT_FOUND;
    name[len] = 0;
    if (ios_dylib_path( dir, name, path, sizeof(path) )) return STATUS_NOT_FOUND;
    if (stat( path, &st ) == -1) return STATUS_NOT_FOUND;

    pthread_mutex_lock( &images_mutex );
    if (!(first = find_first( path )))
    {
        void *handle = dlopen( path, RTLD_NOW | RTLD_LOCAL );
        char *image, *data, *end;
        uint64_t *teb_offset;

        if (!handle)
        {
            pthread_mutex_unlock( &images_mutex );
            ios_dylib_trace( "dlopen %s failed: %s\n", path, dlerror() );
            return STATUS_NOT_FOUND;
        }
        image = dlsym( handle, "myiosdeck_pe_image" );
        data = dlsym( handle, "myiosdeck_pe_data" );
        end = dlsym( handle, "myiosdeck_pe_image_end" );
        teb_offset = dlsym( handle, "myiosdeck_teb_offset" );
        if (!image || !data || !end || !teb_offset || !(image <= data && data <= end))
        {
            pthread_mutex_unlock( &images_mutex );
            ios_dylib_trace( "%s lacks the image symbols, mapped the upstream way\n", path );
            return STATUS_NOT_FOUND;
        }
        *teb_offset = ios_teb_tls_slot_offset;
        set_ntdll_tsd_offset( name, image, end );
        if ((first = add_image( ios_file_part( name ), machine, handle, image, data, end, teb_offset )))
            snprintf( first->path_tail, sizeof(first->path_tail), "%s", path );
        if (first && !first->has_view) entry = first;
    }
    else if (!first->has_view) entry = first;

    if (!entry && first)
    {
        /* Part C4: another pseudo-process maps this DLL: a fresh instance of it */
        memset( &inst, 0, sizeof(inst) );
        err[0] = 0;
        if (!mid_pe_dylib_instance( first->handle, path, &inst, err, sizeof(err) ))
        {
            pthread_mutex_unlock( &images_mutex );
            ios_dylib_trace( "no new instance of %s: %s\n", path, err );
            return STATUS_NO_MEMORY;
        }
        if (inst.teb_offset) *inst.teb_offset = ios_teb_tls_slot_offset;
        set_ntdll_tsd_offset( name, inst.image, inst.end );
        entry = add_image( first->name, machine, NULL, inst.image, inst.data, inst.end, inst.teb_offset );
        if (entry) snprintf( entry->path_tail, sizeof(entry->path_tail), "%s", path );
    }
    if (entry) entry->has_view = 1;
    pthread_mutex_unlock( &images_mutex );

    if (!entry)
    {
        ios_dylib_trace( "image table full (%u entries), %s not mapped\n", IOS_DYLIB_MAX, path );
        return STATUS_NO_MEMORY;
    }
    *ret = entry;
    return STATUS_SUCCESS;
}

#endif /* WINE_IOS */
