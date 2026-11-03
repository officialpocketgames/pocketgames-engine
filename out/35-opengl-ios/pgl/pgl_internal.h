// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35: what pgl's own files share (pgl_core.c, pgl_present.m, pgl_pool.m). Not for the
// driver: it uses pgl.h.

#ifndef PGL_INTERNAL_H
#define PGL_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "pgl.h"

/* ---- logging (pgl_core.c) ---- */
void pgl_log( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));
/* the first time and then every 256th time *counter is bumped */
void pgl_log_rare( _Atomic uint64_t *counter, const char *fmt, ... ) __attribute__((format(printf, 2, 3)));

/* ---- objects ---- */

/* C3.1: gl4es's shared lists (display lists, textures, buffers, programs) are plain hash maps,
 * so the contexts of one share group take turns in gl4es: every opengl32 call of a thread whose
 * context shares with another runs under its group's lock (pgl_call_in_pool). Contexts that
 * share with nothing run at the same time without it. */
struct pgl_group
{
    pthread_mutex_t lock;          /* recursive */
    _Atomic int members;           /* contexts of the group whose GL objects are alive */
    _Atomic int refs;              /* context shells, plus callers between enter and leave */
};

/* the calling thread's group, locked, when its context shares with a live context; else NULL */
struct pgl_group *pgl_group_enter( void );
void pgl_group_leave( struct pgl_group *group );

struct pgl_context
{
    struct pgl_owned owned;        /* in the registry while alive (H2) */
    EGLContext egl;
    void *gl4es;                   /* gl4es state (C3.1) */
    struct pgl_group *group;       /* its share group (C3.1) */
    _Atomic(uintptr_t) thread;     /* the thread it is current on (pthread_self), 0: none */
    int dead;                      /* H2 destroyed its GL objects; only the shell is left */
};

struct pgl_drawable
{
    struct pgl_owned owned;
    _Atomic int refs;
    pthread_mutex_t lock;
    uint64_t hwnd;                 /* 0: a pbuffer */
    unsigned int config;           /* index into pgl_configs */
    int width, height;             /* wanted size */
    int interval;                  /* the window's swap interval */
    int dead;                      /* H2 */

    /* D4.1: attachments, made through gl4es in the context `attached` */
    pgl_context *attached;
    GLuint fbo, color_rb, depth_rb;
    int att_width, att_height;

    /* D5: the present state (pgl_present.m), created at the first swap */
    void *present;
};

/* ---- globals (pgl_core.c) ---- */
extern EGLDisplay pgl_display;
extern int pgl_es_major;           /* 2, or 3 with PG_GL_ES3=1 */

/* extension entry points looked up once by pgl_init */
extern PFNEGLCREATEIMAGEKHRPROC pgl_eglCreateImageKHR;
extern PFNEGLDESTROYIMAGEKHRPROC pgl_eglDestroyImageKHR;
extern PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC pgl_glEGLImageTargetRenderbufferStorageOES;
extern PFNGLBLITFRAMEBUFFERANGLEPROC pgl_glBlitFramebufferANGLE;   /* ES 2 */
extern void (GL_APIENTRY *pgl_glBlitFramebuffer)( GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum ); /* ES 3 */

/* the calling thread's current context (pgl_make_current) */
pgl_context *pgl_thread_context( void );

/* ---- pgl_present.m ---- */

/* D3.1: the MTLDevice ANGLE uses (through EGL_ANGLE_device_metal) and the present queue; name
 * receives the device's name. 0 on success, else why is set. */
int pgl_metal_init( EGLDeviceEXT device, char *name, size_t name_size, const char **why );

/* D5: per-drawable present state */
int pgl_present_swap( pgl_drawable *d, int visible );        /* d->lock held */
void pgl_present_detach( pgl_drawable *d );                  /* d->lock held, its context current */
void pgl_present_stop( pgl_drawable *d );                    /* d->lock held: release the view */
void pgl_present_destroy( pgl_drawable *d );                 /* d->lock held: everything */

#endif /* PGL_INTERNAL_H */
