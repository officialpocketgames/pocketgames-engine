/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 J3: fakes for the dispatch's host tests.
 *   - Section A of interfaces/ios_wow_native.h: ios_wow_native_read/_write follow clarification
 *     4 (0 below 64 KB or past 4 GB) and also refuse the marked page G_REFUSED, as J3 asks.
 *   - ios_wow_base (spec 06), ios_wow_window_alloc (Part C), __wine_dbg_output.
 *   - A native frontend: IDirect3D9Ex, a device, textures with one surface level, a vertex
 *     buffer, a query, a vertex declaration and a vertex shader, with the reference rules the
 *     dispatch relies on (sub-resources count on their container, a bound texture is kept by the
 *     device, d3d9w_object_gone when an object is destroyed). Methods a test does not use abort. */

#define _GNU_SOURCE
#include <windows.h>
#include <initguid.h>   /* this file defines the IIDs the fakes compare with */
#include <d3d9.h>
#include "fake_native.h"

#include <stdio.h>
#include <sys/mman.h>

/* ------------------------------------------------------------------ the platform */

uint8_t *fake_window;
static unsigned long base_override;
static uint32_t data_next = G_DATA, arena_next = G_ARENA;
int fake_reads, fake_writes, fake_log_lines;
char fake_last_log[1024];
ios_wow_native_fn fake_registered;

static void fail(const char *what)
{
    fprintf(stderr, "fake_native: %s\n", what);
    abort();
}

void fake_window_init(void)
{
    uint8_t *p = mmap(NULL, 8ull << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    if (p == MAP_FAILED)
        fail("cannot reserve 8 GB");
    fake_window = (uint8_t *)(((uintptr_t)p + (4ull << 30) - 1) & ~((4ull << 30) - 1));
    if (mprotect(fake_window + G_STACK, 0x10000, PROT_READ | PROT_WRITE) ||
        mprotect(fake_window + G_DATA, 0x100000, PROT_READ | PROT_WRITE) ||
        mprotect(fake_window + G_ARENA, 64u << 20, PROT_READ | PROT_WRITE))
        fail("mprotect");
}

void *gptr(uint32_t g)
{
    return fake_window + g;
}

uint32_t galloc(uint32_t n)
{
    uint32_t g = data_next;

    data_next = (data_next + n + 15) & ~15u;
    if (data_next > G_DATA + 0x100000)
        fail("G_DATA is full");
    memset(gptr(g), 0, n);
    return g;
}

void fake_set_base_override(unsigned long base)
{
    base_override = base;
}

unsigned long ios_wow_base(void)
{
    return base_override ? base_override : (unsigned long)(uintptr_t)fake_window;
}

static int refused(uint32_t g, size_t n)
{
    if (g < 0x10000u || (uint64_t)g + n > 0x100000000ull)
        return 1;
    return (uint64_t)g < G_REFUSED + 0x1000ull && (uint64_t)g + n > G_REFUSED;
}

int ios_wow_native_read(void *dst, uint32_t guest, size_t n)
{
    fake_reads++;
    if (refused(guest, n))
        return 0;
    memcpy(dst, fake_window + guest, n);
    return 1;
}

int ios_wow_native_write(uint32_t guest, const void *src, size_t n)
{
    fake_writes++;
    if (refused(guest, n))
        return 0;
    memcpy(fake_window + guest, src, n);
    return 1;
}

uint32_t ios_wow_native_register(uint32_t lib, ios_wow_native_fn fn)
{
    if (lib != IOS_WOW_NATIVE_LIB_D3D9)
        return 0;
    fake_registered = fn;
    return 0x7ffe0004u;   /* the BOP page's g + 4, as the app's CPU hands it out */
}

void *ios_wow_window_alloc(size_t size, size_t align, uint32_t *guest)
{
    uint32_t g = (uint32_t)((arena_next + align - 1) & ~(uint64_t)(align - 1));

    if ((uint64_t)g + size > G_ARENA + (64u << 20))
        return NULL;
    arena_next = g + (uint32_t)size;
    *guest = g;
    return fake_window + g;
}

int __wine_dbg_output(const char *str)
{
    fake_log_lines++;
    snprintf(fake_last_log, sizeof(fake_last_log), "%s", str);
    if (getenv("TEST_VERBOSE"))
        fputs(str, stderr);
    return (int)strlen(str);
}

/* ------------------------------------------------------------------ fake objects */

struct fake_calls fc;
int fake_live_objects, fake_textures_destroyed;
float fake_npatch = 4.5f;
int fake_lock_outside, fake_query_ready;

static void unexpected(void)
{
    fail("a native method the test did not expect was called");
}

#define FILL(vtbl) do { void **s_ = (void **)&(vtbl); size_t i_; \
    for (i_ = 0; i_ < sizeof(vtbl) / sizeof(void *); i_++) s_[i_] = (void *)unexpected; } while (0)

