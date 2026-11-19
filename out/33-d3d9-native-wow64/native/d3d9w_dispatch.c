/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 D3, E2, G: the native dispatch's core.
 *
 * - d3d9w_dispatch: the function the gate calls. It finds the method by the id's low 24 bits
 *   and runs its generated (or hand-written) dispatch function.
 * - The proxy table (clarification 5): proxies are 16-byte slots of 64 KB arena chunks in
 *   guest-window memory, one per native interface pointer handed to the program. A proxy's guest
 *   address names its slot, so finding the native object of This costs two loads and no guest
 *   memory access. A hash from native pointer to slot finds the proxy of an object native code
 *   returns. A slot lives as long as the native object: d3d9w_object_gone (called by the
 *   frontend, on any thread) frees it.
 * - One process at a time (clarification 10), and the end of a process: the gate's reserved
 *   method 0xffffff (clarification 13). */

#include "d3d9w_dispatch.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#include "d3d9w_iids.h"   /* gen/ */

/* ntdll's unix side: one line to Wine's debug output. */
int __wine_dbg_output(const char *str);

uint32_t d3d9w_chunk_of_page[65536];
struct d3d9w_chunk *d3d9w_chunks[D3D9W_MAX_CHUNKS];

#define NO_SLOT 0xffffffffu

static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t nchunks;            /* chunks in d3d9w_chunks */
static uint32_t next_unused;        /* slots of the last chunk never handed out yet */
static uint32_t free_head = NO_SLOT;
static uint32_t live;               /* LIVE slots */
static struct d3d9w_shared *shared; /* host address of slot 0 of chunk 0 */
static uint32_t vtables[D3D9W_NUM_KINDS];
static int have_vtables;
static unsigned long owner_base;    /* the window of the process that owns the table */
static int multi_process;           /* a second process has owned it: check every call */

struct hent
{
    void *key;
    uint32_t slot;
};
static struct hent *htab;
static uint32_t hcap, hcount;

/* ------------------------------------------------------------------ logging */

void d3d9w_log(const char *fmt, ...)
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
    __wine_dbg_output(buf);
}

static uint32_t logged_ids[65536 / 32];

void d3d9w_log_once(uint32_t method, const char *what)
{
    uint32_t bit = method & 0xffffu;
    uint32_t mask = 1u << (bit & 31);

    if (__atomic_fetch_or(&logged_ids[bit >> 5], mask, __ATOMIC_RELAXED) & mask)
        return;
    d3d9w_log("d3d9w: %s (method %#x)", what, method);
}

/* ------------------------------------------------------------------ slots */

static inline struct d3d9w_slot *slot_at(uint32_t s)
{
    return &d3d9w_chunks[s / D3D9W_SLOTS_PER_CHUNK]->slots[s % D3D9W_SLOTS_PER_CHUNK];
}

static inline struct d3d9w_proxy *proxy_host(uint32_t s)
{
    struct d3d9w_chunk *c = d3d9w_chunks[s / D3D9W_SLOTS_PER_CHUNK];
    return (struct d3d9w_proxy *)(c->host + (s % D3D9W_SLOTS_PER_CHUNK) * D3D9W_SLOT_BYTES);
}

static inline uint32_t proxy_guest(uint32_t s)
{
    struct d3d9w_chunk *c = d3d9w_chunks[s / D3D9W_SLOTS_PER_CHUNK];
    return c->guest + (s % D3D9W_SLOTS_PER_CHUNK) * D3D9W_SLOT_BYTES;
}

/* The slot a guest address names, or NO_SLOT. */
static uint32_t slot_of_guest(uint32_t g)
{
    uint32_t c = __atomic_load_n(&d3d9w_chunk_of_page[g >> 16], __ATOMIC_ACQUIRE);

    if (!c || (g & (D3D9W_SLOT_BYTES - 1)))
        return NO_SLOT;
    return (c - 1) * D3D9W_SLOTS_PER_CHUNK + (g & (D3D9W_CHUNK_BYTES - 1)) / D3D9W_SLOT_BYTES;
}

/* Lock NOT held: the dispatch never calls out while it holds the table lock. 64 KB of
 * guest-window memory for a chunk (Part C: a game thread inside a gate call, as the allocator
 * requires). */
