// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part B: pointers inside the argument blocks of 32-bit (WoW64) unix calls.
//
// A 32-bit caller lives in a guest window at host base B = ios_wow_base() (spec 10 Part E). The
// WoW64 layer converts only the outer args pointer of a unix call; every pointer inside the
// block, and inside the structs it points to, is a 32-bit guest address:
//   into the host:   guest a  -> B + a,  0 -> NULL
//   back to guest:   host p   -> p - B,  NULL -> 0   (ios_wow_guest_addr(), spec 10 E9)
// With B = 0 (a 64-bit caller, or a build without guest windows) both are exactly upstream's
// ULongToPtr / PtrToUlong. Handles, key and session handles, sizes and flags are not
// addresses and are never passed through these.
//
// The *_at() forms take B explicitly; they are what the host tests check. The thunks use
// ios_g2h() / ios_h2g(), which take B from the calling thread.
//
// Include after the Wine headers (ULONG). Every function here is static inline, so no file
// that includes it defines a symbol.

#ifndef MYIOSDECK_IOS_GUEST_H
#define MYIOSDECK_IOS_GUEST_H

#include <stdint.h>
#include <stddef.h>

/* guest address -> host pointer for a window at base */
static inline void *ios_guest_to_host_at( uint64_t base, uint32_t guest )
{
    return guest ? (void *)(uintptr_t)(base + guest) : NULL;
}

/* host pointer -> guest address for a window at base */
static inline uint32_t ios_host_to_guest_at( uint64_t base, const void *host )
{
    return host ? (uint32_t)((uintptr_t)host - base) : 0;
}

/* An args block whose guest address is 0: a 32-bit NULL may reach the unix side as B itself
 * (dnsapi's set_serverlist, Part H3). */
static inline int ios_guest_args_is_null( uint64_t base, const void *args )
{
    return !args || (uintptr_t)args == base;
}

#ifndef IOS_GUEST_PURE_ONLY

extern unsigned long ios_wow_base( void );               /* spec 06 / spec 10 Part E3 */
extern ULONG ios_wow_guest_addr( const void *host );     /* spec 10 Part E9 */

/* guest address of the calling program -> host pointer (replaces ULongToPtr in thunks) */
static inline void *ios_g2h( ULONG guest )
{
    return ios_guest_to_host_at( ios_wow_base(), guest );
}

/* host pointer -> guest address of the calling program (replaces PtrToUlong in thunks) */
static inline ULONG ios_h2g( const void *host )
{
    return host ? ios_wow_guest_addr( host ) : 0;
}

#endif /* IOS_GUEST_PURE_ONLY */

#endif /* MYIOSDECK_IOS_GUEST_H */
