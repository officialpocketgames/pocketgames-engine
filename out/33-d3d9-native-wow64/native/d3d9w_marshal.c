/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 D4 and F3: copying what the program passes into native memory before the native
 * method runs. Native code never dereferences a program pointer (D4); every helper here
 * returns 0 when ios_wow_native_read refuses a range (clarification 4), and the dispatch then
 * answers D3DERR_INVALIDCALL without calling the native method.
 *
 * Variable-length data the API gives no length for (shader tokens, vertex declarations) is
 * read in pieces that never cross a 4 KB page boundary the data does not reach, so a read never
 * touches memory past the end of what the program passed. */

#include "d3d9w_dispatch.h"

#include <pthread.h>

int d3d9w_array_in(uint32_t g, uint64_t n, void *stack, size_t stack_size, void **heap, void **p)
{
    void *buf;

    *heap = NULL;
    *p = NULL;
    if (!g)
        return 1;
    if (n > D3D9W_MAX_COPY)
        return 0;
    if (n <= stack_size)
        buf = stack;
    else if (!(buf = *heap = malloc((size_t)n)))
        return 0;
    if (n && !d3d9w_read(buf, g, (size_t)n))
        return 0;
    *p = buf;
    return 1;
}

int d3d9w_iface_array_in(uint32_t g, uint64_t count, uint32_t mask, void *stack,
                         size_t stack_size, void **heap, void **p)
{
    uint32_t piece[256];
    void **out;
    uint64_t i, j, n;

    *heap = NULL;
    *p = NULL;
    if (!g)
        return 1;
    if (count > D3D9W_MAX_COPY / sizeof(void *))
        return 0;
    n = count * sizeof(void *);
    if (n <= stack_size)
        out = stack;
    else if (!(out = *heap = malloc((size_t)n)))
        return 0;
    for (i = 0; i < count; i += j)
    {
        uint64_t len = count - i < 256 ? count - i : 256;

        if (!d3d9w_read(piece, g + (uint32_t)(i * 4), (size_t)len * 4))
            return 0;
        for (j = 0; j < len; j++)
        {
            out[i + j] = NULL;
            if (piece[j] && !(out[i + j] = d3d9w_native_of(piece[j], mask)))
                return 0;
        }
    }
    *p = out;
    return 1;
}

/* Bytes that can be read at g without going past the 4 KB page g lies on (at least want when
 * a single item straddles two pages). */
static size_t page_piece(uint32_t g, size_t want, size_t unit)
{
    size_t left = 0x1000u - (g & 0xfffu);

    left -= left % unit;
    if (!left)
        left = unit;
    return left < want ? left : want;
}

/* A growing copy: the stack buffer first, then the heap. */
struct grow
{
    uint8_t *buf;
    size_t used, cap;
    void **heap;
};

static int grow_to(struct grow *gr, size_t need)
{
    size_t cap = gr->cap;
    uint8_t *n;

    if (need <= cap)
        return 1;
    if (need > D3D9W_MAX_COPY)
        return 0;
    while (cap < need)
        cap *= 2;
    if (!(n = malloc(cap)))
        return 0;
    memcpy(n, gr->buf, gr->used);
    free(*gr->heap);
    *gr->heap = n;
    gr->buf = n;
    gr->cap = cap;
    return 1;
}

/* Reads from g + used until at least need bytes are held, page piece by page piece. */
static int grow_read(struct grow *gr, uint32_t g, size_t need, size_t unit)
{
    while (gr->used < need)
    {
        size_t want = gr->cap - gr->used, n;

        if (!want)
        {
            if (!grow_to(gr, gr->cap * 2))
                return 0;
            want = gr->cap - gr->used;
        }
        n = page_piece(g + (uint32_t)gr->used, want, unit);
        if ((uint64_t)g + gr->used + n > 0x100000000ull)
            return 0;
        if (!d3d9w_read(gr->buf + gr->used, g + (uint32_t)gr->used, n))
            return 0;
        gr->used += n;
    }
    return 1;
}

#define D3D9W_SHADER_END        0x0000ffffu
#define D3D9W_SHADER_COMMENT    0xfffeu
#define D3D9W_SHADER_OP_DEF     0x51u
#define D3D9W_MAX_SHADER_TOKENS (1u << 20)

/* Shader tokens up to the end token (spec 33 F3), walking the stream as the Direct3D 9 shader
 * token format lays it out, so that literal values and comment payloads that happen to equal
 * the end token do not end it early:
 *   - the version token: 0xFFFE.... (vertex) or 0xFFFF.... (pixel), major in bits 8-15;
 *   - a comment token (low 16 bits 0xFFFE) is followed by bits 16-30 tokens of payload;
 *   - from shader model 2 on, bits 24-27 of an instruction token count its following tokens;
 *   - in shader model 1, def takes 5 tokens, and other instructions are followed by their
 *     parameter tokens, which all have bit 31 set.
 * The copy always ends with the end token: native code scans for it and is never handed a
 * stream without one. */