static void *grab_chunk(uint32_t *guest)
{
    void *host;

    *guest = 0;
    host = ios_wow_window_alloc(D3D9W_CHUNK_BYTES, D3D9W_CHUNK_BYTES, guest);
    if (!host || (*guest & (D3D9W_CHUNK_BYTES - 1)) || *guest < D3D9W_CHUNK_BYTES)
    {
        d3d9w_log("d3d9w: no guest memory for a proxy chunk (%p, %#x)", host, *guest);
        return NULL;
    }
    return host;
}

/* Lock held. Makes a grabbed chunk part of the table. The first chunk's slot 0 is the shared
 * header (struct d3d9w_shared). */
static int install_chunk(uint8_t *host, uint32_t guest)
{
    struct d3d9w_chunk *c;

    if (nchunks >= D3D9W_MAX_CHUNKS)
    {
        d3d9w_log("d3d9w: all %u proxy chunks are in use", D3D9W_MAX_CHUNKS);
        return 0;
    }
    if (!(c = calloc(1, sizeof(*c))))
        return 0;
    c->host = host;
    c->guest = guest;
    memset(host, 0, D3D9W_CHUNK_BYTES);
    d3d9w_chunks[nchunks] = c;
    __atomic_store_n(&d3d9w_chunk_of_page[guest >> 16], nchunks + 1, __ATOMIC_RELEASE);
    next_unused = 0;
    if (!nchunks++)
    {
        next_unused = 1;
        c->slots[0].state = D3D9W_SLOT_RESERVED;
        shared = (struct d3d9w_shared *)host;
        shared->magic = D3D9W_SHARED_MAGIC;
    }
    return 1;
}

/* Lock held. A free slot, or NO_SLOT when a new chunk is needed. */
static uint32_t alloc_slot(void)
{
    uint32_t s;

    if (free_head != NO_SLOT)
    {
        s = free_head;
        free_head = slot_at(s)->next_free;
        return s;
    }
    if (!nchunks || next_unused >= D3D9W_SLOTS_PER_CHUNK)
        return NO_SLOT;
    return (nchunks - 1) * D3D9W_SLOTS_PER_CHUNK + next_unused++;
}

/* Lock held. The slot goes back to the free list; its proxy keeps its vtable (a stale pointer
 * still reaches a stub, which then finds no object) and is marked GONE. */
static void release_slot(uint32_t s)
{
    struct d3d9w_slot *slot = slot_at(s);

    __atomic_or_fetch(&proxy_host(s)->flags, D3D9W_PROXY_GONE, __ATOMIC_RELEASE);
    __atomic_store_n(&slot->native, NULL, __ATOMIC_RELEASE);
    slot->state = D3D9W_SLOT_FREE;
    slot->gen = (slot->gen + 1) & 0xfffu;
    slot->next_free = free_head;
    free_head = s;
}

/* ------------------------------------------------------------------ native pointer -> slot */

static inline uint32_t hindex(const void *p)
{
    uint64_t x = (uint64_t)(uintptr_t)p;

    x ^= x >> 29;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 32;
    return (uint32_t)x & (hcap - 1);
}

static struct hent *hfind(const void *p)
{
    uint32_t i;

    if (!hcap)
        return NULL;
    for (i = hindex(p); htab[i].key; i = (i + 1) & (hcap - 1))
        if (htab[i].key == p)
            return &htab[i];
    return NULL;
}

static int hinsert(void *p, uint32_t slot)
{
    uint32_t i;

    if ((hcount + 1) * 2 > hcap)
    {
        uint32_t old_cap = hcap, j;
        struct hent *old = htab, *n = calloc(hcap ? hcap * 2 : 1024, sizeof(*n));

        if (!n)
            return 0;
        htab = n;
        hcap = old_cap ? old_cap * 2 : 1024;
        for (j = 0; j < old_cap; j++)
        {
            if (!old[j].key)
                continue;
            for (i = hindex(old[j].key); htab[i].key; i = (i + 1) & (hcap - 1))
                ;
            htab[i] = old[j];
        }
        free(old);
    }
    for (i = hindex(p); htab[i].key; i = (i + 1) & (hcap - 1))
        ;
    htab[i].key = p;
    htab[i].slot = slot;
    hcount++;
    return 1;
}

