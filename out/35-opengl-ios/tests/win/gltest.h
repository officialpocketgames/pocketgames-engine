// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part J4: what the Windows test programs share. Each program draws with OpenGL 2.1 and
// prints one line per check:
//     PASS <program>: <check>
//     FAIL <program>: <check>: <what was wrong>
// then "RESULT <program>: <n> passed, <m> failed" and exits with 0 only when nothing failed.
// Pixels are checked with glReadPixels (origin bottom left). Built for i686 and x86-64 with
// llvm-mingw (build.sh); run on the device only, in a test build (never on the owner's PC).
//
// Constants and entry points beyond OpenGL 1.1 are declared here, with values from the Khronos
// registry (gl.xml and wgl.xml at OpenGL-Registry 0b449b97), so the programs need nothing but the
// toolchain's windows.h and GL/gl.h.

#ifndef GLTEST_H
#define GLTEST_H

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

/* ---- GL beyond 1.1 ---- */
#define GT_GL_ARRAY_BUFFER                 0x8892
#define GT_GL_ELEMENT_ARRAY_BUFFER         0x8893
#define GT_GL_STATIC_DRAW                  0x88E4
#define GT_GL_WRITE_ONLY                   0x88B9
#define GT_GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GT_GL_TEXTURE_MAX_LEVEL            0x813D
#define GT_GL_VERTEX_PROGRAM_ARB           0x8620
#define GT_GL_FRAGMENT_PROGRAM_ARB         0x8804
#define GT_GL_PROGRAM_FORMAT_ASCII_ARB     0x8875
#define GT_GL_PROGRAM_ERROR_POSITION_ARB   0x864B
#define GT_GL_VERTEX_SHADER                0x8B31
#define GT_GL_FRAGMENT_SHADER              0x8B30
#define GT_GL_COMPILE_STATUS               0x8B81
#define GT_GL_LINK_STATUS                  0x8B82
#define GT_GL_SHADING_LANGUAGE_VERSION     0x8B8C

/* ---- WGL extensions ---- */
#define GT_WGL_SUPPORT_OPENGL_ARB          0x2010
#define GT_WGL_DOUBLE_BUFFER_ARB           0x2011
#define GT_WGL_PIXEL_TYPE_ARB              0x2013
#define GT_WGL_COLOR_BITS_ARB              0x2014
#define GT_WGL_ALPHA_BITS_ARB              0x201B
#define GT_WGL_DEPTH_BITS_ARB              0x2022
#define GT_WGL_TYPE_RGBA_ARB               0x202B
#define GT_WGL_DRAW_TO_PBUFFER_ARB         0x202D
#define GT_WGL_PBUFFER_WIDTH_ARB           0x2034
#define GT_WGL_PBUFFER_HEIGHT_ARB          0x2035
#define GT_WGL_BIND_TO_TEXTURE_RGBA_ARB    0x2071
#define GT_WGL_TEXTURE_FORMAT_ARB          0x2072
#define GT_WGL_TEXTURE_TARGET_ARB          0x2073
#define GT_WGL_TEXTURE_RGBA_ARB            0x2076
#define GT_WGL_TEXTURE_2D_ARB              0x207A
#define GT_WGL_FRONT_LEFT_ARB              0x2083
#define GT_WGL_CONTEXT_MAJOR_VERSION_ARB   0x2091
#define GT_WGL_CONTEXT_MINOR_VERSION_ARB   0x2092
#define GT_WGL_CONTEXT_PROFILE_MASK_ARB    0x9126
#define GT_WGL_CONTEXT_CORE_PROFILE_BIT    0x0001
#define GT_WGL_CONTEXT_ES2_PROFILE_BIT     0x0004
#define GT_ERROR_INVALID_VERSION_ARB       0x2095
#define GT_ERROR_INVALID_PROFILE_ARB       0x2096

DECLARE_HANDLE( GT_HPBUFFERARB );

typedef const char *(WINAPI *gt_wglGetExtensionsStringARB_t)( HDC );
typedef BOOL (WINAPI *gt_wglChoosePixelFormatARB_t)( HDC, const int *, const FLOAT *, UINT, int *, UINT * );
typedef GT_HPBUFFERARB (WINAPI *gt_wglCreatePbufferARB_t)( HDC, int, int, int, const int * );
typedef HDC (WINAPI *gt_wglGetPbufferDCARB_t)( GT_HPBUFFERARB );
typedef int (WINAPI *gt_wglReleasePbufferDCARB_t)( GT_HPBUFFERARB, HDC );
typedef BOOL (WINAPI *gt_wglDestroyPbufferARB_t)( GT_HPBUFFERARB );
typedef BOOL (WINAPI *gt_wglQueryPbufferARB_t)( GT_HPBUFFERARB, int, int * );
typedef BOOL (WINAPI *gt_wglBindTexImageARB_t)( GT_HPBUFFERARB, int );
typedef BOOL (WINAPI *gt_wglReleaseTexImageARB_t)( GT_HPBUFFERARB, int );
typedef HGLRC (WINAPI *gt_wglCreateContextAttribsARB_t)( HDC, HGLRC, const int * );
typedef BOOL (WINAPI *gt_wglSwapIntervalEXT_t)( int );
typedef int (WINAPI *gt_wglGetSwapIntervalEXT_t)( void );

/* ---- the harness ---- */

struct gt_window
{
    HWND hwnd;
    HDC hdc;
    HGLRC ctx;
    int width, height;     /* client size */
};

/* starts the program's output */
void gt_begin( const char *program );

/* A window with a client area of width x height, a pixel format with PFD_SUPPORT_OPENGL,
 * PFD_DRAW_TO_WINDOW and flags (PFD_DOUBLEBUFFER or 0), 24-bit colour, 8 alpha, 24 depth and
 * 8 stencil bits, and a context made current on it. FAIL line and 0 on failure. */
int gt_window( struct gt_window *win, int width, int height, DWORD flags, int visible );
void gt_close( struct gt_window *win );
/* handles the window messages waiting on this thread */
void gt_pump( void );

/* wglGetProcAddress, with a FAIL line when the context does not have it */
PROC gt_proc( const char *name );
/* the WGL extension string of hdc (WGL_ARB_extensions_string), "" without one */
const char *gt_wgl_extensions( HDC hdc );
/* whether the space-separated list holds the name as a whole word */
int gt_has_extension( const char *list, const char *name );

/* a PASS or FAIL line; returns ok */
int gt_check( const char *what, int ok );
/* the same with a printf-style explanation on failure */
int gt_checkf( const char *what, int ok, const char *fmt, ... ) __attribute__((format(printf, 3, 4)));
/* reads the pixel (x, y) of the read buffer and compares each channel within tolerance */
int gt_pixel( const char *what, int x, int y, int r, int g, int b, int tolerance );
/* an INFO line (not a check) */
void gt_info( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));

/* viewport on the whole width x height, identity modelview, orthographic -1..1 on all axes */
void gt_ortho( int width, int height );
/* an axis-aligned quad in the current coordinates, texture coordinates 0..1, at depth z */
void gt_quad( float x0, float y0, float x1, float y1, float z );
/* the pixel at the centre of the normalized-coordinate point (nx, ny) of a width x height view */
int gt_px( float nx, int width );

/* the summary line; the exit code (0: everything passed) */
int gt_end( void );

#endif /* GLTEST_H */
