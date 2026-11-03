/*
 *    Copyright (c) 2000 Lionel Ulmer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* Changed for MYIOSDECK (spec 35 Parts D7, G1, H): the WoW64 address macros the generated
 * thunks use (WOW_HOST, WOW_GUEST, WOW_HANDLE and two for buffer offsets), get_teb64 returns the
 * calling thread's own 64-bit TEB, and the iOS additions of opengl_ios.c. Upstream: Wine 11.4,
 * dlls/opengl32/unix_private.h. */

#ifndef __WINE_OPENGL32_UNIX_PRIVATE_H
#define __WINE_OPENGL32_UNIX_PRIVATE_H

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wingdi.h"
#include "ntgdi.h"

#include "wine/opengl_driver.h"
#include "unix_thunks.h"

struct registry_entry
{
    const char *name;      /* name of the extension */
    const char *extension; /* name of the GL/WGL extension */
    size_t offset;         /* offset in the opengl_funcs table */
};

extern const struct registry_entry extension_registry[];
extern const int extension_registry_size;

extern struct opengl_funcs null_opengl_funcs;

static inline const struct opengl_funcs *get_dc_funcs( HDC hdc )
{
    DWORD has_opengl;

    if (NtGdiGetDCDword( hdc, NtGdiHasOpenGL, &has_opengl ) && has_opengl)
        return __wine_get_opengl_driver( WINE_OPENGL_DRIVER_VERSION );

    RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
    return &null_opengl_funcs;
}

static inline const struct opengl_funcs *get_pbuffer_funcs( HPBUFFERARB client_pbuffer )
{
    struct opengl_client_pbuffer *client = opengl_client_pbuffer_from_client( client_pbuffer );
    return client_pbuffer ? (struct opengl_funcs *)(UINT_PTR)client->unix_funcs : NULL;
}

static inline const struct opengl_funcs *get_context_funcs( HGLRC client_context )
{
    struct opengl_client_context *client = opengl_client_context_from_client( client_context );
    return client_context ? (struct opengl_funcs *)(UINT_PTR)client->unix_funcs : NULL;
}

static inline GLsync get_unix_sync( GLsync sync )
{
    return (GLsync)(UINT_PTR)sync->unix_handle;
}

/* ---- spec 35: the iOS additions (opengl_ios.c) ---- */

/* D7: the calling thread's allocation ceiling (spec 12 B1's rule): a WoW64 thread gets the
 * guest ceiling, every other thread 0; cached per thread. Replaces upstream's global. */
extern ULONG_PTR opengl_ios_zero_bits( void );

/* D7: the process's call_gl_debug_message_callback (a PE address of that process) */
extern UINT64 opengl_ios_debug_callback( void );
extern void opengl_ios_set_debug_callback( UINT64 callback );

/* D7, G1.4: the calling process's WoW64 string table: the address of its entry array pointer;
 * *count receives the address of its count. Called with wgl_lock held. */
extern void **opengl_ios_wow64_strings( SIZE_T **count );

/* G1.3: ptr is inside the calling process's guest window */
extern BOOL opengl_ios_in_window( const void *ptr );

/* H2: opengl32's records of the calling process's contexts (opengl_ios.c keeps the lists,
 * unix_wgl.c frees what they hold) */
extern void opengl_ios_context_created( struct list *entry );
extern void opengl_ios_context_deleted( struct list *entry );
extern void opengl_ios_free_contexts( struct list *contexts, unsigned int *count );

#ifdef _WIN64

#include "opengl_ios_wow.h"

/* ios_wow_host_base(): spec 17 E1's helper in the shared wine/unixlib.h (out/09-ntdll-processes/
 * include/): the calling thread's window base B (spec 10 E3), 0 for none */
extern void opengl_ios_wow_outside( const void *host );   /* logs, rate-limited */

/* G1.1: guest to host; 0 stays NULL */
static inline void *opengl_wow_host( ULONG guest )
{
    return opengl_wow_host_at( ios_wow_host_base(), guest );
}

