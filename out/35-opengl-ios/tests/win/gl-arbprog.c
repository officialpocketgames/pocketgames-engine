// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 J4: ARB assembly programs (GL_ARB_vertex_program, GL_ARB_fragment_program): a vertex
// program passing position and colour through, a fragment program scaling the colour by a
// local parameter and adding an environment parameter, and a program with an error.

#include "gltest.h"

typedef void (APIENTRY *gen_programs_t)( GLsizei, GLuint * );
typedef void (APIENTRY *delete_programs_t)( GLsizei, const GLuint * );
typedef void (APIENTRY *bind_program_t)( GLenum, GLuint );
typedef void (APIENTRY *program_string_t)( GLenum, GLenum, GLsizei, const void * );
typedef void (APIENTRY *program_param_t)( GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat );

static const char vertex_program[] =
    "!!ARBvp1.0\n"
    "MOV result.position, vertex.position;\n"
    "MOV result.color, vertex.color;\n"
    "END\n";

static const char fragment_program[] =
    "!!ARBfp1.0\n"
    "PARAM scale = program.local[0];\n"
    "PARAM bias = program.env[1];\n"
    "TEMP c;\n"
    "MUL c, fragment.color, scale;\n"
    "ADD result.color, c, bias;\n"
    "END\n";

static const char broken_program[] =
    "!!ARBfp1.0\n"
    "MOV result.color, nothing_by_that_name;\n"
    "END\n";

int main( void )
{
    struct gt_window win;
    gen_programs_t gen;
    delete_programs_t del;
    bind_program_t bind;
    program_string_t string;
    program_param_t local, env;
    GLuint names[3];
    GLint pos;
    const char *ext;

    gt_begin( "gl-arbprog" );
    if (!gt_window( &win, 128, 128, PFD_DOUBLEBUFFER, 1 )) return gt_end();
    ext = (const char *)glGetString( GL_EXTENSIONS );
    gt_check( "GL_ARB_vertex_program advertised", gt_has_extension( ext, "GL_ARB_vertex_program" ) );
    gt_check( "GL_ARB_fragment_program advertised", gt_has_extension( ext, "GL_ARB_fragment_program" ) );
    gen = (gen_programs_t)(void *)gt_proc( "glGenProgramsARB" );
    del = (delete_programs_t)(void *)gt_proc( "glDeleteProgramsARB" );
    bind = (bind_program_t)(void *)gt_proc( "glBindProgramARB" );
    string = (program_string_t)(void *)gt_proc( "glProgramStringARB" );
    local = (program_param_t)(void *)gt_proc( "glProgramLocalParameter4fARB" );
    env = (program_param_t)(void *)gt_proc( "glProgramEnvParameter4fARB" );
    if (!gen || !del || !bind || !string || !local || !env) { gt_close( &win ); return gt_end(); }

    gt_ortho( 128, 128 );
    glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT );

    gen( 3, names );
    bind( GT_GL_VERTEX_PROGRAM_ARB, names[0] );
    string( GT_GL_VERTEX_PROGRAM_ARB, GT_GL_PROGRAM_FORMAT_ASCII_ARB, sizeof(vertex_program) - 1, vertex_program );
    glGetIntegerv( GT_GL_PROGRAM_ERROR_POSITION_ARB, &pos );
    gt_checkf( "vertex program loads", pos == -1, "error at %d: %s", pos,
               (const char *)glGetString( 0x8874 /* GL_PROGRAM_ERROR_STRING_ARB */ ) );
    bind( GT_GL_FRAGMENT_PROGRAM_ARB, names[1] );
    string( GT_GL_FRAGMENT_PROGRAM_ARB, GT_GL_PROGRAM_FORMAT_ASCII_ARB, sizeof(fragment_program) - 1, fragment_program );
    glGetIntegerv( GT_GL_PROGRAM_ERROR_POSITION_ARB, &pos );
    gt_checkf( "fragment program loads", pos == -1, "error at %d", pos );

    /* white * (0.25, 0.5, 0.75) + (0, 0.25, 0.25): (64, 191, 255) on the left half */
    local( GT_GL_FRAGMENT_PROGRAM_ARB, 0, 0.25f, 0.5f, 0.75f, 1.0f );
    env( GT_GL_FRAGMENT_PROGRAM_ARB, 1, 0.0f, 0.25f, 0.25f, 0.0f );
    glEnable( GT_GL_VERTEX_PROGRAM_ARB );
    glEnable( GT_GL_FRAGMENT_PROGRAM_ARB );
    glColor3f( 1.0f, 1.0f, 1.0f );
    gt_quad( -1.0f, -1.0f, 0.0f, 1.0f, 0.0f );
    /* another local value on the right half */
    local( GT_GL_FRAGMENT_PROGRAM_ARB, 0, 1.0f, 0.0f, 0.0f, 1.0f );
    gt_quad( 0.0f, -1.0f, 1.0f, 1.0f, 0.0f );
    glDisable( GT_GL_FRAGMENT_PROGRAM_ARB );
    glDisable( GT_GL_VERTEX_PROGRAM_ARB );
    gt_pixel( "fragment program, local and env parameters", 32, 64, 64, 191, 255, 3 );
    gt_pixel( "local parameter changed between draws", 96, 64, 255, 64, 64, 3 );

    /* the fixed pipeline again */
    glColor3f( 0.0f, 1.0f, 0.0f );
    gt_quad( -0.25f, -0.25f, 0.25f, 0.25f, 0.0f );
    gt_pixel( "fixed function after the programs are disabled", 64, 64, 0, 255, 0, 2 );

    /* an error: a position, GL_INVALID_OPERATION */
    while (glGetError() != GL_NO_ERROR) {}
    bind( GT_GL_FRAGMENT_PROGRAM_ARB, names[2] );
    string( GT_GL_FRAGMENT_PROGRAM_ARB, GT_GL_PROGRAM_FORMAT_ASCII_ARB, sizeof(broken_program) - 1, broken_program );
    glGetIntegerv( GT_GL_PROGRAM_ERROR_POSITION_ARB, &pos );
    gt_check( "a broken program reports its error position", pos >= 0 );
    gt_check( "a broken program raises GL_INVALID_OPERATION", glGetError() == GL_INVALID_OPERATION );

    del( 3, names );
    gt_check( "no GL error", glGetError() == GL_NO_ERROR );
    SwapBuffers( win.hdc );
    gt_close( &win );
    return gt_end();
}
