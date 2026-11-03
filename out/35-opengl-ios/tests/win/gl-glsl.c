// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 J4: GLSL 1.20 (#version 120): built-in matrices and attributes, a varying, a uniform,
// a uniform array, a 1.20 array constructor and a non-square matrix; compile and link status,
// and a shader with an error.

#include "gltest.h"

typedef char gl_char;
typedef GLuint (APIENTRY *create_shader_t)( GLenum );
typedef void (APIENTRY *shader_source_t)( GLuint, GLsizei, const gl_char *const *, const GLint * );
typedef void (APIENTRY *compile_shader_t)( GLuint );
typedef void (APIENTRY *get_shader_iv_t)( GLuint, GLenum, GLint * );
typedef void (APIENTRY *get_log_t)( GLuint, GLsizei, GLsizei *, gl_char * );
typedef GLuint (APIENTRY *create_program_t)( void );
typedef void (APIENTRY *attach_shader_t)( GLuint, GLuint );
typedef void (APIENTRY *link_program_t)( GLuint );
typedef void (APIENTRY *use_program_t)( GLuint );
typedef GLint (APIENTRY *get_uniform_location_t)( GLuint, const gl_char * );
typedef void (APIENTRY *uniform4f_t)( GLint, GLfloat, GLfloat, GLfloat, GLfloat );
typedef void (APIENTRY *uniform1fv_t)( GLint, GLsizei, const GLfloat * );
typedef void (APIENTRY *delete_t)( GLuint );

static const char vertex_shader[] =
    "#version 120\n"
    "varying vec2 uv;\n"
    "void main()\n"
    "{\n"
    "    uv = gl_MultiTexCoord0.xy;\n"
    "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "}\n";

/* left half: colour * 0.25, right half: colour * 0.75 (an array constructor, new in 1.20);
 * the blue channel comes from a mat2x3 (non-square, new in 1.20) and a uniform array */
static const char fragment_shader[] =
    "#version 120\n"
    "uniform vec4 colour;\n"
    "uniform float extra[2];\n"
    "varying vec2 uv;\n"
    "const float k[2] = float[2]( 0.25, 0.75 );\n"
    "void main()\n"
    "{\n"
    "    mat2x3 m = mat2x3( 1.0, 0.0, 0.0,  0.0, 1.0, 0.0 );\n"
    "    vec3 rg = m * vec2( colour.r, colour.g );\n"
    "    float s = k[uv.x < 0.5 ? 0 : 1];\n"
    "    gl_FragColor = vec4( rg.x * s, rg.y * s, extra[0] + extra[1], 1.0 );\n"
    "}\n";

static const char broken_shader[] =
    "#version 120\n"
    "void main() { gl_FragColor = no_such_variable; }\n";

static GLuint compile( create_shader_t create, shader_source_t source, compile_shader_t comp, get_shader_iv_t iv,
                       get_log_t log, GLenum type, const char *text, const char *what, int expect_ok )
{
    GLuint shader = create( type );
    GLint ok = 0;
    char info[256] = "";

    source( shader, 1, &text, NULL );
    comp( shader );
    iv( shader, GT_GL_COMPILE_STATUS, &ok );
    log( shader, sizeof(info), NULL, info );
    if (expect_ok) gt_checkf( what, ok, "%s", info );
    else gt_checkf( what, !ok && info[0], "status %d, log \"%s\"", ok, info );
    return shader;
}

