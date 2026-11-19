/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 E1 and H: the thunk's exports and the decision which frontend serves the process.
 *
 * At the program's first Direct3DCreate9 or Direct3DCreate9Ex the thunk takes the native path
 * when PG_D3D9_NATIVE is "1" and its unix library's registration call returns a gate. Otherwise
 * it loads d3d9_dxmt32.dll (the full i386 frontend, renamed by the app's build) and calls the
 * same export there; from then on that frontend serves the process. One log line says which. */

#include "d3d9_thunk.h"
#include "d3d9_thunk_pure.h"

#include <stdarg.h>
#include <stdio.h>

/* Wine's unix calls: winecrt0 finds the module's unix library, ntdll exports the dispatcher. */
typedef UINT64 d3d9t_unixlib_handle;
extern d3d9t_unixlib_handle __wine_unixlib_handle;
extern LONG (WINAPI *__wine_unix_call_dispatcher)(d3d9t_unixlib_handle, unsigned int, void *);
extern LONG WINAPI __wine_init_unix_call(void);

/* The stubs jump through this (gen/d3d9w_stubs_i386.S): the guest address of the app's gate. */
uint32_t d3d9w_gate;

enum { PATH_UNDECIDED, PATH_NATIVE, PATH_FULL };

static volatile LONG path = PATH_UNDECIDED;
static CRITICAL_SECTION decide_cs;
static HMODULE volatile full_module;
static volatile LONG full_load_failed;
static volatile LONG local_perf_level;

/* ------------------------------------------------------------------ logging */

static int (__cdecl *volatile dbg_output)(const char *);

void d3d9t_log(const char *fmt, ...)
{
    char buf[900];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    buf[n] = '\n';
    buf[n + 1] = 0;
    if (!dbg_output)
    {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");

        if (ntdll)
            dbg_output = (int (__cdecl *)(const char *))(void *)GetProcAddress(ntdll, "__wine_dbg_output");
    }
    if (dbg_output)
        dbg_output(buf);
    else
        OutputDebugStringA(buf);
}

/* ------------------------------------------------------------------ the full frontend */

static HMODULE load_full(void)
{
    HMODULE m = full_module;

    if (m || full_load_failed)
        return m;
    if (!(m = LoadLibraryW(L"d3d9_dxmt32.dll")))
    {
        if (!InterlockedExchange(&full_load_failed, 1))
            d3d9t_log("d3d9: d3d9_dxmt32.dll, the full Direct3D 9 frontend, could not be loaded "
                      "(error %lu)", GetLastError());
        return NULL;
    }
    if (InterlockedCompareExchangePointer((void *volatile *)&full_module, m, NULL))
        FreeLibrary(m);   /* another thread loaded it first */
    return full_module;
}

static FARPROC full_proc(const char *name)
{
    HMODULE m = load_full();

    return m ? GetProcAddress(m, name) : NULL;
}

/* For the exports that do not decide the path: the full frontend only when it is already
 * loaded. */
static FARPROC loaded_full_proc(const char *name)
{
    HMODULE m = full_module;

    return m ? GetProcAddress(m, name) : NULL;
}

/* ------------------------------------------------------------------ Part H */

/* decide_cs held. */
static LONG use_full(const char *why)
{
    path = PATH_FULL;
    d3d9t_log("d3d9: the full Direct3D 9 frontend (d3d9_dxmt32.dll) serves this process: %s", why);
    return PATH_FULL;
}

/* decide_cs held. Hands the dispatch the vtables and makes this process the owner
 * (clarification 10). */
static uint32_t init_native(void)
{
    uint32_t vt[D3D9W_NUM_KINDS], shared_guest = 0, status;
    unsigned int k;

    for (k = 0; k < D3D9W_NUM_KINDS; k++)
        vt[k] = (uint32_t)(uintptr_t)d3d9w_vtables[k];
    status = d3d9w_s_Exports_Init(D3D9W_ABI_VERSION, vt, D3D9W_NUM_KINDS, &shared_guest);
    if (status == D3D9W_INIT_OK)
        d3d9t_set_shared(shared_guest);
    return status;
}

/* decide_cs held. */
static LONG decide(void)
{
    struct d3d9w_register_params params;
    char value[8];
    DWORD len;
    LONG status;

    len = GetEnvironmentVariableA("PG_D3D9_NATIVE", value, sizeof(value));
    if (len >= sizeof(value) || !d3d9t_env_wants_native(value, len))
        return use_full("PG_D3D9_NATIVE is not 1");
    if (__wine_init_unix_call())
        return use_full("the native library's unix side is missing (no d3d9_unix_call_wow64_funcs)");
    memset(&params, 0, sizeof(params));
    params.version = D3D9W_ABI_VERSION;
    if ((status = __wine_unix_call_dispatcher(__wine_unixlib_handle, 0, &params)))
    {
        d3d9t_log("d3d9: the registration call failed with status %#lx", (unsigned long)status);
        return use_full("the registration call failed");
    }
    if (!params.gate)
        return use_full(d3d9t_nogate_reason(params.reason));
    d3d9w_gate = params.gate;
    switch (init_native())
    {
    case D3D9W_INIT_OK:
        break;
    case D3D9W_INIT_BUSY:
        return use_full("another process uses the native frontend (spec 33 clarification 10)");
    case D3D9W_INIT_VERSION:
        return use_full(d3d9t_nogate_reason(D3D9W_NOGATE_VERSION));
    default:
        return use_full("the native library could not set up its proxy memory");
    }
    d3d9t_log("d3d9: the native Direct3D 9 frontend serves this process through the gate at %#x "
              "(spec 33)", d3d9w_gate);
    return PATH_NATIVE;
}

static LONG current_path(void)
{
    LONG p = path;

    if (p != PATH_UNDECIDED)
        return p;
    EnterCriticalSection(&decide_cs);
    if (path == PATH_UNDECIDED)
        path = decide();
    p = path;
    LeaveCriticalSection(&decide_cs);
    return p;
}

/* A create answered "busy": another process took the native frontend over while this one had
 * no objects. Take it back if that process has none either; otherwise this process goes to the
 * full frontend for good. */
static BOOL reclaim_native(void)
{
    BOOL ok;

    EnterCriticalSection(&decide_cs);
    ok = path == PATH_NATIVE && init_native() == D3D9W_INIT_OK;
    if (!ok && path == PATH_NATIVE)
        use_full("another process uses the native frontend (spec 33 clarification 10)");
    LeaveCriticalSection(&decide_cs);
    return ok;
}

/* ------------------------------------------------------------------ the exports (E1) */

IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version)
{
    IDirect3D9 *(WINAPI *full)(UINT);
    int attempt;

    for (attempt = 0; attempt < 2 && current_path() == PATH_NATIVE; attempt++)
    {
        uint32_t status = D3D9W_CREATE_FAILED;
        uint32_t proxy = d3d9w_s_Exports_Direct3DCreate9(sdk_version, &status);

        if (status != D3D9W_CREATE_BUSY)
            return (IDirect3D9 *)(uintptr_t)proxy;
        if (!reclaim_native())
            break;
    }
    if (!(full = (IDirect3D9 *(WINAPI *)(UINT))(void *)full_proc("Direct3DCreate9")))
        return NULL;
    return full(sdk_version);
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk_version, IDirect3D9Ex **out)
{
    HRESULT (WINAPI *full)(UINT, IDirect3D9Ex **);
    int attempt;

    for (attempt = 0; attempt < 2 && current_path() == PATH_NATIVE; attempt++)
    {
        uint32_t status = D3D9W_CREATE_FAILED;
        HRESULT hr = d3d9w_s_Exports_Direct3DCreate9Ex(sdk_version, out, &status);

        if (status != D3D9W_CREATE_BUSY)
            return hr;
        if (!reclaim_native())
            break;
    }
    if (!(full = (HRESULT (WINAPI *)(UINT, IDirect3D9Ex **))(void *)full_proc("Direct3DCreate9Ex")))
        return D3DERR_NOTAVAILABLE;
    return full(sdk_version, out);
}

