/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 E3: guest-side state on resource proxies.
 *
 * - SetPrivateData with D3DSPD_IUNKNOWN and GetPrivateData of such an entry: native code cannot
 *   call the program's AddRef and Release, so the thunk keeps these entries and their
 *   references here. Other private data goes to native code.
 * - IDirect3DSurface9::GetDC and ReleaseDC: LockRect through the proxy, then gdi32's
 *   D3DKMTCreateDCFromMemory, and the reverse, as the PE build of the frontend does.
 *
 * A proxy with such state is pinned (the Pin call): when its native object goes, the dispatch
 * keeps the slot, marks the proxy GONE and bumps the shared sweep counter. The thunk then drops
 * the state the PE build drops in the object's destructor (Release the program's interfaces,
 * delete the DC) and unpins the slot. It looks at the counter on every Release of a resource,
 * swap chain or device, and in the methods below. */

#include "d3d9_thunk.h"
#include "d3d9_thunk_pure.h"

enum entry_type { ENTRY_PRIVATE, ENTRY_DC };

struct entry
{
    struct entry *next;
    enum entry_type type;
    const struct d3d9w_proxy *proxy;
    uint32_t handle;            /* the proxy's handle when the entry was made */
    GUID guid;                  /* ENTRY_PRIVATE */
    IUnknown *unk;              /* ENTRY_PRIVATE: one reference held */
    HDC hdc;                    /* ENTRY_DC */
    HANDLE hbitmap;
};

static CRITICAL_SECTION entries_cs;
static struct entry *entries;
static volatile const struct d3d9w_shared *shared;
static uint32_t last_sweep;

void d3d9t_resource_init(void)
{
    InitializeCriticalSection(&entries_cs);
}

void d3d9t_set_shared(uint32_t shared_guest)
{
    shared = (const struct d3d9w_shared *)(uintptr_t)shared_guest;
}

/* ------------------------------------------------------------------ gdi32's DC over memory */

/* The documented layouts of D3DKMT_CREATEDCFROMMEMORY and D3DKMT_DESTROYDCFROMMEMORY; the
 * entry points are looked up in gdi32 (Wine exports both). */
struct kmt_create_dc
{
    void *pMemory;
    D3DFORMAT Format;
    UINT Width;
    UINT Height;
    UINT Pitch;
    HDC hDeviceDc;
    PALETTEENTRY *pColorTable;
    HDC hDc;
    HANDLE hBitmap;
};

struct kmt_destroy_dc
{
    HDC hDc;
    HANDLE hBitmap;
};

static LONG kmt_create_dc(struct kmt_create_dc *args)
{
    static LONG (WINAPI *volatile fn)(struct kmt_create_dc *);

    if (!fn)
        fn = (LONG (WINAPI *)(struct kmt_create_dc *))(void *)GetProcAddress(
            GetModuleHandleA("gdi32.dll"), "D3DKMTCreateDCFromMemory");
    return fn ? fn(args) : -1;
}

static LONG kmt_destroy_dc(const struct kmt_destroy_dc *args)
{
    static LONG (WINAPI *volatile fn)(const struct kmt_destroy_dc *);

    if (!fn)
        fn = (LONG (WINAPI *)(const struct kmt_destroy_dc *))(void *)GetProcAddress(
            GetModuleHandleA("gdi32.dll"), "D3DKMTDestroyDCFromMemory");
    return fn ? fn(args) : -1;
}

/* ------------------------------------------------------------------ entries */

static inline const struct d3d9w_proxy *proxy_of(const void *iface)
{
    return (const struct d3d9w_proxy *)iface;
}

/* entries_cs held. */
static struct entry *find_locked(const void *iface, enum entry_type type, const GUID *guid)
{
    struct entry *e;

    for (e = entries; e; e = e->next)
        if (e->proxy == proxy_of(iface) && e->type == type &&
            (type != ENTRY_PRIVATE || IsEqualGUID(&e->guid, guid)))
            return e;
    return NULL;
}

