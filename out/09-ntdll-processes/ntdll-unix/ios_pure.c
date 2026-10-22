// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09: pure helpers of the iOS process model (see ios_pure.h). Only the C library and
// pthreads are used, so the host tests in tests/ can link this file alone.

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "ios_pure.h"

/* ---------- small string helpers ---------- */

static char ascii_lower( char c )
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* case-insensitive (ASCII) substring test */
static int contains_nocase( const char *haystack, const char *needle )
{
    size_t n = strlen( needle ), i;

    if (!n) return 1;
    for (; *haystack; haystack++)
    {
        for (i = 0; i < n && haystack[i]; i++)
            if (ascii_lower( haystack[i] ) != ascii_lower( needle[i] )) break;
        if (i == n) return 1;
    }
    return 0;
}

static int equal_nocase( const char *a, const char *b )
{
    for (; *a && *b; a++, b++) if (ascii_lower( *a ) != ascii_lower( *b )) return 0;
    return !*a && !*b;
}

const char *ios_file_part( const char *path )
{
    const char *p, *ret = path;

    if (!path) return "";
    for (p = path; *p; p++) if (*p == '\\' || *p == '/') ret = p + 1;
    return ret;
}

/* ---------- Part A ---------- */

static int unixlib_name( const char *dll_name, const char *tail, char *buf, size_t size );

int ios_unixlib_symbol( const char *dll_name, int wow64, char *buf, size_t size )
{
    return unixlib_name( dll_name, wow64 ? "_unix_call_wow64_funcs" : "_unix_call_funcs", buf, size );
}

int ios_unixlib_init_symbol( const char *dll_name, char *buf, size_t size )
{
    return unixlib_name( dll_name, "_unix_lib_init", buf, size );
}

static int unixlib_name( const char *dll_name, const char *tail, char *buf, size_t size )
{
    const char *name = ios_file_part( dll_name ), *end, *p;
    char base[128];
    size_t len, i;

    if (!buf || !size) return -1;
    if (!(end = strrchr( name, '.' )) || end == name) end = name + strlen( name );
    len = end - name;
    if (!len || len >= sizeof(base)) return -1;

    for (i = 0, p = name; i < len; i++, p++)
    {
        char c = ascii_lower( *p );
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
        base[i] = c;
    }
    base[len] = 0;

    if (!strcmp( base, "winemetal" ))
        len = snprintf( buf, size, "dxmt_%s%s", base, tail );
    else
        len = snprintf( buf, size, "%s%s", base, tail );
    return len < size ? 0 : -1;
}

/* ---------- Part C5 / C6 ---------- */

int ios_is_helper_name( const char *path )
{
    static const char * const words[] = { "crash", "crs-handler", "handler", "report", "helper" };
    const char *name = ios_file_part( path );
    size_t i;

    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (contains_nocase( name, words[i] )) return 1;
    return 0;
}

const char *ios_refused_spawn_match( const char *path )
{
    static const char * const refused[] =
    {
        "steamerrorreporter", "gldriverquery", "vulkandriverquery",
        "steamsysinfo", "hardwareupdater", "unitycrashhandler64",
    };
    size_t i;

    if (!path) return NULL;
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
        if (contains_nocase( path, refused[i] )) return refused[i];
    return NULL;
}

int ios_is_conhost( const char *path )
{
    return equal_nocase( ios_file_part( path ), "conhost.exe" );
}

/* ---------- Part B2: registry ----------
 *
 * A slot goes FREE -> BUSY (claimed by one publisher with a compare-and-swap) -> LIVE (after key
 * and value are written, release order) and back LIVE -> BUSY -> FREE when removed. Readers only
 * trust a slot they see LIVE (acquire order), so they never see a half-written one. */

