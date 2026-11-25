// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 4: the unix side of openal32.dll. One unix call carries every export
// (pe/dlls/openal32/unixlib.h); this file unpacks it and calls OpenAL Soft (oa_api).
//
// - Handles. ALCdevice and ALCcontext pointers never reach Windows code: it gets small 32-bit
//   ids (fit a 32-bit program), checked on every use.
// - Current context. All pseudo-processes share one OpenAL Soft, whose "current context" is
//   app-wide. So alcMakeContextCurrent is kept per pseudo-process, alcSetThreadContext per
//   thread, and before each al* call the thread's OpenAL Soft thread context is set to the one
//   that applies (ALC_EXT_thread_local_context).
// - 32-bit callers (the wow64 table): the arguments the function list marks as pointers are
//   guest addresses, converted by rule A4.
// - Strings: two steps; the PE side passes a buffer and its size, this side answers the size
//   needed and copies when it fits. Device lists end in a double NUL. Extensions whose
//   functions this DLL doesn't export are left out of the extension strings.
// - App inactive: pg_openal_set_app_active pauses every device (alcDevicePauseSOFT) and resumes
//   them after, except those the program paused itself.
// - Logs: "[openal] device opened: <name>, <rate> Hz" once per pseudo-process,
//   "[openal] call <name> unsupported" once per name.
// - A pseudo-process that ends (spec 09's end hook) gets its contexts destroyed and devices
//   closed.

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ios_media_wow.h"
#include "media_log.h"
#include "openal_api.h"
#include "unixlib.h"

#define LOG_PREFIX "[openal] "
#define OA_MAX_HANDLES 256
#define OA_MAX_PROCS 64

typedef int32_t oa_status;   /* NTSTATUS */
typedef oa_status (*oa_unixlib_entry)( void *args );

enum { H_FREE, H_DEVICE, H_CONTEXT };

struct oa_handle
{
    int kind;
    void *host;
    const void *peb;
    uint16_t gen;
    uint32_t device;       /* a context's device id */
    int game_paused;       /* a device the program paused itself */
};

struct oa_proc
{
    const void *peb;
    uint32_t current;      /* alcMakeContextCurrent */
    int logged_open;
};

