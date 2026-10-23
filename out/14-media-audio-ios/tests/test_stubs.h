// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F: what the test stubs offer the tests (plain C).

#ifndef TEST_STUBS_H
#define TEST_STUBS_H

#include <stddef.h>
#include <stdint.h>

/* the log (stubs_common.c captures __wine_dbg_output; TEST_VERBOSE=1 echoes it) */
void test_log_clear( void );
int test_log_count( const char *needle );

/* the fake guest window of a 32-bit caller */
#define TEST_ARENA_SIZE (4u << 20)
#define TEST_ARENA_GUEST 0x00100000u
extern unsigned long test_wow_base;
extern int test_window_reject;          /* ios_wow_in_window answers 0 */
void test_enter_window( void );         /* behave as a 32-bit caller */
void test_leave_window( void );         /* behave as a 64-bit caller */
void *test_arena_alloc( size_t size );  /* memory inside the window */
uint32_t test_guest( const void *host );

extern void *test_peb;
unsigned long ios_wow_base( void );
int ios_wow_in_window( const void *addr );

/* stubs_audio.c: the output unit and the clock */
typedef void (*test_render_fn)( float *out, uint32_t frames, uint32_t bytes );
extern test_render_fn test_unit_render;
extern uint32_t test_unit_rate, test_unit_channels;
extern int test_unit_fail, test_unit_running, test_unit_creates;
extern int test_unit_start_fail;       /* ia_unit_start fails (spec 22 F3) */
extern void (*test_end_hook)( const void *peb );   /* what the driver gave ios_process_end_hook_add */
extern int test_end_hook_adds;
extern uint64_t test_now;              /* virtual monotonic ns; sleeps advance it */
extern int test_events_set;

/* stubs_wine.c: NtWaitForSingleObject on this handle joins test_timer_thread */
#define TEST_TIMER_THREAD_HANDLE ((void *)(uintptr_t)0x5550)
#include <pthread.h>
extern pthread_t test_timer_thread;
extern int test_timer_thread_running;

#endif /* TEST_STUBS_H */
