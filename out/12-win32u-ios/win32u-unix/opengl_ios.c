// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part D1-D5, D6 (win32u side): the iOS GL driver under win32u's opengl.c (Wine 11.4's
// driver interface, include/wine/opengl_driver.h, WINE_OPENGL_DRIVER_VERSION 37). The user
// driver's pOpenGLInit installs it when PG_OPENGL=1; everything GL is done by pgl
// (out/35-opengl-ios/pgl, MIT): ANGLE contexts with gl4es states, FBO drawables, presenting into
// the window's CAMetalLayer.
//
// win32u is built without SONAME_LIBEGL (spec 12 A3, w32ios_config.h): display_funcs_init then
// finds no EGL library, calls this pOpenGLInit, initialises no EGL platform and takes every GL
// function from p_get_proc_address. A drawable's EGL surface stays NULL.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "ntstatus.h"
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/opengl_driver.h"
#include "wine/debug.h"
#include "w32ios.h"
#include "pgl.h"

WINE_DEFAULT_DEBUG_CHANNEL(wgl);

/* D2: pgl's flag values are Microsoft's */
_Static_assert( PGL_PFD_DOUBLEBUFFER == PFD_DOUBLEBUFFER, "PFD flags" );
_Static_assert( PGL_PFD_STEREO == PFD_STEREO, "PFD flags" );
_Static_assert( PGL_PFD_DRAW_TO_WINDOW == PFD_DRAW_TO_WINDOW, "PFD flags" );
_Static_assert( PGL_PFD_DRAW_TO_BITMAP == PFD_DRAW_TO_BITMAP, "PFD flags" );
_Static_assert( PGL_PFD_SUPPORT_GDI == PFD_SUPPORT_GDI, "PFD flags" );
_Static_assert( PGL_PFD_SUPPORT_OPENGL == PFD_SUPPORT_OPENGL, "PFD flags" );
_Static_assert( PGL_PFD_SUPPORT_COMPOSITION == PFD_SUPPORT_COMPOSITION, "PFD flags" );
/* D3.2: and the WGL attribute values are Khronos's */
_Static_assert( PGL_WGL_CONTEXT_MAJOR_VERSION == WGL_CONTEXT_MAJOR_VERSION_ARB, "WGL" );
_Static_assert( PGL_WGL_CONTEXT_MINOR_VERSION == WGL_CONTEXT_MINOR_VERSION_ARB, "WGL" );
_Static_assert( PGL_WGL_CONTEXT_FLAGS == WGL_CONTEXT_FLAGS_ARB, "WGL" );
_Static_assert( PGL_WGL_CONTEXT_PROFILE_MASK == WGL_CONTEXT_PROFILE_MASK_ARB, "WGL" );
_Static_assert( PGL_WGL_CONTEXT_OPENGL_NO_ERROR == WGL_CONTEXT_OPENGL_NO_ERROR_ARB, "WGL" );
_Static_assert( PGL_WGL_CONTEXT_ES2_PROFILE_BIT == WGL_CONTEXT_ES2_PROFILE_BIT_EXT, "WGL" );
_Static_assert( PGL_ERROR_INVALID_VERSION_ARB == ERROR_INVALID_VERSION_ARB, "WGL" );
_Static_assert( PGL_ERROR_INVALID_PROFILE_ARB == ERROR_INVALID_PROFILE_ARB, "WGL" );

/* ---------------------------------------------------------------- logging */

/* pgl's lines (already "[opengl] ..."), straight to Wine's debug output */
static void gl_log_line( const char *line )
{
    char buffer[640];
    snprintf( buffer, sizeof(buffer), "%s\n", line );
    __wine_dbg_output( buffer );
}

/* once per pseudo-process: a small ring of the PEBs that logged (a reused PEB may log again) */
static BOOL first_for_process( void **ring, LONG *next, unsigned int size )
{
    void *peb = ios_current_peb();
    unsigned int i;

    for (i = 0; i < size; i++) if (ring[i] == peb) return FALSE;
    ring[(unsigned int)InterlockedIncrement( next ) % size] = peb;
    return TRUE;
}

/* ---------------------------------------------------------------- objects */

struct ios_client_surface
{
    struct client_surface base;
    pgl_drawable *pgl;
};