int ios_registry_publish( struct ios_registry *reg, void *key, void *value )
{
    int i;

    if (!key) return -1;
    if (ios_registry_lookup( reg, key )) return -1;
    for (i = 0; i < IOS_REGISTRY_SLOTS; i++)
    {
        struct ios_registry_slot *slot = &reg->slot[i];
        int expected = IOS_SLOT_FREE;

        if (!atomic_compare_exchange_strong_explicit( &slot->state, &expected, IOS_SLOT_BUSY,
                                                      memory_order_acquire, memory_order_relaxed ))
            continue;
        atomic_store_explicit( &slot->value, value, memory_order_relaxed );
        atomic_store_explicit( &slot->key, key, memory_order_relaxed );
        atomic_store_explicit( &slot->state, IOS_SLOT_LIVE, memory_order_release );
        return 0;
    }
    return -1;
}

void *ios_registry_lookup( struct ios_registry *reg, const void *key )
{
    int i;

    if (!key) return NULL;
    for (i = 0; i < IOS_REGISTRY_SLOTS; i++)
    {
        struct ios_registry_slot *slot = &reg->slot[i];
        void *value;

        if (atomic_load_explicit( &slot->state, memory_order_acquire ) != IOS_SLOT_LIVE) continue;
        if (atomic_load_explicit( &slot->key, memory_order_relaxed ) != key) continue;
        value = atomic_load_explicit( &slot->value, memory_order_relaxed );
        /* still the same live slot? (it may have been removed while we read it) */
        if (atomic_load_explicit( &slot->state, memory_order_acquire ) != IOS_SLOT_LIVE) continue;
        return value;
    }
    return NULL;
}

int ios_registry_remove( struct ios_registry *reg, const void *key )
{
    int i;

    if (!key) return -1;
    for (i = 0; i < IOS_REGISTRY_SLOTS; i++)
    {
        struct ios_registry_slot *slot = &reg->slot[i];
        int expected = IOS_SLOT_LIVE;

        if (atomic_load_explicit( &slot->key, memory_order_relaxed ) != key) continue;
        if (!atomic_compare_exchange_strong_explicit( &slot->state, &expected, IOS_SLOT_BUSY,
                                                      memory_order_acq_rel, memory_order_relaxed ))
            continue;
        if (atomic_load_explicit( &slot->key, memory_order_relaxed ) != key)
        {
            /* the slot was reused for another key between the two reads: put it back */
            atomic_store_explicit( &slot->state, IOS_SLOT_LIVE, memory_order_release );
            continue;
        }
        atomic_store_explicit( &slot->key, NULL, memory_order_relaxed );
        atomic_store_explicit( &slot->value, NULL, memory_order_relaxed );
        atomic_store_explicit( &slot->state, IOS_SLOT_FREE, memory_order_release );
        return 0;
    }
    return -1;
}

int ios_registry_count( struct ios_registry *reg )
{
    int i, count = 0;

    for (i = 0; i < IOS_REGISTRY_SLOTS; i++)
        if (atomic_load_explicit( &reg->slot[i].state, memory_order_acquire ) == IOS_SLOT_LIVE) count++;
    return count;
}

/* ---------- Part C5: live children ---------- */

struct ios_child
{
    unsigned int id;        /* 0: unused */
    char         name[IOS_CHILD_NAME_MAX];
    double       start;
    int          helper;
    int          system;
};

static pthread_mutex_t children_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct ios_child children[IOS_MAX_CHILDREN];
static unsigned int next_child_id = 1;

unsigned int ios_children_add( const char *path, double now )
{
    unsigned int id = 0;
    int i;

    pthread_mutex_lock( &children_mutex );
    for (i = 0; i < IOS_MAX_CHILDREN; i++)
    {
        if (children[i].id) continue;
        id = next_child_id++;
        if (!next_child_id) next_child_id = 1;
        children[i].id = id;
        snprintf( children[i].name, sizeof(children[i].name), "%s", ios_file_part( path ) );
        children[i].start = now;
        children[i].helper = ios_is_helper_name( path );
        children[i].system = 0;
        break;
    }
    pthread_mutex_unlock( &children_mutex );
    return id;
}

