/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 J3: the dispatch against a fake device on Linux. A 4 GB-aligned mapping is the guest
 * window; ios_wow_native_read/_write follow clarification 4 and refuse a marked page; every call
 * is made as the gate makes it, with the stack an i386 caller leaves (return address at esp,
 * arguments from esp + 4). Covers every argument kind and special case of the table on a sample
 * of methods: pointer conversion and out-arguments, proxy identity across Get* and
 * QueryInterface, refcounts and proxy freeing, the float return, a refused pointer giving
 * D3DERR_INVALIDCALL without a native call, an unknown id, pinning, and one process at a time. */

#include "fake_native.h"

#include <stdarg.h>
#include <stdio.h>

void fake_native_init(void);
extern const void *d3d9_unix_call_funcs[];
extern const void *d3d9_unix_call_wow64_funcs[];

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b); \
    checks++; if (a_ != b_) { failures++; fprintf(stderr, "FAIL %s:%d: %s == %s (%#llx != %#llx)\n", \
    __FILE__, __LINE__, #a, #b, a_, b_); } } while (0)

static uint32_t rd32(uint32_t g)
{
    uint32_t v;
    memcpy(&v, gptr(g), 4);
    return v;
}

static void wr32(uint32_t g, uint32_t v)
{
    memcpy(gptr(g), &v, 4);
}

/* The gate's view of an i386 stdcall: [esp] the return address, the arguments after it. */
static struct ios_wow_native_ret gate_call(uint32_t method, int nargs, ...)
{
    struct ios_wow_native_ret ret = { 0xcccccccc, 0xcccccccc, 0xcccccccc, 0.0f };
    uint32_t esp = G_STACK + 0x8000;
    va_list ap;
    int i;

    wr32(esp, 0x00401234u);
    va_start(ap, nargs);
    for (i = 0; i < nargs; i++)
        wr32(esp + 4 + 4 * i, va_arg(ap, uint32_t));
    va_end(ap);
    d3d9w_dispatch(D3D9W_ID(method), esp, &ret);
    CHECK_EQ(ret.kind, IOS_WOW_NATIVE_RET_EAX);
    CHECK_EQ(ret.pop, 4u * D3D9W_METHOD_WORDS(method));
    if ((uint32_t)nargs != D3D9W_METHOD_WORDS(method))
    {
        failures++;
        fprintf(stderr, "FAIL: test passes %d words to method %#x\n", nargs, method);
    }
    return ret;
}

#define CALL(m, ...) gate_call(m, (int)(sizeof((uint32_t[]){ __VA_ARGS__ }) / 4), __VA_ARGS__)
#define CALL0(m) gate_call(m, 0)

static uint32_t guid_at(const GUID *g)
{
    uint32_t a = galloc(sizeof(GUID));
    memcpy(gptr(a), g, sizeof(GUID));
    return a;
}

/* The fake vtables the "thunk" hands Init: the guest addresses only have to be distinct. */
static uint32_t vtables_g;
#define FAKE_VTBL(k) (0x00500000u + (k) * 0x400u)

static void proxy_read(uint32_t g, struct d3d9w_proxy *p)
{
    memcpy(p, gptr(g), sizeof(*p));
}

/* ------------------------------------------------------------------ the tests */

static uint32_t d3d9_g, device_g, shared_g;

static void test_registration(void)
{
    struct d3d9w_register_params p;
    LONG (*wow64)(void *) = (LONG (*)(void *))d3d9_unix_call_wow64_funcs[0];
    LONG (*native64)(void *) = (LONG (*)(void *))d3d9_unix_call_funcs[0];

    memset(&p, 0, sizeof(p));
    p.version = D3D9W_ABI_VERSION;
    CHECK_EQ(wow64(&p), 0);
    CHECK_EQ(p.gate, 0x7ffe0004u);
    CHECK(fake_registered == d3d9w_dispatch);
    p.version = D3D9W_ABI_VERSION + 1;
    wow64(&p);
    CHECK_EQ(p.gate, 0);
    CHECK_EQ(p.reason, D3D9W_NOGATE_VERSION);
    CHECK(native64(&p) != 0);   /* 64-bit callers keep ARM64EC */
}

static void test_init_and_create(void)
{
    uint32_t vt[D3D9W_NUM_KINDS], status_g = galloc(4), out_g = galloc(4), k;
    struct ios_wow_native_ret r;
    struct d3d9w_proxy p;

    for (k = 0; k < D3D9W_NUM_KINDS; k++)
        vt[k] = FAKE_VTBL(k);
    vtables_g = galloc(sizeof(vt));
    memcpy(gptr(vtables_g), vt, sizeof(vt));

    r = CALL(D3D9W_M_Exports_Init, D3D9W_ABI_VERSION, vtables_g, D3D9W_NUM_KINDS, out_g);
    CHECK_EQ(r.eax, D3D9W_INIT_OK);
    shared_g = rd32(out_g);
    CHECK(shared_g >= G_ARENA && !(shared_g & 0xffff));
    CHECK_EQ(((struct d3d9w_shared *)gptr(shared_g))->magic, D3D9W_SHARED_MAGIC);

    r = CALL(D3D9W_M_Exports_Direct3DCreate9, D3D_SDK_VERSION, status_g);
    CHECK_EQ(rd32(status_g), D3D9W_CREATE_OK);
    d3d9_g = r.eax;
    CHECK(d3d9_g > shared_g && !(d3d9_g & 15));
    proxy_read(d3d9_g, &p);
    CHECK_EQ(p.vtbl, FAKE_VTBL(D3D9W_KIND_D3D9));
    CHECK_EQ(p.flags, 0);
    CHECK_EQ(D3D9W_HANDLE_INDEX(p.handle), (d3d9_g - shared_g) / D3D9W_SLOT_BYTES);
}

/* Converted struct in and out (D3DPRESENT_PARAMETERS: the 4-byte HWND), a handle argument,
 * and an interface out. */
static void test_create_device(void)
{
    struct d3d9w_g_D3DPRESENT_PARAMETERS *pp;
    uint32_t pp_g = galloc(sizeof(*pp)), out_g = galloc(4);
    struct ios_wow_native_ret r;
    struct d3d9w_proxy p;

    pp = gptr(pp_g);
    pp->BackBufferWidth = 800;
    pp->BackBufferHeight = 600;
    pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    pp->hDeviceWindow = 0x10020;
    pp->Windowed = TRUE;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    wr32(out_g, 0xdddddddd);
    r = CALL(D3D9W_M_D3D9_CreateDevice, d3d9_g, 0, D3DDEVTYPE_HAL, 0x10010, D3DCREATE_HARDWARE_VERTEXPROCESSING,
             pp_g, out_g);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK_EQ(fc.create_device, 1);
    CHECK_EQ((uintptr_t)fc.create_device_window, 0x10020);
    CHECK_EQ(pp->BackBufferCount, 1);          /* written back */
    CHECK_EQ(pp->hDeviceWindow, 0x10020);
    device_g = rd32(out_g);
    proxy_read(device_g, &p);
    CHECK_EQ(p.vtbl, FAKE_VTBL(D3D9W_KIND_Device));
}

static void test_values(void)
{
    struct ios_wow_native_ret r;
    uint32_t out_g = galloc(sizeof(struct d3d9w_guest_window_state));
    struct d3d9w_guest_window_state *ws = gptr(out_g);
    int reads = fake_reads;

    /* the hottest method: values only, one read of the stack, no allocation */
    r = CALL(D3D9W_M_Device_SetSamplerState, device_g, 3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK_EQ(fc.set_sampler, 1);
    CHECK_EQ(fc.sampler_args[0], 3);
    CHECK_EQ(fc.sampler_args[1], D3DSAMP_MAGFILTER);
    CHECK_EQ(fc.sampler_args[2], D3DTEXF_LINEAR);
    CHECK_EQ(fake_reads - reads, 1);

    /* float return: the bits in EAX (clarification 3) */
    r = CALL(D3D9W_M_Device_GetNPatchMode, device_g);
    CHECK_EQ(r.eax, d3d9w_float_bits(4.5f));

    /* handle return */
    r = CALL(D3D9W_M_D3D9_GetAdapterMonitor, d3d9_g, 0);
    CHECK_EQ(r.eax, 0x10001);

    /* the seam's window state, i386 layout */
    r = CALL(D3D9W_M_Exports_WindowState, device_g, out_g);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK_EQ(ws->device_window, 0x10020);
    CHECK_EQ(ws->focus_window, 0x10010);
    CHECK_EQ(ws->windowed, 1);
    CHECK_EQ(ws->backbuffer_width, 800);
    CHECK_EQ(ws->mode_format, D3DFMT_X8R8G8B8);
}

static void test_arrays(void)
{
    struct ios_wow_native_ret r;
    uint32_t c_g = galloc(300 * 16), rects_g = galloc(2 * sizeof(D3DRECT));
    float *c = gptr(c_g);
    D3DRECT *rects = gptr(rects_g);
    int i;

    for (i = 0; i < 300 * 4; i++)
        c[i] = (float)i;
    /* within the stack buffer (clarification 1: 256 registers) */
    r = CALL(D3D9W_M_Device_SetVertexShaderConstantF, device_g, 8, c_g, 3);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK_EQ(fc.constf_start, 8);
    CHECK_EQ(fc.constf_count, 3);
    CHECK(fc.constf_first[0] == 0.0f && fc.constf_last[3] == 11.0f);
    /* more than 256 registers (a software-vertex-processing device): the heap */
    r = CALL(D3D9W_M_Device_SetVertexShaderConstantF, device_g, 0, c_g, 300);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK(fc.constf_last[3] == 1199.0f);

    rects[0].x1 = 1; rects[0].y1 = 2; rects[0].x2 = 3; rects[0].y2 = 4;
    rects[1].x2 = 99;
    r = CALL(D3D9W_M_Device_Clear, device_g, 2, rects_g, D3DCLEAR_TARGET, 0xff00ff00, d3d9w_float_bits(0.25f), 0);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK_EQ(fc.clear_count, 2);
    CHECK_EQ(fc.clear_rects[0].y2, 4);
    CHECK_EQ(fc.clear_rects[1].x2, 99);
    CHECK(fc.clear_z == 0.25f);
}

/* D4: a pointer the read refuses gives D3DERR_INVALIDCALL (or 0 for a count) and native code
 * never runs. */
static void test_refused(void)
{
    struct ios_wow_native_ret r;
    int clears = fc.clear, rasters = fc.get_raster;
    uint32_t filter_g = galloc(sizeof(D3DDISPLAYMODEFILTER));

    r = CALL(D3D9W_M_Device_Clear, device_g, 1, G_REFUSED + 0x100, D3DCLEAR_TARGET, 0, 0, 0);
    CHECK_EQ(r.eax, (uint32_t)D3DERR_INVALIDCALL);
    CHECK_EQ(fc.clear, clears);
    r = CALL(D3D9W_M_Device_GetRasterStatus, device_g, 0, 0x1000);   /* below 64 KB */
    CHECK_EQ(r.eax, (uint32_t)D3DERR_INVALIDCALL);
    CHECK_EQ(fc.get_raster, rasters);
    r = CALL(D3D9W_M_Device_GetRasterStatus, device_g, 0, 0xfffffffc);   /* past 4 GB */
    CHECK_EQ(r.eax, (uint32_t)D3DERR_INVALIDCALL);
    CHECK_EQ(fc.get_raster, rasters);
    /* a count: 0 */
    r = CALL(D3D9W_M_D3D9_GetAdapterModeCountEx, d3d9_g, 0, G_REFUSED);
    CHECK_EQ(r.eax, 0);
    ((D3DDISPLAYMODEFILTER *)gptr(filter_g))->Format = D3DFMT_X8R8G8B8;
    r = CALL(D3D9W_M_D3D9_GetAdapterModeCountEx, d3d9_g, 0, filter_g);
    CHECK_EQ(r.eax, 3);
    /* an interface argument that is no proxy */
    r = CALL(D3D9W_M_Device_SetTexture, device_g, 0, G_DATA);
    CHECK_EQ(r.eax, (uint32_t)D3DERR_INVALIDCALL);
    /* This that is no proxy */
    r = CALL(D3D9W_M_Device_SetSamplerState, G_DATA, 0, 0, 0);
    CHECK_EQ(r.eax, (uint32_t)D3DERR_INVALIDCALL);
    /* This of another kind */
    r = CALL(D3D9W_M_Device_SetSamplerState, d3d9_g, 0, 0, 0);
    CHECK_EQ(r.eax, (uint32_t)D3DERR_INVALIDCALL);
}

static void test_out_structs(void)
{
    struct ios_wow_native_ret r;
    uint32_t rs_g = galloc(8 + 4), cp_g = galloc(16 + 4), id_g = galloc(1100 + 4);
    struct d3d9w_g_D3DDEVICE_CREATION_PARAMETERS *cp = gptr(cp_g);

    wr32(rs_g + 8, 0xc0ffee00);
    r = CALL(D3D9W_M_Device_GetRasterStatus, device_g, 0, rs_g);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK_EQ(rd32(rs_g), TRUE);
    CHECK_EQ(rd32(rs_g + 4), 77);
    CHECK_EQ(rd32(rs_g + 8), 0xc0ffee00);   /* nothing past the struct */

    wr32(cp_g + 16, 0xc0ffee01);
    r = CALL(D3D9W_M_Device_GetCreationParameters, device_g, cp_g);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK_EQ(cp->hFocusWindow, 0x10010);
    CHECK_EQ(cp->BehaviorFlags, D3DCREATE_HARDWARE_VERTEXPROCESSING);
    CHECK_EQ(rd32(cp_g + 16), 0xc0ffee01);

    /* 1100 bytes on i386, 1104 natively: the 4 after the program's struct stay as they were */
    wr32(id_g + 1100, 0xc0ffee02);
    r = CALL(D3D9W_M_D3D9_GetAdapterIdentifier, d3d9_g, 0, 0, id_g);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK(!strcmp((const char *)gptr(id_g) + 512, "fake adapter"));
    CHECK_EQ(rd32(id_g + 1064), 0x106b);   /* VendorId */
    CHECK_EQ(rd32(id_g + 1096), 7);        /* WHQLLevel */
    CHECK_EQ(rd32(id_g + 1100), 0xc0ffee02);
}

/* E2, G1, G2, clarification 5. */
static void test_identity_and_lifetime(void)
{
    struct ios_wow_native_ret r;
    uint32_t out_g = galloc(4), tex_g, tex2_g, surf_g, surf2_g, box_g;
    uint32_t iid_tex = guid_at(&IID_IDirect3DTexture9), iid_base = guid_at(&IID_IDirect3DBaseTexture9);
    uint32_t iid_unk = guid_at(&IID_IUnknown), iid_dev9ex = guid_at(&IID_IDirect3DDevice9Ex);
    uint32_t iid_res = guid_at(&IID_IDirect3DResource9);
    struct d3d9w_proxy p;
    int destroyed = fake_textures_destroyed;
    uint32_t handle;

    r = CALL(D3D9W_M_Device_CreateTexture, device_g, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out_g, 0);
    CHECK_EQ(r.eax, D3D_OK);
    tex_g = rd32(out_g);
    proxy_read(tex_g, &p);
    CHECK_EQ(p.vtbl, FAKE_VTBL(D3D9W_KIND_Texture));
    handle = p.handle;

    /* SetTexture passes the native object; GetTexture (an IDirect3DBaseTexture9) gives the same
     * proxy back */
    r = CALL(D3D9W_M_Device_SetTexture, device_g, 0, tex_g);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK(fc.set_texture_native != NULL);
    r = CALL(D3D9W_M_Device_GetTexture, device_g, 0, out_g);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK_EQ(rd32(out_g), tex_g);
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, tex_g).eax, 1);

    /* QueryInterface: the same proxy for every interface of the object */
    box_g = galloc(4);
    CHECK_EQ(CALL(D3D9W_M_Texture_QueryInterface, tex_g, iid_base, box_g).eax, S_OK);
    CHECK_EQ(rd32(box_g), tex_g);
    CHECK_EQ(CALL(D3D9W_M_Texture_QueryInterface, tex_g, iid_unk, box_g).eax, S_OK);
    CHECK_EQ(rd32(box_g), tex_g);
    CHECK_EQ(CALL(D3D9W_M_Texture_QueryInterface, tex_g, iid_res, box_g).eax, S_OK);
    CHECK_EQ(rd32(box_g), tex_g);
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, tex_g).eax, 3);
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, tex_g).eax, 2);
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, tex_g).eax, 1);
    wr32(box_g, 0xbad);
    CHECK_EQ(CALL(D3D9W_M_Texture_QueryInterface, tex_g, iid_dev9ex, box_g).eax, (uint32_t)E_NOINTERFACE);
    CHECK_EQ(rd32(box_g), 0);

    /* sub-resource: one proxy, its container is the texture's proxy */
    r = CALL(D3D9W_M_Texture_GetSurfaceLevel, tex_g, 0, out_g);
    CHECK_EQ(r.eax, D3D_OK);
    surf_g = rd32(out_g);
    proxy_read(surf_g, &p);
    CHECK_EQ(p.vtbl, FAKE_VTBL(D3D9W_KIND_Surface));
    r = CALL(D3D9W_M_Texture_GetSurfaceLevel, tex_g, 0, out_g);
    surf2_g = rd32(out_g);
    CHECK_EQ(surf2_g, surf_g);
    r = CALL(D3D9W_M_Surface_GetContainer, surf_g, iid_tex, box_g);
    CHECK_EQ(r.eax, S_OK);
    CHECK_EQ(rd32(box_g), tex_g);
    /* the texture's count: 1 (program) + 2 (surface refs) + 1 (container) */
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, tex_g).eax, 3);
    CHECK_EQ(CALL(D3D9W_M_Surface_Release, surf_g).eax, 2);
    CHECK_EQ(CALL(D3D9W_M_Surface_Release, surf_g).eax, 1);

    /* the last public Release: the device still holds it, so the proxy stays (clarification 5) */
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, tex_g).eax, 0);
    CHECK_EQ(fake_textures_destroyed, destroyed);
    r = CALL(D3D9W_M_Device_GetTexture, device_g, 0, out_g);
    CHECK_EQ(rd32(out_g), tex_g);
    proxy_read(tex_g, &p);
    CHECK_EQ(p.handle, handle);
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, tex_g).eax, 0);

    /* unbinding destroys it: both proxies go */
    r = CALL(D3D9W_M_Device_SetTexture, device_g, 0, 0);
    CHECK_EQ(fake_textures_destroyed, destroyed + 1);
    proxy_read(tex_g, &p);
    CHECK(p.flags & D3D9W_PROXY_GONE);
    proxy_read(surf_g, &p);
    CHECK(p.flags & D3D9W_PROXY_GONE);
    /* a call on the stale pointer finds no object */
    CHECK_EQ(CALL(D3D9W_M_Texture_GetLevelCount, tex_g).eax, 0);
    CHECK_EQ(CALL(D3D9W_M_Texture_AddDirtyRect, tex_g, 0).eax, (uint32_t)D3DERR_INVALIDCALL);

    /* a new object reuses a freed slot under a new generation */
    r = CALL(D3D9W_M_Device_CreateTexture, device_g, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out_g, 0);
    tex2_g = rd32(out_g);
    CHECK(tex2_g == tex_g || tex2_g == surf_g);
    proxy_read(tex2_g, &p);
    CHECK(D3D9W_HANDLE_GEN(p.handle) != 0);
    CHECK_EQ(p.flags, 0);
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, tex2_g).eax, 0);

    /* an interface out the program did not ask for (NULL) */
    r = CALL(D3D9W_M_Device_CreateQuery, device_g, D3DQUERYTYPE_EVENT, 0);
    CHECK_EQ(r.eax, D3D_OK);
}

