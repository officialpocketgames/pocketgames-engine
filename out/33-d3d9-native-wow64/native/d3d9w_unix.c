/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 D2: the thunk's own unix library. One call, entry 0: at the thunk's first
 * Direct3DCreate9(Ex), register d3d9w_dispatch with the app's 32-bit CPU and hand back the gate
 * address (0: no gate, Part H falls back to the full frontend).
 *
 * The library is found the way spec 05 has winemetal's found: by name (spec 09 A and its
 * clarification 6). The thunk is d3d9.dll, so its tables are d3d9_unix_call_funcs and
 * d3d9_unix_call_wow64_funcs, with default visibility so the app's dead-stripping keeps them.
 * Only i386 code calls it: the 64-bit entry answers STATUS_NOT_IMPLEMENTED (64-bit programs keep
 * the ARM64EC frontend, which has no unix library of its own). */

#include "d3d9w_dispatch.h"

typedef LONG d3d9w_status;   /* NTSTATUS, which windows.h does not define */

#define D3D9W_STATUS_SUCCESS          0x00000000
#define D3D9W_STATUS_NOT_IMPLEMENTED  ((d3d9w_status)0xC0000002)

static d3d9w_status d3d9w_register_64(void *args)
{
    (void)args;
    return D3D9W_STATUS_NOT_IMPLEMENTED;
}

/* WoW64 entry (spec 06 C): Wine converts the outer pointer; struct d3d9w_register_params has
 * only 32-bit fields, so nothing inside needs converting. */
static d3d9w_status d3d9w_register_wow64(void *args)
{
    struct d3d9w_register_params *p = args;
    uint32_t gate;

    p->gate = 0;
    p->reason = D3D9W_NOGATE_NONE;
    if (p->version != D3D9W_ABI_VERSION)
    {
        p->reason = D3D9W_NOGATE_VERSION;
        return D3D9W_STATUS_SUCCESS;
    }
    if (!ios_wow_base())
    {
        p->reason = D3D9W_NOGATE_NOT_WOW64;
        return D3D9W_STATUS_SUCCESS;
    }
    /* Section A is weak: a CPU without a gate or an App Store build has none. */
    if (!ios_wow_native_register)
    {
        p->reason = D3D9W_NOGATE_NO_CPU_GATE;
        return D3D9W_STATUS_SUCCESS;
    }
    if (!(gate = ios_wow_native_register(IOS_WOW_NATIVE_LIB_D3D9, d3d9w_dispatch)))
        p->reason = D3D9W_NOGATE_REFUSED;
    p->gate = gate;
    return D3D9W_STATUS_SUCCESS;
}

__attribute__((visibility("default"), used))
const void *d3d9_unix_call_funcs[] =
{
    (const void *)d3d9w_register_64,
};

__attribute__((visibility("default"), used))
const void *d3d9_unix_call_wow64_funcs[] =
{
    (const void *)d3d9w_register_wow64,
};