struct ios_gl_drawable
{
    struct opengl_drawable base;
    pgl_drawable *pgl;
    struct pgl_pbuffer_texture texture;   /* pbuffers: WGL_ARB_render_texture (D4.2) */
};

static struct ios_client_surface *impl_from_client_surface( struct client_surface *client )
{
    return CONTAINING_RECORD( client, struct ios_client_surface, base );
}

static struct ios_gl_drawable *impl_from_opengl_drawable( struct opengl_drawable *base )
{
    return CONTAINING_RECORD( base, struct ios_gl_drawable, base );
}

/* D4.1: one colour attachment serves front and back */
static void map_buffers( struct opengl_drawable *base )
{
    static const GLenum front_back[] =
        { GL_FRONT_LEFT, GL_BACK_LEFT, GL_FRONT, GL_BACK, GL_LEFT, GL_FRONT_AND_BACK };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(base->buffer_map); i++) base->buffer_map[i] = GL_NONE;
    for (i = 0; i < ARRAY_SIZE(front_back); i++) base->buffer_map[front_back[i] - GL_FRONT_LEFT] = GL_COLOR_ATTACHMENT0;
}

static void client_size( HWND hwnd, int *width, int *height )
{
    RECT rect = { 0 };

    NtUserGetClientRect( hwnd, &rect, NtUserGetDpiForWindow( hwnd ) );
    *width = rect.right > 0 ? rect.right : 1;
    *height = rect.bottom > 0 ? rect.bottom : 1;
}

/* ---------------------------------------------------------------- client surface (D4.1) */

static void ios_client_surface_destroy( struct client_surface *client )
{
    struct ios_client_surface *surface = impl_from_client_surface( client );

    TRACE( "%s\n", debugstr_client_surface( client ) );
    pgl_drawable_stop_presenting( surface->pgl );   /* D5.1: release the Metal view */
    pgl_drawable_release( surface->pgl );
}

static void ios_client_surface_detach( struct client_surface *client )
{
    pgl_drawable_stop_presenting( impl_from_client_surface( client )->pgl );
}

static void ios_client_surface_update( struct client_surface *client )
{
    /* resizing happens at the next flush (GL_FLUSH_UPDATED) */
}

static void ios_client_surface_present( struct client_surface *client, HDC hdc )
{
    /* a GL drawable presents itself at swap */
}

static const struct client_surface_funcs ios_client_surface_funcs =
{
    .destroy = ios_client_surface_destroy,
    .detach = ios_client_surface_detach,
    .update = ios_client_surface_update,
    .present = ios_client_surface_present,
};

/* ---------------------------------------------------------------- drawables (D4) */

static void ios_drawable_destroy( struct opengl_drawable *base )
{
    TRACE( "%s\n", debugstr_opengl_drawable( base ) );
    pgl_drawable_release( impl_from_opengl_drawable( base )->pgl );
}

/* set_context(private) with that context current (win32u guarantees it), set_context(NULL) with
 * it still current */
static void ios_drawable_set_context( struct opengl_drawable *base, void *private )
{
    struct ios_gl_drawable *gl = impl_from_opengl_drawable( base );

    TRACE( "%s, private %p\n", debugstr_opengl_drawable( base ), private );
    if (private)
    {
        GLuint fbo = pgl_drawable_attach( gl->pgl );
        if (!fbo) ERR( "no framebuffer for %s\n", debugstr_opengl_drawable( base ) );
        base->draw_fbo = base->read_fbo = fbo;
    }
    else
    {
        pgl_drawable_detach( gl->pgl );
        base->draw_fbo = base->read_fbo = 0;
    }
}

static BOOL ios_drawable_swap( struct opengl_drawable *base )
{
    struct ios_gl_drawable *gl = impl_from_opengl_drawable( base );
    HWND hwnd = base->client ? base->client->hwnd : 0;

    TRACE( "%s\n", debugstr_opengl_drawable( base ) );
    return pgl_drawable_swap( gl->pgl, hwnd && is_window_visible( hwnd ) );
}

