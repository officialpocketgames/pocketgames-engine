// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 J4: OpenGL on a memory DC with a DIB section selected (PFD_DRAW_TO_BITMAP): the drawing
// is read back with glReadPixels and, after glFinish, from the DIB's own bits.

#include <stdlib.h>

#include "gltest.h"

#define W 64
#define H 64

/* the DIB is bottom-up (positive height): row 0 is the bottom row, as in GL; pixels are B, G, R, x */
static int dib_pixel( const char *what, const GLubyte *bits, int x, int y, int r, int g, int b )
{
    const GLubyte *p = bits + (y * W + x) * 4;

    return gt_checkf( what, abs( p[2] - r ) <= 2 && abs( p[1] - g ) <= 2 && abs( p[0] - b ) <= 2,
                      "DIB pixel (%d,%d) is (%u,%u,%u), expected (%d,%d,%d)", x, y, p[2], p[1], p[0], r, g, b );
}

int main( void )
{
    PIXELFORMATDESCRIPTOR pfd, got;
    BITMAPINFO info;
    void *bits = NULL;
    HBITMAP bitmap, old;
    HGLRC ctx;
    HDC dc;
    int format;

    gt_begin( "gl-bitmap" );
    dc = CreateCompatibleDC( NULL );
    memset( &info, 0, sizeof(info) );
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = W;
    info.bmiHeader.biHeight = H;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection( dc, &info, DIB_RGB_COLORS, &bits, NULL, 0 );
    if (!gt_check( "CreateDIBSection", bitmap && bits )) return gt_end();
    old = SelectObject( dc, bitmap );

    memset( &pfd, 0, sizeof(pfd) );
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_BITMAP | PFD_SUPPORT_OPENGL | PFD_SUPPORT_GDI;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    format = ChoosePixelFormat( dc, &pfd );
    if (!gt_check( "ChoosePixelFormat for a bitmap", format != 0 )) return gt_end();
    DescribePixelFormat( dc, format, sizeof(got), &got );
    gt_checkf( "the format draws to a bitmap", (got.dwFlags & PFD_DRAW_TO_BITMAP) != 0, "flags %#lx", (unsigned long)got.dwFlags );
    gt_check( "SetPixelFormat on the memory DC", SetPixelFormat( dc, format, &pfd ) );
    ctx = wglCreateContext( dc );
    if (!gt_check( "wglCreateContext on the memory DC", ctx != NULL ) ||
        !gt_check( "wglMakeCurrent on the memory DC", wglMakeCurrent( dc, ctx ) ))
        return gt_end();

    gt_ortho( W, H );
    glClearColor( 0.0f, 0.0f, 1.0f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT );
    glColor3f( 1.0f, 0.0f, 0.0f );
    gt_quad( -1.0f, -1.0f, 0.0f, 1.0f, 0.0f );
    glFinish();
    gt_pixel( "glReadPixels: left half red", 16, 32, 255, 0, 0, 2 );
    gt_pixel( "glReadPixels: right half blue", 48, 32, 0, 0, 255, 2 );
    GdiFlush();
    dib_pixel( "DIB bits: left half red", bits, 16, 32, 255, 0, 0 );
    dib_pixel( "DIB bits: right half blue", bits, 48, 32, 0, 0, 255 );
    gt_check( "no GL error", glGetError() == GL_NO_ERROR );

    wglMakeCurrent( NULL, NULL );
    wglDeleteContext( ctx );
    SelectObject( dc, old );
    DeleteObject( bitmap );
    DeleteDC( dc );
    return gt_end();
}
