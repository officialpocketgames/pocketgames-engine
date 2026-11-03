// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part J1: the address conversions behind WOW_HOST, WOW_GUEST and the buffer-offset
// forms (opengl32-unix/opengl_ios_wow.h) on a fake window base: NULL, a normal address, the
// window's last byte, a host pointer outside the window, buffer offsets. WOW_HANDLE is Wine's
// ULongToHandle (a value).

#include <stdio.h>
#include <stdint.h>

#include "opengl_ios_wow.h"

static int checks, failures;

#define CHECK( cond ) do { checks++; if (!(cond)) { failures++; \
    printf( "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond ); } } while (0)

int main( void )
{
    const uintptr_t base = (uintptr_t)0x300000000ull;   /* a 4 GB aligned window above 4 GB */
    int outside;

    /* guest to host */
    CHECK( opengl_wow_host_at( base, 0 ) == NULL );
    CHECK( opengl_wow_host_at( base, 0x401000 ) == (void *)(base + 0x401000) );
    CHECK( opengl_wow_host_at( base, 0xffffffffu ) == (void *)(base + 0xffffffffull) );
    CHECK( opengl_wow_host_at( 0, 0x401000 ) == (void *)0x401000 );          /* no window: plain */

    /* host to guest */
    CHECK( opengl_wow_guest_at( base, NULL, &outside ) == 0 && !outside );
    CHECK( opengl_wow_guest_at( base, (void *)(base + 0x401000), &outside ) == 0x401000 && !outside );
    CHECK( opengl_wow_guest_at( base, (void *)(base + 0xffffffffull), &outside ) == 0xffffffffu && !outside );
    CHECK( opengl_wow_guest_at( base, (void *)base, &outside ) == 0 && !outside );       /* the window's first byte */
    CHECK( opengl_wow_guest_at( base, (void *)(base + 0x100000000ull), &outside ) == 0 && outside );   /* one past */
    CHECK( opengl_wow_guest_at( base, (void *)(base - 1), &outside ) == 0 && outside );
    CHECK( opengl_wow_guest_at( base, (void *)0x7f0012345678ull, &outside ) == 0 && outside );   /* a host heap pointer */
    CHECK( opengl_wow_guest_at( base, (void *)0x1000, &outside ) == 0 && outside );   /* below the window */
    CHECK( opengl_wow_guest_at( 0, (void *)0x1000, &outside ) == 0x1000 && !outside );
    CHECK( opengl_wow_guest_at( 0, (void *)0x100000000ull, &outside ) == 0 && outside );

    /* round trip */
    CHECK( opengl_wow_guest_at( base, opengl_wow_host_at( base, 0x12345678 ), &outside ) == 0x12345678 && !outside );

    /* a value handed back that may be a buffer offset (glGetPointerv with a buffer bound) */
    CHECK( opengl_wow_guest_or_offset_at( base, (void *)0x40, &outside ) == 0x40 && !outside );
    CHECK( opengl_wow_guest_or_offset_at( base, (void *)0xfffffff0ull, &outside ) == 0xfffffff0u && !outside );
    CHECK( opengl_wow_guest_or_offset_at( base, (void *)(base + 0x5000), &outside ) == 0x5000 && !outside );
    CHECK( opengl_wow_guest_or_offset_at( base, NULL, &outside ) == 0 && !outside );
    CHECK( opengl_wow_guest_or_offset_at( base, (void *)0x7f0012345678ull, &outside ) == 0 && outside );

    /* an argument that is a buffer offset when a buffer is bound */
    CHECK( opengl_wow_host_or_offset_at( base, 0x40, 1 ) == (void *)0x40 );
    CHECK( opengl_wow_host_or_offset_at( base, 0x40, 0 ) == (void *)(base + 0x40) );
    CHECK( opengl_wow_host_or_offset_at( base, 0, 0 ) == NULL );
    CHECK( opengl_wow_host_or_offset_at( base, 0, 1 ) == NULL );

    printf( "test_wow: %d checks, %d failures\n", checks, failures );
    return failures != 0;
}