static void ios_drawable_flush( struct opengl_drawable *base, UINT flags )
{
    struct ios_gl_drawable *gl = impl_from_opengl_drawable( base );
    int width, height;

    TRACE( "%s, flags %#x\n", debugstr_opengl_drawable( base ), flags );
    if ((flags & GL_FLUSH_UPDATED) && base->client && base->client->hwnd)
    {
        client_size( base->client->hwnd, &width, &height );
        pgl_drawable_resize( gl->pgl, width, height );
    }
    if (flags & GL_FLUSH_INTERVAL) pgl_drawable_set_interval( gl->pgl, base->interval );
    /* front-buffer rendering on a single-buffered format, at glFlush or glFinish */
    if (flags & GL_FLUSH_PRESENT) ios_drawable_swap( base );
}

static const struct opengl_drawable_funcs ios_drawable_funcs =
{
    .destroy = ios_drawable_destroy,
    .flush = ios_drawable_flush,
    .swap = ios_drawable_swap,
    .set_context = ios_drawable_set_context,
};

static void ios_pbuffer_flush( struct opengl_drawable *base, UINT flags )
{
}

static BOOL ios_pbuffer_swap( struct opengl_drawable *base )
{
    return TRUE;
}

static const struct opengl_drawable_funcs ios_pbuffer_funcs =
{
    .destroy = ios_drawable_destroy,
    .flush = ios_pbuffer_flush,
    .swap = ios_pbuffer_swap,
    .set_context = ios_drawable_set_context,
};

/* D4.1: window drawables */
static BOOL ios_surface_create( HWND hwnd, int format, struct opengl_drawable **drawable )
{
    struct ios_client_surface *client;
    struct ios_gl_drawable *gl;
    unsigned int config, flag_set;
    pgl_drawable *pgl;
    int width, height;

    TRACE( "hwnd %p, format %d, drawable %p\n", hwnd, format, drawable );

    if (!pgl_format_split( format, &config, &flag_set )) return FALSE;
    client_size( hwnd, &width, &height );
    if (!(pgl = pgl_drawable_create( (UINT_PTR)hwnd, config, width, height, ios_current_peb() ))) return FALSE;

    if (!(client = client_surface_create( sizeof(*client), &ios_client_surface_funcs, hwnd )))
    {
        pgl_drawable_release( pgl );
        return FALSE;
    }
    client->pgl = pgl;   /* the reference pgl_drawable_create gave */
    gl = opengl_drawable_create( sizeof(*gl), &ios_drawable_funcs, format, &client->base );
    if (gl)
    {
        pgl_drawable_add_ref( pgl );
        gl->pgl = pgl;
        map_buffers( &gl->base );
    }
    client_surface_release( &client->base );
    if (!gl) return FALSE;

    TRACE( "created %s, %dx%d\n", debugstr_opengl_drawable( &gl->base ), width, height );
    *drawable = &gl->base;
    return TRUE;
}

/* D4.2: pbuffers */
static BOOL ios_pbuffer_create( HDC hdc, int format, BOOL largest, GLenum texture_format, GLenum texture_target,
                                GLint max_level, GLsizei *width, GLsizei *height, struct opengl_drawable **drawable )
{
    struct ios_gl_drawable *gl;
    unsigned int config, flag_set;
    pgl_drawable *pgl;

    TRACE( "hdc %p, format %d, largest %u, %dx%d\n", hdc, format, largest, *width, *height );

    if (!pgl_format_split( format, &config, &flag_set )) return FALSE;
    if (largest)
    {
        if (*width > PGL_PBUFFER_MAX) *width = PGL_PBUFFER_MAX;
        if (*height > PGL_PBUFFER_MAX) *height = PGL_PBUFFER_MAX;
    }
    else if (*width > PGL_PBUFFER_MAX || *height > PGL_PBUFFER_MAX) return FALSE;
    if (*width < 1) *width = 1;
    if (*height < 1) *height = 1;

    if (!(pgl = pgl_drawable_create( 0, config, *width, *height, ios_current_peb() ))) return FALSE;
    if (!(gl = opengl_drawable_create( sizeof(*gl), &ios_pbuffer_funcs, format, NULL )))
    {
        pgl_drawable_release( pgl );
        return FALSE;
    }
    gl->pgl = pgl;
    gl->texture.target = texture_target;
    gl->texture.format = texture_format;
    map_buffers( &gl->base );
    *drawable = &gl->base;
    return TRUE;
}

