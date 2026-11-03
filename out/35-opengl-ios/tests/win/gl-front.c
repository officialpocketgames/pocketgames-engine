// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 J4: single-buffered rendering to the front buffer, finished with glFlush (no swap).

#include "gltest.h"

int main( void )
{
    PIXELFORMATDESCRIPTOR got;
    struct gt_window win;
    GLint buffer = 0;

    gt_begin( "gl-front" );
    if (!gt_window( &win, 128, 128, 0, 1 )) return gt_end();
    DescribePixelFormat( win.hdc, GetPixelFormat( win.hdc ), sizeof(got), &got );
    if (got.dwFlags & PFD_DOUBLEBUFFER) gt_info( "ChoosePixelFormat gave a double-buffered format" );
    else gt_check( "a single-buffered format", 1 );
    glGetIntegerv( GL_DRAW_BUFFER, &buffer );
    if (!(got.dwFlags & PFD_DOUBLEBUFFER))
        gt_checkf( "single-buffered: GL_DRAW_BUFFER starts as GL_FRONT", buffer == GL_FRONT, "%#x", buffer );

    glDrawBuffer( GL_FRONT );
    glReadBuffer( GL_FRONT );
    glGetIntegerv( GL_DRAW_BUFFER, &buffer );
    gt_checkf( "glDrawBuffer(GL_FRONT)", buffer == GL_FRONT, "%#x", buffer );
    gt_ortho( 128, 128 );
    glClearColor( 0.0f, 1.0f, 0.0f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT );
    glColor3f( 1.0f, 0.0f, 0.0f );
    gt_quad( -1.0f, -1.0f, 0.0f, 0.0f, 0.0f );
    glFlush();
    gt_pixel( "front buffer: the quad", 32, 32, 255, 0, 0, 2 );
    gt_pixel( "front buffer: the clear colour", 96, 96, 0, 255, 0, 2 );

    /* more drawing after the flush lands too */
    glColor3f( 0.0f, 0.0f, 1.0f );
    gt_quad( 0.0f, 0.0f, 1.0f, 1.0f, 0.0f );
    glFlush();
    gt_pixel( "front buffer: drawn after the first glFlush", 96, 96, 0, 0, 255, 2 );
    gt_check( "no GL error", glGetError() == GL_NO_ERROR );
    Sleep( 100 );   /* leave it on screen a moment */
    gt_close( &win );
    return gt_end();
}