static void test_lock_and_data(void)
{
    struct ios_wow_native_ret r;
    uint32_t out_g = galloc(4), tex_g, surf_g, lr_g = galloc(8), vb_g, q_g, data_g = galloc(8);
    int unlocks;

    CALL(D3D9W_M_Device_CreateTexture, device_g, 32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out_g, 0);
    tex_g = rd32(out_g);
    CALL(D3D9W_M_Texture_GetSurfaceLevel, tex_g, 0, out_g);
    surf_g = rd32(out_g);

    /* F5: LockRect hands out the guest address of window memory */
    r = CALL(D3D9W_M_Surface_LockRect, surf_g, lr_g, 0, 0);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK_EQ(rd32(lr_g), 128);
    CHECK_EQ(rd32(lr_g + 4), G_DATA + 0x80000);
    /* memory outside the window: refused, and unlocked again */
    unlocks = fc.unlocks;
    fake_lock_outside = 1;
    r = CALL(D3D9W_M_Surface_LockRect, surf_g, lr_g, 0, 0);
    fake_lock_outside = 0;
    CHECK_EQ(r.eax, (uint32_t)D3DERR_INVALIDCALL);
    CHECK_EQ(fc.unlocks, unlocks + 1);
    CHECK_EQ(rd32(lr_g + 4), 0);

    /* a vertex buffer's Lock, and stream source in and out */
    CALL(D3D9W_M_Device_CreateVertexBuffer, device_g, 256, 0, 0, D3DPOOL_DEFAULT, out_g, 0);
    vb_g = rd32(out_g);
    r = CALL(D3D9W_M_VertexBuffer_Lock, vb_g, 64, 32, out_g, 0);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK(rd32(out_g) >= G_DATA && rd32(out_g) < G_DATA + 0x100000);
    CHECK_EQ(CALL(D3D9W_M_Device_SetStreamSource, device_g, 0, vb_g, 0, 32).eax, D3D_OK);
    {
        uint32_t off_g = galloc(4), stride_g = galloc(4);

        r = CALL(D3D9W_M_Device_GetStreamSource, device_g, 0, out_g, off_g, stride_g);
        CHECK_EQ(rd32(out_g), vb_g);
        CHECK_EQ(rd32(off_g), 16);
        CHECK_EQ(rd32(stride_g), 32);
        CHECK_EQ(CALL(D3D9W_M_VertexBuffer_Release, vb_g).eax, 1);
    }

    /* an interface array (CheckResourceResidency) */
    {
        uint32_t arr_g = galloc(8);

        wr32(arr_g, surf_g);
        wr32(arr_g + 4, vb_g);
        r = CALL(D3D9W_M_Device_CheckResourceResidency, device_g, arr_g, 2);
        CHECK_EQ(r.eax, D3D_OK);
        CHECK_EQ(fc.residency_count, 2);
        CHECK(fc.residency_first != NULL);
        wr32(arr_g + 4, G_DATA);   /* not a proxy */
        r = CALL(D3D9W_M_Device_CheckResourceResidency, device_g, arr_g, 2);
        CHECK_EQ(r.eax, (uint32_t)D3DERR_INVALIDCALL);
    }

    /* Query::GetData: untouched while not ready, written with S_OK */
    CALL(D3D9W_M_Device_CreateQuery, device_g, D3DQUERYTYPE_OCCLUSION, out_g);
    q_g = rd32(out_g);
    wr32(data_g, 0xabcd);
    fake_query_ready = 0;
    r = CALL(D3D9W_M_Query_GetData, q_g, data_g, 4, D3DGETDATA_FLUSH);
    CHECK_EQ(r.eax, S_FALSE);
    CHECK_EQ(rd32(data_g), 0xabcd);
    fake_query_ready = 1;
    r = CALL(D3D9W_M_Query_GetData, q_g, data_g, 4, 0);
    CHECK_EQ(r.eax, S_OK);
    CHECK_EQ(rd32(data_g), 1234);
    CHECK_EQ(CALL(D3D9W_M_Query_Release, q_g).eax, 0);

    CHECK_EQ(CALL(D3D9W_M_Surface_Release, surf_g).eax, 1);
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, tex_g).eax, 0);
    /* unbound, the vertex buffer goes with its last Release */
    CHECK_EQ(CALL(D3D9W_M_Device_SetStreamSource, device_g, 0, 0, 0, 0).eax, D3D_OK);
    CHECK_EQ(CALL(D3D9W_M_VertexBuffer_Release, vb_g).eax, 0);
}

