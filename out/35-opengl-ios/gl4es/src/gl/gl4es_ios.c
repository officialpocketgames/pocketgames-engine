// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 C3: the functions this tree adds to gl4es for pgl (include/gl4es_ios.h), and the line
// collector behind gl4es_ios_config.h's printf redirection (C3.3). Not part of upstream gl4es;
// built with it (src/CMakeLists.txt) and pre-linked into the same object (build-gl4es.sh), so it
// sees gl4es's internal names.

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gl4esinit.h>
#include "gl4es_ios.h"

#include "framebuffers.h"
#include "gl4es.h"
#include "glstate.h"
#include "matvec.h"
#include "pixel.h"
#include "math/eval.h"

/* this file implements what gl4es_ios_config.h redirects */
#undef printf
#undef fprintf
#undef vprintf
#undef vfprintf
#undef puts
#undef fputs
#undef fflush

void *NewGLState( void *shared_glstate, int es2only );
void DeleteGLState( void *oldstate );
void ActivateGLState( void *new_glstate );
glframebuffer_t *find_framebuffer( GLuint framebuffer );   /* framebuffers.c */
extern glstate_t default_glstate;

/* ---------------------------------------------------------------- C3.3: logging */

static void (*_Atomic log_fn)( const char *line );
static GL4ES_IOS_TLS char log_line[512];
static GL4ES_IOS_TLS size_t log_len;

void gl4es_ios_set_log( void (*log)( const char *line ) )
{
    atomic_store( &log_fn, log );
}

/* hands the collected line to the callback, without upstream's "LIBGL: " prefix */
static void log_flush( void )
{
    void (*fn)( const char *line ) = atomic_load( &log_fn );
    const char *text = log_line;

    log_line[log_len] = 0;
    log_len = 0;
    if (!strncmp( text, "LIBGL: ", 7 )) text += 7;
    if (fn && *text) fn( text );
}

/* collects text per thread; a newline ends a line, a full buffer cuts one */
static void log_append( const char *text, size_t len )
{
    for (; len; text++, len--)
    {
        if (*text == '\n')
        {
            log_flush();
            continue;
        }
        if (log_len == sizeof(log_line) - 1) log_flush();
        log_line[log_len++] = *text;
    }
}

int gl4es_ios_vfprintf( FILE *fp, const char *fmt, va_list args )
{
    char buf[1024], *text = buf;
    va_list again;
    int len;

    if (fp != stdout && fp != stderr) return vfprintf( fp, fmt, args );

    va_copy( again, args );
    len = vsnprintf( buf, sizeof(buf), fmt, args );
    if (len >= (int)sizeof(buf) && (text = malloc( (size_t)len + 1 )))
        vsnprintf( text, (size_t)len + 1, fmt, again );
    else if (len >= (int)sizeof(buf))
        text = buf, len = sizeof(buf) - 1;
    va_end( again );

    if (len > 0) log_append( text, (size_t)len );
    if (text != buf) free( text );
    return len;
}

int gl4es_ios_fprintf( FILE *fp, const char *fmt, ... )
{
    va_list args;
    int len;

    va_start( args, fmt );
    len = gl4es_ios_vfprintf( fp, fmt, args );
    va_end( args );
    return len;
}

int gl4es_ios_printf( const char *fmt, ... )
{
    va_list args;
    int len;

    va_start( args, fmt );
    len = gl4es_ios_vfprintf( stdout, fmt, args );
    va_end( args );
    return len;
}

/* fp NULL: puts (the text and a newline) */
int gl4es_ios_fputs( const char *str, FILE *fp )
{
    if (fp && fp != stdout && fp != stderr) return fputs( str, fp );
    log_append( str, strlen( str ) );
    if (!fp) log_append( "\n", 1 );
    return 0;
}

int gl4es_ios_fflush( FILE *fp )
{
    if (fp && fp != stdout && fp != stderr) return fflush( fp );
    return 0;
}

/* ---------------------------------------------------------------- C3.1: states */

/* NewGLState and DeleteGLState change a share group's count and lists: one at a time */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

void *gl4es_ios_state_create( void *share )
{
    void *state;

    pthread_mutex_lock( &state_lock );
    state = NewGLState( share, 0 );
    pthread_mutex_unlock( &state_lock );
    return state;
}

void gl4es_ios_state_activate( void *state )
{
    ActivateGLState( state );
}

void *gl4es_ios_state_current( void )
{
    return glstate == &default_glstate ? NULL : glstate;
}

void gl4es_ios_state_destroy( void *state )
{
    if (!state) return;
    pthread_mutex_lock( &state_lock );
    DeleteGLState( state );
    pthread_mutex_unlock( &state_lock );
}

/* ---------------------------------------------------------------- C3.2: initialisation */

static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void initialize_once( void )
{
    float matrix[16] = { 0 };
    unsigned char pixel[4] = { 0 };

    initialize_gl4es();   /* the fixed configuration is in init.c (GL4ES_IOS) */

    /* gl4es builds these read-only tables on first use; build them now, while one thread runs,
     * so that they are app-wide and fixed afterwards (C3.1) */
    _math_init_eval();
    (void)is_identity( matrix );
    pixel_srgb_inplace( pixel, 1, 1 );
}

void gl4es_ios_initialize( void )
{
    pthread_once( &init_once, initialize_once );
}

/* ---------------------------------------------------------------- C3.4: strings */

char gl4es_ios_renderer[128] = "Apple GPU";

void gl4es_ios_set_renderer( const char *renderer )
{
    if (renderer && *renderer) snprintf( gl4es_ios_renderer, sizeof(gl4es_ios_renderer), "%s", renderer );
}

/* ---------------------------------------------------------------- C3.5: buffer data */

void gl4es_ios_state_set_buffer_alloc( void *state, void *(*alloc_fn)( size_t size ), void (*free_fn)( void *ptr ) )
{
    glstate_t *s = state;

    if (!s) return;
    s->buffer_alloc = alloc_fn ? alloc_fn : malloc;
    s->buffer_free = free_fn ? free_fn : free;
}

/* ---------------------------------------------------------------- D5.3: names */

/* gl4es hands an application's framebuffer names to GLES unchanged (glGenFramebuffers), and its
 * framebuffer 0 is GLES's main framebuffer object, 0 when gl4es keeps none (LIBGL_FB unset) */
unsigned int gl4es_ios_gles_framebuffer( unsigned int name )
{
    if (!name) return glstate->fbo.mainfbo_fbo;
    return find_framebuffer( name ) ? name : 0;
}
