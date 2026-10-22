// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08 Part B7: the server's log. wineserver_log_set_file() names the file, ws_log() writes
// one timestamped line to it, and everything the server code writes to stderr (object.h maps
// stderr to ws_log_stream()) goes there too, a timestamp in front of every line.
//
// Timestamps are UTC from gettimeofday/gmtime_r: localtime would take the time zone lock that
// every guest thread shares (spec 08 Part F1 has the same concern).

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "ios_server.h"

#define LOG_TAG "[wineserver] "

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int log_fd = -1;                 /* -1: the process's stderr (fd 2) */
static pthread_once_t stream_once = PTHREAD_ONCE_INIT;
static FILE *log_stream;
static char partial[2048];              /* stream text not yet ended by a newline */
static size_t partial_len;

static size_t format_stamp( char *buf, size_t size )
{
    struct timeval tv;
    struct tm tm;
    int len;

    gettimeofday( &tv, NULL );
    gmtime_r( &tv.tv_sec, &tm );
    len = snprintf( buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%03dZ " LOG_TAG,
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                    tm.tm_sec, (int)(tv.tv_usec / 1000) );
    if (len < 0) return 0;
    return (size_t)len < size ? (size_t)len : size - 1;
}

/* write one line (text without its newline); log_mutex must be held */
static void write_line_locked( const char *text, size_t len )
{
    char stamp[64];
    struct iovec vec[3];
    int fd = log_fd >= 0 ? log_fd : 2;

    vec[0].iov_base = stamp;
    vec[0].iov_len  = format_stamp( stamp, sizeof(stamp) );
    vec[1].iov_base = (void *)text;
    vec[1].iov_len  = len;
    vec[2].iov_base = (void *)"\n";
    vec[2].iov_len  = 1;
    /* one writev per line: with O_APPEND, lines from other writers don't interleave */
    while (writev( fd, vec, 3 ) == -1 && errno == EINTR) ;
}

/* write text that may hold several lines; a missing final newline is added */
static void write_text_locked( const char *text, size_t len )
{
    while (len)
    {
        const char *nl = memchr( text, '\n', len );
        size_t line = nl ? (size_t)(nl - text) : len;

        write_line_locked( text, line );
        if (!nl) break;
        text += line + 1;
        len -= line + 1;
    }
}

/* funopen write function of the stderr stand-in; called with the FILE locked */
static int stream_write( void *cookie, const char *buf, int size )
{
    int i;

    pthread_mutex_lock( &log_mutex );
    for (i = 0; i < size; i++)
    {
        if (buf[i] == '\n')
        {
            write_line_locked( partial, partial_len );
            partial_len = 0;
            continue;
        }
        if (partial_len == sizeof(partial))  /* overlong line: write it in pieces */
        {
            write_line_locked( partial, partial_len );
            partial_len = 0;
        }
        partial[partial_len++] = buf[i];
    }
    pthread_mutex_unlock( &log_mutex );
    return size;
}

static void create_stream(void)
{
    log_stream = funopen( NULL, NULL, stream_write, NULL, NULL );
    if (log_stream) setvbuf( log_stream, NULL, _IOLBF, 0 );
}

FILE *ws_log_stream(void)
{
    pthread_once( &stream_once, create_stream );
    return log_stream ? log_stream : stderr;
}

void ws_perror( const char *str )
{
    int err = errno;

    if (str && *str) fprintf( ws_log_stream(), "%s: %s\n", str, strerror( err ));
    else fprintf( ws_log_stream(), "%s\n", strerror( err ));
    errno = err;
}

void wineserver_log_set_file( const char *path )
{
    int fd = -1, old;

    if (path && *path) fd = open( path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644 );

    pthread_mutex_lock( &log_mutex );
    old = log_fd;
    log_fd = fd;
    pthread_mutex_unlock( &log_mutex );
    if (old >= 0) close( old );

    if (path && *path && fd == -1)
        ws_log( "cannot open log file %s: %s; logging to stderr", path, strerror( errno ));
}

void ws_log( const char *fmt, ... )
{
    char buf[2048];
    va_list args;
    int len;

    va_start( args, fmt );
    len = vsnprintf( buf, sizeof(buf), fmt, args );
    va_end( args );
    if (len < 0) return;
    if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;  /* truncated */
    while (len && buf[len - 1] == '\n') len--;

    pthread_mutex_lock( &log_mutex );
    write_text_locked( buf, len );
    pthread_mutex_unlock( &log_mutex );
}

/* Contract Section A: "a line to the app UI log". The server has no channel to the app's UI,
 * so the line goes to the server log with a [ui] tag (QUESTIONS.md, spec 08 question 2). */
void wine_ui_log( const char *message )
{
    ws_log( "[ui] %s", message ? message : "" );
}
