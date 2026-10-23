// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part A4 and A5: guest pointers of 32-bit callers, and the calling pseudo-process.
// Plain C types only, so files that may not include Wine headers can use it too.
//
// A 32-bit (WoW64) pseudo-process owns a window [B, B + 4 GB) of the app's address space; the
// address a its x86 code sees is host address B + a (spec 10 Part E). Every guest pointer a
// ..._wow64_funcs entry reads is converted with spec 06's rule 1: 0 stays NULL, anything else
// becomes ios_wow_base() + value. A pointer written back into a 32-bit field must lie inside
// the caller's window; otherwise 0 is written and the call fails (never truncate).

#ifndef IOS_MEDIA_WOW_H
#define IOS_MEDIA_WOW_H

#include <stddef.h>
#include <stdint.h>

/* provided by ntdll's unix side */
extern unsigned long ios_wow_base( void );            /* spec 06 / spec 10 Part E3 */
extern int ios_wow_in_window( const void *addr );     /* spec 10 Part E9 */
extern void *ios_current_peb( void );                 /* spec 09 Part B1 */
/* spec 22 C6: fn runs with an ended pseudo-process's PEB after its threads are gone */
extern int ios_process_end_hook_add( void (*fn)( const void *peb ) );

/* rule A4 into the host: a 32-bit guest address -> host pointer (0 stays NULL) */
static inline void *iom_guest_ptr( uint32_t value )
{
    return value ? (void *)(uintptr_t)(ios_wow_base() + (uintptr_t)value) : NULL;
}

/* rule A4 back to the program: the guest address of a host pointer, for a 32-bit field.
 * NULL gives 0 and succeeds; a pointer outside the caller's window gives 0 and fails. */
static inline int iom_guest_addr( const void *host, uint32_t *out )
{
    unsigned long base;

    *out = 0;
    if (!host) return 1;
    base = ios_wow_base();
    if (!base || !ios_wow_in_window( host )) return 0;
    *out = (uint32_t)((uintptr_t)host - base);
    return 1;
}

#endif /* IOS_MEDIA_WOW_H */
