// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part J4: the harness of the Windows test programs (gltest.h).

#include <stdarg.h>
#include <stdlib.h>

#include "gltest.h"

static const char *program = "gltest";
static LONG passed, failed;
static CRITICAL_SECTION output;

void gt_begin( const char *name )
{
    program = name;
    InitializeCriticalSection( &output );
    printf( "START %s (%d-bit)\n", program, (int)(sizeof(void *) * 8) );
    fflush( stdout );
}

static void line( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));
static void line( const char *fmt, ... )
{
    va_list args;

    EnterCriticalSection( &output );
    va_start( args, fmt );
    vprintf( fmt, args );
    va_end( args );
    fflush( stdout );
    LeaveCriticalSection( &output );
}

int gt_check( const char *what, int ok )
{
    if (ok)
    {
        InterlockedIncrement( &passed );
        line( "PASS %s: %s\n", program, what );
    }
    else
    {
        InterlockedIncrement( &failed );
        line( "FAIL %s: %s\n", program, what );
    }
    return ok;
}

int gt_checkf( const char *what, int ok, const char *fmt, ... )
{
    char why[256];
    va_list args;

    if (ok) return gt_check( what, 1 );
    va_start( args, fmt );
    vsnprintf( why, sizeof(why), fmt, args );
    va_end( args );
    InterlockedIncrement( &failed );
    line( "FAIL %s: %s: %s\n", program, what, why );
    return 0;
}

void gt_info( const char *fmt, ... )
{
    char text[256];
    va_list args;

    va_start( args, fmt );
    vsnprintf( text, sizeof(text), fmt, args );
    va_end( args );
    line( "INFO %s: %s\n", program, text );
}

int gt_pixel( const char *what, int x, int y, int r, int g, int b, int tolerance )
{
    GLubyte px[4] = { 0, 0, 0, 0 };

    glPixelStorei( GL_PACK_ALIGNMENT, 1 );
    glReadPixels( x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px );
    return gt_checkf( what, abs( px[0] - r ) <= tolerance && abs( px[1] - g ) <= tolerance && abs( px[2] - b ) <= tolerance,
                      "pixel (%d,%d) is (%u,%u,%u), expected (%d,%d,%d) +-%d",
                      x, y, px[0], px[1], px[2], r, g, b, tolerance );
}

static LRESULT CALLBACK wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_CLOSE) return 0;   /* the program decides when its windows go */
    return DefWindowProcA( hwnd, msg, wparam, lparam );
}

void gt_pump( void )
{
    MSG msg;

    while (PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE ))
    {
        TranslateMessage( &msg );
        DispatchMessageA( &msg );
    }
}

int gt_window( struct gt_window *win, int width, int height, DWORD flags, int visible )
{
    static LONG registered;
    PIXELFORMATDESCRIPTOR pfd;
    RECT rect = { 0, 0, width, height };
    int format;

    memset( win, 0, sizeof(*win) );
    if (!InterlockedExchange( &registered, 1 ))
    {
        WNDCLASSA wc;

        memset( &wc, 0, sizeof(wc) );
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleA( NULL );
        wc.hCursor = LoadCursorA( NULL, (LPCSTR)IDC_ARROW );
        wc.lpszClassName = "gltest";
        if (!RegisterClassA( &wc )) return gt_check( "RegisterClass", 0 );
    }
    AdjustWindowRect( &rect, WS_OVERLAPPEDWINDOW, FALSE );
    win->hwnd = CreateWindowA( "gltest", program, WS_OVERLAPPEDWINDOW, 0, 0, rect.right - rect.left,
                               rect.bottom - rect.top, NULL, NULL, GetModuleHandleA( NULL ), NULL );
    if (!win->hwnd) return gt_check( "CreateWindow", 0 );
    if (visible) ShowWindow( win->hwnd, SW_SHOW );
    gt_pump();
    win->hdc = GetDC( win->hwnd );
    win->width = width;
    win->height = height;

    memset( &pfd, 0, sizeof(pfd) );
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | flags;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    if (!(format = ChoosePixelFormat( win->hdc, &pfd ))) return gt_check( "ChoosePixelFormat", 0 );
    if (!SetPixelFormat( win->hdc, format, &pfd )) return gt_check( "SetPixelFormat", 0 );
    if (!(win->ctx = wglCreateContext( win->hdc ))) return gt_check( "wglCreateContext", 0 );
    if (!wglMakeCurrent( win->hdc, win->ctx )) return gt_check( "wglMakeCurrent", 0 );
    return 1;
}

void gt_close( struct gt_window *win )
{
    if (win->ctx)
    {
        if (wglGetCurrentContext() == win->ctx) wglMakeCurrent( NULL, NULL );
        wglDeleteContext( win->ctx );
    }
    if (win->hdc) ReleaseDC( win->hwnd, win->hdc );
    if (win->hwnd) DestroyWindow( win->hwnd );
    gt_pump();
    memset( win, 0, sizeof(*win) );
}

PROC gt_proc( const char *name )
{
    PROC proc = wglGetProcAddress( name );

    if (!proc) gt_checkf( "wglGetProcAddress", 0, "%s is missing", name );
    return proc;
}

const char *gt_wgl_extensions( HDC hdc )
{
    gt_wglGetExtensionsStringARB_t get = (gt_wglGetExtensionsStringARB_t)(void *)wglGetProcAddress( "wglGetExtensionsStringARB" );
    const char *list = get ? get( hdc ) : NULL;

    return list ? list : "";
}

int gt_has_extension( const char *list, const char *name )
{
    size_t len = strlen( name );
    const char *p = list;

    while (p && (p = strstr( p, name )))
    {
        if ((p == list || p[-1] == ' ') && (p[len] == ' ' || !p[len])) return 1;
        p += len;
    }
    return 0;
}

void gt_ortho( int width, int height )
{
    glViewport( 0, 0, width, height );
    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();
    glOrtho( -1.0, 1.0, -1.0, 1.0, -1.0, 1.0 );
    glMatrixMode( GL_MODELVIEW );
    glLoadIdentity();
}

void gt_quad( float x0, float y0, float x1, float y1, float z )
{
    glBegin( GL_QUADS );
    glTexCoord2f( 0.0f, 0.0f ); glVertex3f( x0, y0, z );
    glTexCoord2f( 1.0f, 0.0f ); glVertex3f( x1, y0, z );
    glTexCoord2f( 1.0f, 1.0f ); glVertex3f( x1, y1, z );
    glTexCoord2f( 0.0f, 1.0f ); glVertex3f( x0, y1, z );
    glEnd();
}

int gt_px( float nx, int width )
{
    return (int)((nx + 1.0f) * 0.5f * (float)width);
}

int gt_end( void )
{
    line( "RESULT %s: %ld passed, %ld failed\n", program, (long)passed, (long)failed );
    return failed || !passed ? 1 : 0;
}