/* F3's length rules. */
static void test_lengths(void)
{
    struct ios_wow_native_ret r;
    uint32_t out_g = galloc(4), vs_g, size_g = galloc(4), fn_g;
    void *heap;
    const DWORD *copy;
    size_t n;
    uint8_t stack[256];
    /* vs_2_0: a def whose literal is the end token's value, a comment whose payload holds it too */
    static const DWORD vs2[] = {
        0xfffe0200,
        0x05000051, 0xa00f0000, 0x0000ffff, 0x3f800000, 0x00000000, 0x3f800000,   /* def c0 */
        0x0002fffe, 0x0000ffff, 0x12345678,                                       /* comment */
        0x02000001, 0x800f0000, 0xa0e40000,                                       /* mov */
        0x0000ffff,
    };
    /* vs_1_1: def (6 tokens) and parameter tokens with bit 31 */
    static const DWORD vs1[] = {
        0xfffe0101,
        0x00000051, 0xa00f0000, 0x0000ffff, 0x00000000, 0x00000000, 0x00000000,
        0x00000001, 0xc00f0000, 0xa0e40000,
        0x0000ffff,
    };
    uint32_t g;

    /* ending right at the page the read refuses (G_DATA's region ends where G_REFUSED starts):
     * the walk must not read past the end token */
    g = G_REFUSED - sizeof(vs2);
    memcpy(gptr(g), vs2, sizeof(vs2));
    CHECK(d3d9w_shader_in(g, stack, sizeof(stack), &heap, &copy, &n));
    CHECK_EQ(n, sizeof(vs2) / 4);
    CHECK(!memcmp(copy, vs2, sizeof(vs2)));
    free(heap);
    g = galloc(sizeof(vs1));
    memcpy(gptr(g), vs1, sizeof(vs1));
    CHECK(d3d9w_shader_in(g, stack, sizeof(stack), &heap, &copy, &n));
    CHECK_EQ(n, sizeof(vs1) / 4);
    free(heap);
    /* a stream that runs into the refused page before its end token: refused */
    g = G_REFUSED - 8;
    wr32(g, 0xfffe0200);
    wr32(g + 4, 0x02000001);   /* mov: two more tokens, on the refused page */
    CHECK(!d3d9w_shader_in(g, stack, sizeof(stack), &heap, &copy, &n));
    free(heap);

    g = galloc(sizeof(vs2));
    memcpy(gptr(g), vs2, sizeof(vs2));
    r = CALL(D3D9W_M_Device_CreateVertexShader, device_g, g, out_g);
    CHECK_EQ(r.eax, D3D_OK);
    vs_g = rd32(out_g);
    /* GetFunction: the size query, then the bytes */
    wr32(size_g, 0);
    r = CALL(D3D9W_M_VertexShader_GetFunction, vs_g, 0, size_g);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK_EQ(rd32(size_g), 256);
    fn_g = galloc(256);
    r = CALL(D3D9W_M_VertexShader_GetFunction, vs_g, fn_g, size_g);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK(!memcmp(gptr(fn_g), vs2, sizeof(vs2)));
    CHECK_EQ(CALL(D3D9W_M_VertexShader_Release, vs_g).eax, 0);

    /* DrawPrimitiveUP: 2 triangles x 3 vertices x 16 bytes */
    {
        uint32_t v_g = galloc(96);
        int i, draws;

        for (i = 0; i < 96; i++)
            ((uint8_t *)gptr(v_g))[i] = (uint8_t)i;
        r = CALL(D3D9W_M_Device_DrawPrimitiveUP, device_g, D3DPT_TRIANGLELIST, 2, v_g, 16);
        CHECK_EQ(r.eax, D3D_OK);
        CHECK_EQ(fc.draw_up_len, 96);
        CHECK(!memcmp(fc.draw_up_bytes, gptr(v_g), 96));
        /* the 96 bytes end on the refused page: refused */
        draws = fc.draw_up;
        g = G_REFUSED - 95;
        CHECK_EQ(CALL(D3D9W_M_Device_DrawPrimitiveUP, device_g, D3DPT_TRIANGLELIST, 2, g, 16).eax,
                 (uint32_t)D3DERR_INVALIDCALL);
        CHECK_EQ(fc.draw_up, draws);
    }
    /* DrawIndexedPrimitiveUP: (min + count) x stride vertices, 16-bit indices */
    {
        uint32_t i_g = galloc(6 * 2), v_g = galloc(6 * 8);
        uint16_t *idx = gptr(i_g);
        int i;

        for (i = 0; i < 6; i++)
            idx[i] = (uint16_t)(5 - i);
        for (i = 0; i < 48; i++)
            ((uint8_t *)gptr(v_g))[i] = (uint8_t)(100 + i);
        r = CALL(D3D9W_M_Device_DrawIndexedPrimitiveUP, device_g, D3DPT_TRIANGLELIST, 2, 4, 2, i_g,
                 D3DFMT_INDEX16, v_g, 8);
        CHECK_EQ(r.eax, D3D_OK);
        CHECK_EQ(fc.iup_indices[0], 5);
        CHECK_EQ(fc.iup_indices[5], 0);
        CHECK(!memcmp(fc.iup_vertices, gptr(v_g), 48));
    }
    /* Present: two optional RECTs and an RGNDATA of two rectangles */
    {
        uint32_t src_g = galloc(16), rgn_g = galloc(32 + 2 * 16);
        RGNDATAHEADER *h = gptr(rgn_g);
        RECT *rc = gptr(rgn_g + 32);

        ((RECT *)gptr(src_g))->right = 320;
        h->dwSize = 32;
        h->iType = RDH_RECTANGLES;
        h->nCount = 2;
        rc[1].bottom = 77;
        r = CALL(D3D9W_M_Device_Present, device_g, src_g, 0, 0, rgn_g);
        CHECK_EQ(r.eax, D3D_OK);
        CHECK_EQ(fc.present_src.right, 320);
        CHECK(fc.present_has_rgn);
        CHECK_EQ(fc.rgn_count, 2);
        CHECK_EQ(fc.rgn_rects[1].bottom, 77);
    }
    /* CreateVertexDeclaration up to D3DDECL_END, GetDeclaration back */
    {
        static const D3DVERTEXELEMENT9 el[] = {
            { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
            { 0xff, 0, D3DDECLTYPE_UNUSED, 0, 0, 0 },
        };
        uint32_t el_g = galloc(sizeof(el)), decl_g, back_g = galloc(sizeof(el)), n_g = galloc(4);

        memcpy(gptr(el_g), el, sizeof(el));
        r = CALL(D3D9W_M_Device_CreateVertexDeclaration, device_g, el_g, out_g);
        CHECK_EQ(r.eax, D3D_OK);
        decl_g = rd32(out_g);
        r = CALL(D3D9W_M_VertexDecl_GetDeclaration, decl_g, back_g, n_g);
        CHECK_EQ(r.eax, D3D_OK);
        CHECK_EQ(rd32(n_g), 3);
        CHECK(!memcmp(gptr(back_g), el, sizeof(el)));
        CHECK_EQ(CALL(D3D9W_M_VertexDecl_Release, decl_g).eax, 0);
    }
}

/* Reset: a converted struct in and out. */
static void test_reset(void)
{
    struct d3d9w_g_D3DPRESENT_PARAMETERS *pp;
    uint32_t pp_g = galloc(sizeof(*pp));
    struct ios_wow_native_ret r;

    pp = gptr(pp_g);
    pp->BackBufferWidth = 0;
    pp->hDeviceWindow = 0x10044;
    pp->Windowed = TRUE;
    r = CALL(D3D9W_M_Device_Reset, device_g, pp_g);
    CHECK_EQ(r.eax, D3D_OK);
    CHECK_EQ((uintptr_t)fc.reset_window, 0x10044);
    CHECK_EQ(pp->BackBufferWidth, 640);
}

/* E3: pinned proxies outlive their object until the thunk unpins them. */
static void test_pin(void)
{
    struct ios_wow_native_ret r;
    uint32_t out_g = galloc(4), tex_g, sweep0;
    struct d3d9w_shared *sh = gptr(shared_g);
    struct d3d9w_proxy p;

    CALL(D3D9W_M_Device_CreateTexture, device_g, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out_g, 0);
    tex_g = rd32(out_g);
    CHECK_EQ(CALL(D3D9W_M_Exports_Pin, tex_g).eax, D3D_OK);
    proxy_read(tex_g, &p);
    CHECK(p.flags & D3D9W_PROXY_PINNED);
    sweep0 = sh->sweep;
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, tex_g).eax, 0);
    proxy_read(tex_g, &p);
    CHECK(p.flags & D3D9W_PROXY_GONE);
    CHECK_EQ(sh->sweep, sweep0 + 1);
    /* the slot is kept: a new object gets another one */
    CALL(D3D9W_M_Device_CreateTexture, device_g, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out_g, 0);
    CHECK(rd32(out_g) != tex_g);
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, rd32(out_g)).eax, 0);
    /* unpinned, it goes back */
    CHECK_EQ(CALL(D3D9W_M_Exports_Unpin, tex_g).eax, D3D_OK);
    CALL(D3D9W_M_Device_CreateTexture, device_g, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out_g, 0);
    CHECK_EQ(rd32(out_g), tex_g);
    CHECK_EQ(CALL(D3D9W_M_Texture_Release, rd32(out_g)).eax, 0);
    /* SetPrivateData with D3DSPD_IUNKNOWN is the thunk's: native code never gets it */
    {
        int sets = fc.set_private;
        uint32_t guid_g = guid_at(&IID_IUnknown);

        CALL(D3D9W_M_Device_CreateTexture, device_g, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out_g, 0);
        tex_g = rd32(out_g);
        r = CALL(D3D9W_M_Texture_SetPrivateData, tex_g, guid_g, G_DATA, 4, D3DSPD_IUNKNOWN);
        CHECK_EQ(r.eax, (uint32_t)D3DERR_INVALIDCALL);
        CHECK_EQ(fc.set_private, sets);
        r = CALL(D3D9W_M_Texture_SetPrivateData, tex_g, guid_g, G_DATA, 16, 0);
        CHECK_EQ(r.eax, D3D_OK);
        CHECK_EQ(fc.set_private, sets + 1);
        CHECK_EQ(CALL(D3D9W_M_Texture_Release, tex_g).eax, 0);
    }
}

