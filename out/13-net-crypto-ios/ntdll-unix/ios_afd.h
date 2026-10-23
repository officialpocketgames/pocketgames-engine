// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part G3: guest addresses and the GET_EVENTS handle in ntdll's socket.c.
//
// socket.c serves the AFD ioctls under every Winsock call, for 64-bit and 32-bit programs at
// once. Who is 32-bit is decided per call by ios_wow_base() != 0 (the calling pseudo-process's
// window base, spec 10 Part E), not by in_wow64_call().
//
// - Guest addresses in AFD requests from a 32-bit caller (the 64-bit fields of afd_recvmsg /
//   afd_sendmsg / afd_transmit params, the 32-bit WSABUF arrays and their buffers, the control
//   buffer) become B + (low 32 bits); 0 stays NULL. For a 64-bit caller they are used as they
//   are, as upstream.
// - IOCTL_AFD_GET_EVENTS carries the event to reset as a HANDLE in the input-buffer argument.
//   The WoW64 layer converts that argument as an address, so a 32-bit caller's handle h
//   arrives as B + h. For B != 0: a value v with B <= v and v - B <= 0xffffffff is the handle
//   (LONG)(v - B) sign-extended (so B itself is NULL); any other value is passed unchanged.
//   MADEIRA_AFD_EVENT_HANDLE=0 turns this correction off.
//
// Nothing here does I/O or takes a lock: these sit on the send and receive paths. The *_at
// forms are pure (B passed in) and are what the host tests check.

#ifndef MYIOSDECK_IOS_AFD_H
#define MYIOSDECK_IOS_AFD_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* a guest address of an AFD request -> host pointer, for a caller whose window base is base
 * (0: a 64-bit caller, the value is used as it is) */
static inline void *ios_afd_user_ptr_at( uint64_t base, uint64_t value )
{
    if (!base) return (void *)(uintptr_t)value;
    if (!value) return NULL;
    return (void *)(uintptr_t)(base + (uint32_t)value);
}

/* the GET_EVENTS input-buffer argument v (already converted by the WoW64 layer as an address)
 * -> the event handle value to pass to the server */
static inline uint64_t ios_afd_event_handle_at( uint64_t base, uint64_t v, int enabled )
{
    if (!enabled || !base) return v;
    if (v >= base && v - base <= 0xffffffffull)
        return (uint64_t)(int64_t)(int32_t)(uint32_t)(v - base);
    return v;
}

/* the value of MADEIRA_AFD_EVENT_HANDLE -> whether the correction is on (only "0" turns it
 * off) */
static inline int ios_afd_event_env_enabled( const char *value )
{
    return !(value && !strcmp( value, "0" ));
}

#ifndef IOS_AFD_PURE_ONLY

extern unsigned long ios_wow_base( void );   /* spec 06 / spec 10 Part E3 */

/* the calling program is 32-bit (replaces in_wow64_call() in socket.c) */
static inline int ios_afd_is32( void )
{
    return ios_wow_base() != 0;
}

/* replaces u64_to_user_ptr() in socket.c: every use there is a guest address of an AFD
 * request */
static inline void *ios_afd_user_ptr( uint64_t value )
{
    return ios_afd_user_ptr_at( ios_wow_base(), value );
}

#endif /* IOS_AFD_PURE_ONLY */

#endif /* MYIOSDECK_IOS_AFD_H */
