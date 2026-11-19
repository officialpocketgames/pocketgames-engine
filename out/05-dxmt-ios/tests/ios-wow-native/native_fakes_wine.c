/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */
/*
 * Spec 33 Part B, host tests: fakes of what the app links next to the ios_wow_native library,
 * compiled against Wine 11.4's headers so their calling conventions are Wine's:
 *   - the unix side's 64-bit call table (dxmt_winemetal_unix_call_funcs): every entry counts the
 *     call and returns success without touching the arguments, so every Metal object is NULL;
 *   - win32u's NtUserCall* entries and NtUserEnumDisplaySettings: one 1280x720 monitor, one
 *     800x600 window at (100, 50);
 *   - ntdll's NtAllocateVirtualMemory / NtFreeVirtualMemory and spec 06's ios_wow_base: two
 *     guest windows, A and B, in a PROT_NONE reservation; the calling thread picks one.
 * (The DXSO translator's fakes are in test_native_seam.cpp, against airconv_public.h.)
 */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntuser.h"

/* ---- the unix side's 64-bit table ---- */

static atomic_ulong unix_calls;

static NTSTATUS fake_unix_call( void *args )
{
    (void)args;
    atomic_fetch_add( &unix_calls, 1 );
    return 0;
}

const void *dxmt_winemetal_unix_call_funcs[512] = { [0 ... 511] = (const void *)fake_unix_call };

unsigned long fake_unix_calls( void )
{
    return atomic_load( &unix_calls );
}

/* ---- win32u ---- */

#define FAKE_HWND     ((HWND)(ULONG_PTR)0x10020)
#define FAKE_MONITOR  ((HMONITOR)(ULONG_PTR)0x10001)

ULONG_PTR WINAPI NtUserCallHwnd( HWND hwnd, DWORD code )
{
    switch (code)
    {
    case NtUserCallHwnd_IsWindow:
    case NtUserCallHwnd_IsWindowVisible:
        return hwnd == FAKE_HWND;
    default:
        return 0;
    }
}

ULONG_PTR WINAPI NtUserCallHwndParam( HWND hwnd, DWORD_PTR param, DWORD code )
{
    if (hwnd != FAKE_HWND) return 0;
    switch (code)
    {
    case NtUserCallHwndParam_GetClientRect:
    {
        struct get_window_rects_params *params = (void *)param;
        SetRect( params->rect, 0, 0, 800, 600 );
        return TRUE;
    }
    case NtUserCallHwndParam_ClientToScreen:
    {
        POINT *pt = (void *)param;
        pt->x += 100;
        pt->y += 50;
        return TRUE;
    }
    case NtUserCallHwndParam_MonitorFromWindow:
        return HandleToUlong( FAKE_MONITOR );
    default:
        return 0;
    }
}

ULONG_PTR WINAPI NtUserCallOneParam( ULONG_PTR arg, ULONG code )
{
    if (code == NtUserCallOneParam_GetSystemMetrics)
        return arg == SM_CXSCREEN ? 1280 : arg == SM_CYSCREEN ? 720 : 0;
    return 0;
}

ULONG_PTR WINAPI NtUserCallTwoParam( ULONG_PTR arg1, ULONG_PTR arg2, ULONG code )
{
    switch (code)
    {
    case NtUserCallTwoParam_GetMonitorInfo:
    {
        MONITORINFO *info = (void *)arg2;
        if (arg1 != HandleToUlong( FAKE_MONITOR )) return FALSE;
        SetRect( &info->rcMonitor, 0, 0, 1280, 720 );
        SetRect( &info->rcWork, 0, 0, 1280, 700 );
        info->dwFlags = MONITORINFOF_PRIMARY;
        return TRUE;
    }
    case NtUserCallTwoParam_MonitorFromRect:
        return HandleToUlong( FAKE_MONITOR );
    default:
        return 0;
    }
}

BOOL WINAPI NtUserEnumDisplaySettings( UNICODE_STRING *device, DWORD mode, DEVMODEW *dm, DWORD flags )
{
    static const DWORD sizes[][2] = { { 640, 480 }, { 800, 600 }, { 1280, 720 } };
    (void)flags;
    if (!device || device->Length) return FALSE;
    if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS) mode = 2;
    if (mode > 2) return FALSE;
    dm->dmPelsWidth = sizes[mode][0];
    dm->dmPelsHeight = sizes[mode][1];
    dm->dmBitsPerPel = 32;
    dm->dmDisplayFrequency = 60;
    return TRUE;
}

/* ---- ntdll's memory calls and ios_wow_base: two guest windows ---- */

static uintptr_t windows[2], next_free[2];
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int current_window = 1; /* 0: none, 1: A, 2: B */

static void reserve_windows( void )
{
    if (windows[0]) return;
    void *r = mmap( NULL, (size_t)12 << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0 );
    uintptr_t base = ((uintptr_t)r + 0xffffffffull) & ~(uintptr_t)0xffffffffull;
    windows[0] = base;
    windows[1] = base + 0x100000000ull;
    next_free[0] = windows[0] + 0x10000;
    next_free[1] = windows[1] + 0x10000;
}

void fake_set_window( int which )
{
    pthread_mutex_lock( &window_lock );
    reserve_windows();
    pthread_mutex_unlock( &window_lock );
    current_window = which;
}

uintptr_t fake_window_base( int which )
{
    return which ? windows[which - 1] : 0;
}

unsigned long ios_wow_base( void )
{
    return current_window ? (unsigned long)windows[current_window - 1] : 0;
}

NTSTATUS WINAPI NtAllocateVirtualMemory( HANDLE process, PVOID *ret, ULONG_PTR zero_bits, SIZE_T *size_ptr,
                                         ULONG type, ULONG protect )
{
    void *p;
    int w = current_window - 1;
    (void)process; (void)zero_bits; (void)type; (void)protect;
    if (w < 0) return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock( &window_lock );
    p = mmap( (void *)next_free[w], *size_ptr, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0 );
    next_free[w] += *size_ptr;
    pthread_mutex_unlock( &window_lock );
    if (p == MAP_FAILED) return STATUS_NO_MEMORY;
    *ret = p;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtFreeVirtualMemory( HANDLE process, PVOID *addr_ptr, SIZE_T *size_ptr, ULONG type )
{
    (void)process; (void)addr_ptr; (void)size_ptr; (void)type;
    return STATUS_SUCCESS; /* the reservation stays; this test never runs out */
}
