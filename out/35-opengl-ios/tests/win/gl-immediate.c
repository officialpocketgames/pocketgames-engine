// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 J4: immediate mode with lighting, fog and the texture environment. Each part draws in
// its own quarter of a 128 x 128 window and is read back before the swap.

#include "gltest.h"

int main( void )
{
    static const GLfloat zero[4] = { 0.0f, 0.0f, 0.0f, 1.0f }, white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const GLfloat along_z[4] = { 0.0f, 0.0f, 1.0f, 0.0f }, material[4] = { 0.5f, 0.25f, 1.0f, 1.0f };
    static const GLfloat fog_colour[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    static const GLubyte texels[2 * 2 * 4] = { 255, 128, 0, 255, 255, 128, 0, 255, 255, 128, 0, 255, 255, 128, 0, 255 };
    struct gt_window win;
    GLuint tex;

    gt_begin( "gl-immediate" );
    if (!gt_window( &win, 128, 128, PFD_DOUBLEBUFFER, 1 )) return gt_end();
    gt_ortho( 128, 128 );
    glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

    /* a plain coloured quad: lower left */
    glColor3f( 0.0f, 1.0f, 0.0f );
    gt_quad( -1.0f, -1.0f, 0.0f, 0.0f, 0.0f );
    gt_pixel( "glBegin/glColor", 32, 32, 0, 255, 0, 2 );

    /* lighting: one directional light along +z, diffuse only, no ambient: the material's
     * diffuse colour (lower right) */
    glEnable( GL_LIGHTING );
    glEnable( GL_LIGHT0 );
    glLightModelfv( GL_LIGHT_MODEL_AMBIENT, zero );
    glLightfv( GL_LIGHT0, GL_AMBIENT, zero );
    glLightfv( GL_LIGHT0, GL_DIFFUSE, white );
    glLightfv( GL_LIGHT0, GL_SPECULAR, zero );
    glLightfv( GL_LIGHT0, GL_POSITION, along_z );
    glMaterialfv( GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, material );
    glMaterialfv( GL_FRONT_AND_BACK, GL_SPECULAR, zero );
    glMaterialfv( GL_FRONT_AND_BACK, GL_EMISSION, zero );
    glNormal3f( 0.0f, 0.0f, 1.0f );
    gt_quad( 0.0f, -1.0f, 1.0f, 0.0f, 0.0f );
    glDisable( GL_LIGHTING );
    gt_pixel( "lighting (directional, diffuse)", 96, 32, 128, 64, 255, 3 );

    /* linear fog from 0 to 1, the quad 0.5 from the eye: half red, half the blue fog (upper left) */
    glEnable( GL_FOG );
    glFogi( GL_FOG_MODE, GL_LINEAR );
    glFogf( GL_FOG_START, 0.0f );
    glFogf( GL_FOG_END, 1.0f );
    glFogfv( GL_FOG_COLOR, fog_colour );
    glHint( GL_FOG_HINT, GL_NICEST );
    glColor3f( 1.0f, 0.0f, 0.0f );
    gt_quad( -1.0f, 0.0f, 0.0f, 1.0f, -0.5f );
    glDisable( GL_FOG );
    gt_pixel( "linear fog", 32, 96, 128, 0, 128, 4 );

    /* texture environment (upper right): GL_MODULATE with the colour, then GL_REPLACE */
    glGenTextures( 1, &tex );
    glBindTexture( GL_TEXTURE_2D, tex );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels );
    glEnable( GL_TEXTURE_2D );
    glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
    glColor3f( 0.5f, 1.0f, 1.0f );
    gt_quad( 0.0f, 0.0f, 0.5f, 1.0f, 0.0f );
    glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
    gt_quad( 0.5f, 0.0f, 1.0f, 1.0f, 0.0f );
    glDisable( GL_TEXTURE_2D );
    gt_pixel( "texture environment GL_MODULATE", 80, 96, 128, 128, 0, 3 );
    gt_pixel( "texture environment GL_REPLACE", 112, 96, 255, 128, 0, 2 );

    gt_check( "no GL error", glGetError() == GL_NO_ERROR );
    SwapBuffers( win.hdc );
    glDeleteTextures( 1, &tex );
    gt_close( &win );
    return gt_end();
}
