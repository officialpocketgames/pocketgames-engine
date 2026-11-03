// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part J3: the macOS render test. pgl, gl4es and ANGLE (its Metal backend, as on iOS),
// with no Wine. A context and a 256 x 256 FBO drawable; each scene is drawn through gl4es's
// desktop GL entry points, swapped (D5.3's copy into a present texture and the shared-event
// signal; there is no window, so no layer pass), and the present texture is read back after the
// signal and compared with the expected pixels (+-2). Scenes: the orientation of the present
// texture, an immediate-mode triangle with fixed-function colour, a DXT1-textured quad, a display
// list drawn twice, vertex arrays from client memory, a GLSL 1.20 program, an
// ARB_fragment_program; then two contexts sharing a texture on two threads.
//
// Prints PASS/FAIL per check and SKIP when the machine has no Metal device. Exit status 1 on any
// FAIL. Only the CI's macOS runner runs it (never a GPU test on the owner's PC, Part J).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <GLES2/gl2.h>
#include "pgl.h"

/* ---- what DXMT's unix half gives pgl in the app: stand-ins with no window layer ---- */

static int32_t no_layer( void *params ) { (void)params; return 0; }
const void *dxmt_winemetal_unix_call_funcs[256] = { [0 ... 255] = (const void *)no_layer };
static uint64_t presents;
int pg_get_vsync_locked( void ) { return 0; }
void pg_present_count_add( uint32_t n ) { presents += n; }

/* ---- desktop GL through gl4es (pgl_get_proc_address) ---- */

#define GL_QUADS                          0x0007
#define GL_COMPILE                        0x1300
#define GL_MODELVIEW                      0x1700
#define GL_PROJECTION                     0x1701
#define GL_VERTEX_ARRAY                   0x8074
#define GL_COLOR_ARRAY                    0x8076
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT   0x83F0
#define GL_FRAGMENT_PROGRAM_ARB           0x8804
#define GL_PROGRAM_FORMAT_ASCII_ARB       0x8875

#define GL_FUNCS \
    F( void, glBindFramebuffer, (GLenum, GLuint) ) \
    F( void, glViewport, (GLint, GLint, GLsizei, GLsizei) ) \
    F( void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat) ) \
    F( void, glClear, (GLbitfield) ) \
    F( void, glMatrixMode, (GLenum) ) \
    F( void, glLoadIdentity, (void) ) \
    F( void, glTranslatef, (GLfloat, GLfloat, GLfloat) ) \
    F( void, glBegin, (GLenum) ) \
    F( void, glEnd, (void) ) \
    F( void, glColor3f, (GLfloat, GLfloat, GLfloat) ) \
    F( void, glVertex2f, (GLfloat, GLfloat) ) \
    F( void, glTexCoord2f, (GLfloat, GLfloat) ) \
    F( void, glRectf, (GLfloat, GLfloat, GLfloat, GLfloat) ) \
    F( void, glEnable, (GLenum) ) \
    F( void, glDisable, (GLenum) ) \
    F( void, glGenTextures, (GLsizei, GLuint *) ) \
    F( void, glBindTexture, (GLenum, GLuint) ) \
    F( void, glTexParameteri, (GLenum, GLenum, GLint) ) \
    F( void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) ) \
    F( void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *) ) \
    F( GLuint, glGenLists, (GLsizei) ) \
    F( void, glNewList, (GLuint, GLenum) ) \
    F( void, glEndList, (void) ) \
    F( void, glCallList, (GLuint) ) \
    F( void, glEnableClientState, (GLenum) ) \
    F( void, glDisableClientState, (GLenum) ) \
    F( void, glVertexPointer, (GLint, GLenum, GLsizei, const void *) ) \
    F( void, glColorPointer, (GLint, GLenum, GLsizei, const void *) ) \
    F( void, glDrawArrays, (GLenum, GLint, GLsizei) ) \
    F( GLuint, glCreateShader, (GLenum) ) \
    F( void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *) ) \
    F( void, glCompileShader, (GLuint) ) \
    F( void, glGetShaderiv, (GLuint, GLenum, GLint *) ) \
    F( GLuint, glCreateProgram, (void) ) \
    F( void, glAttachShader, (GLuint, GLuint) ) \
    F( void, glLinkProgram, (GLuint) ) \
    F( void, glGetProgramiv, (GLuint, GLenum, GLint *) ) \
    F( void, glUseProgram, (GLuint) ) \
    F( GLint, glGetUniformLocation, (GLuint, const GLchar *) ) \
    F( void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat) ) \
    F( void, glGenProgramsARB, (GLsizei, GLuint *) ) \
    F( void, glBindProgramARB, (GLenum, GLuint) ) \
    F( void, glProgramStringARB, (GLenum, GLenum, GLsizei, const void *) ) \
    F( void, glFinish, (void) ) \
    F( const GLubyte *, glGetString, (GLenum) )

