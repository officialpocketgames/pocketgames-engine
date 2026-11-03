// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part D3, D4, D6 and H: pgl's display, contexts, drawables' attachments, GL function
// lookup and clean-up, on ANGLE's EGL and gl4es. The Metal side (device, present ring, layer)
// is pgl_present.m.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pgl_internal.h"
#include "gl4esinit.h"
#include "gl4es_ios.h"

EGLDisplay pgl_display = EGL_NO_DISPLAY;
int pgl_es_major = 2;

PFNEGLCREATEIMAGEKHRPROC pgl_eglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC pgl_eglDestroyImageKHR;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC pgl_glEGLImageTargetRenderbufferStorageOES;
PFNGLBLITFRAMEBUFFERANGLEPROC pgl_glBlitFramebufferANGLE;
void (GL_APIENTRY *pgl_glBlitFramebuffer)( GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum );

/* ---------------------------------------------------------------- logging */

static void (*log_fn)( const char *line );

void pgl_set_log( void (*log)( const char *line ) )
{
    log_fn = log;
}

static void log_line( const char *line )
{
    if (log_fn) log_fn( line );
    else fprintf( stderr, "%s\n", line );
}

void pgl_log( const char *fmt, ... )
{
    char buf[512];
    va_list args;

    va_start( args, fmt );
    vsnprintf( buf, sizeof(buf), fmt, args );
    va_end( args );
    log_line( buf );
}

void pgl_log_rare( _Atomic uint64_t *counter, const char *fmt, ... )
{
    uint64_t n = atomic_fetch_add( counter, 1 );
    char buf[512];
    va_list args;
    int len;

    if (n && n % 256) return;
    va_start( args, fmt );
    len = vsnprintf( buf, sizeof(buf), fmt, args );
    va_end( args );
    if (n && len > 0 && (size_t)len < sizeof(buf) - 24)
        snprintf( buf + len, sizeof(buf) - (size_t)len, " (%llu times)", (unsigned long long)(n + 1) );
    log_line( buf );
}

/* gl4es's messages (C3.3) */
static void gl4es_log( const char *line )
{
    static _Atomic uint64_t lines;
    uint64_t n = atomic_fetch_add( &lines, 1 );

    if (n < 64 || !(n % 1024)) pgl_log( "[opengl] gl4es: %s", line );
}

/* ---------------------------------------------------------------- objects */

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pgl_registry registry;

static _Thread_local pgl_context *tls_context;
static _Thread_local pgl_drawable *tls_draw;

pgl_context *pgl_thread_context( void )
{
    return tls_context;
}

pgl_context *pgl_current( void )
{
    return tls_context;
}

static void registry_add( struct pgl_owned *item, const void *owner, enum pgl_owned_kind kind )
{
    pthread_mutex_lock( &registry_lock );
    pgl_registry_add( &registry, item, owner, kind );
    pthread_mutex_unlock( &registry_lock );
}

static void registry_remove( struct pgl_owned *item )
{
    pthread_mutex_lock( &registry_lock );
    pgl_registry_remove( item );
    pthread_mutex_unlock( &registry_lock );
}

/* ---------------------------------------------------------------- D3.1 */

static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static int init_status = -1;
static char renderer_name[256];
static EGLContext bootstrap = EGL_NO_CONTEXT;

/* gl4es's GLES functions come from ANGLE's eglGetProcAddress only (C2.2) */
static void *angle_proc( const char *name )
{
    return (void *)eglGetProcAddress( name );
}

/* D4.3: gl4es's main framebuffer is the current draw drawable */
static void main_fb_size( int *width, int *height )
{
    pgl_drawable *d = tls_draw;
    int w = 1, h = 1;

    if (d) pgl_drawable_size( d, &w, &h );
    *width = w;
    *height = h;
}

static int has_extension( const char *list, const char *ext )
{
    size_t len = strlen( ext );
    const char *p = list;

    while (p && (p = strstr( p, ext )))
    {
        if ((p == list || p[-1] == ' ') && (!p[len] || p[len] == ' ')) return 1;
        p += len;
    }
    return 0;
}

#define GET_PROC( var, type, name ) ((var) = (type)eglGetProcAddress( name ))

