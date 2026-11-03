// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part E: the layer's clock, sleep, memory readings and log on iOS (Apple: mach_time.h's
// mach_absolute_time, mach_timebase_info and mach_wait_until; task_info with TASK_VM_INFO for
// phys_footprint; os/proc.h's os_proc_available_memory; sysctlbyname "hw.memsize"). Required
// reason (spec 00 D5): mach_absolute_time, 35F9.1 (time between events inside the app). The host
// tests define these functions themselves instead of building this file.

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include "ios_vk_internal.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/proc.h>
#include <sys/sysctl.h>

static mach_timebase_info_data_t timebase(void)
{
    static mach_timebase_info_data_t info;
    if (!info.denom) mach_timebase_info( &info );
    return info;
}

uint64_t ivk_now_ns( void )
{
    mach_timebase_info_data_t tb = timebase();
    return mach_absolute_time() * tb.numer / tb.denom;
}

void ivk_sleep_until_ns( uint64_t deadline )
{
    mach_timebase_info_data_t tb = timebase();
    mach_wait_until( deadline * tb.denom / tb.numer );
}

uint64_t ivk_footprint( void )
{
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;

    if (task_info( mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count ) != KERN_SUCCESS) return 0;
    return info.phys_footprint;
}

uint64_t ivk_available_memory( void )
{
    return os_proc_available_memory();
}

uint64_t ivk_memsize( void )
{
    uint64_t value = 0;
    size_t len = sizeof(value);
    if (sysctlbyname( "hw.memsize", &value, &len, NULL, 0 ) || len != sizeof(value)) return 0;
    return value;
}

#else  /* a non-Apple build of the library (not used by the app): no memory readings */

uint64_t ivk_now_ns( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void ivk_sleep_until_ns( uint64_t deadline )
{
    uint64_t now = ivk_now_ns();
    struct timespec ts;

    if (deadline <= now) return;
    ts.tv_sec = (time_t)((deadline - now) / 1000000000ull);
    ts.tv_nsec = (long)((deadline - now) % 1000000000ull);
    nanosleep( &ts, NULL );
}

uint64_t ivk_footprint( void ) { return 0; }
uint64_t ivk_available_memory( void ) { return 0; }
uint64_t ivk_memsize( void ) { return 0; }

#endif

void ivk_log( const char *fmt, ... )
{
    char line[600];
    va_list args;

    va_start( args, fmt );
    vsnprintf( line, sizeof(line), fmt, args );
    va_end( args );
    fprintf( stderr, "[vk-ios] %s\n", line );
}