/* G3: an unknown id answers E_NOTIMPL, pops what its number says, logs once. */
static void test_unknown(void)
{
    struct ios_wow_native_ret r = { 0, 0, 0, 0 };
    uint32_t esp = G_STACK + 0x8000;
    int lines = fake_log_lines;

    d3d9w_dispatch(IOS_WOW_NATIVE_ID(1, (3u << 16) | (D3D9W_KIND_Query << 8) | 200), esp, &r);
    CHECK_EQ(r.eax, (uint32_t)E_NOTIMPL);
    CHECK_EQ(r.pop, 12);
    CHECK_EQ(fake_log_lines, lines + 1);
    d3d9w_dispatch(IOS_WOW_NATIVE_ID(1, (3u << 16) | (D3D9W_KIND_Query << 8) | 200), esp, &r);
    CHECK_EQ(fake_log_lines, lines + 1);
    /* a known slot with the wrong argument count is a mismatched build too */
    d3d9w_dispatch(IOS_WOW_NATIVE_ID(1, (2u << 16) | D3D9W_METHOD_SLOT(D3D9W_M_Device_SetSamplerState)), esp, &r);
    CHECK_EQ(r.eax, (uint32_t)E_NOTIMPL);
    CHECK_EQ(r.pop, 8);
    /* another library's id */
    d3d9w_dispatch(IOS_WOW_NATIVE_ID(2, D3D9W_M_Device_SetSamplerState), esp, &r);
    CHECK_EQ(r.eax, (uint32_t)E_NOTIMPL);
}

