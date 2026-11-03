// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 J4, C3.4 and D3.2: the strings a program reads, and wglCreateContextAttribsARB: 2.1
// works, 3.3 core and 3.0 fail cleanly with ERROR_INVALID_VERSION_ARB, an ES2 profile with
// ERROR_INVALID_PROFILE_ARB.

#include "gltest.h"

/* Wine sets the WGL error code itself; Windows drivers report it with the 0xC007 facility */
static int last_error_is( DWORD code )
{
    DWORD err = GetLastError();
    return err == code || err == (0xC0070000 | code);
}

static int names_a_library( const char *s )
{
    static const char *const names[] = { "gl4es", "GL4ES", "ANGLE", "Angle", "MoltenVK", "Mesa", "wrapper" };
    unsigned int i;

    for (i = 0; s && i < sizeof(names) / sizeof(names[0]); i++) if (strstr( s, names[i] )) return 1;
    return 0;
}

int main( void )
{
    static const int core33[] = { GT_WGL_CONTEXT_MAJOR_VERSION_ARB, 3, GT_WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                                  GT_WGL_CONTEXT_PROFILE_MASK_ARB, GT_WGL_CONTEXT_CORE_PROFILE_BIT, 0 };
    static const int gl30[] = { GT_WGL_CONTEXT_MAJOR_VERSION_ARB, 3, GT_WGL_CONTEXT_MINOR_VERSION_ARB, 0, 0 };
    static const int gl21[] = { GT_WGL_CONTEXT_MAJOR_VERSION_ARB, 2, GT_WGL_CONTEXT_MINOR_VERSION_ARB, 1, 0 };
    static const int es2[] = { GT_WGL_CONTEXT_MAJOR_VERSION_ARB, 2, GT_WGL_CONTEXT_MINOR_VERSION_ARB, 0,
                               GT_WGL_CONTEXT_PROFILE_MASK_ARB, GT_WGL_CONTEXT_ES2_PROFILE_BIT, 0 };
    struct gt_window win;
    gt_wglCreateContextAttribsARB_t create;
    const char *vendor, *renderer, *version, *glsl, *ext;
    HGLRC ctx;

    gt_begin( "gl-context" );
    if (!gt_window( &win, 64, 64, PFD_DOUBLEBUFFER, 1 )) return gt_end();

    /* C3.4 */
    vendor = (const char *)glGetString( GL_VENDOR );
    renderer = (const char *)glGetString( GL_RENDERER );
    version = (const char *)glGetString( GL_VERSION );
    glsl = (const char *)glGetString( GT_GL_SHADING_LANGUAGE_VERSION );
    ext = (const char *)glGetString( GL_EXTENSIONS );
    gt_info( "GL_VENDOR \"%s\", GL_RENDERER \"%s\", GL_VERSION \"%s\", GLSL \"%s\"", vendor ? vendor : "(null)",
             renderer ? renderer : "(null)", version ? version : "(null)", glsl ? glsl : "(null)" );
    gt_check( "GL_VENDOR is Apple", vendor && !strcmp( vendor, "Apple" ) );
    gt_check( "GL_RENDERER is set", renderer && renderer[0] );
    gt_check( "GL_VERSION is 2.1.0", version && !strcmp( version, "2.1.0" ) );
    gt_check( "GL_SHADING_LANGUAGE_VERSION is 1.20", glsl && !strcmp( glsl, "1.20" ) );
    gt_check( "no library named in the strings", !names_a_library( vendor ) && !names_a_library( renderer ) &&
              !names_a_library( version ) && !names_a_library( glsl ) && !names_a_library( ext ) );
    gt_checkf( "the extension string fits a 4,096-byte buffer", ext && strlen( ext ) < 4000, "%u bytes",
               ext ? (unsigned int)strlen( ext ) : 0u );
    gt_check( "GL_ARB_vertex_buffer_object advertised", gt_has_extension( ext, "GL_ARB_vertex_buffer_object" ) );
    gt_check( "WGL_ARB_create_context advertised", gt_has_extension( gt_wgl_extensions( win.hdc ), "WGL_ARB_create_context" ) );

    /* D3.2 */
    create = (gt_wglCreateContextAttribsARB_t)(void *)gt_proc( "wglCreateContextAttribsARB" );
    if (create)
    {
        SetLastError( 0 );
        ctx = create( win.hdc, NULL, core33 );
        gt_checkf( "3.3 core fails with ERROR_INVALID_VERSION_ARB", !ctx && last_error_is( GT_ERROR_INVALID_VERSION_ARB ),
                   "context %p, error %#lx", (void *)ctx, (unsigned long)GetLastError() );
        if (ctx) wglDeleteContext( ctx );

        SetLastError( 0 );
        ctx = create( win.hdc, NULL, gl30 );
        gt_checkf( "3.0 fails with ERROR_INVALID_VERSION_ARB", !ctx && last_error_is( GT_ERROR_INVALID_VERSION_ARB ),
                   "context %p, error %#lx", (void *)ctx, (unsigned long)GetLastError() );
        if (ctx) wglDeleteContext( ctx );

        SetLastError( 0 );
        ctx = create( win.hdc, NULL, es2 );
        gt_checkf( "an ES2 profile fails with ERROR_INVALID_PROFILE_ARB", !ctx && last_error_is( GT_ERROR_INVALID_PROFILE_ARB ),
                   "context %p, error %#lx", (void *)ctx, (unsigned long)GetLastError() );
        if (ctx) wglDeleteContext( ctx );

        ctx = create( win.hdc, win.ctx, gl21 );
        if (gt_check( "2.1 sharing with the first context", ctx != NULL ))
        {
            gt_check( "wglMakeCurrent with the 2.1 context", wglMakeCurrent( win.hdc, ctx ) );
            version = (const char *)glGetString( GL_VERSION );
            gt_check( "the 2.1 context's GL_VERSION", version && !strncmp( version, "2.1", 3 ) );
            glViewport( 0, 0, 64, 64 );
            glClearColor( 1.0f, 0.0f, 1.0f, 1.0f );
            glClear( GL_COLOR_BUFFER_BIT );
            gt_pixel( "the 2.1 context draws", 32, 32, 255, 0, 255, 2 );
            wglMakeCurrent( win.hdc, win.ctx );
            wglDeleteContext( ctx );
        }
    }
    gt_check( "no GL error", glGetError() == GL_NO_ERROR );
    gt_close( &win );
    return gt_end();
}
