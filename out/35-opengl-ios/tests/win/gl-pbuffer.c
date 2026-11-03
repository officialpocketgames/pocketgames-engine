// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 J4 and clarification 2.6: WGL_ARB_pbuffer with WGL_ARB_render_texture. A 64 x 64
// pbuffer with its own context (sharing lists with the window's) is drawn red on the left, green
// on the right; the window's context binds it to a texture with wglBindTexImageARB and draws it.

#include "gltest.h"

int main( void )
{
    static const int choose[] =
    {
        GT_WGL_DRAW_TO_PBUFFER_ARB, 1, GT_WGL_SUPPORT_OPENGL_ARB, 1, GT_WGL_PIXEL_TYPE_ARB, GT_WGL_TYPE_RGBA_ARB,
        GT_WGL_COLOR_BITS_ARB, 24, GT_WGL_ALPHA_BITS_ARB, 8, GT_WGL_BIND_TO_TEXTURE_RGBA_ARB, 1, 0
    };
    static const int create[] =
    {
        GT_WGL_TEXTURE_FORMAT_ARB, GT_WGL_TEXTURE_RGBA_ARB, GT_WGL_TEXTURE_TARGET_ARB, GT_WGL_TEXTURE_2D_ARB, 0
    };
    struct gt_window win;
    gt_wglChoosePixelFormatARB_t choose_format;
    gt_wglCreatePbufferARB_t create_pbuffer;
    gt_wglGetPbufferDCARB_t get_dc;
    gt_wglReleasePbufferDCARB_t release_dc;
    gt_wglDestroyPbufferARB_t destroy;
    gt_wglQueryPbufferARB_t query;
    gt_wglBindTexImageARB_t bind_tex;
    gt_wglReleaseTexImageARB_t release_tex;
    GT_HPBUFFERARB pbuffer;
    const char *ext;
    HGLRC pctx;
    HDC pdc;
    int format = 0, width = 0, height = 0;
    UINT count = 0;
    GLuint tex;

    gt_begin( "gl-pbuffer" );
    if (!gt_window( &win, 128, 128, PFD_DOUBLEBUFFER, 1 )) return gt_end();
    ext = gt_wgl_extensions( win.hdc );
    gt_check( "WGL_ARB_pbuffer advertised", gt_has_extension( ext, "WGL_ARB_pbuffer" ) );
    gt_check( "WGL_ARB_render_texture advertised", gt_has_extension( ext, "WGL_ARB_render_texture" ) );
    gt_check( "WGL_ARB_pixel_format advertised", gt_has_extension( ext, "WGL_ARB_pixel_format" ) );
    choose_format = (gt_wglChoosePixelFormatARB_t)(void *)gt_proc( "wglChoosePixelFormatARB" );
    create_pbuffer = (gt_wglCreatePbufferARB_t)(void *)gt_proc( "wglCreatePbufferARB" );
    get_dc = (gt_wglGetPbufferDCARB_t)(void *)gt_proc( "wglGetPbufferDCARB" );
    release_dc = (gt_wglReleasePbufferDCARB_t)(void *)gt_proc( "wglReleasePbufferDCARB" );
    destroy = (gt_wglDestroyPbufferARB_t)(void *)gt_proc( "wglDestroyPbufferARB" );
    query = (gt_wglQueryPbufferARB_t)(void *)gt_proc( "wglQueryPbufferARB" );
    bind_tex = (gt_wglBindTexImageARB_t)(void *)gt_proc( "wglBindTexImageARB" );
    release_tex = (gt_wglReleaseTexImageARB_t)(void *)gt_proc( "wglReleaseTexImageARB" );
    if (!choose_format || !create_pbuffer || !get_dc || !release_dc || !destroy || !query || !bind_tex || !release_tex)
    {
        gt_close( &win );
        return gt_end();
    }

    if (!gt_check( "wglChoosePixelFormatARB finds a pbuffer format that binds to RGBA textures",
                   choose_format( win.hdc, choose, NULL, 1, &format, &count ) && count >= 1 ))
    {
        gt_close( &win );
        return gt_end();
    }
    pbuffer = create_pbuffer( win.hdc, format, 64, 64, create );
    if (!gt_check( "wglCreatePbufferARB", pbuffer != NULL ))
    {
        gt_close( &win );
        return gt_end();
    }
    query( pbuffer, GT_WGL_PBUFFER_WIDTH_ARB, &width );
    query( pbuffer, GT_WGL_PBUFFER_HEIGHT_ARB, &height );
    gt_checkf( "wglQueryPbufferARB size", width == 64 && height == 64, "%d x %d", width, height );
    pdc = get_dc( pbuffer );
    pctx = pdc ? wglCreateContext( pdc ) : NULL;
    gt_check( "a context on the pbuffer's DC", pctx != NULL );
    /* before either context has objects: the pbuffer's context shares the window's lists */
    gt_check( "wglShareLists", pctx && wglShareLists( win.ctx, pctx ) );

    /* draw into the pbuffer and read it back there */
    if (pctx && wglMakeCurrent( pdc, pctx ))
    {
        gt_ortho( 64, 64 );
        glClearColor( 0.0f, 1.0f, 0.0f, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
        glColor3f( 1.0f, 0.0f, 0.0f );
        gt_quad( -1.0f, -1.0f, 0.0f, 1.0f, 0.0f );
        glFinish();
        gt_pixel( "pbuffer: left half red", 16, 32, 255, 0, 0, 2 );
        gt_pixel( "pbuffer: right half green", 48, 32, 0, 255, 0, 2 );
    }
    else gt_check( "wglMakeCurrent on the pbuffer", 0 );

    /* the window's context: the pbuffer as a texture */
    wglMakeCurrent( win.hdc, win.ctx );
    gt_ortho( 128, 128 );
    glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT );
    glGenTextures( 1, &tex );
    glBindTexture( GL_TEXTURE_2D, tex );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    gt_check( "wglBindTexImageARB", bind_tex( pbuffer, GT_WGL_FRONT_LEFT_ARB ) );
    glEnable( GL_TEXTURE_2D );
    glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
    gt_quad( -1.0f, -1.0f, 1.0f, 1.0f, 0.0f );
    glDisable( GL_TEXTURE_2D );
    gt_pixel( "texture from the pbuffer: left half red", 32, 64, 255, 0, 0, 2 );
    gt_pixel( "texture from the pbuffer: right half green", 96, 64, 0, 255, 0, 2 );
    gt_check( "wglReleaseTexImageARB", release_tex( pbuffer, GT_WGL_FRONT_LEFT_ARB ) );

    /* the window's own drawing still goes to the window */
    glColor3f( 0.0f, 0.0f, 1.0f );
    gt_quad( -0.25f, -0.25f, 0.25f, 0.25f, 0.0f );
    gt_pixel( "the window's framebuffer after the bind", 64, 64, 0, 0, 255, 2 );
    gt_check( "no GL error", glGetError() == GL_NO_ERROR );
    SwapBuffers( win.hdc );

    glDeleteTextures( 1, &tex );
    if (pctx) wglDeleteContext( pctx );
    if (pdc) release_dc( pbuffer, pdc );
    gt_check( "wglDestroyPbufferARB", destroy( pbuffer ) );
    gt_close( &win );
    return gt_end();
}
