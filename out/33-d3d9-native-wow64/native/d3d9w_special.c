/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 F3, F5: the methods the table marks @special=<helper>. The generated function has
 * read the arguments (a[0] is This's guest address, a[1] the first argument) and resolved This
 * to the native object of the method's kind; the helper does the rest and sets ret->eax. */

#include "d3d9w_dispatch.h"

#define SP(name) void d3d9w_sp_##name(unsigned int kind, void *This, const uint32_t *a, \
                                      struct ios_wow_native_ret *ret)

SP(set_private);
SP(get_private);
SP(get_function);
SP(get_declaration);
SP(get_data);
SP(lock_rect);
SP(lock_box);
SP(lock_buffer);
SP(draw_up);
SP(draw_indexed_up);

/* ------------------------------------------------------------------ private data */

/* The resource interfaces share these methods' vtable slots; each is called through its own
 * interface type all the same. */
#define RESOURCE_CALL(kind, This, method, ...) \
    ((kind) == D3D9W_KIND_Surface ? IDirect3DSurface9_##method((IDirect3DSurface9 *)(This), __VA_ARGS__) : \
     (kind) == D3D9W_KIND_Texture ? IDirect3DTexture9_##method((IDirect3DTexture9 *)(This), __VA_ARGS__) : \
     (kind) == D3D9W_KIND_CubeTexture ? IDirect3DCubeTexture9_##method((IDirect3DCubeTexture9 *)(This), __VA_ARGS__) : \
     (kind) == D3D9W_KIND_VolumeTexture ? IDirect3DVolumeTexture9_##method((IDirect3DVolumeTexture9 *)(This), __VA_ARGS__) : \
     (kind) == D3D9W_KIND_Volume ? IDirect3DVolume9_##method((IDirect3DVolume9 *)(This), __VA_ARGS__) : \
     (kind) == D3D9W_KIND_VertexBuffer ? IDirect3DVertexBuffer9_##method((IDirect3DVertexBuffer9 *)(This), __VA_ARGS__) : \
     IDirect3DIndexBuffer9_##method((IDirect3DIndexBuffer9 *)(This), __VA_ARGS__))

/* SetPrivateData(REFGUID, const void *data, DWORD size, DWORD flags). An IUnknown entry is the
 * thunk's (spec 33 E3: native code cannot AddRef a program object), so one never gets here. */
SP(set_private)
{
    D3D9W_STACK_ARRAY(b);
    void *heap = NULL, *p = NULL;
    GUID guid;
    HRESULT hr;

    if (a[4] & D3DSPD_IUNKNOWN)
    {
        d3d9w_log_once(0xf000u | kind, "SetPrivateData with D3DSPD_IUNKNOWN reached native code");
        ret->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    if (!d3d9w_read(&guid, a[1], sizeof(guid)) || !d3d9w_array_in(a[2], a[3], b, sizeof(b), &heap, &p))
    {
        free(heap);
        ret->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    hr = RESOURCE_CALL(kind, This, SetPrivateData, &guid, p, a[3], a[4]);
    free(heap);
    ret->eax = (uint32_t)hr;
}

/* A buffer the method fills, with its size in and out through a pointer (GetPrivateData,
 * GetFunction): the size is copied in, the buffer only out, and only on success. */
struct sized_out
{
    D3D9W_STACK_ARRAY(stack);
    void *heap;
    void *buf;          /* NULL when the program passed no buffer */
    DWORD size, in_size;
    DWORD *psize;
};

static int sized_out_in(struct sized_out *s, uint32_t g_buf, uint32_t g_size)
{
    s->heap = NULL;
    s->buf = NULL;
    s->size = 0;
    s->psize = NULL;
    if (g_size)
    {
        if (!d3d9w_read(&s->size, g_size, sizeof(s->size)))
            return 0;
        s->psize = &s->size;
    }
    s->in_size = s->size;
    if (g_buf)
    {
        size_t n = s->psize ? s->size : 0;

        if (n > D3D9W_MAX_COPY || !d3d9w_guest_ok(g_buf, n))
            return 0;
        if (n <= sizeof(s->stack))
            s->buf = s->stack;
        else if (!(s->buf = s->heap = malloc(n)))
            return 0;
    }
    return 1;
}

static void sized_out_back(struct sized_out *s, HRESULT hr, uint32_t g_buf, uint32_t g_size)
{
    if (s->psize)
        d3d9w_write(g_size, &s->size, sizeof(s->size));
    if (hr == D3D_OK && g_buf && s->psize)
        d3d9w_write(g_buf, s->buf, s->size < s->in_size ? s->size : s->in_size);
    free(s->heap);
}

/* GetPrivateData(REFGUID, void *data, DWORD *size). The thunk answers IUnknown entries itself
 * and asks native for the rest. */
SP(get_private)
{
    struct sized_out s;
    GUID guid;
    HRESULT hr;

    s.heap = NULL;
    if (!d3d9w_read(&guid, a[1], sizeof(guid)) || !sized_out_in(&s, a[2], a[3]))
    {
        free(s.heap);
        ret->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    hr = RESOURCE_CALL(kind, This, GetPrivateData, &guid, s.buf, s.psize);
    sized_out_back(&s, hr, a[2], a[3]);
    ret->eax = (uint32_t)hr;
}

/* IDirect3DVertexShader9 / IDirect3DPixelShader9::GetFunction(void *data, UINT *size). */
SP(get_function)
{
    struct sized_out s;
    HRESULT hr;

    if (!sized_out_in(&s, a[1], a[2]))
    {
        free(s.heap);
        ret->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    if (kind == D3D9W_KIND_VertexShader)
        hr = IDirect3DVertexShader9_GetFunction((IDirect3DVertexShader9 *)This, s.buf, (UINT *)s.psize);
    else
        hr = IDirect3DPixelShader9_GetFunction((IDirect3DPixelShader9 *)This, s.buf, (UINT *)s.psize);
    sized_out_back(&s, hr, a[1], a[2]);
    ret->eax = (uint32_t)hr;
}

/* IDirect3DVertexDeclaration9::GetDeclaration(D3DVERTEXELEMENT9 *elements, UINT *count): the
 * API passes no buffer size, so the count is asked first and the buffer sized from it. */
SP(get_declaration)
{
    D3DVERTEXELEMENT9 stack[D3D9W_MAX_DECL_ELEMENTS], *el = NULL;
    IDirect3DVertexDeclaration9 *decl = This;
    void *heap = NULL;
    UINT n = 0, *pn = NULL;
    HRESULT hr;

    (void)kind;
    if (a[2])
    {
        if (!d3d9w_read(&n, a[2], sizeof(n)))
            goto refused;
        pn = &n;
    }
    if (a[1])
    {
        UINT count = 0;

        if (!d3d9w_guest_ok(a[1], sizeof(*el)))
            goto refused;
        if (FAILED(IDirect3DVertexDeclaration9_GetDeclaration(decl, NULL, &count)))
            count = 0;
        el = stack;
        if (count > D3D9W_MAX_DECL_ELEMENTS && !(el = heap = calloc(count, sizeof(*el))))
            goto refused;
    }
    hr = IDirect3DVertexDeclaration9_GetDeclaration(decl, el, pn);
    if (pn)
        d3d9w_write(a[2], &n, sizeof(n));
    if (SUCCEEDED(hr) && el && pn)
        d3d9w_write(a[1], el, (size_t)n * sizeof(*el));
    free(heap);
    ret->eax = (uint32_t)hr;
    return;
refused:
    ret->eax = (uint32_t)D3DERR_INVALIDCALL;
}

/* IDirect3DQuery9::GetData(void *data, DWORD size, DWORD flags), polled every frame: the
 * program's bytes are copied in (they stay as they were when the result is not ready) and
 * written back only with S_OK. */
SP(get_data)
{
    D3D9W_STACK_ARRAY(b);
    void *heap = NULL, *p = NULL;
    HRESULT hr;

    (void)kind;
    if (!d3d9w_array_in(a[1], a[2], b, sizeof(b), &heap, &p))
    {
        free(heap);
        ret->eax = (uint32_t)D3DERR_INVALIDCALL;
        return;
    }
    hr = IDirect3DQuery9_GetData((IDirect3DQuery9 *)This, p, a[2], a[3]);
    if (hr == S_OK && a[1] && a[2])
        d3d9w_write(a[1], p, a[2]);
    free(heap);
    ret->eax = (uint32_t)hr;
}

/* ------------------------------------------------------------------ the lock family (F5) */

/* A lock that handed out memory outside the program's window: the native frontend placed CPU
 * memory the program can see somewhere else (Part C says it never does). The lock is undone and
 * refused rather than give the program a truncated pointer. */
static void lock_outside(unsigned int kind, const char *what)
{
    d3d9w_log_once(0xf100u | kind, what);
}

/* Surface: (LOCKED_RECT *, const RECT *, flags); texture: (level, ...); cube: (face, level, ...). */
SP(lock_rect)
{
    const uint32_t *x = a + (kind == D3D9W_KIND_Surface ? 0 : kind == D3D9W_KIND_Texture ? 1 : 2);
    struct d3d9w_g_D3DLOCKED_RECT glr;
    D3DLOCKED_RECT lr, *plr = NULL;
    RECT rect, *prect = NULL;
    HRESULT hr;
    int outside = 0;

    if (x[1])
    {
        if (!d3d9w_read(&glr, x[1], sizeof(glr)))
            goto refused;
        d3d9w_in_D3DLOCKED_RECT(&lr, &glr);
        plr = &lr;
    }
    if (x[2])
    {
        if (!d3d9w_read(&rect, x[2], D3D9W_GSIZE_RECT))
            goto refused;
        prect = &rect;
    }
    if (kind == D3D9W_KIND_Surface)
        hr = IDirect3DSurface9_LockRect((IDirect3DSurface9 *)This, plr, prect, x[3]);
    else if (kind == D3D9W_KIND_Texture)
        hr = IDirect3DTexture9_LockRect((IDirect3DTexture9 *)This, a[1], plr, prect, x[3]);
    else
        hr = IDirect3DCubeTexture9_LockRect((IDirect3DCubeTexture9 *)This, (D3DCUBEMAP_FACES)a[1],
                                            a[2], plr, prect, x[3]);
    if (SUCCEEDED(hr) && plr && lr.pBits)
    {
        d3d9w_guest_of(lr.pBits, &outside);
        if (outside)
        {
            if (kind == D3D9W_KIND_Surface)
                IDirect3DSurface9_UnlockRect((IDirect3DSurface9 *)This);
            else if (kind == D3D9W_KIND_Texture)
                IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)This, a[1]);
            else
                IDirect3DCubeTexture9_UnlockRect((IDirect3DCubeTexture9 *)This,
                                                 (D3DCUBEMAP_FACES)a[1], a[2]);
            lock_outside(kind, "LockRect gave memory outside the program's window: refused");
            hr = D3DERR_INVALIDCALL;
            lr.pBits = NULL;
        }
    }
    if (plr)
    {
        d3d9w_out_D3DLOCKED_RECT(&glr, &lr);
        d3d9w_write(x[1], &glr, sizeof(glr));
    }
    ret->eax = (uint32_t)hr;
    return;
refused:
    ret->eax = (uint32_t)D3DERR_INVALIDCALL;
}

/* Volume: (LOCKED_BOX *, const D3DBOX *, flags); volume texture: (level, ...). */
SP(lock_box)
{
    const uint32_t *x = a + (kind == D3D9W_KIND_Volume ? 0 : 1);
    struct d3d9w_g_D3DLOCKED_BOX glb;
    D3DLOCKED_BOX lb, *plb = NULL;
    D3DBOX box, *pbox = NULL;
    HRESULT hr;
    int outside = 0;

    if (x[1])
    {
        if (!d3d9w_read(&glb, x[1], sizeof(glb)))
            goto refused;
        d3d9w_in_D3DLOCKED_BOX(&lb, &glb);
        plb = &lb;
    }
    if (x[2])
    {
        if (!d3d9w_read(&box, x[2], D3D9W_GSIZE_D3DBOX))
            goto refused;
        pbox = &box;
    }
    if (kind == D3D9W_KIND_Volume)
        hr = IDirect3DVolume9_LockBox((IDirect3DVolume9 *)This, plb, pbox, x[3]);
    else
        hr = IDirect3DVolumeTexture9_LockBox((IDirect3DVolumeTexture9 *)This, a[1], plb, pbox, x[3]);
    if (SUCCEEDED(hr) && plb && lb.pBits)
    {
        d3d9w_guest_of(lb.pBits, &outside);
        if (outside)
        {
            if (kind == D3D9W_KIND_Volume)
                IDirect3DVolume9_UnlockBox((IDirect3DVolume9 *)This);
            else
                IDirect3DVolumeTexture9_UnlockBox((IDirect3DVolumeTexture9 *)This, a[1]);
            lock_outside(kind, "LockBox gave memory outside the program's window: refused");
            hr = D3DERR_INVALIDCALL;
            lb.pBits = NULL;
        }
    }
    if (plb)
    {
        d3d9w_out_D3DLOCKED_BOX(&glb, &lb);
        d3d9w_write(x[1], &glb, sizeof(glb));
    }
    ret->eax = (uint32_t)hr;
    return;
refused:
    ret->eax = (uint32_t)D3DERR_INVALIDCALL;
}

/* IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9::Lock(offset, size, void **data, flags). */
SP(lock_buffer)
{
    uint32_t cur = 0, g;
    void *data = NULL;
    HRESULT hr;
    int outside = 0;

    if (a[3])
    {
        if (!d3d9w_read(&cur, a[3], sizeof(cur)))
        {
            ret->eax = (uint32_t)D3DERR_INVALIDCALL;
            return;
        }
        data = d3d9w_host_of(cur);
    }
    if (kind == D3D9W_KIND_VertexBuffer)
        hr = IDirect3DVertexBuffer9_Lock((IDirect3DVertexBuffer9 *)This, a[1], a[2],
                                         a[3] ? &data : NULL, a[4]);
    else
        hr = IDirect3DIndexBuffer9_Lock((IDirect3DIndexBuffer9 *)This, a[1], a[2],
                                        a[3] ? &data : NULL, a[4]);
    if (a[3])
    {
        g = d3d9w_guest_of(data, &outside);
        if (SUCCEEDED(hr) && outside)
        {
            if (kind == D3D9W_KIND_VertexBuffer)
                IDirect3DVertexBuffer9_Unlock((IDirect3DVertexBuffer9 *)This);
            else
                IDirect3DIndexBuffer9_Unlock((IDirect3DIndexBuffer9 *)This);
            lock_outside(kind, "Lock gave memory outside the program's window: refused");
            hr = D3DERR_INVALIDCALL;
            g = 0;
        }
        d3d9w_write(a[3], &g, sizeof(g));
    }
    ret->eax = (uint32_t)hr;
}

/* ------------------------------------------------------------------ the UP draws (F3) */

/* DrawPrimitiveUP(type, primitive count, const void *data, stride): vertices x stride. */
SP(draw_up)
{
    uint64_t bytes = d3d9w_prim_vertices((D3DPRIMITIVETYPE)a[1], a[2]) * a[4];
    uint32_t dummy = 0;
    const void *p = NULL;
    HRESULT hr;

    (void)kind;
    if (a[3])
    {
        p = &dummy;   /* nothing to read: the frontend checks the pointer, not the bytes */
        if (bytes && a[2])
        {
            void *buf;

            if (bytes > D3D9W_MAX_COPY || !(buf = d3d9w_scratch((size_t)bytes)) ||
                !d3d9w_read(buf, a[3], (size_t)bytes))
            {
                ret->eax = (uint32_t)D3DERR_INVALIDCALL;
                return;
            }
            p = buf;
        }
    }
    hr = IDirect3DDevice9Ex_DrawPrimitiveUP((IDirect3DDevice9Ex *)This, (D3DPRIMITIVETYPE)a[1],
                                            a[2], p, a[4]);
    ret->eax = (uint32_t)hr;
}

/* DrawIndexedPrimitiveUP(type, min vertex, vertex count, primitive count, const void *indices,
 * index format, const void *vertices, stride): the frontend reads (min + count) x stride bytes of
 * vertices from the start and the indices the primitives use (16-bit for D3DFMT_INDEX16,
 * otherwise 32-bit, as it does). */
SP(draw_indexed_up)
{
    uint64_t ib = d3d9w_prim_vertices((D3DPRIMITIVETYPE)a[1], a[4]) * (a[6] == D3DFMT_INDEX16 ? 2 : 4);
    uint64_t vb = ((uint64_t)a[2] + a[3]) * a[8];
    uint32_t dummy[2] = { 0, 0 };
    const void *pi = NULL, *pv = NULL;
    HRESULT hr;

    (void)kind;
    if (a[5])
        pi = &dummy[0];
    if (a[7])
        pv = &dummy[1];
    if (a[5] && a[7] && a[4] && a[3] && a[8] && ib && vb)
    {
        uint64_t vb_off = (ib + 15) & ~(uint64_t)15;
        uint8_t *buf;

        if (vb_off + vb > D3D9W_MAX_COPY || !(buf = d3d9w_scratch((size_t)(vb_off + vb))) ||
            !d3d9w_read(buf, a[5], (size_t)ib) || !d3d9w_read(buf + vb_off, a[7], (size_t)vb))
        {
            ret->eax = (uint32_t)D3DERR_INVALIDCALL;
            return;
        }
        pi = buf;
        pv = buf + vb_off;
    }
    hr = IDirect3DDevice9Ex_DrawIndexedPrimitiveUP((IDirect3DDevice9Ex *)This, (D3DPRIMITIVETYPE)a[1],
                                                   a[2], a[3], a[4], pi, (D3DFORMAT)a[6], pv, a[8]);
    ret->eax = (uint32_t)hr;
}
