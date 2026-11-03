// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Parts B1 and E: the pure parts of opengl32's iOS wrappers (no Wine types; the host
// tests build them on Linux): the answers while OpenGL is off, the switch, and the call
// statistics' report.

#ifndef OPENGL_IOS_PURE_H
#define OPENGL_IOS_PURE_H

#include <stddef.h>
#include <stdint.h>

/* NTSTATUS values the wrappers return */
#define OPENGL_IOS_STATUS_SUCCESS        0x00000000u
#define OPENGL_IOS_STATUS_NOT_SUPPORTED  0xc00000bbu

/* opengl32's first three unix entries, in Wine 11.4's enum unix_funcs order (checked against
 * the enum where the generated tables are built) */
#define OPENGL_IOS_PROCESS_ATTACH  0u
#define OPENGL_IOS_THREAD_ATTACH   1u
#define OPENGL_IOS_PROCESS_DETACH  2u

/* B1: what entry index answers while OpenGL is off for the calling process: the three setup
 * entries succeed, every other is STATUS_NOT_SUPPORTED */
static inline uint32_t opengl_ios_off_status( unsigned int index )
{
    return index <= OPENGL_IOS_PROCESS_DETACH ? OPENGL_IOS_STATUS_SUCCESS : OPENGL_IOS_STATUS_NOT_SUPPORTED;
}

/* B1 and E1: a switch is on only when its value is exactly "1" */
int opengl_ios_switch_on( const char *value );

/* E2: a report is due at a swap 10 s or more after the period started (0: no counted call yet) */
#define OPENGL_IOS_REPORT_NS (10ull * 1000 * 1000 * 1000)
int opengl_ios_report_due( uint64_t now_ns, uint64_t period_start_ns );

/* E2: the report line for one period of `seconds`:
 *   [opengl] calls: <total>/s, <per swap>/swap (<swaps>/s), via gate <g>/s; top: <name> <n>/s (<m>/swap), ...
 * with the 16 busiest entries (ties: the lower index first). counts[swap_index] is the number of
 * swaps; names[i] is entry i's name. Returns the length written (always NUL-terminated). */
#define OPENGL_IOS_REPORT_TOP 16u
size_t opengl_ios_format_report( char *buf, size_t size, const uint32_t *counts, unsigned int n,
                                 const char *const *names, unsigned int swap_index, uint64_t gate,
                                 double seconds );

#endif /* OPENGL_IOS_PURE_H */
