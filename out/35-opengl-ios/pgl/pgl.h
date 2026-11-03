// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// pgl: the native GL backend of spec 35 (Part D3-D6): OpenGL 2.1 from gl4es on a native ANGLE
// with its Metal backend, presented into a window's CAMetalLayer. A small C API with no Wine
// types. win32u's iOS GL driver (out/12-win32u-ios/win32u-unix/opengl_ios.c) and opengl32's
// unix side call it; the macOS render test (tests/macos/) calls it with no Wine at all.
//
// Threads: every function may be called from any thread unless it says otherwise. pgl starts no
// thread of its own (ANGLE may).

#ifndef PGL_H
#define PGL_H

#include <stddef.h>
#include <stdint.h>

#include "pgl_pure.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pgl_context pgl_context;
typedef struct pgl_drawable pgl_drawable;

/* ---- logging ---- */

/* Where pgl's (and gl4es's) messages go, one line each without a newline. The driver sends
 * them to Wine's debug output. Before this is called they go to stderr. pgl rate-limits its
 * own repeated messages. */
void pgl_set_log( void (*log)( const char *line ) );

/* ---- D3.1 ---- */

/* Once per app: ANGLE's Metal display, the checks of D3.1, the Metal device and present queue,
 * the bootstrap context and gl4es's initialisation. Later calls return the first call's result.
 * 0 on success; on failure the reason has been logged once. */
int pgl_init( void );

/* The Metal device's name (GL_RENDERER), "" before pgl_init succeeded. */
const char *pgl_renderer( void );

/* ---- D3.2-D3.4 ---- */

/* An ANGLE context with no config (ES 2.0, or 3.0 with PG_GL_ES3=1) sharing with share's, and a
 * gl4es state sharing with share's. owner: the creating pseudo-process (its PEB), for H2.
 * req has been checked with pgl_parse_context_attribs. NULL on failure (logged). */
pgl_context *pgl_context_create( pgl_context *share, const struct pgl_context_request *req, const void *owner );

/* D3.3: from any thread. Releases the context first if it is current on the calling thread. */
void pgl_context_destroy( pgl_context *ctx );

/* D3.4: makes ctx current on the calling thread, surfaceless (every drawable is an FBO), with
 * its gl4es state; NULL releases the thread's context. draw is the drawable gl4es's main
 * framebuffer size is taken from (NULL: none). Returns 1 on success. */
int pgl_make_current( pgl_context *ctx, pgl_drawable *draw );

/* The calling thread's current context, or NULL. */
pgl_context *pgl_current( void );

/* ---- D4: drawables ---- */

/* A drawable of configuration config (pgl_configs[]), width x height (at least 1 x 1). hwnd: the
 * window it presents into (D5), 0 for a pbuffer. owner as for contexts. One reference, for the
 * caller. */
pgl_drawable *pgl_drawable_create( uint64_t hwnd, unsigned int config, int width, int height, const void *owner );

/* One more reference, and dropping one. The last one frees the drawable: its attachments must
 * have been detached (pgl_drawable_detach) unless its program ended (H2). */
void pgl_drawable_add_ref( pgl_drawable *d );
void pgl_drawable_release( pgl_drawable *d );

/* D4.1 set_context(private): with a context current, creates the drawable's FBO and its
 * attachments through gl4es, sized to the drawable, and returns the FBO's name (gl4es's), 0 on
 * failure. A drawable has attachments in one context at a time. */
unsigned int pgl_drawable_attach( pgl_drawable *d );

/* D4.1 set_context(NULL): with the same context still current, deletes the FBO, its
 * attachments and the present ring's per-context objects. */
void pgl_drawable_detach( pgl_drawable *d );

/* GL_FLUSH_UPDATED: the new size (at least 1 x 1). With attachments, they are reallocated now
 * (the context they belong to must be current); otherwise at the next attach. */
void pgl_drawable_resize( pgl_drawable *d, int width, int height );

void pgl_drawable_size( pgl_drawable *d, int *width, int *height );

/* GL_FLUSH_INTERVAL: the window's swap interval (D5.4: displaySyncEnabled follows it). */
void pgl_drawable_set_interval( pgl_drawable *d, int interval );

/* D5.3: present the drawable's colour attachment into its window. The drawable's context must
 * be current on the calling thread. visible 0 (a hidden window): no Metal pass, the frame
 * counts as presented. Returns 1 (a swap never fails toward the program). */
int pgl_drawable_swap( pgl_drawable *d, int visible );

/* The client surface was detached or destroyed: stop presenting and release the Metal view. The
 * next swap asks for the view again. */
void pgl_drawable_stop_presenting( pgl_drawable *d );

/* D4.2, clarification 2.6: a pbuffer's texture binding (WGL_ARB_render_texture), as the pbuffer
 * was created (target, format) and last set with wglSetPbufferAttribARB (face, level) */
struct pgl_pbuffer_texture
{
    unsigned int target;           /* GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, ... */
    unsigned int format;           /* GL_RGB or GL_RGBA */
    unsigned int cube_face;        /* GL_TEXTURE_CUBE_MAP_POSITIVE_X..., 0: positive X */
    int level;
};

/* wglBindTexImageARB (source GL_FRONT, GL_BACK, ...) and wglReleaseTexImageARB (GL_NONE): copies
 * the pbuffer's colour into the texture bound to tex->target in the calling thread's context
 * (pgl_pbuffer_copy), whose share group must hold the pbuffer's attachments. 1 on success. */
int pgl_pbuffer_bind( pgl_drawable *d, const struct pgl_pbuffer_texture *tex, unsigned int source );

/* Part J3 (the render test): the ring texture (an id<MTLTexture>) the last swap copied the
 * colour attachment into, its MTLSharedEvent and the value the event reaches when the copy is
 * done. NULL before the first swap. Not used by the driver. */
void *pgl_drawable_last_copy( pgl_drawable *d, void **event, uint64_t *value );

/* ---- D6 ---- */

/* gl4es_GetProcAddress, except glGetString and glGetStringi, which return C3.4's strings
 * (pgl_gl_get_string), and names gl4es doesn't implement, which are NULL. */
void *pgl_get_proc_address( const char *name );

/* ---- E4 ---- */

/* Calls fn(args) inside an autorelease pool drained before it returns. The signature is that of
 * a Wine unix-call entry (NTSTATUS is a 32-bit LONG). */
int32_t pgl_call_in_pool( int32_t (*fn)( void *args ), void *args );

/* ---- H ---- */

/* H1: the calling thread is exiting. If it has a context current: eglMakeCurrent(NO_CONTEXT),
 * clear the thread's gl4es state, then eglReleaseThread. Nothing when pgl_init never ran. */
void pgl_thread_exit( void );

/* H2: every context and drawable owner created, with their present rings and Metal views, is
 * destroyed. Objects a caller still references stay as empty shells until released. Sets the
 * counts when the pointers are non-NULL. */
void pgl_process_ended( const void *owner, unsigned int *contexts, unsigned int *drawables );

#ifdef __cplusplus
}
#endif

#endif /* PGL_H */