#define F( ret, name, args ) static ret (*p_##name) args;
GL_FUNCS
#undef F

static int checks, failures;

static void result( int ok, const char *what )
{
    checks++;
    if (!ok) failures++;
    printf( "%s %s\n", ok ? "PASS" : "FAIL", what );
}

static int load_gl( void )
{
    int ok = 1;
#define F( ret, name, args ) \
    if (!(p_##name = (ret (*) args)pgl_get_proc_address( #name ))) { printf( "FAIL gl4es has no %s\n", #name ); ok = 0; }
    GL_FUNCS
#undef F
    return ok;
}

/* ---- reading the present texture back ---- */

#define W 256
#define H 256
static uint8_t pixels[W * H * 4];   /* BGRA, row 0 = the texture's first row */

static int read_back( pgl_drawable *d )
{
    void *event = NULL;
    uint64_t value = 0;
    id<MTLTexture> tex = (id<MTLTexture>)pgl_drawable_last_copy( d, &event, &value );
    id<MTLSharedEvent> ev = (id<MTLSharedEvent>)event;
    int waited;

    if (!tex || !ev) return 0;
    for (waited = 0; ev.signaledValue < value && waited < 2000; waited++) usleep( 1000 );   /* D5.3.3's signal */
    if (ev.signaledValue < value) return 0;
    @autoreleasepool
    {
        id<MTLCommandQueue> queue = [tex.device newCommandQueue];
        id<MTLBuffer> buffer = [tex.device newBufferWithLength:sizeof(pixels) options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake( 0, 0, 0 )
                   sourceSize:MTLSizeMake( W, H, 1 ) toBuffer:buffer destinationOffset:0
          destinationBytesPerRow:W * 4 destinationBytesPerImage:sizeof(pixels)];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        memcpy( pixels, buffer.contents, sizeof(pixels) );
        [buffer release];
        [queue release];
    }
    return 1;
}

/* the pixel at GL coordinates (x, y), y = 0 the bottom row: row y of the present texture
 * (D5.4's shader shows row 0 at the bottom of the layer) */
static int pixel_is( int x, int y, int r, int g, int b )
{
    const uint8_t *p = pixels + ((size_t)y * W + (size_t)x) * 4;
    int ok = abs( p[2] - r ) <= 2 && abs( p[1] - g ) <= 2 && abs( p[0] - b ) <= 2;
    if (!ok) printf( "  pixel (%d,%d) is %d,%d,%d, expected %d,%d,%d\n", x, y, p[2], p[1], p[0], r, g, b );
    return ok;
}

static void begin_scene( GLuint fbo )
{
    p_glBindFramebuffer( GL_FRAMEBUFFER, fbo );
    p_glViewport( 0, 0, W, H );
    p_glClearColor( 0, 0, 0, 1 );
    p_glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
    p_glMatrixMode( GL_PROJECTION );
    p_glLoadIdentity();
    p_glMatrixMode( GL_MODELVIEW );
    p_glLoadIdentity();
    p_glColor3f( 1, 1, 1 );
}

static int end_scene( pgl_drawable *d )
{
    pgl_drawable_swap( d, 1 );
    return read_back( d );
}

static void full_quad( void )
{
    p_glBegin( GL_QUADS );
    p_glTexCoord2f( 0, 0 ); p_glVertex2f( -1, -1 );
    p_glTexCoord2f( 1, 0 ); p_glVertex2f( 1, -1 );
    p_glTexCoord2f( 1, 1 ); p_glVertex2f( 1, 1 );
    p_glTexCoord2f( 0, 1 ); p_glVertex2f( -1, 1 );
    p_glEnd();
}

/* ---- scenes ---- */

static void scenes( pgl_drawable *d, GLuint fbo )
{
    /* orientation: bottom half red, top half green */
    begin_scene( fbo );
    p_glColor3f( 1, 0, 0 ); p_glRectf( -1, -1, 1, 0 );
    p_glColor3f( 0, 1, 0 ); p_glRectf( -1, 0, 1, 1 );
    result( end_scene( d ) && pixel_is( 128, 64, 255, 0, 0 ) && pixel_is( 128, 192, 0, 255, 0 ),
            "orientation: GL's bottom row is the present texture's row 0" );

    /* an immediate-mode triangle, fixed-function colour */
    begin_scene( fbo );
    p_glBegin( GL_TRIANGLES );
    p_glColor3f( 1, 0, 0 ); p_glVertex2f( -1, -1 );
    p_glColor3f( 1, 0, 0 ); p_glVertex2f( 1, -1 );
    p_glColor3f( 1, 0, 0 ); p_glVertex2f( -1, 1 );
    p_glEnd();
    result( end_scene( d ) && pixel_is( 64, 64, 255, 0, 0 ) && pixel_is( 192, 192, 0, 0, 0 ),
            "immediate-mode triangle" );

    /* a quad textured with one DXT1 block (solid blue) */
    {
        static const uint8_t dxt1[8] = { 0x1f, 0x00, 0x1f, 0x00, 0, 0, 0, 0 };   /* c0 = c1 = blue (RGB565) */
        GLuint tex;
        begin_scene( fbo );
        p_glGenTextures( 1, &tex );
        p_glBindTexture( GL_TEXTURE_2D, tex );
        p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
        p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
        p_glCompressedTexImage2D( GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 4, 4, 0, sizeof(dxt1), dxt1 );
        p_glEnable( GL_TEXTURE_2D );
        full_quad();
        p_glDisable( GL_TEXTURE_2D );
        result( end_scene( d ) && pixel_is( 128, 128, 0, 0, 255 ) && pixel_is( 10, 245, 0, 0, 255 ),
                "DXT1-textured quad" );
    }

    /* a display list drawn twice */
    {
        GLuint list;
        begin_scene( fbo );
        list = p_glGenLists( 1 );
        p_glNewList( list, GL_COMPILE );
        p_glColor3f( 0, 1, 0 );
        p_glRectf( 0, 0, 0.2f, 0.2f );
        p_glEndList();
        p_glTranslatef( -0.5f, -0.5f, 0 );
        p_glCallList( list );
        p_glLoadIdentity();
        p_glTranslatef( 0.5f, 0.5f, 0 );
        p_glCallList( list );
        result( end_scene( d ) && pixel_is( 76, 76, 0, 255, 0 ) && pixel_is( 204, 204, 0, 255, 0 ) &&
                pixel_is( 128, 128, 0, 0, 0 ), "display list drawn twice" );
    }

    /* vertex arrays from client memory */
    {
        static const GLfloat verts[] = { -1, -1, 0, -1, -1, 1, 0, 1 };
        static const GLubyte colors[] = { 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255 };
        begin_scene( fbo );
        p_glEnableClientState( GL_VERTEX_ARRAY );
        p_glEnableClientState( GL_COLOR_ARRAY );
        p_glVertexPointer( 2, GL_FLOAT, 0, verts );
        p_glColorPointer( 4, GL_UNSIGNED_BYTE, 0, colors );
        p_glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
        p_glDisableClientState( GL_COLOR_ARRAY );
        p_glDisableClientState( GL_VERTEX_ARRAY );
        result( end_scene( d ) && pixel_is( 64, 128, 255, 255, 0 ) && pixel_is( 192, 128, 0, 0, 0 ),
                "vertex arrays from client memory" );
    }

    /* a GLSL 1.20 program */
    {
        static const GLchar *vs_src = "#version 120\nvoid main() { gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }\n";
        static const GLchar *fs_src = "#version 120\nuniform vec4 c;\nvoid main() { gl_FragColor = c; }\n";
        GLuint vs, fs, prog;
        GLint ok_vs = 0, ok_fs = 0, ok_link = 0;
        begin_scene( fbo );
        vs = p_glCreateShader( GL_VERTEX_SHADER );
        fs = p_glCreateShader( GL_FRAGMENT_SHADER );
        p_glShaderSource( vs, 1, &vs_src, NULL );
        p_glShaderSource( fs, 1, &fs_src, NULL );
        p_glCompileShader( vs );
        p_glCompileShader( fs );
        p_glGetShaderiv( vs, GL_COMPILE_STATUS, &ok_vs );
        p_glGetShaderiv( fs, GL_COMPILE_STATUS, &ok_fs );
        prog = p_glCreateProgram();
        p_glAttachShader( prog, vs );
        p_glAttachShader( prog, fs );
        p_glLinkProgram( prog );
        p_glGetProgramiv( prog, GL_LINK_STATUS, &ok_link );
        p_glUseProgram( prog );
        p_glUniform4f( p_glGetUniformLocation( prog, "c" ), 1, 0, 1, 1 );
        full_quad();
        p_glUseProgram( 0 );
        result( ok_vs && ok_fs && ok_link && end_scene( d ) && pixel_is( 128, 128, 255, 0, 255 ),
                "GLSL 1.20 program" );
    }

    /* an ARB_fragment_program */
    {
        static const char fp[] = "!!ARBfp1.0\nMOV result.color, {0.0, 1.0, 1.0, 1.0};\nEND\n";
        GLuint id;
        begin_scene( fbo );
        p_glGenProgramsARB( 1, &id );
        p_glBindProgramARB( GL_FRAGMENT_PROGRAM_ARB, id );
        p_glProgramStringARB( GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen( fp ), fp );
        p_glEnable( GL_FRAGMENT_PROGRAM_ARB );
        full_quad();
        p_glDisable( GL_FRAGMENT_PROGRAM_ARB );
        result( end_scene( d ) && pixel_is( 128, 128, 0, 255, 255 ), "ARB_fragment_program" );
    }
}

/* ---- two contexts sharing a texture on two threads ---- */

static pgl_context *shared_ctx;
static GLuint shared_tex;
static int shared_ok;

static void *second_thread( void *arg )
{
    static char owner;
    pgl_drawable *d2 = pgl_drawable_create( 2, 0, W, H, &owner );
    GLuint fbo;

    (void)arg;
    if (!d2 || !pgl_make_current( shared_ctx, d2 ) || !(fbo = pgl_drawable_attach( d2 ))) return NULL;
    begin_scene( fbo );
    p_glBindTexture( GL_TEXTURE_2D, shared_tex );
    p_glEnable( GL_TEXTURE_2D );
    full_quad();
    p_glDisable( GL_TEXTURE_2D );
    shared_ok = end_scene( d2 ) && pixel_is( 128, 128, 255, 0, 0 );
    pgl_drawable_detach( d2 );
    pgl_make_current( NULL, NULL );
    pgl_thread_exit();
    pgl_drawable_release( d2 );
    return NULL;
}

int main( void )
{
    static char owner;
    struct pgl_context_request req = { .major = 2, .minor = 1 };
    pgl_context *ctx;
    pgl_drawable *d;
    unsigned int contexts, drawables;
    GLuint fbo;

    @autoreleasepool
    {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev)
        {
            printf( "SKIP no Metal device\n" );
            return 0;
        }
        [dev release];
    }

    result( pgl_init() == 0, "pgl_init (ANGLE Metal display, gl4es)" );
    if (failures) return 1;
    printf( "renderer: %s\n", pgl_renderer() );

    ctx = pgl_context_create( NULL, &req, &owner );
    d = pgl_drawable_create( 1, 0, W, H, &owner );
    result( ctx && d && pgl_make_current( ctx, d ), "context and drawable" );
    if (failures) return 1;
    fbo = pgl_drawable_attach( d );
    result( fbo != 0, "the drawable's framebuffer (through gl4es)" );
    if (failures || !load_gl()) return 1;

    result( !strcmp( (const char *)p_glGetString( GL_VERSION ), PGL_GL_VERSION_VALUE ) &&
            !strcmp( (const char *)p_glGetString( GL_VENDOR ), PGL_GL_VENDOR_VALUE ) &&
            !strcmp( (const char *)p_glGetString( GL_SHADING_LANGUAGE_VERSION ), PGL_GLSL_VERSION_VALUE ) &&
            strlen( (const char *)p_glGetString( GL_EXTENSIONS ) ) < PGL_EXTENSIONS_MAX,
            "the strings a program reads (C3.4)" );

    scenes( d, fbo );

    /* the texture both contexts see */
    {
        static const uint8_t red[2 * 2 * 4] = { 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255 };
        pthread_t thread;

        shared_ctx = pgl_context_create( ctx, &req, &owner );
        p_glGenTextures( 1, &shared_tex );
        p_glBindTexture( GL_TEXTURE_2D, shared_tex );
        p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
        p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
        p_glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, red );
        p_glFinish();
        result( shared_ctx && !pthread_create( &thread, NULL, second_thread, NULL ) && !pthread_join( thread, NULL ) && shared_ok,
                "two contexts sharing a texture on two threads" );
    }

    result( presents > 0, "every swap counted as a present (pg_present_count_add)" );

    pgl_drawable_detach( d );
    pgl_make_current( NULL, NULL );
    pgl_drawable_release( d );
    pgl_process_ended( &owner, &contexts, &drawables );
    result( contexts == 2 && drawables == 0, "H2: the program's two contexts destroyed at its end" );

    printf( "render test: %d checks, %d failures\n", checks, failures );
    return failures != 0;
}