/* Clarification 10: one process at a time. */
static void test_one_process(void)
{
    uint32_t out_g = galloc(4), status_g = galloc(4);
    struct ios_wow_native_ret r;
    unsigned long other = (unsigned long)(uintptr_t)fake_window + (16ul << 32);

    CHECK(d3d9w_live_count() > 0);
    fake_set_base_override(other);
    r = CALL(D3D9W_M_Exports_Init, D3D9W_ABI_VERSION, vtables_g, D3D9W_NUM_KINDS, out_g);
    CHECK_EQ(r.eax, D3D9W_INIT_BUSY);
    r = CALL(D3D9W_M_Exports_Direct3DCreate9, D3D_SDK_VERSION, status_g);
    CHECK_EQ(r.eax, 0);
    CHECK_EQ(rd32(status_g), D3D9W_CREATE_BUSY);
    fake_set_base_override(0);

    /* the owner lets everything go: the next process may take over */
    CHECK_EQ(CALL(D3D9W_M_Device_Release, device_g).eax, 0);
    CHECK_EQ(CALL(D3D9W_M_D3D9_Release, d3d9_g).eax, 0);
    CHECK_EQ(d3d9w_live_count(), 0);
    CHECK_EQ(fake_live_objects, 0);
    fake_set_base_override(other);
    r = CALL(D3D9W_M_Exports_Init, D3D9W_ABI_VERSION, vtables_g, D3D9W_NUM_KINDS, out_g);
    CHECK_EQ(r.eax, D3D9W_INIT_OK);
    /* and the old process's stale proxies find nothing, through the thunk's own calls neither */
    fake_set_base_override(0);
    r = CALL(D3D9W_M_Device_SetSamplerState, device_g, 0, 0, 0);
    CHECK_EQ(r.eax, (uint32_t)D3DERR_INVALIDCALL);
    r = CALL(D3D9W_M_Exports_Pin, device_g);
    CHECK_EQ(r.eax, (uint32_t)D3DERR_INVALIDCALL);
}