/* Removal with backward shift: no tombstones. */
static void hremove(struct hent *e)
{
    uint32_t i = (uint32_t)(e - htab), j = i, k;

    for (;;)
    {
        htab[i].key = NULL;
        for (;;)
        {
            j = (j + 1) & (hcap - 1);
            if (!htab[j].key)
            {
                hcount--;
                return;
            }
            k = hindex(htab[j].key);
            /* move j back to i unless its home k lies cyclically in (i, j] */
            if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
                continue;
            break;
        }
        htab[i] = htab[j];
        i = j;
    }
}

/* ------------------------------------------------------------------ proxies */

static int kind_of_resource_type(D3DRESOURCETYPE t)
{
    switch (t)
    {
    case D3DRTYPE_SURFACE: return D3D9W_KIND_Surface;
    case D3DRTYPE_VOLUME: return D3D9W_KIND_Volume;
    case D3DRTYPE_TEXTURE: return D3D9W_KIND_Texture;
    case D3DRTYPE_VOLUMETEXTURE: return D3D9W_KIND_VolumeTexture;
    case D3DRTYPE_CUBETEXTURE: return D3D9W_KIND_CubeTexture;
    case D3DRTYPE_VERTEXBUFFER: return D3D9W_KIND_VertexBuffer;
    case D3DRTYPE_INDEXBUFFER: return D3D9W_KIND_IndexBuffer;
    default: return -1;
    }
}

/* Which of the kinds in mask native is: no lock held (it calls native code). */
static int classify(void *native, uint32_t mask)
{
    int k;

    if (!(mask & (mask - 1)))
        return __builtin_ctz(mask);
    if (!(mask & ~D3D9W_KINDS_Resource))
    {
        k = kind_of_resource_type(IDirect3DResource9_GetType((IDirect3DResource9 *)native));
        return k >= 0 && (mask & (1u << k)) ? k : -1;
    }
    /* Anything else: ask the object which of the table's interfaces it is. */
    for (k = 0; k < D3D9W_NUM_KINDS; k++)
    {
        void *q = NULL;

        if (!(mask & (1u << k)))
            continue;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)native, &d3d9w_kind_iid[k], &q)) && q)
        {
            IUnknown_Release((IUnknown *)q);
            if (q == native)
                return k;
        }
    }
    return -1;
}

uint32_t d3d9w_proxy_for(void *native, uint32_t mask)
{
    struct d3d9w_proxy *p;
    struct d3d9w_slot *slot;
    struct hent *e;
    uint32_t s, g;
    int kind;

    if (!native)
        return 0;
    pthread_mutex_lock(&table_lock);
    if ((e = hfind(native)))
    {
        s = e->slot;
        if (!(mask & (1u << slot_at(s)->kind)))
            d3d9w_log("d3d9w: object %p has a proxy of kind %u, asked for mask %#x",
                      native, slot_at(s)->kind, mask);
        g = proxy_guest(s);
        pthread_mutex_unlock(&table_lock);
        return g;
    }
    pthread_mutex_unlock(&table_lock);

    if ((kind = classify(native, mask)) < 0)
    {
        d3d9w_log("d3d9w: object %p is none of the interfaces %#x", native, mask);
        return 0;
    }

    pthread_mutex_lock(&table_lock);
    for (;;)
    {
        uint32_t chunk_guest;
        uint8_t *chunk;

        if ((e = hfind(native)))    /* another thread made it meanwhile */
        {
            g = proxy_guest(e->slot);
            pthread_mutex_unlock(&table_lock);
            return g;
        }
        if (!have_vtables)
        {
            pthread_mutex_unlock(&table_lock);
            return 0;
        }
        if ((s = alloc_slot()) != NO_SLOT)
            break;
        /* every slot is in use: a new chunk, taken without the lock */
        pthread_mutex_unlock(&table_lock);
        chunk = grab_chunk(&chunk_guest);
        pthread_mutex_lock(&table_lock);
        /* (memory of this process's window only goes into this process's table) */
        if (!chunk || owner_base != ios_wow_base() || !install_chunk(chunk, chunk_guest))
        {
            pthread_mutex_unlock(&table_lock);
            return 0;
        }
    }
    if (!hinsert(native, s))
    {
        slot_at(s)->next_free = free_head;
        free_head = s;
        pthread_mutex_unlock(&table_lock);
        return 0;
    }
    slot = slot_at(s);
    p = proxy_host(s);
    p->vtbl = vtables[kind];
    p->handle = s | (uint32_t)slot->gen << 20;
    p->reserved = 0;
    __atomic_store_n(&p->flags, 0, __ATOMIC_RELEASE);
    slot->kind = (uint8_t)kind;
    slot->state = D3D9W_SLOT_LIVE;
    __atomic_store_n(&slot->native, native, __ATOMIC_RELEASE);
    live++;
    if (shared)
        __atomic_store_n(&shared->live, live, __ATOMIC_RELAXED);
    g = proxy_guest(s);
    pthread_mutex_unlock(&table_lock);
    return g;
}

