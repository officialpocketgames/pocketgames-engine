/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 D3: the native dispatch's internal header. Everything the generated per-method
 * functions (gen/d3d9w_dispatch_gen.c) and the hand-written helpers share.
 *
 * Built for aarch64-apple-ios by build-ios.sh and for the Linux host by the tests, against
 * Wine 11.4's include/ (Windows types, d3d9.h C vtables). */

#ifndef D3D9W_DISPATCH_H
#define D3D9W_DISPATCH_H

#define COBJMACROS
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d9.h>

#include "ios_wow_native.h"     /* interfaces/ */
#include "d3d9w_native.h"       /* interfaces/: the seam with the native frontend */
#include "d3d9w_ids.h"          /* gen/ */
#include "d3d9w_proxy.h"        /* include/ */
#include "d3d9w_layout.h"       /* include/ */

/* Spec 06: the calling thread's guest window base (Wine's iOS unix side defines it). */
unsigned long ios_wow_base(void);

/* Spec 33 C1: session N's allocator (out/33-d3d9-native-wow64/alloc/). Committed read-write
 * memory inside the calling process's window: returns the host pointer, *guest its address. */
void *ios_wow_window_alloc(size_t size, size_t align, uint32_t *guest);

/* ------------------------------------------------------------------ the method table */

#define D3D9W_MAX_SLOTS 256

struct d3d9w_method
{
    void (*fn)(uint32_t esp, struct ios_wow_native_ret *ret);
    uint8_t words;          /* stdcall argument words, This included */
    const char *name;
    uint32_t refused;       /* EAX of a refused call (D4): D3DERR_INVALIDCALL, or 0 */
};

extern const struct d3d9w_method d3d9w_methods[D3D9W_NUM_KINDS][D3D9W_MAX_SLOTS];
extern const struct d3d9w_method d3d9w_extras[D3D9W_MAX_SLOTS];

/* The native function the registration call hands the gate (ios_wow_native_fn). */
void d3d9w_dispatch(uint32_t id, uint32_t esp, struct ios_wow_native_ret *ret);

/* ------------------------------------------------------------------ the proxy table */

enum d3d9w_slot_state { D3D9W_SLOT_FREE, D3D9W_SLOT_LIVE, D3D9W_SLOT_ZOMBIE, D3D9W_SLOT_RESERVED };

struct d3d9w_slot
{
    void *native;           /* the native interface pointer; NULL unless LIVE */
    uint32_t next_free;     /* free list link while FREE */
    uint8_t kind;
    uint8_t state;
    uint16_t gen;           /* bumped when the slot is freed (12 bits used in the handle) */
};

struct d3d9w_chunk
{
    uint8_t *host;          /* host address of the 64 KB of guest memory */
    uint32_t guest;
    struct d3d9w_slot slots[D3D9W_SLOTS_PER_CHUNK];
};

/* guest address >> 16 -> chunk index + 1 (0: not a proxy chunk). Written under the table lock,
 * read without it. */
extern uint32_t d3d9w_chunk_of_page[65536];
extern struct d3d9w_chunk *d3d9w_chunks[D3D9W_MAX_CHUNKS];

/* The native object behind a proxy's guest address when it is a live proxy of a kind in mask,
 * else NULL. No lock and no guest memory access: the address says which slot it is. */
static inline void *d3d9w_native_of(uint32_t g, uint32_t mask)
{
    uint32_t c = __atomic_load_n(&d3d9w_chunk_of_page[g >> 16], __ATOMIC_ACQUIRE);
    struct d3d9w_slot *s;
    void *n;

    if (!c || (g & (D3D9W_SLOT_BYTES - 1)))
        return NULL;
    s = &d3d9w_chunks[c - 1]->slots[(g & (D3D9W_CHUNK_BYTES - 1)) / D3D9W_SLOT_BYTES];
    n = __atomic_load_n(&s->native, __ATOMIC_ACQUIRE);
    if (!n || !(mask & (1u << s->kind)))
        return NULL;
    return n;
}

/* This of a method of kind k. */
#define d3d9w_this(g, k) ((void *)d3d9w_native_of((g), 1u << (k)))

/* Writes the proxy of native (creating it, of a kind in mask) to the guest pointer g_out.
 * native may be NULL (writes 0). Returns 0 when no proxy could be made: then native's
 * reference is released and 0 written. */
int d3d9w_put_iface(uint32_t g_out, void *native, uint32_t mask);
/* As d3d9w_put_iface for QueryInterface and GetContainer: the kind follows from iid; the
 * object This itself keeps its own proxy. Returns 0 (after releasing native) for an
 * interface the table does not know. */
