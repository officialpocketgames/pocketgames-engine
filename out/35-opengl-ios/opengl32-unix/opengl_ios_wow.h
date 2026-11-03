// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part G1: the address conversions of opengl32's WoW64 thunks, as pure functions of the
// window base (no Wine types: the host tests use them with a fake base). A 32-bit program lives
// in a 4 GB guest window at host address B = ios_wow_base(): its address a is host B + a
// (spec 06, spec 10 E). unix_private.h wraps them in the WOW_HOST / WOW_GUEST / WOW_HANDLE
// macros the generated thunks use.

#ifndef OPENGL_IOS_WOW_H
#define OPENGL_IOS_WOW_H

#include <stdint.h>

#define OPENGL_WOW_WINDOW_SIZE ((uint64_t)1 << 32)

/* guest to host: 0 stays NULL; with base 0 (no window) the plain conversion */
static inline void *opengl_wow_host_at( uintptr_t base, uint32_t guest )
{
    if (!guest) return 0;
    return (void *)(base + guest);
}

/* host to guest: NULL stays 0; a pointer outside the window is 0 and *outside is set */
static inline uint32_t opengl_wow_guest_at( uintptr_t base, const void *host, int *outside )
{
    uintptr_t p = (uintptr_t)host;

    *outside = 0;
    if (!p) return 0;
    if (!base) { if (p < OPENGL_WOW_WINDOW_SIZE) return (uint32_t)p; }
    else if (p >= base && p - base < OPENGL_WOW_WINDOW_SIZE) return (uint32_t)(p - base);
    *outside = 1;
    return 0;
}

/* a value handed back that may be an offset into a buffer object rather than a pointer
 * (glGetPointerv and glGetVertexAttribPointerv with a buffer bound): inside the window it is
 * converted; below 4 GB it is an offset and stays; anything else is 0 and *outside is set */
static inline uint32_t opengl_wow_guest_or_offset_at( uintptr_t base, const void *host, int *outside )
{
    uintptr_t p = (uintptr_t)host;
    uint32_t guest = opengl_wow_guest_at( base, host, outside );

    if (*outside && p < OPENGL_WOW_WINDOW_SIZE)
    {
        *outside = 0;
        return (uint32_t)p;
    }
    return guest;
}

/* an argument that is a pointer, or an offset into the buffer object bound to its target
 * (glVertexPointer, glDrawElements, glTexImage2D, ... with a buffer bound): bound selects the
 * offset (passed as is) */
static inline void *opengl_wow_host_or_offset_at( uintptr_t base, uint32_t guest, int bound )
{
    if (bound) return (void *)(uintptr_t)guest;
    return opengl_wow_host_at( base, guest );
}

#endif /* OPENGL_IOS_WOW_H */