/* entries_cs held. */
static void unlink_locked(struct entry *gone)
{
    struct entry **p;

    for (p = &entries; *p; p = &(*p)->next)
        if (*p == gone)
        {
            *p = gone->next;
            return;
        }
}

/* entries_cs held. Unpins a proxy that has no entry left. */
static void unpin_if_unused_locked(const struct d3d9w_proxy *proxy)
{
    struct entry *e;

    for (e = entries; e; e = e->next)
        if (e->proxy == proxy)
            return;
    d3d9w_s_Exports_Unpin((void *)proxy);
}

/* entries_cs held. Pins the proxy and adds the entry; FALSE when the object is already gone. */
static BOOL add_locked(struct entry *e, const void *iface)
{
    e->proxy = proxy_of(iface);
    e->handle = e->proxy->handle;
    if (FAILED(d3d9w_s_Exports_Pin((void *)iface)))
        return FALSE;
    e->next = entries;
    entries = e;
    return TRUE;
}

static void drop_entry(struct entry *e)
{
    if (e->type == ENTRY_PRIVATE)
    {
        if (e->unk)
            IUnknown_Release(e->unk);
    }
    else
    {
        struct kmt_destroy_dc destroy = { e->hdc, e->hbitmap };

        kmt_destroy_dc(&destroy);
    }
    HeapFree(GetProcessHeap(), 0, e);
}

/* Drops the state of proxies whose native object went (their PE destructor's work), then gives
 * their slots back. The program's Release calls run outside the lock. */
static void sweep(void)
{
    struct entry *e, **p, *dropped = NULL;
    const struct d3d9w_proxy *gone[64];
    unsigned int ngone = 0, i;
    BOOL more = FALSE;

    EnterCriticalSection(&entries_cs);
    last_sweep = shared->sweep;
    for (p = &entries; (e = *p);)
    {
        if ((e->proxy->flags & D3D9W_PROXY_GONE) && e->proxy->handle == e->handle)
        {
            for (i = 0; i < ngone && gone[i] != e->proxy; i++)
                ;
            if (i == ngone)
            {
                if (ngone == 64)
                {
                    more = TRUE;   /* a batch is full: this one waits for the next sweep */
                    p = &e->next;
                    continue;
                }
                gone[ngone++] = e->proxy;
            }
            *p = e->next;
            e->next = dropped;
            dropped = e;
            continue;
        }
        p = &e->next;
    }
    /* Every entry of these proxies is unlinked: their slots can go back. */
    for (i = 0; i < ngone; i++)
        d3d9w_s_Exports_Unpin((void *)gone[i]);
    if (more)
        last_sweep--;   /* look again at the next chance */
    LeaveCriticalSection(&entries_cs);
    while ((e = dropped))
    {
        dropped = e->next;
        drop_entry(e);
    }
}

void d3d9t_sweep_if_needed(void)
{
    if (shared && shared->sweep != last_sweep)
        sweep();
}

/* ------------------------------------------------------------------ private data */

typedef HRESULT (WINAPI *set_private_fn)(void *, REFGUID, const void *, DWORD, DWORD);
typedef HRESULT (WINAPI *get_private_fn)(void *, REFGUID, void *, DWORD *);
typedef HRESULT (WINAPI *free_private_fn)(void *, REFGUID);

