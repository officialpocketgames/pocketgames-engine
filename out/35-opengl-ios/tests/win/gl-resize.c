// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 J4: a window resized while it has a context (larger, then smaller: the drawable follows
// the client area), and WGL_EXT_swap_control (the interval is set and read back; the time per
// swap with each interval is printed as INFO, since the app's frame-rate setting also paces).

#include "gltest.h"

static void resize( struct gt_window *win, int width, int height )
{
    RECT rect = { 0, 0, width, height };

    AdjustWindowRect( &rect, WS_OVERLAPPEDWINDOW, FALSE );
    SetWindowPos( win->hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER );
    gt_pump();
    GetClientRect( win->hwnd, &rect );
    win->width = rect.right;
    win->height = rect.bottom;
    SwapBuffers( win->hdc );   /* a swap: the drawable takes the new size, as on Windows */
}

static double ms_per_swap( HDC hdc, int swaps )
{
    LARGE_INTEGER freq, start, end;
    int i;

    QueryPerformanceFrequency( &freq );
    QueryPerformanceCounter( &start );
    for (i = 0; i < swaps; i++)
    {
        glClearColor( (float)(i & 1), 0.0f, 0.0f, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
        SwapBuffers( hdc );
    }
    glFinish();
    QueryPerformanceCounter( &end );
    return (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart / swaps;
}

int main( void )
{
    struct gt_window win;
    gt_wglSwapIntervalEXT_t set_interval;
    gt_wglGetSwapIntervalEXT_t get_interval;
    const char *wgl_ext;
    GLint viewport[4];

    gt_begin( "gl-resize" );
    if (!gt_window( &win, 128, 128, PFD_DOUBLEBUFFER, 1 )) return gt_end();

    /* larger */
    resize( &win, 200, 150 );
    gt_checkf( "client area 200 x 150", win.width == 200 && win.height == 150, "%d x %d", win.width, win.height );
    glViewport( 0, 0, win.width, win.height );
    glGetIntegerv( GL_VIEWPORT, viewport );
    gt_check( "GL_VIEWPORT follows glViewport", viewport[2] == 200 && viewport[3] == 150 );
    glClearColor( 0.0f, 0.0f, 1.0f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT );
    gt_pixel( "after growing: the far corner", 199, 149, 0, 0, 255, 2 );
    gt_pixel( "after growing: outside the old size", 160, 140, 0, 0, 255, 2 );
    SwapBuffers( win.hdc );

    /* smaller */
    resize( &win, 64, 48 );
    glViewport( 0, 0, win.width, win.height );
    glClearColor( 1.0f, 0.0f, 0.0f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT );
    gt_ortho( win.width, win.height );
    glColor3f( 0.0f, 1.0f, 0.0f );
    gt_quad( 0.0f, 0.0f, 1.0f, 1.0f, 0.0f );
    gt_pixel( "after shrinking: lower left", 8, 8, 255, 0, 0, 2 );
    gt_pixel( "after shrinking: upper right corner", 63, 47, 0, 255, 0, 2 );
    SwapBuffers( win.hdc );

    /* WGL_EXT_swap_control */
    wgl_ext = gt_wgl_extensions( win.hdc );
    gt_check( "WGL_EXT_swap_control advertised", gt_has_extension( wgl_ext, "WGL_EXT_swap_control" ) );
    set_interval = (gt_wglSwapIntervalEXT_t)(void *)gt_proc( "wglSwapIntervalEXT" );
    get_interval = (gt_wglGetSwapIntervalEXT_t)(void *)gt_proc( "wglGetSwapIntervalEXT" );
    if (set_interval && get_interval)
    {
        gt_check( "wglSwapIntervalEXT(1)", set_interval( 1 ) );
        gt_check( "wglGetSwapIntervalEXT is 1", get_interval() == 1 );
        gt_info( "interval 1: %.2f ms per swap", ms_per_swap( win.hdc, 30 ) );
        gt_check( "wglSwapIntervalEXT(0)", set_interval( 0 ) );
        gt_check( "wglGetSwapIntervalEXT is 0", get_interval() == 0 );
        gt_info( "interval 0: %.2f ms per swap", ms_per_swap( win.hdc, 30 ) );
        gt_check( "wglSwapIntervalEXT(-1) refused without WGL_EXT_swap_control_tear",
                  gt_has_extension( wgl_ext, "WGL_EXT_swap_control_tear" ) || !set_interval( -1 ) );
    }
    gt_check( "no GL error", glGetError() == GL_NO_ERROR );
    gt_close( &win );
    return gt_end();
}
