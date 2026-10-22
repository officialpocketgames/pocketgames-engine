// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F: stand-ins for what ntdll's unix side provides in the app (the log sink, the
// guest window, the calling pseudo-process). No Wine headers.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_stubs.h"

/* ---------------------------------------------------------------- the log */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static char *log_buf;
static size_t log_len, log_cap;

int __wine_dbg_output( const char *str )
{
    size_t n = strlen( str );

    if (getenv( "TEST_VERBOSE" )) fputs( str, stderr );
    pthread_mutex_lock( &log_lock );
    if (log_len + n + 1 > log_cap)
    {
        size_t cap = log_cap ? log_cap : 65536;
        char *p;
        while (cap < log_len + n + 1) cap *= 2;
        if (!(p = realloc( log_buf, cap )))
        {
            pthread_mutex_unlock( &log_lock );
            return 0;
        }
        log_buf = p;
        log_cap = cap;
    }
    memcpy( log_buf + log_len, str, n + 1 );
    log_len += n;
    pthread_mutex_unlock( &log_lock );
    return (int)n;
}

void test_log_clear( void )
{
    pthread_mutex_lock( &log_lock );
    log_len = 0;
    if (log_buf) log_buf[0] = 0;
    pthread_mutex_unlock( &log_lock );
}

int test_log_count( const char *needle )
{
    int n = 0;
    const char *p;

    pthread_mutex_lock( &log_lock );
    for (p = log_buf; p && (p = strstr( p, needle )); p += strlen( needle )) n++;
    pthread_mutex_unlock( &log_lock );
    return n;
}

/* ---------------------------------------------------------------- the guest window */

unsigned long test_wow_base;
int test_window_reject;

unsigned long ios_wow_base( void )
{
    return test_wow_base;
}

int ios_wow_in_window( const void *addr )
{
    uintptr_t a = (uintptr_t)addr;

    if (test_window_reject || !test_wow_base) return 0;
    return a >= test_wow_base && a - test_wow_base < ((uintptr_t)1 << 32);
}

/* an arena inside the fake window: guest address = offset + TEST_ARENA_GUEST */
static unsigned char arena[TEST_ARENA_SIZE] __attribute__((aligned(16)));
static size_t arena_used;

void test_enter_window( void )
{
    test_wow_base = (uintptr_t)arena - TEST_ARENA_GUEST;
    arena_used = 0;
}

void test_leave_window( void )
{
    test_wow_base = 0;
}

void *test_arena_alloc( size_t size )
{
    void *p;

    size = (size + 15) & ~(size_t)15;
    if (arena_used + size > sizeof(arena)) return NULL;
    p = arena + arena_used;
    arena_used += size;
    memset( p, 0, size );
    return p;
}

uint32_t test_guest( const void *host )
{
    return host ? (uint32_t)((uintptr_t)host - test_wow_base) : 0;
}

/* ---------------------------------------------------------------- the pseudo-process */

void *test_peb = (void *)0x7f0000001000ull;

void *ios_current_peb( void )
{
    return test_peb;
}

/* spec 22 C6: the hook the audio driver registers, for the test to run as ntdll would */
void (*test_end_hook)( const void *peb );
int test_end_hook_adds;

int ios_process_end_hook_add( void (*fn)( const void *peb ) )
{
    test_end_hook_adds++;
    test_end_hook = fn;
    return 0;
}