static void init_once_fn( void )
{
    static const EGLAttrib display_attribs[] =
    {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
        EGL_NONE,
    };
    static const char *const display_exts[] =
    {
        "EGL_KHR_create_context", "EGL_KHR_surfaceless_context", "EGL_KHR_no_config_context",
        "EGL_KHR_image_base", "EGL_ANGLE_metal_texture_client_buffer", "EGL_ANGLE_metal_shared_event_sync",
    };
    PFNEGLQUERYDISPLAYATTRIBEXTPROC p_eglQueryDisplayAttribEXT;
    PFNEGLQUERYDEVICESTRINGEXTPROC p_eglQueryDeviceStringEXT;
    const char *client_exts, *exts, *why = NULL;
    EGLAttrib device = 0;
    EGLint major, minor, attribs[3];
    const char *es3 = getenv( "PG_GL_ES3" );
    unsigned int i;

    pgl_registry_init( &registry );
    if (es3 && !strcmp( es3, "1" )) pgl_es_major = 3;

    client_exts = eglQueryString( EGL_NO_DISPLAY, EGL_EXTENSIONS );
    if (!client_exts || !has_extension( client_exts, "EGL_EXT_device_query" ))
    {
        pgl_log( "[opengl] pgl: ANGLE has no EGL_EXT_device_query" );
        return;
    }
    pgl_display = eglGetPlatformDisplay( EGL_PLATFORM_ANGLE_ANGLE, (void *)EGL_DEFAULT_DISPLAY, display_attribs );
    if (pgl_display == EGL_NO_DISPLAY || !eglInitialize( pgl_display, &major, &minor ))
    {
        pgl_log( "[opengl] pgl: no ANGLE Metal display (EGL error %#x)", eglGetError() );
        pgl_display = EGL_NO_DISPLAY;
        return;
    }
    exts = eglQueryString( pgl_display, EGL_EXTENSIONS );
    for (i = 0; i < sizeof(display_exts) / sizeof(display_exts[0]); i++)
    {
        if (exts && has_extension( exts, display_exts[i] )) continue;
        pgl_log( "[opengl] pgl: ANGLE's display lacks %s", display_exts[i] );
        return;
    }

    GET_PROC( p_eglQueryDisplayAttribEXT, PFNEGLQUERYDISPLAYATTRIBEXTPROC, "eglQueryDisplayAttribEXT" );
    GET_PROC( p_eglQueryDeviceStringEXT, PFNEGLQUERYDEVICESTRINGEXTPROC, "eglQueryDeviceStringEXT" );
    if (!p_eglQueryDisplayAttribEXT || !p_eglQueryDeviceStringEXT ||
        !p_eglQueryDisplayAttribEXT( pgl_display, EGL_DEVICE_EXT, &device ) || !device)
    {
        pgl_log( "[opengl] pgl: no EGL device for the display" );
        return;
    }
    exts = p_eglQueryDeviceStringEXT( (EGLDeviceEXT)device, EGL_EXTENSIONS );
    if (!exts || !has_extension( exts, "EGL_ANGLE_device_metal" ))
    {
        pgl_log( "[opengl] pgl: ANGLE's device lacks EGL_ANGLE_device_metal" );
        return;
    }
    if (pgl_metal_init( (EGLDeviceEXT)device, renderer_name, sizeof(renderer_name), &why ))
    {
        pgl_log( "[opengl] pgl: %s", why ? why : "no Metal device" );
        renderer_name[0] = 0;
        return;
    }

    GET_PROC( pgl_eglCreateImageKHR, PFNEGLCREATEIMAGEKHRPROC, "eglCreateImageKHR" );
    GET_PROC( pgl_eglDestroyImageKHR, PFNEGLDESTROYIMAGEKHRPROC, "eglDestroyImageKHR" );
    GET_PROC( pgl_glEGLImageTargetRenderbufferStorageOES, PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC,
              "glEGLImageTargetRenderbufferStorageOES" );
    GET_PROC( pgl_glBlitFramebufferANGLE, PFNGLBLITFRAMEBUFFERANGLEPROC, "glBlitFramebufferANGLE" );
    GET_PROC( pgl_glBlitFramebuffer, void (GL_APIENTRY *)( GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum ),
              "glBlitFramebuffer" );

    /* the bootstrap context, current while gl4es probes the hardware (C3.2) */
    attribs[0] = EGL_CONTEXT_CLIENT_VERSION;
    attribs[1] = pgl_es_major;
    attribs[2] = EGL_NONE;
    bootstrap = eglCreateContext( pgl_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs );
    if (bootstrap == EGL_NO_CONTEXT ||
        !eglMakeCurrent( pgl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, bootstrap ))
    {
        pgl_log( "[opengl] pgl: no ES %d.0 context (EGL error %#x)", pgl_es_major, eglGetError() );
        return;
    }
    exts = (const char *)glGetString( GL_EXTENSIONS );
    if (!pgl_glEGLImageTargetRenderbufferStorageOES || !exts || !has_extension( exts, "GL_OES_EGL_image" ))
        why = "GL_OES_EGL_image";
    else if (pgl_es_major < 3 && (!pgl_glBlitFramebufferANGLE || !has_extension( exts, "GL_ANGLE_framebuffer_blit" )))
        why = "GL_ANGLE_framebuffer_blit";
    else if (pgl_es_major >= 3 && !pgl_glBlitFramebuffer)
        why = "glBlitFramebuffer";
    if (why)
    {
        pgl_log( "[opengl] pgl: ANGLE's context lacks %s", why );
        eglMakeCurrent( pgl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
        return;
    }
    /* C4: logged once per app */
    pgl_log( "[opengl] ANGLE %s, renderer %s", (const char *)glGetString( GL_VERSION ),
             (const char *)glGetString( GL_RENDERER ) );

    gl4es_ios_set_log( gl4es_log );
    set_getprocaddress( angle_proc );
    set_getmainfbsize( main_fb_size );
    gl4es_ios_initialize();
    gl4es_ios_set_renderer( renderer_name );

    eglMakeCurrent( pgl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
    eglReleaseThread();
    init_status = 0;
}

int pgl_init( void )
{
    pthread_once( &init_once, init_once_fn );
    return init_status;
}

const char *pgl_renderer( void )
{
    return init_status ? "" : renderer_name;
}

/* ---------------------------------------------------------------- D3.2-D3.4 */

/* C3.1: share groups (pgl_internal.h) */
static struct pgl_group *group_new( void )
{
    struct pgl_group *g = calloc( 1, sizeof(*g) );
    pthread_mutexattr_t attr;

    if (!g) return NULL;
    pthread_mutexattr_init( &attr );
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
    pthread_mutex_init( &g->lock, &attr );
    pthread_mutexattr_destroy( &attr );
    atomic_store( &g->refs, 1 );   /* the creating context's */
    return g;
}

static void group_release( struct pgl_group *g )
{
    if (g && atomic_fetch_sub( &g->refs, 1 ) == 1)
    {
        pthread_mutex_destroy( &g->lock );
        free( g );
    }
}

struct pgl_group *pgl_group_enter( void )
{
    pgl_context *ctx = tls_context;
    struct pgl_group *g = ctx ? ctx->group : NULL;

    /* the thread's own context keeps the group alive until the ref below is taken: no other
     * thread can delete a context current here (opengl32 refuses it) */
    if (!g || atomic_load( &g->members ) < 2) return NULL;
    atomic_fetch_add( &g->refs, 1 );
    pthread_mutex_lock( &g->lock );
    return g;
}

void pgl_group_leave( struct pgl_group *g )
{
    if (!g) return;
    pthread_mutex_unlock( &g->lock );
    group_release( g );
}

/* makes ctx (NULL: none) current on the calling thread with its gl4es state */
static int bind_context( pgl_context *ctx )
{
    if (!eglMakeCurrent( pgl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx ? ctx->egl : EGL_NO_CONTEXT ))
        return 0;
    gl4es_ios_state_activate( ctx ? ctx->gl4es : NULL );
    return 1;
}

pgl_context *pgl_context_create( pgl_context *share, const struct pgl_context_request *req, const void *owner )
{
    pgl_context *ctx, *prev = tls_context;
    EGLint attribs[3];

    (void)req;   /* every accepted request (<= 2.1) gets the same context (D3.2) */
    if (pgl_init()) return NULL;
    if (share && share->dead) share = NULL;
    if (!(ctx = calloc( 1, sizeof(*ctx) ))) return NULL;
    if (!(ctx->group = share ? share->group : group_new()))
    {
        free( ctx );
        return NULL;
    }

    attribs[0] = EGL_CONTEXT_CLIENT_VERSION;
    attribs[1] = pgl_es_major;
    attribs[2] = EGL_NONE;
    ctx->egl = eglCreateContext( pgl_display, EGL_NO_CONFIG_KHR, share ? share->egl : EGL_NO_CONTEXT, attribs );
    if (ctx->egl == EGL_NO_CONTEXT)
    {
        pgl_log( "[opengl] pgl: eglCreateContext failed (EGL error %#x)", eglGetError() );
        if (!share) group_release( ctx->group );
        free( ctx );
        return NULL;
    }

    /* the gl4es state is made with its context current, then the thread's own comes back; a
     * state that shares reads its group's lists, so it is made under the group's lock */
    if (share) pthread_mutex_lock( &ctx->group->lock );
    if (eglMakeCurrent( pgl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->egl ))
        ctx->gl4es = gl4es_ios_state_create( share ? share->gl4es : NULL );
    if (share) pthread_mutex_unlock( &ctx->group->lock );
    if (!bind_context( prev )) pgl_log( "[opengl] pgl: the thread's previous context could not be restored" );
    if (!ctx->gl4es)
    {
        pgl_log( "[opengl] pgl: no gl4es state for a new context" );
        eglDestroyContext( pgl_display, ctx->egl );
        if (!share) group_release( ctx->group );
        free( ctx );
        return NULL;
    }

    /* a new group already holds its first context's ref (group_new) */
    atomic_fetch_add( &ctx->group->members, 1 );
    if (share) atomic_fetch_add( &ctx->group->refs, 1 );
    registry_add( &ctx->owned, owner, PGL_OWNED_CONTEXT );
    return ctx;
}

/* frees the context's gl4es state and its ANGLE context; the shell stays */
static void destroy_context_gl( pgl_context *ctx )
{
    pgl_context *prev;
    uintptr_t holder;

    if (tls_context == ctx) pgl_make_current( NULL, NULL );
    prev = tls_context;
    holder = atomic_load( &ctx->thread );

    if (holder && holder != (uintptr_t)pthread_self())
    {
        /* current on another thread (opengl32 refuses that, H1 releases dead threads'): ANGLE
         * defers the destruction until that thread releases it; the gl4es state is left */
        static _Atomic uint64_t busy;
        pgl_log_rare( &busy, "[opengl] pgl: context %p destroyed while current on another thread", (void *)ctx );
    }
    else if (eglMakeCurrent( pgl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->egl ))
    {
        gl4es_ios_state_activate( ctx->gl4es );
        gl4es_ios_state_destroy( ctx->gl4es );
        bind_context( prev );
    }
    ctx->gl4es = NULL;
    eglDestroyContext( pgl_display, ctx->egl );
    ctx->egl = EGL_NO_CONTEXT;
    ctx->dead = 1;
    atomic_fetch_sub( &ctx->group->members, 1 );
}

void pgl_context_destroy( pgl_context *ctx )
{
    if (!ctx) return;
    registry_remove( &ctx->owned );
    if (!ctx->dead) destroy_context_gl( ctx );
    group_release( ctx->group );
    free( ctx );
}

int pgl_make_current( pgl_context *ctx, pgl_drawable *draw )
{
    pgl_context *old = tls_context;

    if (ctx && ctx->dead) return 0;
    if (!bind_context( ctx ))
    {
        static _Atomic uint64_t fails;
        pgl_log_rare( &fails, "[opengl] pgl: eglMakeCurrent failed (EGL error %#x)", eglGetError() );
        return 0;
    }
    if (old && old != ctx) atomic_store( &old->thread, 0 );
    if (ctx) atomic_store( &ctx->thread, (uintptr_t)pthread_self() );
    tls_context = ctx;
    tls_draw = ctx ? draw : NULL;
    return 1;
}

/* ---------------------------------------------------------------- D4 */

pgl_drawable *pgl_drawable_create( uint64_t hwnd, unsigned int config, int width, int height, const void *owner )
{
    pgl_drawable *d;

    if (config >= PGL_CONFIG_COUNT || !(d = calloc( 1, sizeof(*d) ))) return NULL;
    atomic_init( &d->refs, 1 );
    pthread_mutex_init( &d->lock, NULL );
    d->hwnd = hwnd;
    d->config = config;
    d->width = width > 0 ? width : 1;
    d->height = height > 0 ? height : 1;
    d->interval = 1;
    registry_add( &d->owned, owner, PGL_OWNED_DRAWABLE );
    return d;
}

void pgl_drawable_add_ref( pgl_drawable *d )
{
    atomic_fetch_add( &d->refs, 1 );
}

void pgl_drawable_release( pgl_drawable *d )
{
    if (!d || atomic_fetch_sub( &d->refs, 1 ) != 1) return;

    registry_remove( &d->owned );
    pthread_mutex_lock( &d->lock );
    if (d->attached && !d->dead)
    {
        /* its context frees the FBO when it goes */
        static _Atomic uint64_t leaks;
        pgl_log_rare( &leaks, "[opengl] pgl: drawable %p released with its attachments", (void *)d );
    }
    if (d->present) pgl_present_destroy( d );
    pthread_mutex_unlock( &d->lock );
    pthread_mutex_destroy( &d->lock );
    free( d );
}

void pgl_drawable_size( pgl_drawable *d, int *width, int *height )
{
    *width = d->width;
    *height = d->height;
}

/* gl4es's entry points for the attachments (it must know the FBO, D4.1) */
struct gl4es_fbo_funcs
{
    void (*GenFramebuffers)( GLsizei, GLuint * );
    void (*DeleteFramebuffers)( GLsizei, const GLuint * );
    void (*BindFramebuffer)( GLenum, GLuint );
    void (*GenRenderbuffers)( GLsizei, GLuint * );
    void (*DeleteRenderbuffers)( GLsizei, const GLuint * );
    void (*BindRenderbuffer)( GLenum, GLuint );
    void (*RenderbufferStorage)( GLenum, GLenum, GLsizei, GLsizei );
    void (*FramebufferRenderbuffer)( GLenum, GLenum, GLenum, GLuint );
    GLenum (*CheckFramebufferStatus)( GLenum );
    void (*GetIntegerv)( GLenum, GLint * );
    void (*CopyTexImage2D)( GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint );
};

static struct gl4es_fbo_funcs fbo_funcs;

static void load_fbo_funcs( void )
{
#define LOAD( f ) fbo_funcs.f = (typeof(fbo_funcs.f))gl4es_GetProcAddress( "gl" #f )
    LOAD( GenFramebuffers );
    LOAD( DeleteFramebuffers );
    LOAD( BindFramebuffer );
    LOAD( GenRenderbuffers );
    LOAD( DeleteRenderbuffers );
    LOAD( BindRenderbuffer );
    LOAD( RenderbufferStorage );
    LOAD( FramebufferRenderbuffer );
    LOAD( CheckFramebufferStatus );
    LOAD( GetIntegerv );
    LOAD( CopyTexImage2D );
#undef LOAD
}

static int have_fbo_funcs( void )
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;

    pthread_once( &once, load_fbo_funcs );
    return fbo_funcs.GenFramebuffers && fbo_funcs.DeleteFramebuffers && fbo_funcs.BindFramebuffer &&
           fbo_funcs.GenRenderbuffers && fbo_funcs.DeleteRenderbuffers && fbo_funcs.BindRenderbuffer &&
           fbo_funcs.RenderbufferStorage && fbo_funcs.FramebufferRenderbuffer &&
           fbo_funcs.CheckFramebufferStatus && fbo_funcs.GetIntegerv && fbo_funcs.CopyTexImage2D;
}

/* pgl_pbuffer_copy's calls: gl4es's (GLenum, GLuint, GLint and GLsizei are these C types) */
static void copy_get_integerv( unsigned int pname, int *value ) { fbo_funcs.GetIntegerv( pname, value ); }
static void copy_gen_framebuffers( int n, unsigned int *names ) { fbo_funcs.GenFramebuffers( n, names ); }
static void copy_delete_framebuffers( int n, const unsigned int *names ) { fbo_funcs.DeleteFramebuffers( n, names ); }
static void copy_bind_framebuffer( unsigned int target, unsigned int name ) { fbo_funcs.BindFramebuffer( target, name ); }
static void copy_framebuffer_renderbuffer( unsigned int target, unsigned int attachment, unsigned int rb_target,
                                           unsigned int renderbuffer )
{
    fbo_funcs.FramebufferRenderbuffer( target, attachment, rb_target, renderbuffer );
}
static unsigned int copy_check_framebuffer_status( unsigned int target ) { return fbo_funcs.CheckFramebufferStatus( target ); }
static void copy_copy_tex_image_2d( unsigned int target, int level, unsigned int internal_format, int x, int y,
                                    int width, int height, int border )
{
    fbo_funcs.CopyTexImage2D( target, level, internal_format, x, y, width, height, border );
}

static const struct pgl_copy_gl copy_gl =
{
    copy_get_integerv, copy_gen_framebuffers, copy_delete_framebuffers, copy_bind_framebuffer,
    copy_framebuffer_renderbuffer, copy_check_framebuffer_status, copy_copy_tex_image_2d,
};

/* D4.2, clarification 2.6: WGL_ARB_render_texture, in the calling thread's (the program's) context */
int pgl_pbuffer_bind( pgl_drawable *d, const struct pgl_pbuffer_texture *tex, unsigned int source )
{
    pgl_context *ctx = tls_context;
    struct pgl_pbuffer_copy req;
    enum pgl_copy_result res;
    static _Atomic uint64_t unshared, nothing, incomplete;

    if (source == PGL_GL_NONE) return 1;   /* the copy is the binding: nothing to release */
    if (!d || !ctx || ctx->dead || !have_fbo_funcs()) return 0;

    pthread_mutex_lock( &d->lock );
    if (d->attached && d->attached->group != ctx->group)
    {
        pthread_mutex_unlock( &d->lock );
        pgl_log_rare( &unshared, "[opengl] pgl: wglBindTexImageARB from a context that shares no lists with the pbuffer's" );
        return 0;
    }
    req.source = source;
    req.renderbuffer = d->dead || !d->attached ? 0 : d->color_rb;
    req.width = d->att_width;
    req.height = d->att_height;
    req.texture_target = tex->target;
    req.texture_format = tex->format;
    req.cube_face = tex->cube_face;
    req.level = tex->level;
    res = pgl_pbuffer_copy( &copy_gl, &req );
    pthread_mutex_unlock( &d->lock );

    switch (res)
    {
    case PGL_COPY_DONE: return 1;
    case PGL_COPY_NOTHING:
        pgl_log_rare( &nothing, "[opengl] pgl: wglBindTexImageARB on a pbuffer never drawn to" );
        return 1;
    case PGL_COPY_INCOMPLETE:
        pgl_log_rare( &incomplete, "[opengl] pgl: wglBindTexImageARB: the pbuffer's colour buffer cannot be read" );
        return 0;
    case PGL_COPY_BAD_SOURCE:
    default:
        return 0;
    }
}

/* the depth/stencil storage of a configuration (D4.1), 0 for none */
static GLenum depth_format( const struct pgl_config *cfg )
{
    if (!cfg->depth_bits) return 0;
    if (cfg->stencil_bits) return GL_DEPTH24_STENCIL8_OES;
    return cfg->depth_bits == 24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16;
}

/* (re)allocates the attachments at the wanted size; d->lock held, its context current */
static void size_attachments( pgl_drawable *d )
{
    GLenum depth = depth_format( &pgl_configs[d->config] );
    GLint old_rb = 0;
    GLenum status;

    fbo_funcs.GetIntegerv( GL_RENDERBUFFER_BINDING, &old_rb );
    fbo_funcs.BindRenderbuffer( GL_RENDERBUFFER, d->color_rb );
    fbo_funcs.RenderbufferStorage( GL_RENDERBUFFER, GL_RGBA8_OES, d->width, d->height );
    if (d->depth_rb)
    {
        fbo_funcs.BindRenderbuffer( GL_RENDERBUFFER, d->depth_rb );
        fbo_funcs.RenderbufferStorage( GL_RENDERBUFFER, depth, d->width, d->height );
    }
    fbo_funcs.BindRenderbuffer( GL_RENDERBUFFER, (GLuint)old_rb );
    d->att_width = d->width;
    d->att_height = d->height;

    if ((status = fbo_funcs.CheckFramebufferStatus( GL_FRAMEBUFFER )) != GL_FRAMEBUFFER_COMPLETE)
    {
        static _Atomic uint64_t incomplete;
        pgl_log_rare( &incomplete, "[opengl] pgl: drawable framebuffer %ux%u incomplete (%#x)",
                      d->width, d->height, status );
    }
}

unsigned int pgl_drawable_attach( pgl_drawable *d )
{
    pgl_context *ctx = tls_context;
    GLint old_fbo = 0;
    GLuint fbo = 0;

    if (!ctx || !have_fbo_funcs()) return 0;
    pthread_mutex_lock( &d->lock );
    if (d->dead) goto done;
    if (d->attached)
    {
        if (d->attached == ctx) fbo = d->fbo;
        else pgl_log( "[opengl] pgl: drawable %p already has attachments in another context", (void *)d );
        goto done;
    }

    fbo_funcs.GetIntegerv( GL_FRAMEBUFFER_BINDING, &old_fbo );
    fbo_funcs.GenFramebuffers( 1, &d->fbo );
    fbo_funcs.GenRenderbuffers( 1, &d->color_rb );
    if (depth_format( &pgl_configs[d->config] )) fbo_funcs.GenRenderbuffers( 1, &d->depth_rb );
    fbo_funcs.BindFramebuffer( GL_FRAMEBUFFER, d->fbo );
    fbo_funcs.FramebufferRenderbuffer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, d->color_rb );
    if (d->depth_rb)
    {
        fbo_funcs.FramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, d->depth_rb );
        if (pgl_configs[d->config].stencil_bits)
            fbo_funcs.FramebufferRenderbuffer( GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, d->depth_rb );
    }
    size_attachments( d );
    fbo_funcs.BindFramebuffer( GL_FRAMEBUFFER, (GLuint)old_fbo );
    d->attached = ctx;
    fbo = d->fbo;