int d3d9w_put_iface(uint32_t g_out, void *native, uint32_t mask)
{
    uint32_t g = 0;
    int ok = 1;

    if (native && !(g = d3d9w_proxy_for(native, mask)))
    {
        IUnknown_Release((IUnknown *)native);
        ok = 0;
    }
    d3d9w_write(g_out, &g, sizeof(g));
    return ok;
}

static uint32_t mask_of_iid(const GUID *iid)
{
    size_t i;

    for (i = 0; i < sizeof(d3d9w_iid_table) / sizeof(d3d9w_iid_table[0]); i++)
        if (!memcmp(iid, &d3d9w_iid_table[i].iid, sizeof(GUID)))
            return d3d9w_iid_table[i].mask;
    return 0;
}

int d3d9w_put_iface_iid(uint32_t g_out, void *native, const GUID *iid, void *This,
                        unsigned int this_kind)
{
    uint32_t mask, g = 0;

    if (!native)
    {
        d3d9w_write(g_out, &g, sizeof(g));
        return 1;
    }
    mask = native == This ? 1u << this_kind : mask_of_iid(iid);
    if (!mask || !(g = d3d9w_proxy_for(native, mask)))
    {
        IUnknown_Release((IUnknown *)native);
        d3d9w_write(g_out, &g, sizeof(g));
        return 0;
    }
    d3d9w_write(g_out, &g, sizeof(g));
    return 1;
}

/* Spec 33 clarifications 5 and 7: the frontend calls this once for every object it destroys,
 * on any thread, before the memory is freed. Native bookkeeping only. */
void d3d9w_object_gone(void *iface)
{
    struct d3d9w_proxy *p;
    struct hent *e;
    uint32_t s;

    if (!iface)
        return;
    pthread_mutex_lock(&table_lock);
    if (!(e = hfind(iface)))
    {
        pthread_mutex_unlock(&table_lock);
        return;
    }
    s = e->slot;
    hremove(e);
    p = proxy_host(s);
    live--;
    if (shared)
        __atomic_store_n(&shared->live, live, __ATOMIC_RELAXED);
    if (__atomic_load_n(&p->flags, __ATOMIC_ACQUIRE) & D3D9W_PROXY_PINNED)
    {
        /* The thunk keeps guest-side state on this proxy: keep the slot until it drops it. */
        struct d3d9w_slot *slot = slot_at(s);

        __atomic_store_n(&slot->native, NULL, __ATOMIC_RELEASE);
        slot->state = D3D9W_SLOT_ZOMBIE;
        __atomic_or_fetch(&p->flags, D3D9W_PROXY_GONE, __ATOMIC_RELEASE);
        if (shared)
            __atomic_add_fetch(&shared->sweep, 1, __ATOMIC_RELEASE);
    }
    else
        release_slot(s);
    pthread_mutex_unlock(&table_lock);
}

HRESULT d3d9w_unpin(uint32_t g)
{
    uint32_t s = slot_of_guest(g);
    HRESULT hr = D3DERR_INVALIDCALL;

    if (s == NO_SLOT)
        return hr;
    pthread_mutex_lock(&table_lock);
    if (s / D3D9W_SLOTS_PER_CHUNK < nchunks)
    {
        struct d3d9w_slot *slot = slot_at(s);

        if (slot->state == D3D9W_SLOT_ZOMBIE)
        {
            release_slot(s);
            hr = D3D_OK;
        }
        else if (slot->state == D3D9W_SLOT_LIVE)
        {
            __atomic_and_fetch(&proxy_host(s)->flags, ~D3D9W_PROXY_PINNED, __ATOMIC_RELEASE);
            hr = D3D_OK;
        }
    }
    pthread_mutex_unlock(&table_lock);
    return hr;
}