void ios_children_set_system( unsigned int id )
{
    int i;

    if (!id) return;
    pthread_mutex_lock( &children_mutex );
    for (i = 0; i < IOS_MAX_CHILDREN; i++)
        if (children[i].id == id) children[i].system = 1;
    pthread_mutex_unlock( &children_mutex );
}

void ios_children_remove( unsigned int id )
{
    int i;

    if (!id) return;
    pthread_mutex_lock( &children_mutex );
    for (i = 0; i < IOS_MAX_CHILDREN; i++)
        if (children[i].id == id) memset( &children[i], 0, sizeof(children[i]) );
    pthread_mutex_unlock( &children_mutex );
}

int ios_children_live( char *buf, int len, double max_age, double now )
{
    int i, count = 0, pos = 0;

    if (buf && len > 0) buf[0] = 0;
    pthread_mutex_lock( &children_mutex );
    for (i = 0; i < IOS_MAX_CHILDREN; i++)
    {
        const struct ios_child *child = &children[i];

        if (!child->id || child->helper || child->system) continue;
        if (max_age >= 0 && now - child->start > max_age) continue;
        if (buf && len > 0 && pos < len - 1)
        {
            int n = snprintf( buf + pos, len - pos, "%s%s", count ? "," : "", child->name );
            pos = (n < 0) ? pos : (pos + n < len ? pos + n : len - 1);
        }
        count++;
    }
    pthread_mutex_unlock( &children_mutex );
    return count;
}

void ios_children_reset(void)
{
    pthread_mutex_lock( &children_mutex );
    memset( children, 0, sizeof(children) );
    next_child_id = 1;
    pthread_mutex_unlock( &children_mutex );
}

/* ---------- Part F2 ---------- */

unsigned int ios_plausible_mhz( uint64_t counter_hz )
{
    uint64_t mhz = counter_hz / 1000000;

    if (!mhz) return 3000;
    while (mhz < 2000) mhz *= 2;
    if (mhz > 5000) mhz = 5000;
    return (unsigned int)mhz;
}

/* ---------- Parts A and C2: leaving ---------- */

int ios_exit_route( int app_ready, int own_ready )
{
    if (app_ready) return IOS_EXIT_APP_JMP;
    if (own_ready) return IOS_EXIT_OWN_JMP;
    return IOS_EXIT_THREAD;
}

int ios_init_begin( int *state )
{
    int expected = IOS_INIT_NONE;

    if (__atomic_compare_exchange_n( state, &expected, IOS_INIT_RUNNING, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
        return IOS_INIT_RUN;
    return (expected == IOS_INIT_DONE) ? IOS_INIT_SKIP : IOS_INIT_FAILED_BEFORE;
}

void ios_init_end( int *state )
{
    __atomic_store_n( state, IOS_INIT_DONE, __ATOMIC_RELEASE );
}

/* ---------- Part C4: a child's name ---------- */

const void *ios_child_image_name( const void *nt_buffer, unsigned int nt_length,
                                  const void *path_buffer, unsigned int path_length,
                                  unsigned int *len_ret )
{
    if (nt_buffer)
    {
        *len_ret = nt_length;
        return nt_buffer;
    }
    if (path_buffer)
    {
        *len_ret = path_length;
        return path_buffer;
    }
    *len_ret = 0;
    return NULL;
}

/* ---------- Part B: the farm of an image ---------- */

const char *ios_pe_farm_dir( unsigned short main_machine, unsigned short image_machine )
{
    switch (image_machine)
    {
    case IOS_MACHINE_I386:  return "/i386-windows";
    case IOS_MACHINE_AMD64: return "/arm64ec-windows";
    case IOS_MACHINE_ARM64:
        /* an x64 program's 64-bit images, ntdll first, are the ARM64EC build */
        return (main_machine == IOS_MACHINE_AMD64) ? "/arm64ec-windows" : "/aarch64-windows";
    case IOS_MACHINE_ARMNT: return "/arm-windows";
    default:                return "";
    }
}