struct fake_d3d9 { IDirect3D9Ex iface; LONG refs; };
struct fake_device;
struct fake_texture;
struct fake_surface { IDirect3DSurface9 iface; struct fake_texture *container; };
struct fake_texture { IDirect3DTexture9 iface; LONG refs, priv; struct fake_device *dev; struct fake_surface level0; };
struct fake_vb { IDirect3DVertexBuffer9 iface; LONG refs, priv; uint32_t lock_guest; };
struct fake_query { IDirect3DQuery9 iface; LONG refs; };
struct fake_decl { IDirect3DVertexDeclaration9 iface; LONG refs; D3DVERTEXELEMENT9 el[D3D9W_MAX_DECL_ELEMENTS]; UINT n; };
struct fake_vs { IDirect3DVertexShader9 iface; LONG refs; DWORD tokens[64]; UINT n; };
struct fake_device
{
    IDirect3DDevice9Ex iface;
    LONG refs;
    struct fake_d3d9 *parent;
    struct fake_texture *tex[8];
    struct fake_vb *stream0;
    D3DPRESENT_PARAMETERS pp;
    DWORD behavior;
    HWND focus;
};

static IDirect3D9ExVtbl d3d9_vtbl;
static IDirect3DDevice9ExVtbl device_vtbl;
static IDirect3DTexture9Vtbl texture_vtbl;
static IDirect3DSurface9Vtbl surface_vtbl;
static IDirect3DVertexBuffer9Vtbl vb_vtbl;
static IDirect3DQuery9Vtbl query_vtbl;
static IDirect3DVertexDeclaration9Vtbl decl_vtbl;
static IDirect3DVertexShader9Vtbl vs_vtbl;

/* Every live object, so the ones a process end leaves as orphans stay reachable. */
static void *objects[256];

static void *fake_new(size_t size)
{
    void *p = calloc(1, size);
    size_t i;

    if (!p)
        fail("calloc");
    for (i = 0; i < sizeof(objects) / sizeof(objects[0]) && objects[i]; i++)
        ;
    if (i == sizeof(objects) / sizeof(objects[0]))
        fail("too many fake objects");
    objects[i] = p;
    fake_live_objects++;
    return p;
}

static void fake_free(void *iface)
{
    size_t i;

    d3d9w_object_gone(iface);   /* clarification 7: before the memory goes */
    for (i = 0; i < sizeof(objects) / sizeof(objects[0]); i++)
        if (objects[i] == iface)
            objects[i] = NULL;
    fake_live_objects--;
    free(iface);
}

/* Clarification 13: the objects still alive become orphans; nothing is released. */
int fake_process_gone_calls;

void d3d9w_native_process_gone(void)
{
    fake_process_gone_calls++;
}

/* ---- IDirect3D9Ex ---- */

