/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 J2: stand-ins for Wine's winecrt0 and ntdll unix-call symbols, so the host tests can
 * link the i386 thunk with llvm-mingw alone (no Wine i386 build). Never part of a real build:
 * build-pe.sh links Wine's own libraries. */

#include <windows.h>
#include <stdint.h>

uint64_t __wine_unixlib_handle;

static LONG WINAPI no_unix_call(uint64_t handle, unsigned int code, void *args)
{
    (void)handle;
    (void)code;
    (void)args;
    return (LONG)0xc0000135;   /* STATUS_DLL_NOT_FOUND */
}

LONG (WINAPI *__wine_unix_call_dispatcher)(uint64_t, unsigned int, void *) = no_unix_call;

LONG WINAPI __wine_init_unix_call(void)
{
    return (LONG)0xc0000135;
}