HRESULT d3d9w_pin(uint32_t g)
{
    uint32_t s = slot_of_guest(g);
    HRESULT hr = D3DERR_INVALIDCALL;

    if (s == NO_SLOT)
        return hr;
    pthread_mutex_lock(&table_lock);
    if (s / D3D9W_SLOTS_PER_CHUNK < nchunks && slot_at(s)->state == D3D9W_SLOT_LIVE)
    {
        __atomic_or_fetch(&proxy_host(s)->flags, D3D9W_PROXY_PINNED, __ATOMIC_RELEASE);
        hr = D3D_OK;
    }
    pthread_mutex_unlock(&table_lock);
    return hr;
}

uint32_t d3d9w_live_count(void)
{
    uint32_t n;

    pthread_mutex_lock(&table_lock);
    n = live;
    pthread_mutex_unlock(&table_lock);
    return n;
}

/* Lock held. Forgets every chunk. Their guest memory belongs to a process that no longer owns
 * the table (and may have ended): it is not touched. */
static void reset_table_locked(void)
{
    uint32_t i;

    for (i = 0; i < nchunks; i++)
    {
        __atomic_store_n(&d3d9w_chunk_of_page[d3d9w_chunks[i]->guest >> 16], 0, __ATOMIC_RELEASE);
        free(d3d9w_chunks[i]);
        d3d9w_chunks[i] = NULL;
    }
    nchunks = 0;
    next_unused = 0;
    free_head = NO_SLOT;
    live = 0;
    shared = NULL;
    free(htab);
    htab = NULL;
    hcap = hcount = 0;
}

void d3d9w_reset_for_tests(void)
{
    pthread_mutex_lock(&table_lock);
    reset_table_locked();
    owner_base = 0;
    multi_process = 0;
    have_vtables = 0;
    memset(logged_ids, 0, sizeof(logged_ids));
    pthread_mutex_unlock(&table_lock);
}

/* Init, the creates and the end of a process run one at a time (lock order: create_lock, then
 * table_lock), so a new process's first objects never fall between the moment an ending process's
 * proxies are forgotten and the frontend's d3d9w_native_process_gone (clarification 13). The
 * frontend's own threads never take it. */
static pthread_mutex_t create_lock = PTHREAD_MUTEX_INITIALIZER;

void d3d9w_create_lock(void)
{
    pthread_mutex_lock(&create_lock);
}

void d3d9w_create_unlock(void)
{
    pthread_mutex_unlock(&create_lock);
}

static uint32_t init_body(uint32_t version, const uint32_t *vt, uint32_t count, uint32_t *shared_guest);

uint32_t d3d9w_init(uint32_t version, const uint32_t *vt, uint32_t count, uint32_t *shared_guest)
{
    uint32_t status;

    d3d9w_create_lock();
    status = init_body(version, vt, count, shared_guest);
    d3d9w_create_unlock();
    return status;
}

/* Clarification 13: the gate's reserved method 0xffffff, on the thread that ends the process,
 * just before it ends. When that process owns the native frontend, every proxy is forgotten
 * (the window is going: no guest memory is touched) and the frontend orphans the objects that
 * are still alive. Any other process (one that got the full frontend, or gave the native one up)
 * owns nothing here. */
void d3d9w_process_end(void)
{
    unsigned long base = ios_wow_base();
    uint32_t proxies = 0;
    int owner;

    d3d9w_create_lock();
    pthread_mutex_lock(&table_lock);
    owner = owner_base && owner_base == base;
    if (owner)
    {
        proxies = live;
        reset_table_locked();
        owner_base = 0;
        have_vtables = 0;
    }
    pthread_mutex_unlock(&table_lock);
    if (owner)
    {
        d3d9w_log("d3d9w: the process of window %#lx ended with %u proxies: forgotten, its objects "
                  "left to the frontend (spec 33 clarification 13)", base, proxies);
        d3d9w_native_process_gone();
    }
    d3d9w_create_unlock();
}

