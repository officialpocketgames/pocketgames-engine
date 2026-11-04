// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 C3: forced into every gl4es source by build-gl4es.sh (clang -include), with GL4ES_IOS
// defined. Not part of upstream gl4es.
//
// C3.3: gl4es prints its messages with printf, vprintf and fprintf(stderr, ...) in many files
// (logs.c, fpe.c, array.c, arbhelper.c, debug.c, ...). These macros send all of them to
// gl4es_ios.c's line collector, which hands whole lines to the callback pgl sets
// (gl4es_ios_set_log). Nothing reaches stdout or stderr. A write to any other FILE goes through
// unchanged (none of the compiled sources has one on iOS: the PSA cache file is never opened).
//
// C3.1: GL4ES_IOS_TLS marks gl4es's per-thread scratch (fixed-size buffers a function returns).
//
// C2.1: the functions the archive exports are declared visible here (end of the file).
#ifndef GL4ES_IOS_CONFIG_H
#define GL4ES_IOS_CONFIG_H

#include <stdarg.h>
#include <stdio.h>

#define GL4ES_IOS_TLS __thread

int gl4es_ios_vfprintf( FILE *fp, const char *fmt, va_list args );
int gl4es_ios_fprintf( FILE *fp, const char *fmt, ... ) __attribute__((format(printf, 2, 3)));
int gl4es_ios_printf( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));
int gl4es_ios_fputs( const char *str, FILE *fp );
int gl4es_ios_fflush( FILE *fp );

#define printf(...)       gl4es_ios_printf(__VA_ARGS__)
#define fprintf(...)      gl4es_ios_fprintf(__VA_ARGS__)
#define vprintf(f, a)     gl4es_ios_vfprintf(stdout, f, a)
#define vfprintf(p, f, a) gl4es_ios_vfprintf(p, f, a)
#define puts(s)           gl4es_ios_fputs(s, NULL)
#define fputs(s, p)       gl4es_ios_fputs(s, p)
#define fflush(p)         gl4es_ios_fflush(p)

// C2.1: build-gl4es.sh compiles with -fvisibility=hidden, gl4es's EXPORT is empty on Apple
// targets, and the pre-link (ld -r with an export list) can't export a hidden symbol. So the
// functions it keeps global, gl4esinit.h's four and gl4es_ios.h's, are declared here, before any
// of gl4es's code, with default visibility; their definitions inherit it. gl4es_ios.h marks its
// own (GL4ES_IOS_EXPORT).
#include "../../include/gl4esinit.h"
#include "../../include/gl4es_ios.h"
__attribute__((visibility("default"))) void initialize_gl4es(void);
__attribute__((visibility("default"))) void set_getprocaddress(void *(APIENTRY_GL4ES *new_proc_address)(const char *));
__attribute__((visibility("default"))) void set_getmainfbsize(void (APIENTRY_GL4ES *new_getMainFBSize)(int *width, int *height));
__attribute__((visibility("default"))) void *APIENTRY_GL4ES gl4es_GetProcAddress(const char *name);

#endif /* GL4ES_IOS_CONFIG_H */
