// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35: pgl without its GL backend, for a build in which ANGLE or gl4es (with this tree's C3
// changes) could not be built (build-pgl.sh none; ci/ios-part.sh falls back to it and reports
// the part as failed). pgl_init fails, so win32u's pOpenGLInit installs no driver and every
// program gets OpenGL "off" (spec 12 clarification 8's behaviour): the engine still links and
// runs as before spec 35. pgl_pure.c is built as usual (win32u uses its format table).

#include <stdio.h>

#include "pgl.h"

static void (*log_fn)( const char *line );

void pgl_set_log( void (*log)( const char *line ) )
{
    log_fn = log;
}

int pgl_init( void )
{
    static int logged;
    const char *line = "[opengl] pgl: this build has no GL backend (ANGLE and gl4es were not built)";

    if (!logged++)
    {
        if (log_fn) log_fn( line );
        else fprintf( stderr, "%s\n", line );
    }
    return -1;
}

const char *pgl_renderer( void ) { return ""; }
pgl_context *pgl_context_create( pgl_context *share, const struct pgl_context_request *req, const void *owner ) { return NULL; }
void pgl_context_destroy( pgl_context *ctx ) { }
int pgl_make_current( pgl_context *ctx, pgl_drawable *draw ) { return ctx == NULL; }
pgl_context *pgl_current( void ) { return NULL; }
pgl_drawable *pgl_drawable_create( uint64_t hwnd, unsigned int config, int width, int height, const void *owner ) { return NULL; }
void pgl_drawable_add_ref( pgl_drawable *d ) { }
void pgl_drawable_release( pgl_drawable *d ) { }
unsigned int pgl_drawable_attach( pgl_drawable *d ) { return 0; }
void pgl_drawable_detach( pgl_drawable *d ) { }
void pgl_drawable_resize( pgl_drawable *d, int width, int height ) { }
void pgl_drawable_size( pgl_drawable *d, int *width, int *height ) { *width = *height = 1; }
void pgl_drawable_set_interval( pgl_drawable *d, int interval ) { }
int pgl_drawable_swap( pgl_drawable *d, int visible ) { return 1; }
void pgl_drawable_stop_presenting( pgl_drawable *d ) { }
int pgl_pbuffer_bind( pgl_drawable *d, const struct pgl_pbuffer_texture *tex, unsigned int source ) { return 0; }
void *pgl_drawable_last_copy( pgl_drawable *d, void **event, uint64_t *value ) { return NULL; }
void *pgl_get_proc_address( const char *name ) { return NULL; }
void pgl_thread_exit( void ) { }

int32_t pgl_call_in_pool( int32_t (*fn)( void *args ), void *args )
{
    return fn( args );   /* nothing here autoreleases: no GL backend runs */
}

void pgl_process_ended( const void *owner, unsigned int *contexts, unsigned int *drawables )
{
    if (contexts) *contexts = 0;
    if (drawables) *drawables = 0;
}