int d3d9w_put_iface_iid(uint32_t g_out, void *native, const GUID *iid, void *This,
                        unsigned int this_kind);
/* The proxy guest address of native, creating it; 0 on failure (no reference is changed). */
uint32_t d3d9w_proxy_for(void *native, uint32_t mask);

/* The native side of the thunk's calls (kind 15). */
uint32_t d3d9w_init(uint32_t version, const uint32_t *vtables, uint32_t count,
                    uint32_t *shared_guest);
uint32_t d3d9w_owner_status(void);   /* D3D9W_CREATE_OK or D3D9W_CREATE_BUSY for the caller */
/* Init, the creates and the end of a process hold this around their work (clarification 13). */
void d3d9w_create_lock(void);
void d3d9w_create_unlock(void);
/* The gate's reserved method: the calling process is ending (clarification 13). */
void d3d9w_process_end(void);
HRESULT d3d9w_unpin(uint32_t proxy);
HRESULT d3d9w_pin(uint32_t proxy);
/* The slot bookkeeping the tests look at. */
uint32_t d3d9w_live_count(void);
void d3d9w_reset_for_tests(void);

/* ------------------------------------------------------------------ guest memory */

/* Clarification 4: a range below 64 KB or past 4 GB can never be valid. */
static inline int d3d9w_guest_ok(uint32_t g, uint64_t n)
{
    return g >= 0x10000u && (uint64_t)g + n <= 0x100000000ull;
}

static inline int d3d9w_read(void *dst, uint32_t g, size_t n)
{
    return ios_wow_native_read(dst, g, n);
}

static inline int d3d9w_write(uint32_t g, const void *src, size_t n)
{
    return ios_wow_native_write(g, src, n);
}

/* The stdcall arguments start at esp + 4, This first. */
static inline int d3d9w_read_args(uint32_t *a, uint32_t esp, size_t n)
{
    return ios_wow_native_read(a, esp + 4, n);
}

/* Host address of guest address g in the caller's window (0 stays NULL). */
static inline void *d3d9w_host_of(uint32_t g)
{
    return g ? (void *)(uintptr_t)(ios_wow_base() + g) : NULL;
}

/* Guest address of a host pointer: 0 for NULL, and 0 with *outside = 1 for a pointer that
 * does not lie in the caller's window (never truncated, spec 06 C2). */
static inline uint32_t d3d9w_guest_of(const void *p, int *outside)
{
    uintptr_t h = (uintptr_t)p, base = ios_wow_base();

    if (!p)
        return 0;
    if (h < base + 0x10000u || h - base >= 0x100000000ull)
    {
        if (outside)
            *outside = 1;
        return 0;
    }
    return (uint32_t)(h - base);
}

