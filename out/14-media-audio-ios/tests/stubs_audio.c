// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F: a fake output unit and a virtual clock for the audio driver's host tests
// (in place of ios_audio_apple.c).

#include <stdio.h>

#include "ios_audio_pure.h"
#include "test_stubs.h"

test_render_fn test_unit_render;
uint32_t test_unit_rate, test_unit_channels;
int test_unit_fail, test_unit_running, test_unit_creates;
int test_unit_start_fail;   /* spec 22 F3: ia_unit_start fails */
uint64_t test_now = 1000000000ull;

int ia_unit_create( uint32_t rate, uint32_t channels, ia_render_fn render, char *why, size_t why_size )
{
    test_unit_creates++;
    if (test_unit_fail)
    {
        snprintf( why, why_size, "test: creation fails" );
        return -1;
    }
    test_unit_render = render;
    test_unit_rate = rate;
    test_unit_channels = channels;
    return 0;
}

int ia_unit_start( void )
{
    if (test_unit_start_fail) return -1;
    test_unit_running = 1;
    return 0;
}

int ia_unit_stop( void )
{
    test_unit_running = 0;
    return 0;
}

uint64_t ia_now_ns( void )
{
    return __atomic_load_n( &test_now, __ATOMIC_RELAXED );
}

void ia_sleep_until_ns( uint64_t deadline )
{
    uint64_t now = __atomic_load_n( &test_now, __ATOMIC_RELAXED );
    if (deadline > now) __atomic_store_n( &test_now, deadline, __ATOMIC_RELAXED );
}

int ia_set_time_constraint( uint64_t period, uint64_t computation, uint64_t constraint, int preemptible )
{
    return 0;
}