static HRESULT d9_QueryInterface(IDirect3D9Ex *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDirect3D9) ||
        IsEqualGUID(iid, &IID_IDirect3D9Ex))
    {
        *out = This;
        IDirect3D9Ex_AddRef(This);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG d9_AddRef(IDirect3D9Ex *This) { return ++((struct fake_d3d9 *)This)->refs; }
static ULONG d9_Release(IDirect3D9Ex *This)
{
    ULONG r = --((struct fake_d3d9 *)This)->refs;
    if (!r)
        fake_free(This);
    return r;
}
static HRESULT d9_GetAdapterIdentifier(IDirect3D9Ex *This, UINT a, DWORD f, D3DADAPTER_IDENTIFIER9 *id)
{
    memset(id, 0x5a, sizeof(*id));
    strcpy(id->Description, "fake adapter");
    id->VendorId = 0x106b;
    id->WHQLLevel = 7;
    return D3D_OK;
}
static UINT d9_GetAdapterModeCountEx(IDirect3D9Ex *This, UINT a, const D3DDISPLAYMODEFILTER *f)
{
    return f && f->Format == D3DFMT_X8R8G8B8 ? 3 : 0;
}
static HMONITOR d9_GetAdapterMonitor(IDirect3D9Ex *This, UINT a) { return (HMONITOR)(uintptr_t)0x10001; }
static HRESULT d9_GetAdapterDisplayMode(IDirect3D9Ex *This, UINT a, D3DDISPLAYMODE *m)
{
    m->Width = 1920; m->Height = 1080; m->RefreshRate = 60; m->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}
static HRESULT d9_CreateDevice(IDirect3D9Ex *This, UINT a, D3DDEVTYPE t, HWND focus, DWORD flags,
                               D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    struct fake_device *d;

    fc.create_device++;
    fc.create_device_window = pp->hDeviceWindow;
    if (!pp->BackBufferCount)
        pp->BackBufferCount = 1;   /* the frontend writes the canonical value back */
    fc.create_device_bb_count = pp->BackBufferCount;
    d = fake_new(sizeof(*d));
    d->iface.lpVtbl = &device_vtbl;
    d->refs = 1;
    d->parent = (struct fake_d3d9 *)This;
    IDirect3D9Ex_AddRef(This);
    d->pp = *pp;
    d->behavior = flags;
    d->focus = focus;
    *out = (IDirect3DDevice9 *)&d->iface;
    return D3D_OK;
}

/* ---- the seam ---- */

IDirect3D9 *d3d9w_native_create9(UINT sdk)
{
    struct fake_d3d9 *d = fake_new(sizeof(*d));

    d->iface.lpVtbl = &d3d9_vtbl;
    d->refs = 1;
    return (IDirect3D9 *)&d->iface;
}

HRESULT d3d9w_native_create9ex(UINT sdk, IDirect3D9Ex **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = (IDirect3D9Ex *)d3d9w_native_create9(sdk);
    return D3D_OK;
}

HRESULT d3d9w_native_window_state(IDirect3DDevice9 *device, struct d3d9w_window_state *out)
{
    struct fake_device *d = (struct fake_device *)device;

    out->focus_window = d->focus;
    out->device_window = d->pp.hDeviceWindow;
    out->windowed = d->pp.Windowed;
    out->ex = FALSE;
    out->backbuffer_width = d->pp.BackBufferWidth;
    out->backbuffer_height = d->pp.BackBufferHeight;
    out->behavior_flags = d->behavior;
    out->virtual_mode = TRUE;
    out->mode.Width = d->pp.BackBufferWidth;
    out->mode.Height = d->pp.BackBufferHeight;
    out->mode.RefreshRate = 60;
    out->mode.Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

DWORD d3d9w_native_focus_activation(IDirect3DDevice9 *device, BOOL active, RECT *pos)
{
    struct fake_device *d = (struct fake_device *)device;

    if (d->pp.Windowed)
        return 0;
    if (!active)
        return D3D9W_FOCUS_MINIMIZE;
    pos->left = 0;
    pos->top = 0;
    pos->right = (LONG)d->pp.BackBufferWidth;
    pos->bottom = (LONG)d->pp.BackBufferHeight;
    return D3D9W_FOCUS_MOVE;
}

/* ---- textures and their surface (the surface counts on the texture) ---- */

static void texture_maybe_destroy(struct fake_texture *t)
{
    if (t->refs || t->priv)
        return;
    d3d9w_object_gone(&t->level0.iface);   /* the sub-resource goes with its container */
    fake_textures_destroyed++;
    fake_free(&t->iface);
}

static HRESULT tex_QueryInterface(IDirect3DTexture9 *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDirect3DResource9) ||
        IsEqualGUID(iid, &IID_IDirect3DBaseTexture9) || IsEqualGUID(iid, &IID_IDirect3DTexture9))
    {
        *out = This;
        IDirect3DTexture9_AddRef(This);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG tex_AddRef(IDirect3DTexture9 *This) { return ++((struct fake_texture *)This)->refs; }
static ULONG tex_Release(IDirect3DTexture9 *This)
{
    struct fake_texture *t = (struct fake_texture *)This;
    ULONG r = --t->refs;

    texture_maybe_destroy(t);
    return r;
}
static D3DRESOURCETYPE tex_GetType(IDirect3DTexture9 *This) { return D3DRTYPE_TEXTURE; }
static HRESULT tex_GetSurfaceLevel(IDirect3DTexture9 *This, UINT level, IDirect3DSurface9 **out)
{
    struct fake_texture *t = (struct fake_texture *)This;

    if (level)
        return D3DERR_INVALIDCALL;
    t->refs++;
    *out = &t->level0.iface;
    return D3D_OK;
}
static HRESULT tex_SetPrivateData(IDirect3DTexture9 *This, REFGUID g, const void *d, DWORD n, DWORD f)
{
    fc.set_private++;
    return D3D_OK;
}

static HRESULT surf_QueryInterface(IDirect3DSurface9 *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDirect3DResource9) ||
        IsEqualGUID(iid, &IID_IDirect3DSurface9))
    {
        *out = This;
        IDirect3DSurface9_AddRef(This);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG surf_AddRef(IDirect3DSurface9 *This)
{
    return tex_AddRef(&((struct fake_surface *)This)->container->iface);
}
static ULONG surf_Release(IDirect3DSurface9 *This)
{
    return tex_Release(&((struct fake_surface *)This)->container->iface);
}
static HRESULT surf_GetContainer(IDirect3DSurface9 *This, REFIID iid, void **out)
{
    return tex_QueryInterface(&((struct fake_surface *)This)->container->iface, iid, out);
}
static HRESULT surf_GetDesc(IDirect3DSurface9 *This, D3DSURFACE_DESC *desc)
{
    memset(desc, 0, sizeof(*desc));
    desc->Format = D3DFMT_A8R8G8B8;
    desc->Width = desc->Height = 32;
    return D3D_OK;
}
static HRESULT surf_LockRect(IDirect3DSurface9 *This, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    static uint8_t outside[64];

    lr->Pitch = 128;
    lr->pBits = fake_lock_outside ? (void *)outside : gptr(G_DATA + 0x80000);
    return D3D_OK;
}
static HRESULT surf_UnlockRect(IDirect3DSurface9 *This)
{
    fc.unlocks++;
    return D3D_OK;
}

static struct fake_texture *new_texture(struct fake_device *d)
{
    struct fake_texture *t = fake_new(sizeof(*t));

    t->iface.lpVtbl = &texture_vtbl;
    t->refs = 1;
    t->dev = d;
    t->level0.iface.lpVtbl = &surface_vtbl;
    t->level0.container = t;
    return t;
}

/* ---- vertex buffer, query, declaration, shader ---- */

static ULONG vb_AddRef(IDirect3DVertexBuffer9 *This) { return ++((struct fake_vb *)This)->refs; }
static ULONG vb_Release(IDirect3DVertexBuffer9 *This)
{
    struct fake_vb *b = (struct fake_vb *)This;
    ULONG r = --b->refs;

    if (!r && !b->priv)
        fake_free(This);
    return r;
}
static HRESULT vb_Lock(IDirect3DVertexBuffer9 *This, UINT off, UINT size, void **data, DWORD flags)
{
    *data = gptr(((struct fake_vb *)This)->lock_guest + off);
    return D3D_OK;
}
static D3DRESOURCETYPE vb_GetType(IDirect3DVertexBuffer9 *This) { return D3DRTYPE_VERTEXBUFFER; }

static ULONG q_AddRef(IDirect3DQuery9 *This) { return ++((struct fake_query *)This)->refs; }
static ULONG q_Release(IDirect3DQuery9 *This)
{
    ULONG r = --((struct fake_query *)This)->refs;
    if (!r)
        fake_free(This);
    return r;
}
static HRESULT q_GetData(IDirect3DQuery9 *This, void *data, DWORD size, DWORD flags)
{
    fc.get_data++;
    if (!fake_query_ready)
        return S_FALSE;
    if (data && size >= 4)
        *(DWORD *)data = 1234;
    return S_OK;
}

static ULONG decl_AddRef(IDirect3DVertexDeclaration9 *This) { return ++((struct fake_decl *)This)->refs; }
static ULONG decl_Release(IDirect3DVertexDeclaration9 *This)
{
    ULONG r = --((struct fake_decl *)This)->refs;
    if (!r)
        fake_free(This);
    return r;
}
static HRESULT decl_GetDeclaration(IDirect3DVertexDeclaration9 *This, D3DVERTEXELEMENT9 *el, UINT *n)
{
    struct fake_decl *d = (struct fake_decl *)This;

    if (!n)
        return D3DERR_INVALIDCALL;
    if (el)
        memcpy(el, d->el, d->n * sizeof(*el));
    *n = d->n;
    return D3D_OK;
}

static ULONG vs_AddRef(IDirect3DVertexShader9 *This) { return ++((struct fake_vs *)This)->refs; }
static ULONG vs_Release(IDirect3DVertexShader9 *This)
{
    ULONG r = --((struct fake_vs *)This)->refs;
    if (!r)
        fake_free(This);
    return r;
}
static HRESULT vs_GetFunction(IDirect3DVertexShader9 *This, void *data, UINT *size)
{
    struct fake_vs *s = (struct fake_vs *)This;

    if (!size)
        return D3DERR_INVALIDCALL;
    if (!data)
    {
        *size = s->n * 4;
        return D3D_OK;
    }
    if (*size < s->n * 4)
        return D3DERR_INVALIDCALL;
    memcpy(data, s->tokens, s->n * 4);
    *size = s->n * 4;
    return D3D_OK;
}

/* ---- the device ---- */

static HRESULT dev_QueryInterface(IDirect3DDevice9Ex *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDirect3DDevice9))
    {
        *out = This;
        IDirect3DDevice9Ex_AddRef(This);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG dev_AddRef(IDirect3DDevice9Ex *This) { return ++((struct fake_device *)This)->refs; }
static ULONG dev_Release(IDirect3DDevice9Ex *This)
{
    struct fake_device *d = (struct fake_device *)This;
    ULONG r = --d->refs;
    int i;

    if (r)
        return r;
    for (i = 0; i < 8; i++)
        if (d->tex[i])
        {
            d->tex[i]->priv--;
            texture_maybe_destroy(d->tex[i]);
        }
    IDirect3D9Ex_Release(&d->parent->iface);
    fake_free(This);
    return 0;
}
static HRESULT dev_GetDirect3D(IDirect3DDevice9Ex *This, IDirect3D9 **out)
{
    struct fake_device *d = (struct fake_device *)This;

    IDirect3D9Ex_AddRef(&d->parent->iface);
    *out = (IDirect3D9 *)&d->parent->iface;
    return D3D_OK;
}
static HRESULT dev_GetCreationParameters(IDirect3DDevice9Ex *This, D3DDEVICE_CREATION_PARAMETERS *p)
{
    struct fake_device *d = (struct fake_device *)This;

    p->AdapterOrdinal = 0;
    p->DeviceType = D3DDEVTYPE_HAL;
    p->hFocusWindow = d->focus;
    p->BehaviorFlags = d->behavior;
    return D3D_OK;
}
static HRESULT dev_Reset(IDirect3DDevice9Ex *This, D3DPRESENT_PARAMETERS *pp)
{
    fc.reset++;
    fc.reset_window = pp->hDeviceWindow;
    pp->BackBufferWidth = 640;   /* canonicalised back */
    ((struct fake_device *)This)->pp = *pp;
    return D3D_OK;
}
static HRESULT dev_Present(IDirect3DDevice9Ex *This, const RECT *src, const RECT *dst, HWND w, const RGNDATA *rgn)
{
    fc.present++;
    if (src)
        fc.present_src = *src;
    fc.present_has_rgn = rgn != NULL;
    if (rgn)
    {
        fc.rgn_count = rgn->rdh.nCount;
        memcpy(fc.rgn_rects, rgn->Buffer, rgn->rdh.nCount * sizeof(RECT));
    }
    return D3D_OK;
}
static HRESULT dev_GetRasterStatus(IDirect3DDevice9Ex *This, UINT sc, D3DRASTER_STATUS *rs)
{
    fc.get_raster++;
    rs->InVBlank = TRUE;
    rs->ScanLine = 77;
    return D3D_OK;
}
static HRESULT dev_CreateTexture(IDirect3DDevice9Ex *This, UINT w, UINT h, UINT levels, DWORD usage,
                                 D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = &new_texture((struct fake_device *)This)->iface;
    return D3D_OK;
}
static HRESULT dev_CreateVertexBuffer(IDirect3DDevice9Ex *This, UINT len, DWORD usage, DWORD fvf,
                                      D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *shared)
{
    struct fake_vb *b = fake_new(sizeof(*b));

    b->iface.lpVtbl = &vb_vtbl;
    b->refs = 1;
    b->lock_guest = galloc(len);
    *out = &b->iface;
    return D3D_OK;
}
static HRESULT dev_Clear(IDirect3DDevice9Ex *This, DWORD count, const D3DRECT *rects, DWORD flags,
                         D3DCOLOR color, float z, DWORD stencil)
{
    fc.clear++;
    fc.clear_count = count;
    fc.clear_z = z;
    if (rects)
        memcpy(fc.clear_rects, rects, (count < 4 ? count : 4) * sizeof(*rects));
    return D3D_OK;
}
static HRESULT dev_SetTexture(IDirect3DDevice9Ex *This, DWORD stage, IDirect3DBaseTexture9 *tex)
{
    struct fake_device *d = (struct fake_device *)This;
    struct fake_texture *old = d->tex[stage], *t = (struct fake_texture *)tex;

    fc.set_texture++;
    fc.set_texture_native = tex;
    if (t)
        t->priv++;
    d->tex[stage] = t;
    if (old)
    {
        old->priv--;
        texture_maybe_destroy(old);
    }
    return D3D_OK;
}
static HRESULT dev_GetTexture(IDirect3DDevice9Ex *This, DWORD stage, IDirect3DBaseTexture9 **out)
{
    struct fake_texture *t = ((struct fake_device *)This)->tex[stage];

    if (t)
        t->refs++;
    *out = t ? (IDirect3DBaseTexture9 *)&t->iface : NULL;
    return D3D_OK;
}
static HRESULT dev_SetSamplerState(IDirect3DDevice9Ex *This, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v)
{
    fc.set_sampler++;
    fc.sampler_args[0] = s;
    fc.sampler_args[1] = (uint32_t)t;
    fc.sampler_args[2] = v;
    return D3D_OK;
}
static float dev_GetNPatchMode(IDirect3DDevice9Ex *This) { return fake_npatch; }
static HRESULT dev_DrawPrimitiveUP(IDirect3DDevice9Ex *This, D3DPRIMITIVETYPE t, UINT n, const void *data, UINT stride)
{
    fc.draw_up++;
    fc.draw_up_len = (size_t)d3d9w_prim_vertices(t, n) * stride;
    if (fc.draw_up_len <= sizeof(fc.draw_up_bytes))
        memcpy(fc.draw_up_bytes, data, fc.draw_up_len);
    return D3D_OK;
}
static HRESULT dev_DrawIndexedPrimitiveUP(IDirect3DDevice9Ex *This, D3DPRIMITIVETYPE t, UINT min, UINT nv,
                                          UINT n, const void *idx, D3DFORMAT f, const void *v, UINT stride)
{
    fc.draw_iup++;
    memcpy(fc.iup_indices, idx, d3d9w_prim_vertices(t, n) * 2);
    memcpy(fc.iup_vertices, v, (min + nv) * stride);
    return D3D_OK;
}
static HRESULT dev_CreateVertexDeclaration(IDirect3DDevice9Ex *This, const D3DVERTEXELEMENT9 *el,
                                           IDirect3DVertexDeclaration9 **out)
{
    struct fake_decl *d = fake_new(sizeof(*d));

    d->iface.lpVtbl = &decl_vtbl;
    d->refs = 1;
    while (d->n < D3D9W_MAX_DECL_ELEMENTS)
    {
        d->el[d->n] = el[d->n];
        if (el[d->n++].Stream == 0xff)
            break;
    }
    *out = &d->iface;
    return D3D_OK;
}
static HRESULT dev_CreateVertexShader(IDirect3DDevice9Ex *This, const DWORD *tokens, IDirect3DVertexShader9 **out)
{
    struct fake_vs *s = fake_new(sizeof(*s));

    fc.create_vs++;
    s->iface.lpVtbl = &vs_vtbl;
    s->refs = 1;
    /* The test's shaders are at most 64 tokens; the dispatch's copy is at least that long. */
    memcpy(s->tokens, tokens, sizeof(s->tokens));
    s->n = 64;
    fc.vs_tokens = s->n;
    *out = &s->iface;
    return D3D_OK;
}
static HRESULT dev_SetVertexShaderConstantF(IDirect3DDevice9Ex *This, UINT start, const float *data, UINT count)
{
    fc.set_vs_constf++;
    fc.constf_start = start;
    fc.constf_count = count;
    if (data && count)
    {
        memcpy(fc.constf_first, data, sizeof(fc.constf_first));
        memcpy(fc.constf_last, data + 4 * (count - 1), sizeof(fc.constf_last));
    }
    return D3D_OK;
}
static HRESULT dev_SetStreamSource(IDirect3DDevice9Ex *This, UINT n, IDirect3DVertexBuffer9 *vb, UINT off, UINT stride)
{
    struct fake_device *d = (struct fake_device *)This;
    struct fake_vb *old = d->stream0;

    if (vb)
        ((struct fake_vb *)vb)->priv++;
    d->stream0 = (struct fake_vb *)vb;
    if (old && !--old->priv && !old->refs)
        fake_free(&old->iface);
    return D3D_OK;
}
static HRESULT dev_GetStreamSource(IDirect3DDevice9Ex *This, UINT n, IDirect3DVertexBuffer9 **vb, UINT *off, UINT *stride)
{
    struct fake_device *d = (struct fake_device *)This;

    if (d->stream0)
        d->stream0->refs++;
    *vb = d->stream0 ? &d->stream0->iface : NULL;
    *off = 16;
    *stride = 32;
    return D3D_OK;
}
static HRESULT dev_CreateQuery(IDirect3DDevice9Ex *This, D3DQUERYTYPE t, IDirect3DQuery9 **out)
{
    struct fake_query *q;

    if (!out)
        return D3D_OK;   /* a support check */
    q = fake_new(sizeof(*q));
    q->iface.lpVtbl = &query_vtbl;
    q->refs = 1;
    *out = &q->iface;
    return D3D_OK;
}
static HRESULT dev_CheckResourceResidency(IDirect3DDevice9Ex *This, IDirect3DResource9 **res, UINT32 n)
{
    fc.residency++;
    fc.residency_count = n;
    fc.residency_first = n ? res[0] : NULL;
    return D3D_OK;
}

void fake_native_init(void)
{
    FILL(d3d9_vtbl);
    d3d9_vtbl.QueryInterface = d9_QueryInterface;
    d3d9_vtbl.AddRef = d9_AddRef;
    d3d9_vtbl.Release = d9_Release;
    d3d9_vtbl.GetAdapterIdentifier = d9_GetAdapterIdentifier;
    d3d9_vtbl.GetAdapterModeCountEx = d9_GetAdapterModeCountEx;
    d3d9_vtbl.GetAdapterMonitor = d9_GetAdapterMonitor;
    d3d9_vtbl.GetAdapterDisplayMode = d9_GetAdapterDisplayMode;
    d3d9_vtbl.CreateDevice = d9_CreateDevice;

    FILL(device_vtbl);
    device_vtbl.QueryInterface = dev_QueryInterface;
    device_vtbl.AddRef = dev_AddRef;
    device_vtbl.Release = dev_Release;
    device_vtbl.GetDirect3D = dev_GetDirect3D;
    device_vtbl.GetCreationParameters = dev_GetCreationParameters;
    device_vtbl.Reset = dev_Reset;
    device_vtbl.Present = dev_Present;
    device_vtbl.GetRasterStatus = dev_GetRasterStatus;
    device_vtbl.CreateTexture = dev_CreateTexture;
    device_vtbl.CreateVertexBuffer = dev_CreateVertexBuffer;
    device_vtbl.Clear = dev_Clear;
    device_vtbl.SetTexture = dev_SetTexture;
    device_vtbl.GetTexture = dev_GetTexture;
    device_vtbl.SetSamplerState = dev_SetSamplerState;
    device_vtbl.GetNPatchMode = dev_GetNPatchMode;
    device_vtbl.DrawPrimitiveUP = dev_DrawPrimitiveUP;
    device_vtbl.DrawIndexedPrimitiveUP = dev_DrawIndexedPrimitiveUP;
    device_vtbl.CreateVertexDeclaration = dev_CreateVertexDeclaration;
    device_vtbl.CreateVertexShader = dev_CreateVertexShader;
    device_vtbl.SetVertexShaderConstantF = dev_SetVertexShaderConstantF;
    device_vtbl.SetStreamSource = dev_SetStreamSource;
    device_vtbl.GetStreamSource = dev_GetStreamSource;
    device_vtbl.CreateQuery = dev_CreateQuery;
    device_vtbl.CheckResourceResidency = dev_CheckResourceResidency;

    FILL(texture_vtbl);
    texture_vtbl.QueryInterface = tex_QueryInterface;
    texture_vtbl.AddRef = tex_AddRef;
    texture_vtbl.Release = tex_Release;
    texture_vtbl.GetType = tex_GetType;
    texture_vtbl.GetSurfaceLevel = tex_GetSurfaceLevel;
    texture_vtbl.SetPrivateData = tex_SetPrivateData;

    FILL(surface_vtbl);
    surface_vtbl.QueryInterface = surf_QueryInterface;
    surface_vtbl.AddRef = surf_AddRef;
    surface_vtbl.Release = surf_Release;
    surface_vtbl.GetContainer = surf_GetContainer;
    surface_vtbl.GetDesc = surf_GetDesc;
    surface_vtbl.LockRect = surf_LockRect;
    surface_vtbl.UnlockRect = surf_UnlockRect;

    FILL(vb_vtbl);
    vb_vtbl.AddRef = vb_AddRef;
    vb_vtbl.Release = vb_Release;
    vb_vtbl.Lock = vb_Lock;
    vb_vtbl.GetType = vb_GetType;

    FILL(query_vtbl);
    query_vtbl.AddRef = q_AddRef;
    query_vtbl.Release = q_Release;
    query_vtbl.GetData = q_GetData;

    FILL(decl_vtbl);
    decl_vtbl.AddRef = decl_AddRef;
    decl_vtbl.Release = decl_Release;
    decl_vtbl.GetDeclaration = decl_GetDeclaration;

    FILL(vs_vtbl);
    vs_vtbl.AddRef = vs_AddRef;
    vs_vtbl.Release = vs_Release;
    vs_vtbl.GetFunction = vs_GetFunction;
}