int d3d9w_shader_in(uint32_t g, void *stack, size_t stack_size, void **heap, const DWORD **p,
                    size_t *ntokens)
{
    struct grow gr = { stack, 0, stack_size, heap };
    const uint32_t *tok;
    uint32_t version, major;
    size_t pos = 1;

    *heap = NULL;
    *p = NULL;
    if (!grow_read(&gr, g, 4, 4))
        return 0;
    version = ((uint32_t *)gr.buf)[0];
    if ((version >> 16) != 0xfffeu && (version >> 16) != 0xffffu)
        return 0;
    major = (version >> 8) & 0xffu;
    for (;;)
    {
        uint32_t t;

        if (pos >= D3D9W_MAX_SHADER_TOKENS || !grow_read(&gr, g, (pos + 1) * 4, 4))
            return 0;
        tok = (const uint32_t *)gr.buf;
        t = tok[pos];
        if (t == D3D9W_SHADER_END)
        {
            pos++;
            break;
        }
        if ((t & 0xffffu) == D3D9W_SHADER_COMMENT)
            pos += 1 + ((t >> 16) & 0x7fffu);
        else if (major >= 2)
            pos += 1 + ((t >> 24) & 0xfu);
        else if ((t & 0xffffu) == D3D9W_SHADER_OP_DEF)
            pos += 6;
        else
        {
            pos++;
            for (;;)
            {
                if (pos >= D3D9W_MAX_SHADER_TOKENS || !grow_read(&gr, g, (pos + 1) * 4, 4))
                    return 0;
                if (!(((const uint32_t *)gr.buf)[pos] & 0x80000000u))
                    break;
                pos++;
            }
        }
    }
    *p = (const DWORD *)gr.buf;
    if (ntokens)
        *ntokens = pos;
    return 1;
}

/* Vertex elements up to D3DDECL_END (Stream 0xFF, Type D3DDECLTYPE_UNUSED), F3. */
int d3d9w_decl_in(uint32_t g, D3DVERTEXELEMENT9 *out)
{
    size_t have = 0, i;

    for (i = 0; i < D3D9W_MAX_DECL_ELEMENTS; i++)
    {
        if (i == have)
        {
            size_t n = page_piece(g + (uint32_t)(i * sizeof(*out)),
                                  (D3D9W_MAX_DECL_ELEMENTS - i) * sizeof(*out), sizeof(*out));

            if (!d3d9w_read(out + i, g + (uint32_t)(i * sizeof(*out)), n))
                return 0;
            have += n / sizeof(*out);
        }
        if (out[i].Stream == 0xff && out[i].Type == D3DDECLTYPE_UNUSED)
            return 1;
    }
    return 0;   /* no D3DDECL_END within MAXD3DDECLLENGTH + 1 elements */
}

/* An RGNDATA: the header, then nCount rectangles (F3). */
int d3d9w_rgn_in(uint32_t g, void *stack, size_t stack_size, void **heap, const RGNDATA **p)
{
    RGNDATAHEADER h;
    uint64_t n;
    uint8_t *buf;

    *heap = NULL;
    *p = NULL;
    if (!d3d9w_read(&h, g, sizeof(h)))
        return 0;
    n = sizeof(h) + (uint64_t)h.nCount * sizeof(RECT);
    if (n > D3D9W_MAX_COPY)
        return 0;
    if (n < sizeof(RGNDATA))
        n = sizeof(RGNDATA);
    if (n <= stack_size)
        buf = stack;
    else if (!(buf = *heap = malloc((size_t)n)))
        return 0;
    memset(buf, 0, sizeof(RGNDATA));
    memcpy(buf, &h, sizeof(h));
    if (h.nCount && !d3d9w_read(buf + sizeof(h), g + sizeof(h), (size_t)h.nCount * sizeof(RECT)))
        return 0;
    *p = (const RGNDATA *)buf;
    return 1;
}

/* HANDLE *pSharedHandle (spec 33 C3). A D3D9Ex system-memory resource takes the program's own
 * memory through it, as a pointer: native code gets that memory's host address and hands the
 * same guest address back from Lock. A DEFAULT-pool share handle goes the same way; the
 * frontend only checks that one is there (it does not share across processes). */
int d3d9w_shared_in(uint32_t g, HANDLE *out)
{
    uint32_t v;

    if (!d3d9w_read(&v, g, sizeof(v)))
        return 0;
    *out = (HANDLE)d3d9w_host_of(v);
    return 1;
}

uint64_t d3d9w_prim_vertices(D3DPRIMITIVETYPE type, uint32_t count)
{
    switch (type)
    {
    case D3DPT_POINTLIST: return count;
    case D3DPT_LINELIST: return (uint64_t)count * 2;
    case D3DPT_LINESTRIP: return (uint64_t)count + 1;
    case D3DPT_TRIANGLELIST: return (uint64_t)count * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return (uint64_t)count + 2;
    default: return 0;
    }
}

/* Per-thread scratch memory for the UP draws: they come every frame, so the buffer is kept. */
static pthread_key_t scratch_key;
static pthread_once_t scratch_once = PTHREAD_ONCE_INIT;

struct scratch
{
    size_t size;
    void *buf;
};

static void scratch_free(void *p)
{
    struct scratch *s = p;

    free(s->buf);
    free(s);
}

static void scratch_init(void)
{
    pthread_key_create(&scratch_key, scratch_free);
}

void *d3d9w_scratch(size_t n)
{
    struct scratch *s;

    pthread_once(&scratch_once, scratch_init);
    if (!(s = pthread_getspecific(scratch_key)))
    {
        if (!(s = calloc(1, sizeof(*s))))
            return NULL;
        pthread_setspecific(scratch_key, s);
    }
    if (n > s->size)
    {
        size_t size = n < 65536 ? 65536 : n;
        void *b = malloc(size);

        if (!b)
            return NULL;
        free(s->buf);
        s->buf = b;
        s->size = size;
    }
    return s->buf;
}
