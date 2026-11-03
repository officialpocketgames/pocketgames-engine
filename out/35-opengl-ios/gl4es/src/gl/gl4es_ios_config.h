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

#endif /* GL4ES_IOS_CONFIG_H */
