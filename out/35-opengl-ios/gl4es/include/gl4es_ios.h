// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part C3: the functions this tree's gl4es adds for pgl, besides the four of upstream's
// include/gl4esinit.h (initialize_gl4es, set_getprocaddress, set_getmainfbsize,
// gl4es_GetProcAddress). They are the only names the archive exports besides those four
// (C2.1). This header is new in the gl4es tree (out/35-opengl-ios/gl4es/include/); it carries
// no Wine types and is what pgl compiles against.
//
// A "state" is gl4es's per-context state (upstream's glstate_t), made per OpenGL context and
// current per thread (C3.1).

#ifndef GL4ES_IOS_H
#define GL4ES_IOS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C2.1: gl4es is compiled with hidden visibility, and the pre-link exports only what is visible,
 * so each function here is marked (as gl4esinit.h's four are, in gl4es_ios_config.h) */
#define GL4ES_IOS_EXPORT __attribute__((visibility("default")))

/* ---- C3.1: states ---- */

/* A new state. With share non-NULL it shares display lists, textures, buffers, shaders and
 * programs with share (as gl4es's own GLX/EGL layers share them). The GLES context the state
 * will be used with must be current on the calling thread. Returns NULL on failure. */
GL4ES_IOS_EXPORT void *gl4es_ios_state_create( void *share );

/* Makes state the calling thread's current state; NULL leaves the thread with none. Only the
 * calling thread is affected (the current state pointer is thread-local). */
GL4ES_IOS_EXPORT void gl4es_ios_state_activate( void *state );

/* The calling thread's current state, or NULL. */
GL4ES_IOS_EXPORT void *gl4es_ios_state_current( void );

/* Frees a state and everything it alone holds. Its GLES context must be current on the calling
 * thread; the state must not be current on any other thread. When it is the calling thread's
 * current state, the thread is left with none. */
GL4ES_IOS_EXPORT void gl4es_ios_state_destroy( void *state );

/* ---- C3.2: initialisation, once per app ---- */

/* Fixes the configuration in code (OpenGL 2.1 on the ES 2.0 backend; no banner, FPS counter,
 * gamma, stack trace or X11 refresh) and then runs upstream's initialize_gl4es, whose hardware
 * probe needs the bootstrap GLES context current. gl4es's LIBGL_* variables keep their
 * meaning. set_getprocaddress must have been called first. */
GL4ES_IOS_EXPORT void gl4es_ios_initialize( void );

/* ---- C3.3: logging ---- */

/* Every message gl4es would print goes to log as one line without its trailing newline; with
 * log NULL, messages are dropped. Nothing is printed to stdout or stderr. */
GL4ES_IOS_EXPORT void gl4es_ios_set_log( void (*log)( const char *line ) );

/* ---- C3.4: strings ---- */

/* The GL_RENDERER string (the Metal device's name). GL_VENDOR is "Apple", GL_VERSION "2.1.0",
 * GL_SHADING_LANGUAGE_VERSION "1.20"; no string names a library, and the extension string
 * stays under 4,000 bytes. renderer is copied. */
GL4ES_IOS_EXPORT void gl4es_ios_set_renderer( const char *renderer );

/* ---- C3.5: buffer-data allocation ---- */

/* The functions that allocate and free the CPU copy of every buffer object's data of state
 * (default malloc and free). Part G2 uses them for 32-bit programs; NULL restores the default. */
GL4ES_IOS_EXPORT void gl4es_ios_state_set_buffer_alloc( void *state, void *(*alloc_fn)( size_t size ), void (*free_fn)( void *ptr ) );

/* ---- D5.3: names ---- */

/* The GLES name of the framebuffer gl4es knows as name (the same number when gl4es uses GLES
 * names as its own, D5.3.2), with the state's context current. 0 for 0 or an unknown name. */
GL4ES_IOS_EXPORT unsigned int gl4es_ios_gles_framebuffer( unsigned int name );

#ifdef __cplusplus
}
#endif

#endif /* GL4ES_IOS_H */