int main( void )
{
    static const GLfloat extra[2] = { 0.25f, 0.5f };
    struct gt_window win;
    create_shader_t create = NULL;
    shader_source_t source;
    compile_shader_t comp;
    get_shader_iv_t shader_iv, program_iv;
    get_log_t shader_log, program_log;
    create_program_t create_program;
    attach_shader_t attach;
    link_program_t link;
    use_program_t use;
    get_uniform_location_t location;
    uniform4f_t uniform4f;
    uniform1fv_t uniform1fv;
    delete_t delete_shader, delete_program;
    GLuint vs, fs, bad, program;
    GLint ok = 0;
    char info[256] = "";
    const char *version;

    gt_begin( "gl-glsl" );
    if (!gt_window( &win, 128, 128, PFD_DOUBLEBUFFER, 1 )) return gt_end();
    version = (const char *)glGetString( GT_GL_SHADING_LANGUAGE_VERSION );
    gt_checkf( "GL_SHADING_LANGUAGE_VERSION is 1.20", version && !strncmp( version, "1.20", 4 ), "\"%s\"", version ? version : "(null)" );

    create = (create_shader_t)(void *)gt_proc( "glCreateShader" );
    source = (shader_source_t)(void *)gt_proc( "glShaderSource" );
    comp = (compile_shader_t)(void *)gt_proc( "glCompileShader" );
    shader_iv = (get_shader_iv_t)(void *)gt_proc( "glGetShaderiv" );
    shader_log = (get_log_t)(void *)gt_proc( "glGetShaderInfoLog" );
    create_program = (create_program_t)(void *)gt_proc( "glCreateProgram" );
    attach = (attach_shader_t)(void *)gt_proc( "glAttachShader" );
    link = (link_program_t)(void *)gt_proc( "glLinkProgram" );
    program_iv = (get_shader_iv_t)(void *)gt_proc( "glGetProgramiv" );
    program_log = (get_log_t)(void *)gt_proc( "glGetProgramInfoLog" );
    use = (use_program_t)(void *)gt_proc( "glUseProgram" );
    location = (get_uniform_location_t)(void *)gt_proc( "glGetUniformLocation" );
    uniform4f = (uniform4f_t)(void *)gt_proc( "glUniform4f" );
    uniform1fv = (uniform1fv_t)(void *)gt_proc( "glUniform1fv" );
    delete_shader = (delete_t)(void *)gt_proc( "glDeleteShader" );
    delete_program = (delete_t)(void *)gt_proc( "glDeleteProgram" );
    if (!create || !source || !comp || !shader_iv || !shader_log || !create_program || !attach || !link ||
        !program_iv || !program_log || !use || !location || !uniform4f || !uniform1fv || !delete_shader || !delete_program)
    {
        gt_close( &win );
        return gt_end();
    }

    vs = compile( create, source, comp, shader_iv, shader_log, GT_GL_VERTEX_SHADER, vertex_shader, "vertex shader compiles", 1 );
    fs = compile( create, source, comp, shader_iv, shader_log, GT_GL_FRAGMENT_SHADER, fragment_shader, "fragment shader compiles", 1 );
    bad = compile( create, source, comp, shader_iv, shader_log, GT_GL_FRAGMENT_SHADER, broken_shader,
                   "a broken shader fails with a log", 0 );
    program = create_program();
    attach( program, vs );
    attach( program, fs );
    link( program );
    program_iv( program, GT_GL_LINK_STATUS, &ok );
    program_log( program, sizeof(info), NULL, info );
    gt_checkf( "program links", ok, "%s", info );

    gt_ortho( 128, 128 );
    glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT );
    use( program );
    uniform4f( location( program, "colour" ), 1.0f, 1.0f, 1.0f, 1.0f );
    uniform1fv( location( program, "extra" ), 2, extra );
    gt_check( "uniform locations", location( program, "colour" ) >= 0 && location( program, "extra" ) >= 0 &&
              location( program, "not_there" ) == -1 );
    gt_quad( -1.0f, -1.0f, 1.0f, 1.0f, 0.0f );
    use( 0 );
    gt_pixel( "GLSL 1.20: left half", 32, 64, 64, 64, 191, 3 );
    gt_pixel( "GLSL 1.20: right half", 96, 64, 191, 191, 191, 3 );

    delete_shader( vs );
    delete_shader( fs );
    delete_shader( bad );
    delete_program( program );
    gt_check( "no GL error", glGetError() == GL_NO_ERROR );
    SwapBuffers( win.hdc );
    gt_close( &win );
    return gt_end();
}