static inline float d3d9w_as_float(uint32_t v)
{
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

static inline uint32_t d3d9w_float_bits(float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    return v;
}

/* Arrays the program passes are copied into this stack buffer when they fit (clarification 1:
 * 256 shader-constant registers of 16 bytes), and into a heap buffer otherwise. */
#define D3D9W_STACK_BYTES 4096
#define D3D9W_STACK_ARRAY(name) uint8_t name[D3D9W_STACK_BYTES] __attribute__((aligned(16)))
/* Nothing larger is copied in for one argument. */
#define D3D9W_MAX_COPY (64u << 20)
/* MAXD3DDECLLENGTH plus D3DDECL_END. */
#define D3D9W_MAX_DECL_ELEMENTS 65
/* An interface out pointer native code did not write. */
#define D3D9W_UNSET ((void *)(uintptr_t)1)

/* Copies n bytes from guest address g (0: *p = NULL). On success *p is the copy (stack or
 * *heap, which the caller frees). Returns 0 when the program's pointer is refused or n is too
 * large. n == 0 with g != 0 gives a non-NULL *p and reads nothing. */
int d3d9w_array_in(uint32_t g, uint64_t n, void *stack, size_t stack_size, void **heap, void **p);
/* count interface pointers (proxies of a kind in mask) -> native pointers. */
int d3d9w_iface_array_in(uint32_t g, uint64_t count, uint32_t mask, void *stack,
                         size_t stack_size, void **heap, void **p);
/* F3: shader tokens up to the end token (*ntokens, when not NULL: how many, the end token
 * included). */
int d3d9w_shader_in(uint32_t g, void *stack, size_t stack_size, void **heap, const DWORD **p,
                    size_t *ntokens);
/* F3: vertex elements up to D3DDECL_END, at most D3D9W_MAX_DECL_ELEMENTS. */
int d3d9w_decl_in(uint32_t g, D3DVERTEXELEMENT9 *out);
/* F3: an RGNDATA, header plus nCount rectangles. */
int d3d9w_rgn_in(uint32_t g, void *stack, size_t stack_size, void **heap, const RGNDATA **p);
/* C3: HANDLE *pSharedHandle: the D3D9Ex user-memory pointer (a guest address) or a share
 * handle; native code reads it only. */
int d3d9w_shared_in(uint32_t g, HANDLE *out);
/* The number of vertices a primitive count draws (0 for an unknown type). */
uint64_t d3d9w_prim_vertices(D3DPRIMITIVETYPE type, uint32_t count);
/* A per-thread scratch buffer for UP draws (grows, never shrinks below its use). */
void *d3d9w_scratch(size_t n);

/* ------------------------------------------------------------------ converters (F4) */

static inline void d3d9w_in_D3DPRESENT_PARAMETERS(D3DPRESENT_PARAMETERS *d,
                                                  const struct d3d9w_g_D3DPRESENT_PARAMETERS *s)
{
    d->BackBufferWidth = s->BackBufferWidth;
    d->BackBufferHeight = s->BackBufferHeight;
    d->BackBufferFormat = (D3DFORMAT)s->BackBufferFormat;
    d->BackBufferCount = s->BackBufferCount;
    d->MultiSampleType = (D3DMULTISAMPLE_TYPE)s->MultiSampleType;
    d->MultiSampleQuality = s->MultiSampleQuality;
    d->SwapEffect = (D3DSWAPEFFECT)s->SwapEffect;
    d->hDeviceWindow = (HWND)(uintptr_t)s->hDeviceWindow;
    d->Windowed = (BOOL)s->Windowed;
    d->EnableAutoDepthStencil = (BOOL)s->EnableAutoDepthStencil;
    d->AutoDepthStencilFormat = (D3DFORMAT)s->AutoDepthStencilFormat;
    d->Flags = s->Flags;
    d->FullScreen_RefreshRateInHz = s->FullScreen_RefreshRateInHz;
    d->PresentationInterval = s->PresentationInterval;
}

static inline void d3d9w_out_D3DPRESENT_PARAMETERS(struct d3d9w_g_D3DPRESENT_PARAMETERS *d,
                                                   const D3DPRESENT_PARAMETERS *s)
{
    d->BackBufferWidth = s->BackBufferWidth;
    d->BackBufferHeight = s->BackBufferHeight;
    d->BackBufferFormat = (uint32_t)s->BackBufferFormat;
    d->BackBufferCount = s->BackBufferCount;
    d->MultiSampleType = (uint32_t)s->MultiSampleType;
    d->MultiSampleQuality = s->MultiSampleQuality;
    d->SwapEffect = (uint32_t)s->SwapEffect;
    d->hDeviceWindow = (uint32_t)(uintptr_t)s->hDeviceWindow;
    d->Windowed = (uint32_t)s->Windowed;
    d->EnableAutoDepthStencil = (uint32_t)s->EnableAutoDepthStencil;
    d->AutoDepthStencilFormat = (uint32_t)s->AutoDepthStencilFormat;
    d->Flags = s->Flags;
    d->FullScreen_RefreshRateInHz = s->FullScreen_RefreshRateInHz;
    d->PresentationInterval = s->PresentationInterval;
}

static inline void d3d9w_in_D3DDEVICE_CREATION_PARAMETERS(D3DDEVICE_CREATION_PARAMETERS *d,
                                                          const struct d3d9w_g_D3DDEVICE_CREATION_PARAMETERS *s)
{
    d->AdapterOrdinal = s->AdapterOrdinal;
    d->DeviceType = (D3DDEVTYPE)s->DeviceType;
    d->hFocusWindow = (HWND)(uintptr_t)s->hFocusWindow;
    d->BehaviorFlags = s->BehaviorFlags;
}

static inline void d3d9w_out_D3DDEVICE_CREATION_PARAMETERS(struct d3d9w_g_D3DDEVICE_CREATION_PARAMETERS *d,
                                                           const D3DDEVICE_CREATION_PARAMETERS *s)
{
    d->AdapterOrdinal = s->AdapterOrdinal;
    d->DeviceType = (uint32_t)s->DeviceType;
    d->hFocusWindow = (uint32_t)(uintptr_t)s->hFocusWindow;
    d->BehaviorFlags = s->BehaviorFlags;
}

/* pBits: F5. Into native code a guest address becomes its host address; back to the program
 * a host address becomes its guest address, 0 when it is outside the window (the lock helpers
 * refuse such a lock before it gets here). */
static inline void d3d9w_in_D3DLOCKED_RECT(D3DLOCKED_RECT *d, const struct d3d9w_g_D3DLOCKED_RECT *s)
{
    d->Pitch = s->Pitch;
    d->pBits = d3d9w_host_of(s->pBits);
}

static inline void d3d9w_out_D3DLOCKED_RECT(struct d3d9w_g_D3DLOCKED_RECT *d, const D3DLOCKED_RECT *s)
{
    d->Pitch = s->Pitch;
    d->pBits = d3d9w_guest_of(s->pBits, NULL);
}

static inline void d3d9w_in_D3DLOCKED_BOX(D3DLOCKED_BOX *d, const struct d3d9w_g_D3DLOCKED_BOX *s)
{
    d->RowPitch = s->RowPitch;
    d->SlicePitch = s->SlicePitch;
    d->pBits = d3d9w_host_of(s->pBits);
}

static inline void d3d9w_out_D3DLOCKED_BOX(struct d3d9w_g_D3DLOCKED_BOX *d, const D3DLOCKED_BOX *s)
{
    d->RowPitch = s->RowPitch;
    d->SlicePitch = s->SlicePitch;
    d->pBits = d3d9w_guest_of(s->pBits, NULL);
}

static inline void d3d9w_in_D3DPRESENTSTATS(D3DPRESENTSTATS *d, const struct d3d9w_g_D3DPRESENTSTATS *s)
{
    d->PresentCount = s->PresentCount;
    d->PresentRefreshCount = s->PresentRefreshCount;
    d->SyncRefreshCount = s->SyncRefreshCount;
    d->SyncQPCTime.QuadPart = (LONGLONG)((uint64_t)s->SyncQPCTime[1] << 32 | s->SyncQPCTime[0]);
    d->SyncGPUTime.QuadPart = (LONGLONG)((uint64_t)s->SyncGPUTime[1] << 32 | s->SyncGPUTime[0]);
}

static inline void d3d9w_out_D3DPRESENTSTATS(struct d3d9w_g_D3DPRESENTSTATS *d, const D3DPRESENTSTATS *s)
{
    d->PresentCount = s->PresentCount;
    d->PresentRefreshCount = s->PresentRefreshCount;
    d->SyncRefreshCount = s->SyncRefreshCount;
    d->SyncQPCTime[0] = (uint32_t)(uint64_t)s->SyncQPCTime.QuadPart;
    d->SyncQPCTime[1] = (uint32_t)((uint64_t)s->SyncQPCTime.QuadPart >> 32);
    d->SyncGPUTime[0] = (uint32_t)(uint64_t)s->SyncGPUTime.QuadPart;
    d->SyncGPUTime[1] = (uint32_t)((uint64_t)s->SyncGPUTime.QuadPart >> 32);
}

/* ------------------------------------------------------------------ logging */

/* One line to the unix-side log (Wine's __wine_dbg_output), at most 900 bytes (spec 07
 * clarification 4). */
void d3d9w_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Logs once per method number (G3 and refused objects). */
void d3d9w_log_once(uint32_t method, const char *what);

/* ------------------------------------------------------------------ hand-written methods */

#define D3D9W_EXTRA(name) void d3d9w_x_##name(uint32_t esp, struct ios_wow_native_ret *ret)
D3D9W_EXTRA(Direct3DCreate9);
D3D9W_EXTRA(Direct3DCreate9Ex);
D3D9W_EXTRA(D3DPERF_BeginEvent);
D3D9W_EXTRA(D3DPERF_EndEvent);
D3D9W_EXTRA(D3DPERF_GetStatus);
D3D9W_EXTRA(D3DPERF_QueryRepeatFrame);
D3D9W_EXTRA(D3DPERF_SetMarker);
D3D9W_EXTRA(D3DPERF_SetOptions);
D3D9W_EXTRA(D3DPERF_SetRegion);
D3D9W_EXTRA(DebugSetLevel);
D3D9W_EXTRA(DebugSetMute);
D3D9W_EXTRA(Init);
D3D9W_EXTRA(WindowState);
D3D9W_EXTRA(FocusActivation);
D3D9W_EXTRA(Unpin);
D3D9W_EXTRA(Pin);

#endif /* D3D9W_DISPATCH_H */