/* wglSetPbufferAttribARB: the face and level the next wglBindTexImageARB fills */
static BOOL ios_pbuffer_updated( HDC hdc, struct opengl_drawable *drawable, GLenum cube_face, GLint mipmap_level )
{
    struct ios_gl_drawable *gl = impl_from_opengl_drawable( drawable );

    gl->texture.cube_face = cube_face;
    gl->texture.level = mipmap_level;
    return TRUE;
}

/* clarification 2.6: the copy runs in the program's context, on the pbuffer's colour renderbuffer
 * (win32u's default path would make an internal context current on the pbuffer, whose FBO is
 * not shared); GL_NONE releases (nothing to undo) */
static UINT ios_pbuffer_bind( HDC hdc, struct opengl_drawable *drawable, GLenum buffer )
{
    struct ios_gl_drawable *gl = impl_from_opengl_drawable( drawable );

    TRACE( "%s, buffer %#x\n", debugstr_opengl_drawable( drawable ), buffer );
    return pgl_pbuffer_bind( gl->pgl, &gl->texture, buffer ) ? GL_TRUE : GL_FALSE;
}

/* ---------------------------------------------------------------- pixel formats (D2) */

static UINT ios_init_pixel_formats( UINT *onscreen_count )
{
    *onscreen_count = PGL_ONSCREEN_COUNT;
    return PGL_FORMAT_COUNT;
}

static BOOL ios_describe_pixel_format( int format, struct wgl_pixel_format *desc )
{
    PIXELFORMATDESCRIPTOR *pfd = &desc->pfd;
    struct pgl_pixel_format f;

    if (!pgl_describe_format( format, &f )) return FALSE;

    memset( desc, 0, sizeof(*desc) );
    pfd->nSize = sizeof(*pfd);
    pfd->nVersion = 1;
    pfd->dwFlags = f.flags;
    pfd->iPixelType = PFD_TYPE_RGBA;
    pfd->iLayerType = PFD_MAIN_PLANE;
    pfd->cColorBits = f.color_bits;
    pfd->cRedBits = f.red_bits;
    pfd->cRedShift = f.red_shift;
    pfd->cGreenBits = f.green_bits;
    pfd->cGreenShift = f.green_shift;
    pfd->cBlueBits = f.blue_bits;
    pfd->cBlueShift = f.blue_shift;
    pfd->cAlphaBits = f.alpha_bits;
    pfd->cAlphaShift = f.alpha_shift;
    pfd->cDepthBits = f.depth_bits;
    pfd->cStencilBits = f.stencil_bits;

    /* as upstream's describe_egl_config answers for a config with no transparency */
    desc->swap_method = WGL_SWAP_UNDEFINED_ARB;
    desc->transparent = GL_FALSE;
    desc->pixel_type = WGL_TYPE_RGBA_ARB;
    desc->draw_to_pbuffer = f.draw_to_pbuffer;
    desc->max_pbuffer_width = f.max_pbuffer_width;
    desc->max_pbuffer_height = f.max_pbuffer_height;
    desc->max_pbuffer_pixels = f.max_pbuffer_pixels;
    desc->transparent_red_value_valid = GL_TRUE;
    desc->transparent_green_value_valid = GL_TRUE;
    desc->transparent_blue_value_valid = GL_TRUE;
    desc->transparent_alpha_value_valid = GL_TRUE;
    desc->transparent_index_value_valid = GL_TRUE;
    desc->sample_buffers = 0;
    desc->samples = 0;
    desc->bind_to_texture_rgb = f.bind_to_texture_rgb;
    desc->bind_to_texture_rgba = f.bind_to_texture_rgba;
    desc->bind_to_texture_rectangle_rgb = f.bind_to_texture_rectangle_rgb;
    desc->bind_to_texture_rectangle_rgba = f.bind_to_texture_rectangle_rgba;
    desc->framebuffer_srgb_capable = GL_FALSE;
    desc->float_components = GL_FALSE;
    return TRUE;
}

static const char *ios_init_wgl_extensions( struct opengl_funcs *funcs )
{
    return "";   /* win32u registers the WGL extensions it implements itself */
}

/* ---------------------------------------------------------------- contexts (D3) */

