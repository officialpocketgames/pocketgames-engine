// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35: declarations shared by opengl32's iOS files (opengl_ios_tables.c, opengl_ios_call.c,
// opengl_ios.c). Include after Wine's headers.

#ifndef OPENGL_IOS_H
#define OPENGL_IOS_H

#include "opengl_ios_pure.h"

/* ---- E4: the wrappers (opengl_ios_tables.c, generated list) ---- */

/* every entry of both tables: B1's answer when OpenGL is off for the calling process, else E1's
 * count and the upstream entry inside an autorelease pool (opengl_ios_call.c) */
extern NTSTATUS opengl_ios_call( unsigned int index, int wow64, void *args );

/* the tables spec 09's loader binds for opengl32.dll (A4) */
extern const unixlib_entry_t opengl32_unix_call_funcs[];
extern const unixlib_entry_t opengl32_unix_call_wow64_funcs[];

extern const char *const opengl_ios_entry_names[];   /* E2: "glVertex3f", ... */
extern const unsigned int opengl_ios_entry_count;
extern const unsigned int opengl_ios_swap_index;     /* unix_wglSwapBuffers */

/* the upstream tables of the generated unix_thunks.c (built with __wine_unix_call_funcs renamed);
 * build-unix.sh gives a build without the generated thunks empty ones and present 0 */
extern const unixlib_entry_t opengl32_upstream_funcs[];
extern const unixlib_entry_t opengl32_upstream_wow64_funcs[];
extern const int opengl32_upstream_present;

/* ---- the calling thread's answer (opengl_ios_call.c) ---- */

/* 1: OpenGL on for the thread's process, -1: off, 0: not known yet */
extern int opengl_ios_thread_get( void );
extern void opengl_ios_thread_set( int on );

/* E: statistics (opengl_ios_call.c) */
extern void opengl_ios_stats_init( int on );
extern void opengl_ios_log_line( const char *line );   /* to Wine's debug output (opengl_ios.c) */
extern uint64_t opengl_ios_now_ns( void );

/* ---- opengl_ios.c ---- */

/* B1: entry 0, process_attach: decides for the calling process */
extern NTSTATUS opengl_ios_process_attach( int wow64, void *args );
/* B1: the answer of a thread whose process isn't known to the thread yet */
extern int opengl_ios_thread_resolve( void );
/* B1: an entry other than the setup ones while off (logged once per process) */
extern NTSTATUS opengl_ios_off_call( unsigned int index );

#endif /* OPENGL_IOS_H */
