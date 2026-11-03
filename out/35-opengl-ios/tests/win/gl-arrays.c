// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 J4: client vertex arrays (GL_QUADS), vertex buffer objects with offsets into the array
// and element buffers (Part G1's buffer-offset rule in a 32-bit program), glMapBuffer, and an S3TC
// (DXT1) texture with its mipmaps, each level a different colour.

#include <stddef.h>

#include "gltest.h"

typedef void (APIENTRY *gen_buffers_t)( GLsizei, GLuint * );
typedef void (APIENTRY *delete_buffers_t)( GLsizei, const GLuint * );
typedef void (APIENTRY *bind_buffer_t)( GLenum, GLuint );
typedef void (APIENTRY *buffer_data_t)( GLenum, ptrdiff_t, const void *, GLenum );
typedef void *(APIENTRY *map_buffer_t)( GLenum, GLenum );
typedef GLboolean (APIENTRY *unmap_buffer_t)( GLenum );
typedef void (APIENTRY *compressed_tex_image_2d_t)( GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void * );

struct vertex
{
    GLfloat x, y;
    GLubyte r, g, b, a;
};

/* one DXT1 block of a single RGB565 colour: colour0, colour1 = 0, every index 0 */
static void dxt1_block( GLubyte *block, unsigned short rgb565 )
{
    memset( block, 0, 8 );
    block[0] = rgb565 & 0xff;
    block[1] = rgb565 >> 8;
}

static void client_arrays( void )
{
    static const GLfloat pos[] = { -1.0f, -1.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f };
    static const GLubyte col[] = { 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255 };

    glEnableClientState( GL_VERTEX_ARRAY );
    glEnableClientState( GL_COLOR_ARRAY );
    glVertexPointer( 2, GL_FLOAT, 0, pos );
    glColorPointer( 4, GL_UNSIGNED_BYTE, 0, col );
    glDrawArrays( GL_QUADS, 0, 4 );
    glDisableClientState( GL_COLOR_ARRAY );
    glDisableClientState( GL_VERTEX_ARRAY );
    gt_pixel( "client arrays, GL_QUADS", 32, 32, 255, 0, 0, 2 );
}

static void buffers( void )
{
    gen_buffers_t gen = (gen_buffers_t)(void *)gt_proc( "glGenBuffers" );
    delete_buffers_t del = (delete_buffers_t)(void *)gt_proc( "glDeleteBuffers" );
    bind_buffer_t bind = (bind_buffer_t)(void *)gt_proc( "glBindBuffer" );
    buffer_data_t data = (buffer_data_t)(void *)gt_proc( "glBufferData" );
    map_buffer_t map = (map_buffer_t)(void *)gt_proc( "glMapBuffer" );
    unmap_buffer_t unmap = (unmap_buffer_t)(void *)gt_proc( "glUnmapBuffer" );
    /* the lower right quarter, green; two triangles below and above its diagonal */
    static const struct vertex quad[4] =
    {
        { 0.0f, -1.0f, 0, 255, 0, 255 }, { 1.0f, -1.0f, 0, 255, 0, 255 },
        { 1.0f, 0.0f, 0, 255, 0, 255 }, { 0.0f, 0.0f, 0, 255, 0, 255 },
    };
    static const GLushort index[6] = { 0, 1, 2, 0, 2, 3 };
    GLuint names[2];
    struct vertex *mapped;
    int i;

    if (!gen || !del || !bind || !data || !map || !unmap) return;
    gen( 2, names );
    bind( GT_GL_ARRAY_BUFFER, names[0] );
    data( GT_GL_ARRAY_BUFFER, sizeof(quad), quad, GT_GL_STATIC_DRAW );
    bind( GT_GL_ELEMENT_ARRAY_BUFFER, names[1] );
    data( GT_GL_ELEMENT_ARRAY_BUFFER, sizeof(index), index, GT_GL_STATIC_DRAW );

    /* pointers are offsets into the bound buffers */
    glEnableClientState( GL_VERTEX_ARRAY );
    glEnableClientState( GL_COLOR_ARRAY );
    glVertexPointer( 2, GL_FLOAT, sizeof(struct vertex), (const void *)offsetof( struct vertex, x ) );
    glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof(struct vertex), (const void *)offsetof( struct vertex, r ) );
    glDrawElements( GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0 );
    gt_pixel( "VBO, glDrawElements at offset 0 (lower triangle)", 112, 16, 0, 255, 0, 2 );
    gt_pixel( "VBO, glDrawElements at offset 0 (upper triangle)", 80, 48, 0, 255, 0, 2 );

    /* an index offset: only the second triangle */
    glClear( GL_COLOR_BUFFER_BIT );
    glDrawElements( GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, (const void *)(3 * sizeof(GLushort)) );
    gt_pixel( "element buffer offset: the second triangle drawn", 80, 48, 0, 255, 0, 2 );
    gt_pixel( "element buffer offset: the first one not", 112, 16, 0, 0, 0, 0 );

    /* glMapBuffer: the colours turn blue */
    mapped = map( GT_GL_ARRAY_BUFFER, GT_GL_WRITE_ONLY );
    if (gt_check( "glMapBuffer", mapped != NULL ))
    {
        for (i = 0; i < 4; i++)
        {
            mapped[i] = quad[i];
            mapped[i].g = 0;
            mapped[i].b = 255;
        }
        gt_check( "glUnmapBuffer", unmap( GT_GL_ARRAY_BUFFER ) == GL_TRUE );
        glClear( GL_COLOR_BUFFER_BIT );
        glDrawElements( GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0 );
        gt_pixel( "data written through glMapBuffer", 96, 32, 0, 0, 255, 2 );
    }

    glDisableClientState( GL_COLOR_ARRAY );
    glDisableClientState( GL_VERTEX_ARRAY );
    bind( GT_GL_ARRAY_BUFFER, 0 );
    bind( GT_GL_ELEMENT_ARRAY_BUFFER, 0 );
    del( 2, names );
}

