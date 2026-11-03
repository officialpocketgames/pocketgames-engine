// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 J4 and C3.1: two contexts sharing lists, current on two threads at once (a loading
// thread and a render thread). The loader uploads textures while the main thread draws; then each
// thread draws with a texture the other one made.

#include "gltest.h"

#define LOOPS 300

static struct gt_window loader_win;
static HANDLE uploaded, made_by_main, done;
static volatile LONG loader_texture, main_texture, loader_ok;

static void fill( GLuint tex, GLubyte r, GLubyte g, GLubyte b )
{
    GLubyte texels[4 * 4 * 4];
    int i;

    for (i = 0; i < 16; i++)
    {
        texels[4 * i] = r;
        texels[4 * i + 1] = g;
        texels[4 * i + 2] = b;
        texels[4 * i + 3] = 255;
    }
    glBindTexture( GL_TEXTURE_2D, tex );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels );
}

static void draw_textured( GLuint tex )
{
    glBindTexture( GL_TEXTURE_2D, tex );
    glEnable( GL_TEXTURE_2D );
    glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
    gt_quad( -1.0f, -1.0f, 1.0f, 1.0f, 0.0f );
    glDisable( GL_TEXTURE_2D );
}

static DWORD WINAPI loader( void *arg )
{
    GLuint tex = 0, old = 0;
    int i, ok = 1;

    (void)arg;
    if (!wglMakeCurrent( loader_win.hdc, loader_win.ctx ))
    {
        gt_check( "loader: wglMakeCurrent on the second thread", 0 );
        SetEvent( uploaded );
        return 1;
    }
    /* many uploads and deletions while the main thread draws */
    for (i = 0; i < LOOPS; i++)
    {
        glGenTextures( 1, &tex );
        fill( tex, (GLubyte)i, 0, 255 );
        if (old) glDeleteTextures( 1, &old );
        old = tex;
    }
    fill( tex, 0, 0, 255 );   /* the last one blue */
    glFinish();
    InterlockedExchange( &loader_texture, (LONG)tex );
    SetEvent( uploaded );

    /* draw with the main thread's texture in this thread's own window */
    WaitForSingleObject( made_by_main, INFINITE );
    gt_ortho( loader_win.width, loader_win.height );
    glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT );
    draw_textured( (GLuint)main_texture );
    ok = gt_pixel( "loader thread draws the main thread's texture", 32, 32, 255, 255, 0, 2 );
    ok &= gt_check( "loader thread: no GL error", glGetError() == GL_NO_ERROR );
    InterlockedExchange( &loader_ok, ok );
    glDeleteTextures( 1, &tex );
    wglMakeCurrent( NULL, NULL );
    SetEvent( done );
    return 0;
}

int main( void )
{
    struct gt_window win;
    HANDLE thread;
    GLuint tex;
    int i;

    gt_begin( "gl-threads" );
    /* the loader's hidden window first, then the main one (current here) */
    if (!gt_window( &loader_win, 64, 64, PFD_DOUBLEBUFFER, 0 )) return gt_end();
    wglMakeCurrent( NULL, NULL );
    if (!gt_window( &win, 128, 128, PFD_DOUBLEBUFFER, 1 )) return gt_end();
    gt_check( "wglShareLists before any object", wglShareLists( win.ctx, loader_win.ctx ) );

    uploaded = CreateEventA( NULL, TRUE, FALSE, NULL );
    made_by_main = CreateEventA( NULL, TRUE, FALSE, NULL );
    done = CreateEventA( NULL, TRUE, FALSE, NULL );
    thread = CreateThread( NULL, 0, loader, NULL, 0, NULL );
    gt_check( "CreateThread", thread != NULL );

    /* draw while the loader uploads */
    gt_ortho( 128, 128 );
    for (i = 0; i < LOOPS && WaitForSingleObject( uploaded, 0 ) == WAIT_TIMEOUT; i++)
    {
        glClearColor( 0.0f, (float)(i % 2), 0.0f, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
        glColor3f( 1.0f, 0.0f, 0.0f );
        gt_quad( -0.5f, -0.5f, 0.5f, 0.5f, 0.0f );
        if (!(i % 10)) SwapBuffers( win.hdc );
    }
    gt_info( "%d frames drawn while the loader worked", i );
    WaitForSingleObject( uploaded, INFINITE );

    /* the loader's texture, here */
    glClear( GL_COLOR_BUFFER_BIT );
    tex = (GLuint)loader_texture;
    gt_check( "the loader's texture is a texture here", tex && glIsTexture( tex ) );
    draw_textured( tex );
    gt_pixel( "main thread draws the loader's texture", 64, 64, 0, 0, 255, 2 );

    /* a texture of this thread's for the loader */
    glGenTextures( 1, &tex );
    fill( tex, 255, 255, 0 );
    glFinish();
    InterlockedExchange( &main_texture, (LONG)tex );
    SetEvent( made_by_main );
    gt_check( "the loader finished", WaitForSingleObject( done, 30000 ) == WAIT_OBJECT_0 );
    WaitForSingleObject( thread, 30000 );
    CloseHandle( thread );

    gt_check( "no GL error", glGetError() == GL_NO_ERROR );
    SwapBuffers( win.hdc );
    glDeleteTextures( 1, &tex );
    gt_close( &win );
    gt_close( &loader_win );
    return gt_end();
}