done:
    pthread_mutex_unlock( &d->lock );
    return fbo;
}

void pgl_drawable_detach( pgl_drawable *d )
{
    pthread_mutex_lock( &d->lock );
    if (d->attached && !d->dead)
    {
        if (d->attached != tls_context)
            pgl_log( "[opengl] pgl: drawable %p detached without its context current", (void *)d );
        else
        {
            if (d->present) pgl_present_detach( d );
            fbo_funcs.DeleteFramebuffers( 1, &d->fbo );
            fbo_funcs.DeleteRenderbuffers( 1, &d->color_rb );
            if (d->depth_rb) fbo_funcs.DeleteRenderbuffers( 1, &d->depth_rb );
        }
    }
    d->attached = NULL;
    d->fbo = d->color_rb = d->depth_rb = 0;
    pthread_mutex_unlock( &d->lock );
}

void pgl_drawable_resize( pgl_drawable *d, int width, int height )
{
    GLint old_fbo = 0;

    pthread_mutex_lock( &d->lock );
    d->width = width > 0 ? width : 1;
    d->height = height > 0 ? height : 1;
    if (d->attached && !d->dead && d->attached == tls_context &&
        (d->att_width != d->width || d->att_height != d->height))
    {
        fbo_funcs.GetIntegerv( GL_FRAMEBUFFER_BINDING, &old_fbo );
        fbo_funcs.BindFramebuffer( GL_FRAMEBUFFER, d->fbo );
        size_attachments( d );
        fbo_funcs.BindFramebuffer( GL_FRAMEBUFFER, (GLuint)old_fbo );
    }
    pthread_mutex_unlock( &d->lock );
}

