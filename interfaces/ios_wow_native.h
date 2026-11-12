// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The contract between a native library that serves 32-bit (i386, WoW64) programs and the
// app's 32-bit CPU (spec 33). The i386 side of such a library is a thin PE DLL whose entry
// points are stubs:
//
//     mov  eax, IOS_WOW_NATIVE_ID(lib, method)
//     jmp  dword ptr [gate]          ; gate: the guest address ios_wow_native_register returned
//
// The stub is reached by an ordinary stdcall call (a COM method or an export), so at the gate
// [esp] is the caller's return address and the arguments start at esp + 4. The CPU calls the
// registered function on the calling thread and then returns to the caller as stdcall does:
// eax (or st(0)) set as below, eip = [esp], esp += 4 + ret.pop. ebx, esi, edi, ebp and esp are
// preserved; ecx, edx and the flags are not; the direction flag is clear on entry and exit.
//
// Section A: what the app's CPU PROVIDES (the library calls these). All are weak on the
//            library's side: a CPU without a gate (FXI32) provides none of them, and the
//            library then reports "no gate" and its i386 DLL falls back (spec 33 H).
// Section B: what the library provides (one function per library, registered at run time).

#ifndef MYIOSDECK_IOS_WOW_NATIVE_H
#define MYIOSDECK_IOS_WOW_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Library numbers. 1 is Direct3D 9 (spec 33); 2 is reserved for XAudio2. */
#define IOS_WOW_NATIVE_LIB_D3D9   1u
#define IOS_WOW_NATIVE_ID(lib, method) ((uint32_t)(lib) << 24 | ((uint32_t)(method) & 0xffffffu))

/* How the gate returns to the caller. */
enum ios_wow_native_kind
{
    IOS_WOW_NATIVE_RET_EAX = 0,   /* eax = ret.eax */
    IOS_WOW_NATIVE_RET_ST0 = 1,   /* a float pushed on the x87 stack as st(0); eax undefined.
                                     The app's gate (FXN32, FXW) does not support it: it logs once
                                     and gives eax (spec 33 clarification 3). */
};

struct ios_wow_native_ret
{
    uint32_t eax;     /* the 32-bit result */
    uint32_t pop;     /* bytes of arguments the callee pops (stdcall), after the return address */
    uint32_t kind;    /* enum ios_wow_native_kind */
    float    st0;     /* the result when kind is IOS_WOW_NATIVE_RET_ST0 */
};

/* Section B. Runs on the calling guest thread, between two guest instructions, with the
 * thread counted as "in a call" (not in x86) by the CPU's profiler, suspension and
 * GetThreadContext. It may block and may call Wine's unix-side functions that never call back
 * into guest code (spec 33 B4). It must not call guest code. id's low 24 bits are the
 * library's method number; esp is the guest stack pointer at the gate. */
typedef void (*ios_wow_native_fn)( uint32_t id, uint32_t esp, struct ios_wow_native_ret *ret );

/* ---- Section A ---- */

/* Registers fn for library lib (1..255) in the calling process and returns the guest address
 * stubs jump to, or 0 when the calling thread's CPU has no gate. Registering the same lib again
 * in the same process replaces fn and returns the same address. Thread-safe. */
uint32_t ios_wow_native_register( uint32_t lib, ios_wow_native_fn fn ) __attribute__((weak));

/* (Guest address a is host ios_wow_base() + a: the engine's own function, spec 06.) */

/* Copies n bytes between host memory and the calling process's guest memory: returns 1 on
 * success, 0 for a range that cannot be valid (it starts below 64 KB or goes past 4 GB; nothing
 * is copied then). Any other inaccessible byte faults, and a fault inside a gate call is the
 * program's access violation at the gate, as a bad pointer handed to a Windows system DLL is
 * (spec 33 clarification 4). guest is a guest address. Usable from a gate call only: on any other
 * thread they return 0. */
int ios_wow_native_read( void *dst, uint32_t guest, size_t n ) __attribute__((weak));
int ios_wow_native_write( uint32_t guest, const void *src, size_t n ) __attribute__((weak));

#ifdef __cplusplus
}
#endif

#endif /* MYIOSDECK_IOS_WOW_NATIVE_H */