static HRESULT set_private(void *iface, REFGUID guid, const void *data, DWORD size, DWORD flags,
                           set_private_fn set_fn, free_private_fn free_fn)
{
    struct entry *e, *old;
    HRESULT hr;

    d3d9t_sweep_if_needed();
    if (!guid)
        return D3DERR_INVALIDCALL;
    if (flags & D3DSPD_IUNKNOWN)
    {
        IUnknown *unk = (IUnknown *)data;   /* data is the interface pointer itself */

        if (size != sizeof(IUnknown *))
            return D3DERR_INVALIDCALL;
        if (!(e = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*e))))
            return E_OUTOFMEMORY;
        free_fn(iface, guid);   /* a data entry of the same GUID is replaced */
        e->type = ENTRY_PRIVATE;
        e->guid = *guid;
        e->unk = unk;
        if (unk)
            IUnknown_AddRef(unk);
        EnterCriticalSection(&entries_cs);
        if ((old = find_locked(iface, ENTRY_PRIVATE, guid)))
            unlink_locked(old);
        if (!add_locked(e, iface))
        {
            LeaveCriticalSection(&entries_cs);
            if (old)
                drop_entry(old);
            drop_entry(e);
            return D3DERR_INVALIDCALL;
        }
        LeaveCriticalSection(&entries_cs);
        if (old)
            drop_entry(old);
        return D3D_OK;
    }
    hr = set_fn(iface, guid, data, size, flags);
    if (SUCCEEDED(hr))
    {
        EnterCriticalSection(&entries_cs);
        if ((old = find_locked(iface, ENTRY_PRIVATE, guid)))
        {
            unlink_locked(old);
            unpin_if_unused_locked(old->proxy);
        }
        LeaveCriticalSection(&entries_cs);
        if (old)
            drop_entry(old);
    }
    return hr;
}

static HRESULT get_private(void *iface, REFGUID guid, void *data, DWORD *size, get_private_fn get_fn)
{
    struct entry *e;
    HRESULT hr;
    int copy;

    d3d9t_sweep_if_needed();
    if (!guid)
        return D3DERR_INVALIDCALL;
    EnterCriticalSection(&entries_cs);
    if (!(e = find_locked(iface, ENTRY_PRIVATE, guid)))
    {
        LeaveCriticalSection(&entries_cs);
        return get_fn(iface, guid, data, size);
    }
    hr = d3d9t_private_iface_size(data != NULL, size ? *size : 0, &copy);
    if (copy)
    {
        if (e->unk)
            IUnknown_AddRef(e->unk);
        *(IUnknown **)data = e->unk;
    }
    if (size)
        *size = sizeof(IUnknown *);
    LeaveCriticalSection(&entries_cs);
    return hr;
}

static HRESULT free_private(void *iface, REFGUID guid, free_private_fn free_fn)
{
    struct entry *e;

    d3d9t_sweep_if_needed();
    if (!guid)
        return D3DERR_INVALIDCALL;
    EnterCriticalSection(&entries_cs);
    if ((e = find_locked(iface, ENTRY_PRIVATE, guid)))
    {
        unlink_locked(e);
        unpin_if_unused_locked(e->proxy);
    }
    LeaveCriticalSection(&entries_cs);
    if (!e)
        return free_fn(iface, guid);
    drop_entry(e);
    return D3D_OK;
}