void pgl_drawable_set_interval( pgl_drawable *d, int interval )
{
    pthread_mutex_lock( &d->lock );
    d->interval = interval;
    pthread_mutex_unlock( &d->lock );
}

int pgl_drawable_swap( pgl_drawable *d, int visible )
{
    int ret;

    pthread_mutex_lock( &d->lock );
    if (d->dead || !d->hwnd) ret = 1;
    else if (!d->attached || d->attached != tls_context)
    {
        /* D4.1: nothing can be copied without the drawable's context current */
        static _Atomic uint64_t no_context;
        pgl_log_rare( &no_context, "[opengl] pgl: swap of drawable %p without its context current", (void *)d );
        ret = 1;
    }
    else ret = pgl_present_swap( d, visible );
    pthread_mutex_unlock( &d->lock );
    return ret;
}

void pgl_drawable_stop_presenting( pgl_drawable *d )
{
    pthread_mutex_lock( &d->lock );
    if (d->present) pgl_present_stop( d );
    pthread_mutex_unlock( &d->lock );
}

/* ---------------------------------------------------------------- D6 */

static const GLubyte *(*gl4es_glGetString)( GLenum );
static const GLubyte *(*gl4es_glGetStringi)( GLenum, GLuint );
static char extensions[PGL_EXTENSIONS_MAX];
static const char *extension_list[1024];
static GLuint extension_count;