static BOOL ios_context_create( int format, void *share, const int *attribs, void **context )
{
    static void *logged_version[16];
    static LONG logged_version_next;
    struct pgl_context_request req;

    switch (pgl_parse_context_attribs( attribs, &req ))
    {
    case PGL_ATTRIBS_BAD_PROFILE:
        WARN( "OpenGL ES profiles are not supported\n" );
        RtlSetLastWin32Error( ERROR_INVALID_PROFILE_ARB );
        return FALSE;
    case PGL_ATTRIBS_BAD_VERSION:
        if (first_for_process( logged_version, &logged_version_next, ARRAY_SIZE(logged_version) ))
        {
            char line[96];
            snprintf( line, sizeof(line), "[opengl] context %d.%d not available (2.1 max)", req.major, req.minor );
            gl_log_line( line );
        }
        RtlSetLastWin32Error( ERROR_INVALID_VERSION_ARB );
        return FALSE;
    case PGL_ATTRIBS_OK:
        break;
    }
    if (req.unknown) TRACE( "%d attributes ignored\n", req.unknown );

    *context = pgl_context_create( share, &req, ios_current_peb() );
    TRACE( "format %d, share %p -> %p\n", format, share, *context );
    return *context != NULL;
}

static BOOL ios_context_destroy( void *context )
{
    pgl_context_destroy( context );
    return TRUE;
}

static BOOL ios_make_current( struct opengl_drawable *draw, struct opengl_drawable *read, void *context )
{
    TRACE( "draw %s, read %s, context %p\n", debugstr_opengl_drawable( draw ), debugstr_opengl_drawable( read ), context );
    if (!context) return pgl_make_current( NULL, NULL );
    return pgl_make_current( context, draw ? impl_from_opengl_drawable( draw )->pgl : NULL );
}

/* ---------------------------------------------------------------- D6 */

static void *ios_get_proc_address( const char *name )
{
    return pgl_get_proc_address( name );
}

static const struct opengl_driver_funcs ios_gl_driver_funcs =
{
    .p_init_egl_platform = NULL,
    .p_get_proc_address = ios_get_proc_address,
    .p_init_pixel_formats = ios_init_pixel_formats,
    .p_describe_pixel_format = ios_describe_pixel_format,
    .p_init_wgl_extensions = ios_init_wgl_extensions,
    .p_surface_create = ios_surface_create,
    .p_context_create = ios_context_create,
    .p_context_destroy = ios_context_destroy,
    .p_make_current = ios_make_current,
    .p_pbuffer_create = ios_pbuffer_create,
    .p_pbuffer_updated = ios_pbuffer_updated,
    .p_pbuffer_bind = ios_pbuffer_bind,
};

/* ---------------------------------------------------------------- D1 */

static LONG driver_installed;

/* spec 35 B1: opengl32's unix side asks before it turns OpenGL on for a program */
BOOL w32ios_opengl_installed( void )
{
    return ReadNoFence( &driver_installed ) != 0;
}

/***********************************************************************
 *           w32ios_OpenGLInit   (the user driver's pOpenGLInit)
 *
 * win32u calls it once per app, from __wine_get_opengl_driver's pthread_once.
 */
UINT w32ios_OpenGLInit( UINT version, const struct opengl_funcs *opengl_funcs,
                        const struct opengl_driver_funcs **driver_funcs )
{
    const char *on = getenv( "PG_OPENGL" );

    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR( "version mismatch, win32u wants %u but the iOS driver has %u\n", version, WINE_OPENGL_DRIVER_VERSION );
        w32ios_log( "OpenGL driver version %u, expected %u: no OpenGL", version, WINE_OPENGL_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }
    /* Part B1: anything but 1, unset included, keeps today's behaviour (no driver) */
    if (!on || strcmp( on, "1" )) return STATUS_NOT_SUPPORTED;

    pgl_set_log( gl_log_line );
    if (pgl_init())
    {
        gl_log_line( "[opengl] pgl could not start (reason above): no OpenGL" );
        return STATUS_NOT_SUPPORTED;
    }

    *driver_funcs = &ios_gl_driver_funcs;
    InterlockedExchange( &driver_installed, 1 );
    TRACE( "iOS GL driver installed, renderer %s\n", debugstr_a( pgl_renderer() ) );
    return STATUS_SUCCESS;
}