/* The resource interfaces' Release, SetPrivateData, GetPrivateData and FreePrivateData. */
#define RESOURCE_METHODS(K, T) \
    ULONG WINAPI d3d9t_##K##_Release(T *This) \
    { \
        ULONG refs = d3d9w_s_##K##_Release(This); \
        d3d9t_sweep_if_needed(); \
        return refs; \
    } \
    HRESULT WINAPI d3d9t_##K##_SetPrivateData(T *This, REFGUID guid, const void *data, DWORD data_size, \
                                              DWORD flags) \
    { \
        return set_private(This, guid, data, data_size, flags, (set_private_fn)d3d9w_s_##K##_SetPrivateData, \
                           (free_private_fn)d3d9w_s_##K##_FreePrivateData); \
    } \
    HRESULT WINAPI d3d9t_##K##_GetPrivateData(T *This, REFGUID guid, void *data, DWORD *size) \
    { \
        return get_private(This, guid, data, size, (get_private_fn)d3d9w_s_##K##_GetPrivateData); \
    } \
    HRESULT WINAPI d3d9t_##K##_FreePrivateData(T *This, REFGUID guid) \
    { \
        return free_private(This, guid, (free_private_fn)d3d9w_s_##K##_FreePrivateData); \
    }

RESOURCE_METHODS(Surface, IDirect3DSurface9)
RESOURCE_METHODS(Texture, IDirect3DTexture9)
RESOURCE_METHODS(CubeTexture, IDirect3DCubeTexture9)
RESOURCE_METHODS(VolumeTexture, IDirect3DVolumeTexture9)
RESOURCE_METHODS(Volume, IDirect3DVolume9)
RESOURCE_METHODS(VertexBuffer, IDirect3DVertexBuffer9)
RESOURCE_METHODS(IndexBuffer, IDirect3DIndexBuffer9)

ULONG WINAPI d3d9t_SwapChain_Release(IDirect3DSwapChain9Ex *This)
{
    ULONG refs = d3d9w_s_SwapChain_Release(This);

    d3d9t_sweep_if_needed();
    return refs;
}

/* ------------------------------------------------------------------ GetDC / ReleaseDC */

/* A GDI DC over the bytes LockRect exposes, so GDI paints into the memory UnlockRect uploads.
 * One DC per surface; *phdc is written only on success. */
HRESULT WINAPI d3d9t_Surface_GetDC(IDirect3DSurface9 *This, HDC *phdc)
{
    struct kmt_create_dc create;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT locked;
    struct entry *e;
    BOOL open;
    HRESULT hr;

    d3d9t_sweep_if_needed();
    if (!phdc)
        return D3DERR_INVALIDCALL;
    EnterCriticalSection(&entries_cs);
    open = find_locked(This, ENTRY_DC, NULL) != NULL;
    LeaveCriticalSection(&entries_cs);
    if (open)
        return D3DERR_INVALIDCALL;
    if (FAILED(d3d9w_s_Surface_GetDesc(This, &desc)) || !d3d9t_getdc_format_ok(desc.Format))
        return D3DERR_INVALIDCALL;
    if (FAILED(hr = d3d9w_s_Surface_LockRect(This, &locked, NULL, 0)))
        return hr;
    memset(&create, 0, sizeof(create));
    create.pMemory = locked.pBits;
    create.Format = desc.Format;
    create.Width = desc.Width;
    create.Height = desc.Height;
    create.Pitch = (UINT)locked.Pitch;
    create.hDeviceDc = CreateCompatibleDC(NULL);
    create.pColorTable = NULL;
    hr = kmt_create_dc(&create) ? D3DERR_INVALIDCALL : D3D_OK;
    if (create.hDeviceDc)
        DeleteDC(create.hDeviceDc);
    if (FAILED(hr) || !create.hDc)
    {
        d3d9w_s_Surface_UnlockRect(This);
        return D3DERR_INVALIDCALL;
    }
    if (!(e = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*e))))
        hr = E_OUTOFMEMORY;
    else
    {
        e->type = ENTRY_DC;
        e->hdc = create.hDc;
        e->hbitmap = create.hBitmap;
        EnterCriticalSection(&entries_cs);
        if (find_locked(This, ENTRY_DC, NULL) || !add_locked(e, This))
            hr = D3DERR_INVALIDCALL;   /* another thread got there first, or the surface went */
        LeaveCriticalSection(&entries_cs);
    }
    if (FAILED(hr))
    {
        if (e)
        {
            e->next = NULL;
            drop_entry(e);   /* deletes the DC */
        }
        else
        {
            struct kmt_destroy_dc destroy = { create.hDc, create.hBitmap };

            kmt_destroy_dc(&destroy);
        }
        d3d9w_s_Surface_UnlockRect(This);
        return hr;
    }
    *phdc = create.hDc;
    return D3D_OK;
}

HRESULT WINAPI d3d9t_Surface_ReleaseDC(IDirect3DSurface9 *This, HDC hdc)
{
    struct entry *e;

    d3d9t_sweep_if_needed();
    EnterCriticalSection(&entries_cs);
    if ((e = find_locked(This, ENTRY_DC, NULL)) && e->hdc == hdc)
    {
        unlink_locked(e);
        unpin_if_unused_locked(e->proxy);
    }
    else
        e = NULL;
    LeaveCriticalSection(&entries_cs);
    if (!e)
        return D3DERR_INVALIDCALL;
    drop_entry(e);
    /* UnlockRect uploads what GDI painted, through the same path a write lock takes. */
    return d3d9w_s_Surface_UnlockRect(This);
}