static void filter_extensions_once( void )
{
    const char *in = gl4es_glGetString ? (const char *)gl4es_glGetString( GL_EXTENSIONS ) : NULL;
    static char split[PGL_EXTENSIONS_MAX];
    char *p;

    pgl_filter_extensions( in, extensions, sizeof(extensions) );
    memcpy( split, extensions, sizeof(split) );
    for (p = strtok( split, " " ); p && extension_count < 1024; p = strtok( NULL, " " ))
        extension_list[extension_count++] = p;
}

static const char *filtered_extensions( void )
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once( &once, filter_extensions_once );
    return extensions;
}

/* C3.4: the strings a program reads */
static const GLubyte *pgl_glGetString( GLenum name )
{
    switch (name)
    {
    case GL_VENDOR: return (const GLubyte *)PGL_GL_VENDOR_VALUE;
    case GL_RENDERER: return (const GLubyte *)renderer_name;
    case GL_VERSION: return (const GLubyte *)PGL_GL_VERSION_VALUE;
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)PGL_GLSL_VERSION_VALUE;
    case GL_EXTENSIONS: return (const GLubyte *)filtered_extensions();
    default: return gl4es_glGetString ? gl4es_glGetString( name ) : NULL;
    }
}

static const GLubyte *pgl_glGetStringi( GLenum name, GLuint index )
{
    if (name == GL_EXTENSIONS)
    {
        filtered_extensions();
        return index < extension_count ? (const GLubyte *)extension_list[index] : NULL;
    }
    return gl4es_glGetStringi ? gl4es_glGetStringi( name, index ) : NULL;
}