static const struct { const char *name; unsigned int flags, ptrmask; } oa_info[OAF_COUNT] =
{
#define OA(name, flags, ptrmask) { #name, flags, ptrmask },
    OPENAL32_FUNCS
#undef OA
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct oa_handle handles[OA_MAX_HANDLES];
static struct oa_proc procs[OA_MAX_PROCS];
static int app_inactive;
static volatile uint32_t unsupported_logged[(OAF_COUNT + 31) / 32];
static pthread_once_t hook_once = PTHREAD_ONCE_INIT;

static __thread uint32_t tls_override;      /* alcSetThreadContext */
static __thread int tls_override_set;
static __thread void *tls_applied;          /* what OpenAL Soft's thread context is now */

/* ---------------------------------------------------------------- handles (lock held) */

static uint32_t handle_alloc( int kind, void *host, const void *peb )
{
    unsigned int i;

    for (i = 0; i < OA_MAX_HANDLES; i++)
    {
        if (handles[i].kind != H_FREE) continue;
        handles[i].gen = (uint16_t)((handles[i].gen + 1) & 0x7fff);
        if (!handles[i].gen) handles[i].gen = 1;
        handles[i].kind = kind;
        handles[i].host = host;
        handles[i].peb = peb;
        handles[i].device = 0;
        handles[i].game_paused = 0;
        return ((uint32_t)handles[i].gen << 16) | (i + 1);
    }
    return 0;
}

static struct oa_handle *handle_get( uint32_t id, int kind )
{
    uint32_t i = id & 0xffff;

    if (!id || !i || i > OA_MAX_HANDLES) return NULL;
    i--;
    if (handles[i].kind != kind || handles[i].gen != (id >> 16)) return NULL;
    return &handles[i];
}

static void *host_of( uint32_t id, int kind )
{
    struct oa_handle *h = handle_get( id, kind );
    return h ? h->host : NULL;
}

static uint32_t id_of( const struct oa_handle *h )
{
    return ((uint32_t)h->gen << 16) | (uint32_t)(h - handles + 1);
}

static struct oa_proc *proc_get( const void *peb, int create )
{
    unsigned int i;
    struct oa_proc *free_slot = NULL;

    for (i = 0; i < OA_MAX_PROCS; i++)
    {
        if (procs[i].peb == peb) return &procs[i];
        if (!procs[i].peb && !free_slot) free_slot = &procs[i];
    }
    if (!create || !free_slot) return NULL;
    memset( free_slot, 0, sizeof(*free_slot) );
    free_slot->peb = peb;
    return free_slot;
}

/* ---------------------------------------------------------------- pseudo-process end */

static void oa_process_ended( const void *peb )
{
    unsigned int i;
    struct oa_proc *p;

    pthread_mutex_lock( &lock );
    for (i = 0; i < OA_MAX_HANDLES; i++)
        if (handles[i].kind == H_CONTEXT && handles[i].peb == peb)
        {
            oa_api->alcDestroyContext( handles[i].host );
            handles[i].kind = H_FREE;
        }
    for (i = 0; i < OA_MAX_HANDLES; i++)
        if (handles[i].kind == H_DEVICE && handles[i].peb == peb)
        {
            oa_api->alcCloseDevice( handles[i].host );
            handles[i].kind = H_FREE;
        }
    if ((p = proc_get( peb, 0 ))) p->peb = NULL;
    pthread_mutex_unlock( &lock );
}

static void register_hook( void )
{
    ios_process_end_hook_add( oa_process_ended );
}

/* ---------------------------------------------------------------- app inactive */

__attribute__((used, visibility("default"))) void pg_openal_set_app_active( int active )
{
    unsigned int i;

    pthread_mutex_lock( &lock );
    if (app_inactive != !active)
    {
        app_inactive = !active;
        for (i = 0; i < OA_MAX_HANDLES; i++)
        {
            if (handles[i].kind != H_DEVICE) continue;
            if (app_inactive) oa_api->alcDevicePauseSOFT( handles[i].host );
            else if (!handles[i].game_paused) oa_api->alcDeviceResumeSOFT( handles[i].host );
        }
    }
    pthread_mutex_unlock( &lock );
}

/* ---------------------------------------------------------------- strings */

/* extensions whose functions this DLL doesn't export */
static const char *const hidden_extensions[] =
{
    "ALC_EXT_CAPTURE", "ALC_SOFT_loopback", "ALC_SOFT_loopback_bformat", "ALC_SOFT_device_clock",
    "ALC_SOFT_reopen_device", "ALC_SOFT_system_events", "ALC_EXT_debug", "ALC_EXT_direct_context",
    "AL_SOFT_callback_buffer", "AL_SOFT_events", "AL_EXT_FOLDBACK", "AL_SOFT_buffer_sub_data",
    "AL_EXT_debug", "AL_EXT_direct_context", "AL_EXT_STATIC_BUFFER", "AL_SOFT_map_buffer",
    "AL_SOFT_buffer_samples", "AL_SOFTX_map_buffer",
};

static int extension_hidden( const char *name, size_t len )
{
    unsigned int i;

    for (i = 0; i < sizeof(hidden_extensions) / sizeof(hidden_extensions[0]); i++)
        if (strlen( hidden_extensions[i] ) == len && !strncasecmp( hidden_extensions[i], name, len )) return 1;
    return 0;
}

/* a copy of a space-separated extension string without the hidden ones (malloc'd) */
static char *filter_extensions( const char *s )
{
    char *out = malloc( strlen( s ) + 1 ), *o = out;
    const char *p = s;

    if (!out) return NULL;
    while (*p)
    {
        const char *start;
        size_t len;

        while (*p == ' ') p++;
        start = p;
        while (*p && *p != ' ') p++;
        len = (size_t)(p - start);
        if (!len || extension_hidden( start, len )) continue;
        if (o != out) *o++ = ' ';
        memcpy( o, start, len );
        o += len;
    }
    *o = 0;
    return out;
}

/* the size of a string (with its NUL), or of a list ending in a double NUL */
static uint64_t string_size( const char *s, int list )
{
    const char *p = s;

    if (!list) return strlen( s ) + 1;
    if (!*s) return 2;   /* an empty list: two NULs */
    while (*p) p += strlen( p ) + 1;
    return (uint64_t)(p - s) + 1;
}

static uint64_t put_string( const char *s, int list, uint64_t buf, uint64_t size )
{
    uint64_t need;

    if (!s) return 0;
    need = string_size( s, list );
    if (!buf || size < need) return need;
    if (list && !*s) memset( (void *)(uintptr_t)buf, 0, 2 );   /* the source may hold one byte */
    else memcpy( (void *)(uintptr_t)buf, s, need );
    return need;
}

/* ---------------------------------------------------------------- the call */

#define A_I(n) ((ALint)(int32_t)a[n])
#define A_U(n) ((ALuint)(uint32_t)a[n])
#define A_F(n) bits_float( a[n] )
#define A_D(n) bits_double( a[n] )
#define A_L(n) ((ALint64SOFT)a[n])
#define A_P(n) ((void *)(uintptr_t)a[n])

static inline float bits_float( uint64_t v ) { uint32_t u = (uint32_t)v; float f; memcpy( &f, &u, 4 ); return f; }
static inline double bits_double( uint64_t v ) { double d; memcpy( &d, &v, 8 ); return d; }
static inline uint64_t float_bits( float f ) { uint32_t u; memcpy( &u, &f, 4 ); return u; }
static inline uint64_t double_bits( double d ) { uint64_t u; memcpy( &u, &d, 8 ); return u; }

static void log_unsupported( uint32_t func )
{
    uint32_t bit = 1u << (func & 31);

    if (__atomic_fetch_or( &unsupported_logged[func / 32], bit, __ATOMIC_RELAXED ) & bit) return;
    iom_log( LOG_PREFIX, "call %s unsupported", oa_info[func].name );
}

/* before an al* call: OpenAL Soft's thread context = this thread's or this process's current */
static void apply_context( const void *peb )
{
    void *want = NULL;
    struct oa_proc *p;

    pthread_mutex_lock( &lock );
    if (tls_override_set) want = host_of( tls_override, H_CONTEXT );
    else if ((p = proc_get( peb, 0 ))) want = host_of( p->current, H_CONTEXT );
    pthread_mutex_unlock( &lock );
    if (want == tls_applied) return;
    oa_api->alcSetThreadContext( want );
    tls_applied = want;
}

static void log_device_open( const void *peb, ALCdevice *dev )
{
    const char *name = oa_api->alcGetString( dev, ALC_ALL_DEVICES_SPECIFIER );
    ALCint rate = 0;
    struct oa_proc *p = proc_get( peb, 1 );

    if (!p || p->logged_open) return;
    p->logged_open = 1;
    if (!name || !*name) name = oa_api->alcGetString( dev, ALC_DEVICE_SPECIFIER );
    oa_api->alcGetIntegerv( dev, ALC_FREQUENCY, 1, &rate );
    iom_log( LOG_PREFIX, "device opened: %s, %d Hz", name ? name : "?", rate );
}

/* the ALC calls and strings: their handles, current contexts and lists (lock held) */
static uint64_t alc_call( uint32_t func, const uint64_t *a, const void *peb )
{
    const struct oa_api *api = oa_api;
    struct oa_handle *h;
    struct oa_proc *p;
    ALCdevice *dev;
    void *host;
    uint32_t id;

    switch (func)
    {
    case OAF_alcOpenDevice:
        if (!(dev = api->alcOpenDevice( A_P(0) ))) return 0;
        if (!(id = handle_alloc( H_DEVICE, dev, peb )))
        {
            api->alcCloseDevice( dev );
            return 0;
        }
        if (app_inactive) api->alcDevicePauseSOFT( dev );
        log_device_open( peb, dev );
        return id;
    case OAF_alcCloseDevice:
        if (!(h = handle_get( (uint32_t)a[0], H_DEVICE ))) return api->alcCloseDevice( NULL );
        if (!api->alcCloseDevice( h->host )) return ALC_FALSE;   /* contexts left */
        h->kind = H_FREE;
        return ALC_TRUE;
    case OAF_alcCreateContext:
    {
        ALCcontext *ctx;
        uint32_t dev_id = (uint32_t)a[0];

        if (!(ctx = api->alcCreateContext( host_of( dev_id, H_DEVICE ), A_P(1) ))) return 0;
        if (!(id = handle_alloc( H_CONTEXT, ctx, peb )))
        {
            api->alcDestroyContext( ctx );
            return 0;
        }
        handle_get( id, H_CONTEXT )->device = dev_id;
        return id;
    }
    case OAF_alcDestroyContext:
        if (!(h = handle_get( (uint32_t)a[0], H_CONTEXT ))) return 0;
        id = id_of( h );
        if ((p = proc_get( peb, 0 )) && p->current == id) p->current = 0;
        if (tls_override_set && tls_override == id) tls_override_set = 0;
        if (tls_applied == h->host)
        {
            api->alcSetThreadContext( NULL );
            tls_applied = NULL;
        }
        api->alcDestroyContext( h->host );
        h->kind = H_FREE;
        return 0;
    case OAF_alcMakeContextCurrent:
        id = (uint32_t)a[0];
        if (id && !handle_get( id, H_CONTEXT )) return ALC_FALSE;
        if (!(p = proc_get( peb, 1 ))) return ALC_FALSE;
        p->current = id;
        tls_override_set = 0;   /* as ALC_EXT_thread_local_context: the thread's context is released */
        return ALC_TRUE;
    case OAF_alcGetCurrentContext:
        if (tls_override_set) return tls_override;
        return (p = proc_get( peb, 0 )) ? p->current : 0;
    case OAF_alcSetThreadContext:
        id = (uint32_t)a[0];
        if (id && !handle_get( id, H_CONTEXT )) return ALC_FALSE;
        tls_override = id;
        tls_override_set = id != 0;
        return ALC_TRUE;
    case OAF_alcGetThreadContext:
        return tls_override_set ? tls_override : 0;
    case OAF_alcGetContextsDevice:
        return (h = handle_get( (uint32_t)a[0], H_CONTEXT )) ? h->device : 0;
    case OAF_alcProcessContext:
        if ((host = host_of( (uint32_t)a[0], H_CONTEXT ))) api->alcProcessContext( host );
        return 0;
    case OAF_alcSuspendContext:
        if ((host = host_of( (uint32_t)a[0], H_CONTEXT ))) api->alcSuspendContext( host );
        return 0;
    case OAF_alcGetError:
        return (uint64_t)(uint32_t)api->alcGetError( host_of( (uint32_t)a[0], H_DEVICE ) );
    case OAF_alcIsExtensionPresent:
        if (A_P(1) && extension_hidden( A_P(1), strlen( A_P(1) ) )) return ALC_FALSE;
        return (uint8_t)api->alcIsExtensionPresent( host_of( (uint32_t)a[0], H_DEVICE ), A_P(1) );
    case OAF_alcGetEnumValue:
        return (uint64_t)(uint32_t)api->alcGetEnumValue( host_of( (uint32_t)a[0], H_DEVICE ), A_P(1) );
    case OAF_alcGetIntegerv:
        api->alcGetIntegerv( host_of( (uint32_t)a[0], H_DEVICE ), A_I(1), A_I(2), A_P(3) );
        return 0;
    case OAF_alcDevicePauseSOFT:
        if ((h = handle_get( (uint32_t)a[0], H_DEVICE )))
        {
            h->game_paused = 1;
            api->alcDevicePauseSOFT( h->host );
        }
        return 0;
    case OAF_alcDeviceResumeSOFT:
        if ((h = handle_get( (uint32_t)a[0], H_DEVICE )))
        {
            h->game_paused = 0;
            if (!app_inactive) api->alcDeviceResumeSOFT( h->host );   /* held while the app is inactive */
        }
        return 0;
    case OAF_alcResetDeviceSOFT:
        return (uint8_t)api->alcResetDeviceSOFT( host_of( (uint32_t)a[0], H_DEVICE ), A_P(1) );
    case OAF_alcGetString:
    {
        ALCenum param = A_I(1);
        int no_device = !a[0];
        const char *s;
        char *filtered;
        uint64_t need;

        dev = host_of( (uint32_t)a[0], H_DEVICE );
        if (param == ALC_CAPTURE_DEVICE_SPECIFIER) return put_string( "\0", no_device, a[2], a[3] );
        if (param == ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER) return put_string( "", 0, a[2], a[3] );
        s = api->alcGetString( dev, param );
        if (s && param == ALC_EXTENSIONS && (filtered = filter_extensions( s )))
        {
            need = put_string( filtered, 0, a[2], a[3] );
            free( filtered );
            return need;
        }
        return put_string( s, no_device && (param == ALC_DEVICE_SPECIFIER || param == ALC_ALL_DEVICES_SPECIFIER),
                           a[2], a[3] );
    }
    case OAF_alcGetStringiSOFT:
        return put_string( api->alcGetStringiSOFT( host_of( (uint32_t)a[0], H_DEVICE ), A_I(1), A_I(2) ), 0,
                           a[3], a[4] );
    }
    return 0;
}

#define EFX_CASES(Obj, Objs) \
    case OAF_alGen##Objs: api->alGen##Objs( A_I(0), A_P(1) ); break; \
    case OAF_alDelete##Objs: api->alDelete##Objs( A_I(0), A_P(1) ); break; \
    case OAF_alIs##Obj: return (uint8_t)api->alIs##Obj( A_U(0) ); \
    case OAF_al##Obj##i: api->al##Obj##i( A_U(0), A_I(1), A_I(2) ); break; \
    case OAF_al##Obj##iv: api->al##Obj##iv( A_U(0), A_I(1), A_P(2) ); break; \
    case OAF_al##Obj##f: api->al##Obj##f( A_U(0), A_I(1), A_F(2) ); break; \
    case OAF_al##Obj##fv: api->al##Obj##fv( A_U(0), A_I(1), A_P(2) ); break; \
    case OAF_alGet##Obj##i: api->alGet##Obj##i( A_U(0), A_I(1), A_P(2) ); break; \
    case OAF_alGet##Obj##iv: api->alGet##Obj##iv( A_U(0), A_I(1), A_P(2) ); break; \
    case OAF_alGet##Obj##f: api->alGet##Obj##f( A_U(0), A_I(1), A_P(2) ); break; \
    case OAF_alGet##Obj##fv: api->alGet##Obj##fv( A_U(0), A_I(1), A_P(2) ); break;

/* the al* calls, with the context applied */
static uint64_t al_call( uint32_t func, const uint64_t *a )
{
    const struct oa_api *api = oa_api;

    switch ((enum openal32_func)func)
    {
    case OAF_alEnable: api->alEnable( A_I(0) ); break;
    case OAF_alDisable: api->alDisable( A_I(0) ); break;
    case OAF_alIsEnabled: return (uint8_t)api->alIsEnabled( A_I(0) );
    case OAF_alGetString:
    {
        const char *s = api->alGetString( A_I(0) );
        char *filtered;
        uint64_t need;

        if (s && A_I(0) == AL_EXTENSIONS && (filtered = filter_extensions( s )))
        {
            need = put_string( filtered, 0, a[1], a[2] );
            free( filtered );
            return need;
        }
        return put_string( s, 0, a[1], a[2] );
    }
    case OAF_alGetBooleanv: api->alGetBooleanv( A_I(0), A_P(1) ); break;
    case OAF_alGetIntegerv: api->alGetIntegerv( A_I(0), A_P(1) ); break;
    case OAF_alGetFloatv: api->alGetFloatv( A_I(0), A_P(1) ); break;
    case OAF_alGetDoublev: api->alGetDoublev( A_I(0), A_P(1) ); break;
    case OAF_alGetBoolean: return (uint8_t)api->alGetBoolean( A_I(0) );
    case OAF_alGetInteger: return (uint64_t)(uint32_t)api->alGetInteger( A_I(0) );
    case OAF_alGetFloat: return float_bits( api->alGetFloat( A_I(0) ) );
    case OAF_alGetDouble: return double_bits( api->alGetDouble( A_I(0) ) );
    case OAF_alGetError: return (uint64_t)(uint32_t)api->alGetError();
    case OAF_alIsExtensionPresent:
        if (A_P(0) && extension_hidden( A_P(0), strlen( A_P(0) ) )) return 0;
        return (uint8_t)api->alIsExtensionPresent( A_P(0) );
    case OAF_alGetEnumValue: return (uint64_t)(uint32_t)api->alGetEnumValue( A_P(0) );
    case OAF_alListenerf: api->alListenerf( A_I(0), A_F(1) ); break;
    case OAF_alListener3f: api->alListener3f( A_I(0), A_F(1), A_F(2), A_F(3) ); break;
    case OAF_alListenerfv: api->alListenerfv( A_I(0), A_P(1) ); break;
    case OAF_alListeneri: api->alListeneri( A_I(0), A_I(1) ); break;
    case OAF_alListener3i: api->alListener3i( A_I(0), A_I(1), A_I(2), A_I(3) ); break;
    case OAF_alListeneriv: api->alListeneriv( A_I(0), A_P(1) ); break;
    case OAF_alGetListenerf: api->alGetListenerf( A_I(0), A_P(1) ); break;
    case OAF_alGetListener3f: api->alGetListener3f( A_I(0), A_P(1), A_P(2), A_P(3) ); break;
    case OAF_alGetListenerfv: api->alGetListenerfv( A_I(0), A_P(1) ); break;
    case OAF_alGetListeneri: api->alGetListeneri( A_I(0), A_P(1) ); break;
    case OAF_alGetListener3i: api->alGetListener3i( A_I(0), A_P(1), A_P(2), A_P(3) ); break;
    case OAF_alGetListeneriv: api->alGetListeneriv( A_I(0), A_P(1) ); break;
    case OAF_alGenSources: api->alGenSources( A_I(0), A_P(1) ); break;
    case OAF_alDeleteSources: api->alDeleteSources( A_I(0), A_P(1) ); break;
    case OAF_alIsSource: return (uint8_t)api->alIsSource( A_U(0) );
    case OAF_alSourcef: api->alSourcef( A_U(0), A_I(1), A_F(2) ); break;
    case OAF_alSource3f: api->alSource3f( A_U(0), A_I(1), A_F(2), A_F(3), A_F(4) ); break;
    case OAF_alSourcefv: api->alSourcefv( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alSourcei: api->alSourcei( A_U(0), A_I(1), A_I(2) ); break;
    case OAF_alSource3i: api->alSource3i( A_U(0), A_I(1), A_I(2), A_I(3), A_I(4) ); break;
    case OAF_alSourceiv: api->alSourceiv( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetSourcef: api->alGetSourcef( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetSource3f: api->alGetSource3f( A_U(0), A_I(1), A_P(2), A_P(3), A_P(4) ); break;
    case OAF_alGetSourcefv: api->alGetSourcefv( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetSourcei: api->alGetSourcei( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetSource3i: api->alGetSource3i( A_U(0), A_I(1), A_P(2), A_P(3), A_P(4) ); break;
    case OAF_alGetSourceiv: api->alGetSourceiv( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alSourcePlayv: api->alSourcePlayv( A_I(0), A_P(1) ); break;
    case OAF_alSourceStopv: api->alSourceStopv( A_I(0), A_P(1) ); break;
    case OAF_alSourceRewindv: api->alSourceRewindv( A_I(0), A_P(1) ); break;
    case OAF_alSourcePausev: api->alSourcePausev( A_I(0), A_P(1) ); break;
    case OAF_alSourcePlay: api->alSourcePlay( A_U(0) ); break;
    case OAF_alSourceStop: api->alSourceStop( A_U(0) ); break;
    case OAF_alSourceRewind: api->alSourceRewind( A_U(0) ); break;
    case OAF_alSourcePause: api->alSourcePause( A_U(0) ); break;
    case OAF_alSourceQueueBuffers: api->alSourceQueueBuffers( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alSourceUnqueueBuffers: api->alSourceUnqueueBuffers( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGenBuffers: api->alGenBuffers( A_I(0), A_P(1) ); break;
    case OAF_alDeleteBuffers: api->alDeleteBuffers( A_I(0), A_P(1) ); break;
    case OAF_alIsBuffer: return (uint8_t)api->alIsBuffer( A_U(0) );
    case OAF_alBufferData: api->alBufferData( A_U(0), A_I(1), A_P(2), A_I(3), A_I(4) ); break;
    case OAF_alBufferf: api->alBufferf( A_U(0), A_I(1), A_F(2) ); break;
    case OAF_alBuffer3f: api->alBuffer3f( A_U(0), A_I(1), A_F(2), A_F(3), A_F(4) ); break;
    case OAF_alBufferfv: api->alBufferfv( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alBufferi: api->alBufferi( A_U(0), A_I(1), A_I(2) ); break;
    case OAF_alBuffer3i: api->alBuffer3i( A_U(0), A_I(1), A_I(2), A_I(3), A_I(4) ); break;
    case OAF_alBufferiv: api->alBufferiv( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetBufferf: api->alGetBufferf( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetBuffer3f: api->alGetBuffer3f( A_U(0), A_I(1), A_P(2), A_P(3), A_P(4) ); break;
    case OAF_alGetBufferfv: api->alGetBufferfv( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetBufferi: api->alGetBufferi( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetBuffer3i: api->alGetBuffer3i( A_U(0), A_I(1), A_P(2), A_P(3), A_P(4) ); break;
    case OAF_alGetBufferiv: api->alGetBufferiv( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alDopplerFactor: api->alDopplerFactor( A_F(0) ); break;
    case OAF_alDopplerVelocity: api->alDopplerVelocity( A_F(0) ); break;
    case OAF_alSpeedOfSound: api->alSpeedOfSound( A_F(0) ); break;
    case OAF_alDistanceModel: api->alDistanceModel( A_I(0) ); break;
    EFX_CASES(Effect, Effects)
    EFX_CASES(Filter, Filters)
    EFX_CASES(AuxiliaryEffectSlot, AuxiliaryEffectSlots)
    case OAF_alDeferUpdatesSOFT: api->alDeferUpdatesSOFT(); break;
    case OAF_alProcessUpdatesSOFT: api->alProcessUpdatesSOFT(); break;
    case OAF_alGetStringiSOFT: return put_string( api->alGetStringiSOFT( A_I(0), A_I(1) ), 0, a[2], a[3] );
    case OAF_alSourcedSOFT: api->alSourcedSOFT( A_U(0), A_I(1), A_D(2) ); break;
    case OAF_alSource3dSOFT: api->alSource3dSOFT( A_U(0), A_I(1), A_D(2), A_D(3), A_D(4) ); break;
    case OAF_alSourcedvSOFT: api->alSourcedvSOFT( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetSourcedSOFT: api->alGetSourcedSOFT( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetSource3dSOFT: api->alGetSource3dSOFT( A_U(0), A_I(1), A_P(2), A_P(3), A_P(4) ); break;
    case OAF_alGetSourcedvSOFT: api->alGetSourcedvSOFT( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alSourcei64SOFT: api->alSourcei64SOFT( A_U(0), A_I(1), A_L(2) ); break;
    case OAF_alSource3i64SOFT: api->alSource3i64SOFT( A_U(0), A_I(1), A_L(2), A_L(3), A_L(4) ); break;
    case OAF_alSourcei64vSOFT: api->alSourcei64vSOFT( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetSourcei64SOFT: api->alGetSourcei64SOFT( A_U(0), A_I(1), A_P(2) ); break;
    case OAF_alGetSource3i64SOFT: api->alGetSource3i64SOFT( A_U(0), A_I(1), A_P(2), A_P(3), A_P(4) ); break;
    case OAF_alGetSourcei64vSOFT: api->alGetSourcei64vSOFT( A_U(0), A_I(1), A_P(2) ); break;
    default: break;   /* the ALC, PE-only and unsupported entries never come here */
    }
    return 0;
}

static oa_status openal_call( struct openal32_call_params *params, int wow64 )
{
    uint32_t func = params->func;
    uint64_t a[OPENAL32_MAX_ARGS];
    const void *peb = ios_current_peb();
    unsigned int i;

    if (func >= OAF_COUNT) return (oa_status)0xc000000d;   /* STATUS_INVALID_PARAMETER */
    pthread_once( &hook_once, register_hook );
    memcpy( a, params->args, sizeof(a) );
    if (wow64)
        for (i = 0; i < OPENAL32_MAX_ARGS; i++)
            if (oa_info[func].ptrmask & (1u << i)) a[i] = (uint64_t)(uintptr_t)iom_guest_ptr( (uint32_t)a[i] );

    params->ret = 0;
    if (oa_info[func].flags & (OA_UNSUP | OA_PE))
    {
        log_unsupported( func );
        return 0;
    }
    if (oa_info[func].flags & OA_CTX)
    {
        apply_context( peb );
        params->ret = al_call( func, a );
        return 0;
    }
    pthread_mutex_lock( &lock );
    params->ret = alc_call( func, a, peb );
    pthread_mutex_unlock( &lock );
    return 0;
}

static oa_status openal_call64( void *args )
{
    return openal_call( args, 0 );
}

static oa_status openal_call32( void *args )
{
    return openal_call( args, 1 );
}

__attribute__((used, visibility("default"))) const oa_unixlib_entry openal32_unix_call_funcs[] =
{
    [unix_openal32_call] = openal_call64,
};
__attribute__((used, visibility("default"))) const oa_unixlib_entry openal32_unix_call_wow64_funcs[] =
{
    [unix_openal32_call] = openal_call32,
};
_Static_assert( sizeof(openal32_unix_call_funcs) / sizeof(openal32_unix_call_funcs[0]) == unix_openal32_funcs_count,
                "openal32_unix_call_funcs" );