/* Clarification 13: the gate's reserved method 0xffffff when a process ends. */
static void test_process_end(void)
{
    unsigned long other = (unsigned long)(uintptr_t)fake_window + (16ul << 32);
    uint32_t status_g = galloc(4), out_g = galloc(4), d3d_g, dev_g, tex_g, d3d2_g;
    uint32_t pp_g = galloc(sizeof(struct d3d9w_g_D3DPRESENT_PARAMETERS));
    struct d3d9w_g_D3DPRESENT_PARAMETERS *pp = gptr(pp_g);
    struct ios_wow_native_ret r = { 0, 0, 0, 0.0f };
    int reads, writes, objects;

    /* the owner (test_one_process left it to window "other") makes a D3D9, a device, a texture */
    fake_set_base_override(other);
    r = CALL(D3D9W_M_Exports_Direct3DCreate9, D3D_SDK_VERSION, status_g);
    CHECK_EQ(rd32(status_g), D3D9W_CREATE_OK);
    d3d_g = r.eax;
    pp->BackBufferWidth = 320;
    pp->BackBufferHeight = 200;
    pp->Windowed = TRUE;
    pp->hDeviceWindow = 0x10066;
    CHECK_EQ(CALL(D3D9W_M_D3D9_CreateDevice, d3d_g, 0, D3DDEVTYPE_HAL, 0x10066, 0, pp_g, out_g).eax, D3D_OK);
    dev_g = rd32(out_g);
    CHECK_EQ(CALL(D3D9W_M_Device_CreateTexture, dev_g, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out_g, 0).eax, D3D_OK);
    tex_g = rd32(out_g);
    CHECK_EQ(d3d9w_live_count(), 3);
    objects = fake_live_objects;

    /* a process that does not own the native frontend ends: it owns nothing here */
    fake_set_base_override(0);
    d3d9w_dispatch(IOS_WOW_NATIVE_ID(1, IOS_WOW_NATIVE_METHOD_PROCESS_END), 0, &r);
    CHECK_EQ(fake_process_gone_calls, 0);
    CHECK_EQ(d3d9w_live_count(), 3);

    /* the owner ends: every proxy goes, no guest memory is read or written (esp is not even
     * valid), the seam is called once, and no native object is released */
    fake_set_base_override(other);
    reads = fake_reads;
    writes = fake_writes;
    d3d9w_dispatch(IOS_WOW_NATIVE_ID(1, IOS_WOW_NATIVE_METHOD_PROCESS_END), 0, &r);
    CHECK_EQ(fake_reads, reads);
    CHECK_EQ(fake_writes, writes);
    CHECK_EQ(fake_process_gone_calls, 1);
    CHECK_EQ(d3d9w_live_count(), 0);
    CHECK_EQ(fake_live_objects, objects);
    /* its proxies find nothing (the fake would abort if GetLevelCount reached it) */
    CHECK_EQ(CALL(D3D9W_M_Texture_GetLevelCount, tex_g).eax, 0);
    CHECK_EQ(CALL(D3D9W_M_Device_SetSamplerState, dev_g, 0, 0, 0).eax, (uint32_t)D3DERR_INVALIDCALL);

    /* the next process takes the native frontend while the orphans are still alive */
    fake_set_base_override(0);
    r = CALL(D3D9W_M_Exports_Init, D3D9W_ABI_VERSION, vtables_g, D3D9W_NUM_KINDS, out_g);
    CHECK_EQ(r.eax, D3D9W_INIT_OK);
    r = CALL(D3D9W_M_Exports_Direct3DCreate9, D3D_SDK_VERSION, status_g);
    CHECK_EQ(rd32(status_g), D3D9W_CREATE_OK);
    d3d2_g = r.eax;
    CHECK(d3d2_g != 0);
    CHECK_EQ(d3d9w_live_count(), 1);
    CHECK_EQ(CALL(D3D9W_M_D3D9_Release, d3d2_g).eax, 0);
    CHECK_EQ(fake_live_objects, objects);
    /* the new owner's own end calls the seam again; a second end of the old one does not */
    d3d9w_dispatch(IOS_WOW_NATIVE_ID(1, IOS_WOW_NATIVE_METHOD_PROCESS_END), 0, &r);
    CHECK_EQ(fake_process_gone_calls, 2);
    fake_set_base_override(other);
    d3d9w_dispatch(IOS_WOW_NATIVE_ID(1, IOS_WOW_NATIVE_METHOD_PROCESS_END), 0, &r);
    CHECK_EQ(fake_process_gone_calls, 2);
    fake_set_base_override(0);
    CHECK_EQ(d3d9w_live_count(), 0);
}

int main(void)
{
    fake_window_init();
    fake_native_init();

    test_registration();
    test_init_and_create();
    test_create_device();
    test_values();
    test_arrays();
    test_refused();
    test_out_structs();
    test_identity_and_lifetime();
    test_lock_and_data();
    test_lengths();
    test_reset();
    test_pin();
    test_unknown();
    test_one_process();
    test_process_end();

    printf("test_dispatch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