/* G1.1: host to guest; NULL stays 0, outside the caller's window 0 (logged) */
static inline ULONG opengl_wow_guest( const void *host )
{
    int outside;
    ULONG guest = opengl_wow_guest_at( ios_wow_host_base(), host, &outside );
    if (outside) opengl_ios_wow_outside( host );
    return guest;
}

/* a pointer handed back that may be a buffer offset (glGetPointerv and friends) */
static inline ULONG opengl_wow_guest_or_offset( const void *host )
{
    int outside;
    ULONG guest = opengl_wow_guest_or_offset_at( ios_wow_host_base(), host, &outside );
    if (outside) opengl_ios_wow_outside( host );
    return guest;
}

/* an argument that is an offset when a buffer object is bound to binding (a GL_*_BINDING
 * query), else a pointer (opengl_ios.c, through the context's own bindings) */
extern void *opengl_wow_host_or_offset( TEB *teb, GLenum binding, ULONG guest );

#define WOW_HOST(x)                 opengl_wow_host( (ULONG)(x) )
#define WOW_GUEST(p)                opengl_wow_guest( (const void *)(p) )
#define WOW_HANDLE(x)               ULongToHandle( (ULONG)(x) )
#define WOW_GUEST_OR_OFFSET(p)      opengl_wow_guest_or_offset( (const void *)(p) )
#define WOW_HOST_OR_OFFSET(t,b,x)   opengl_wow_host_or_offset( (t), (b), (ULONG)(x) )

/* G1.1: arrays of guest pointers: each element with WOW_HOST. address is the array's host
 * address (the generator passes WOW_HOST of the field; upstream passed the field itself). */
static inline void *copy_wow64_ptr32s( const void *address, ULONG count )
{
    const ULONG *ptrs = address;
    void **tmp;

    if (!ptrs || !(tmp = calloc( count, sizeof(*tmp) ))) return NULL;
    while (count--) tmp[count] = WOW_HOST( ptrs[count] );
    return tmp;
}

/* arrays of 32-bit numbers widened to pointer size (GLintptr / GLsizeiptr arrays), as upstream */
static inline void *copy_wow64_values32( const void *address, ULONG count )
{
    const ULONG *values = address;
    void **tmp;

    if (!values || !(tmp = calloc( count, sizeof(*tmp) ))) return NULL;
    while (count--) tmp[count] = ULongToPtr( values[count] );
    return tmp;
}

/* arrays of guest pointers that are buffer offsets when a buffer is bound to binding
 * (glMultiDrawElements' indices) */
static inline void *copy_wow64_ptr32s_or_offsets( TEB *teb, GLenum binding, const void *address, ULONG count )
{
    const ULONG *ptrs = address;
    void **tmp;

    if (!ptrs || !(tmp = calloc( count, sizeof(*tmp) ))) return NULL;
    while (count--) tmp[count] = WOW_HOST_OR_OFFSET( teb, binding, ptrs[count] );
    return tmp;
}

/* G1.2: the call always runs on the calling thread, whose 64-bit TEB is the unix side's own; the
 * 32-bit TEB pointer would need the window base */
static inline TEB *get_teb64( ULONG teb32 )
{
    (void)teb32;
    return NtCurrentTeb();
}

extern struct buffer *invalidate_buffer_name( TEB *teb, GLuint name );
extern struct buffer *invalidate_buffer_target( TEB *teb, GLenum target );
extern void free_buffer( const struct opengl_funcs *funcs, struct buffer *buffer );
extern NTSTATUS return_wow64_string( const void *str, PTR32 *wow64_str );

#endif

extern pthread_mutex_t wgl_lock;

extern NTSTATUS process_attach( void *args );
extern NTSTATUS thread_attach( void *args );
extern NTSTATUS process_detach( void *args );
extern NTSTATUS get_pixel_formats( void *args );
extern void set_context_attribute( TEB *teb, GLenum name, const void *value, size_t size );
extern void set_current_fbo( TEB *teb, GLenum target, GLuint framebuffer );
extern GLuint get_default_fbo( TEB *teb, GLenum target );
extern void push_default_fbo( TEB *teb );
extern void pop_default_fbo( TEB *teb );
extern void resolve_default_fbo( TEB *teb, BOOL read );

#endif /* __WINE_OPENGL32_UNIX_PRIVATE_H */