/* The rest go to the full frontend when it serves the process (or is loaded), to the gate on
 * the native path, and before either is decided behave as both frontends do. */
#define FORWARD(name) \
    __typeof__(&name) f_ = path == PATH_NATIVE ? NULL : (__typeof__(&name))(void *)loaded_full_proc(#name)

int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name)
{
    FORWARD(D3DPERF_BeginEvent);

    if (f_)
        return f_(color, name);
    if (path == PATH_NATIVE)
        return d3d9w_s_Exports_D3DPERF_BeginEvent(color, name);
    return InterlockedIncrement(&local_perf_level) - 1;
}

int WINAPI D3DPERF_EndEvent(void)
{
    FORWARD(D3DPERF_EndEvent);

    if (f_)
        return f_();
    if (path == PATH_NATIVE)
        return d3d9w_s_Exports_D3DPERF_EndEvent();
    return InterlockedDecrement(&local_perf_level);
}

DWORD WINAPI D3DPERF_GetStatus(void)
{
    FORWARD(D3DPERF_GetStatus);

    if (f_)
        return f_();
    if (path == PATH_NATIVE)
        return d3d9w_s_Exports_D3DPERF_GetStatus();
    return 0;
}

BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
    FORWARD(D3DPERF_QueryRepeatFrame);

    if (f_)
        return f_();
    if (path == PATH_NATIVE)
        return d3d9w_s_Exports_D3DPERF_QueryRepeatFrame();
    return FALSE;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name)
{
    FORWARD(D3DPERF_SetMarker);

    if (f_)
        f_(color, name);
    else if (path == PATH_NATIVE)
        d3d9w_s_Exports_D3DPERF_SetMarker(color, name);
}

void WINAPI D3DPERF_SetOptions(DWORD options)
{
    FORWARD(D3DPERF_SetOptions);

    if (f_)
        f_(options);
    else if (path == PATH_NATIVE)
        d3d9w_s_Exports_D3DPERF_SetOptions(options);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name)
{
    FORWARD(D3DPERF_SetRegion);

    if (f_)
        f_(color, name);
    else if (path == PATH_NATIVE)
        d3d9w_s_Exports_D3DPERF_SetRegion(color, name);
}

void WINAPI DebugSetLevel(DWORD level)
{
    FORWARD(DebugSetLevel);

    if (f_)
        f_(level);
    else if (path == PATH_NATIVE)
        d3d9w_s_Exports_DebugSetLevel(level);
}

BOOL WINAPI DebugSetMute(void)
{
    FORWARD(DebugSetMute);

    if (f_)
        return f_();
    if (path == PATH_NATIVE)
        return d3d9w_s_Exports_DebugSetMute();
    return TRUE;
}

/* Clarification 8: the validator calls back into the program, which native code cannot do, so
 * it always comes from the full frontend. */
void *WINAPI Direct3DShaderValidatorCreate9(void)
{
    void *(WINAPI *full)(void) = (void *(WINAPI *)(void))(void *)full_proc("Direct3DShaderValidatorCreate9");

    return full ? full() : NULL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&decide_cs);
        d3d9t_resource_init();
        d3d9t_device_init();
    }
    return TRUE;
}