static uint32_t init_body(uint32_t version, const uint32_t *vt, uint32_t count, uint32_t *shared_guest)
{
    unsigned long base = ios_wow_base();
    uint32_t status = D3D9W_INIT_OK;

    if (version != D3D9W_ABI_VERSION || count != D3D9W_NUM_KINDS)
    {
        d3d9w_log("d3d9w: the thunk is ABI %u with %u vtables, this library ABI %u with %u",
                  version, count, D3D9W_ABI_VERSION, D3D9W_NUM_KINDS);
        return D3D9W_INIT_VERSION;
    }
    pthread_mutex_lock(&table_lock);
    if (owner_base && owner_base != base)
    {
        if (live)
        {
            d3d9w_log("d3d9w: the process of window %#lx asks for the native frontend while the "
                      "process of window %#lx holds %u of its objects: it gets the full frontend "
                      "(spec 33 clarification 10)", base, owner_base, live);
            pthread_mutex_unlock(&table_lock);
            return D3D9W_INIT_BUSY;
        }
        d3d9w_log("d3d9w: the native frontend passes from the process of window %#lx (no objects "
                  "left) to the process of window %#lx", owner_base, base);
        reset_table_locked();
        multi_process = 1;
    }
    owner_base = base;
    memcpy(vtables, vt, sizeof(vtables));
    have_vtables = 1;
    if (!nchunks)
    {
        /* the first chunk, whose slot 0 is the shared header: taken without the lock */
        uint32_t chunk_guest;
        uint8_t *chunk;

        pthread_mutex_unlock(&table_lock);
        chunk = grab_chunk(&chunk_guest);
        pthread_mutex_lock(&table_lock);
        if (owner_base != base)
            status = D3D9W_INIT_BUSY;   /* another process took over meanwhile */
        else if (!chunk || !install_chunk(chunk, chunk_guest))
            status = D3D9W_INIT_NOMEM;
    }
    if (status == D3D9W_INIT_OK)
        *shared_guest = d3d9w_chunks[0]->guest;
    pthread_mutex_unlock(&table_lock);
    return status;
}

uint32_t d3d9w_owner_status(void)
{
    uint32_t status;

    pthread_mutex_lock(&table_lock);
    status = have_vtables && owner_base == ios_wow_base() ? D3D9W_CREATE_OK : D3D9W_CREATE_BUSY;
    pthread_mutex_unlock(&table_lock);
    return status;
}

/* ------------------------------------------------------------------ the gate's function */

static void unknown_method(uint32_t id, struct ios_wow_native_ret *ret)
{
    ret->eax = (uint32_t)E_NOTIMPL;
    d3d9w_log_once(id & 0xffffffu, "unknown method id (a mismatched build), E_NOTIMPL");
}

void d3d9w_dispatch(uint32_t id, uint32_t esp, struct ios_wow_native_ret *ret)
{
    uint32_t m = id & 0xffffffu;
    uint32_t words = D3D9W_METHOD_WORDS(m), kind = (m >> 8) & 0xffu, slot = m & 0xffu;
    const struct d3d9w_method *e = NULL;

    ret->kind = IOS_WOW_NATIVE_RET_EAX;
    ret->eax = 0;
    ret->pop = 4 * words;    /* right even for an unknown id (G3) */
    ret->st0 = 0.0f;

    if (m == IOS_WOW_NATIVE_METHOD_PROCESS_END)
    {
        /* the process is ending (clarification 13): ret is ignored, esp is not read */
        ret->pop = 0;
        d3d9w_process_end();
        return;
    }
    if (kind < D3D9W_NUM_KINDS)
        e = &d3d9w_methods[kind][slot];
    else if (kind == D3D9W_KIND_EXPORTS)
        e = &d3d9w_extras[slot];
    if (!e || !e->fn || e->words != words || (id >> 24) != D3D9W_LIB)
    {
        unknown_method(id, ret);
        return;
    }
    /* Every call that names a proxy: the interface methods and the thunk's calls from WindowState
     * on (the exports and Init check ownership themselves, or name no proxy). */
    if (__builtin_expect(multi_process, 0) &&
        (kind != D3D9W_KIND_EXPORTS || slot >= (D3D9W_M_Exports_WindowState & 0xffu)) &&
        ios_wow_base() != owner_base)
    {
        /* A process that no longer owns the table calls a method on a stale proxy. */
        d3d9w_log_once(m, "a call from a process that does not own the native frontend");
        ret->eax = e->refused;
        return;
    }
    e->fn(esp, ret);
}
