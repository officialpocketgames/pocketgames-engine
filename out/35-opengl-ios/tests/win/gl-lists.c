// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 J4: display lists: compiled and called twice with different transforms, nested (a list
// calls another by name, so redefining the inner one changes the outer), compile-and-execute,
// glCallLists with a base, and deletion.

#include "gltest.h"

/* a 0.5-wide square at the origin in the list's colour */
static void square( float r, float g, float b )
{
    glColor3f( r, g, b );
    gt_quad( -0.25f, -0.25f, 0.25f, 0.25f, 0.0f );
}

int main( void )
{
    struct gt_window win;
    GLuint base;
    GLubyte ids[2];

    gt_begin( "gl-lists" );
    if (!gt_window( &win, 128, 128, PFD_DOUBLEBUFFER, 1 )) return gt_end();
    gt_ortho( 128, 128 );
    glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT );

    base = glGenLists( 3 );
    gt_check( "glGenLists", base != 0 );

    /* base: a red square */
    glNewList( base, GL_COMPILE );
    square( 1.0f, 0.0f, 0.0f );
    glEndList();
    gt_pixel( "GL_COMPILE draws nothing", 64, 64, 0, 0, 0, 0 );

    /* base + 1: base moved to the upper right (nested) */
    glNewList( base + 1, GL_COMPILE );
    glPushMatrix();
    glTranslatef( 0.5f, 0.5f, 0.0f );
    glCallList( base );
    glPopMatrix();
    glEndList();

    /* base + 2: a green square at the lower right, compiled and drawn at once */
    glNewList( base + 2, GL_COMPILE_AND_EXECUTE );
    glPushMatrix();
    glTranslatef( 0.5f, -0.5f, 0.0f );
    square( 0.0f, 1.0f, 0.0f );
    glPopMatrix();
    glEndList();
    gt_pixel( "GL_COMPILE_AND_EXECUTE draws", 96, 32, 0, 255, 0, 2 );

    /* the same list twice with two transforms */
    glClear( GL_COLOR_BUFFER_BIT );
    glPushMatrix();
    glTranslatef( -0.5f, -0.5f, 0.0f );
    glCallList( base );
    glPopMatrix();
    glCallList( base + 1 );
    gt_pixel( "list at the lower left", 32, 32, 255, 0, 0, 2 );
    gt_pixel( "nested list at the upper right", 96, 96, 255, 0, 0, 2 );
    gt_pixel( "nothing between", 64, 64, 0, 0, 0, 0 );

    /* redefine the inner list: the outer one draws the new content */
    glNewList( base, GL_COMPILE );
    square( 0.0f, 0.0f, 1.0f );
    glEndList();
    glClear( GL_COLOR_BUFFER_BIT );
    glCallList( base + 1 );
    gt_pixel( "nested list after the inner one changed", 96, 96, 0, 0, 255, 2 );

    /* glCallLists with a base: the two outer lists by offset */
    glClear( GL_COLOR_BUFFER_BIT );
    glListBase( base );
    ids[0] = 1;
    ids[1] = 2;
    glCallLists( 2, GL_UNSIGNED_BYTE, ids );
    glListBase( 0 );
    gt_pixel( "glCallLists: first", 96, 96, 0, 0, 255, 2 );
    gt_pixel( "glCallLists: second", 96, 32, 0, 255, 0, 2 );

    glDeleteLists( base, 3 );
    gt_check( "glDeleteLists", !glIsList( base ) && !glIsList( base + 2 ) );
    gt_check( "no GL error", glGetError() == GL_NO_ERROR );
    SwapBuffers( win.hdc );
    gt_close( &win );
    return gt_end();
}