static void s3tc_mipmaps( void )
{
    compressed_tex_image_2d_t compressed = (compressed_tex_image_2d_t)(void *)gt_proc( "glCompressedTexImage2D" );
    static const unsigned short colours[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };   /* red, green, blue, white */
    GLubyte level0[4 * 8], block[8];
    GLuint tex;
    int i;

    gt_check( "GL_EXT_texture_compression_s3tc advertised",
              gt_has_extension( (const char *)glGetString( GL_EXTENSIONS ), "GL_EXT_texture_compression_s3tc" ) );
    if (!compressed) return;

    glGenTextures( 1, &tex );
    glBindTexture( GL_TEXTURE_2D, tex );
    for (i = 0; i < 4; i++) dxt1_block( level0 + 8 * i, colours[0] );
    compressed( GL_TEXTURE_2D, 0, GT_GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 8, 8, 0, sizeof(level0), level0 );
    for (i = 1; i < 4; i++)
    {
        dxt1_block( block, colours[i] );
        compressed( GL_TEXTURE_2D, i, GT_GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 8 >> i, 8 >> i, 0, sizeof(block), block );
    }
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    gt_check( "glCompressedTexImage2D, four levels", glGetError() == GL_NO_ERROR );

    /* pixel coordinates: 8 x 8 texels on 8 x 8, 4 x 4 and 2 x 2 pixels: levels 0, 1 and 2 */
    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();
    glOrtho( 0.0, 128.0, 0.0, 128.0, -1.0, 1.0 );
    glMatrixMode( GL_MODELVIEW );
    glLoadIdentity();
    glClear( GL_COLOR_BUFFER_BIT );
    glEnable( GL_TEXTURE_2D );
    glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
    gt_quad( 8.0f, 72.0f, 16.0f, 80.0f, 0.0f );
    gt_quad( 40.0f, 72.0f, 44.0f, 76.0f, 0.0f );
    gt_quad( 72.0f, 72.0f, 74.0f, 74.0f, 0.0f );
    glDisable( GL_TEXTURE_2D );
    gt_pixel( "DXT1 level 0", 12, 76, 255, 0, 0, 4 );
    gt_pixel( "DXT1 level 1", 41, 73, 0, 255, 0, 4 );
    gt_pixel( "DXT1 level 2", 72, 72, 0, 0, 255, 4 );
    glDeleteTextures( 1, &tex );
    gt_ortho( 128, 128 );
}

int main( void )
{
    struct gt_window win;

    gt_begin( "gl-arrays" );
    if (!gt_window( &win, 128, 128, PFD_DOUBLEBUFFER, 1 )) return gt_end();
    gt_ortho( 128, 128 );
    glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT );

    client_arrays();
    buffers();
    s3tc_mipmaps();

    gt_check( "no GL error", glGetError() == GL_NO_ERROR );
    SwapBuffers( win.hdc );
    gt_close( &win );
    return gt_end();
}