void *pgl_get_proc_address( const char *name )
{
    if (pgl_init() || !name) return NULL;
    if (!strcmp( name, "glGetString" ))
    {
        gl4es_glGetString = (typeof(gl4es_glGetString))gl4es_GetProcAddress( name );
        return (void *)pgl_glGetString;
    }
    if (!strcmp( name, "glGetStringi" ))
    {
        if (!gl4es_glGetString) gl4es_glGetString = (typeof(gl4es_glGetString))gl4es_GetProcAddress( "glGetString" );
        gl4es_glGetStringi = (typeof(gl4es_glGetStringi))gl4es_GetProcAddress( name );
        return (void *)pgl_glGetStringi;
    }
    return gl4es_GetProcAddress( name );
}

/* ---------------------------------------------------------------- H */

void pgl_thread_exit( void )
{
    pgl_context *ctx = tls_context;

    if (init_status) return;
    if (ctx)
    {
        eglMakeCurrent( pgl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
        gl4es_ios_state_activate( NULL );
        atomic_store( &ctx->thread, 0 );
        tls_context = NULL;
        tls_draw = NULL;
    }
    eglReleaseThread();
}

void pgl_process_ended( const void *owner, unsigned int *contexts, unsigned int *drawables )
{
    unsigned int counts[PGL_OWNED_KINDS];
    struct pgl_registry taken, ctxs;
    struct pgl_owned *item;

    if (contexts) *contexts = 0;
    if (drawables) *drawables = 0;
    if (init_status || !owner) return;

    pthread_mutex_lock( &registry_lock );
    pgl_registry_take( &registry, owner, &taken, counts );
    pthread_mutex_unlock( &registry_lock );
    pgl_registry_init( &ctxs );

    /* drawables first: their Metal objects and EGL images; the GL objects of their attachments
     * and rings go with the contexts below */
    while ((item = pgl_registry_first( &taken )))
    {
        pgl_registry_remove( item );
        if (item->kind == PGL_OWNED_CONTEXT)
        {
            pgl_registry_add( &ctxs, item, owner, PGL_OWNED_CONTEXT );
            continue;
        }
        pgl_drawable *d = (pgl_drawable *)item;
        pthread_mutex_lock( &d->lock );
        if (d->present) pgl_present_destroy( d );
        d->attached = NULL;
        d->fbo = d->color_rb = d->depth_rb = 0;
        d->dead = 1;
        pthread_mutex_unlock( &d->lock );
    }
    while ((item = pgl_registry_first( &ctxs )))
    {
        pgl_registry_remove( item );
        destroy_context_gl( (pgl_context *)item );
    }

    if (contexts) *contexts = counts[PGL_OWNED_CONTEXT];
    if (drawables) *drawables = counts[PGL_OWNED_DRAWABLE];
}
