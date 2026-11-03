// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35: pgl's pure functions (pgl_pure.h). No EGL, GL, Metal or Wine types: the host tests
// build this file on Linux.

#include <string.h>

#include "pgl_pure.h"

/* ---- D2 ---- */

/* D2.1: RGBA8 24/8, RGB8 24/8, RGBA8 24/0, RGB8 24/0, RGBA8 16/0, RGB8 16/0, RGBA8 0/0, RGB8 0/0 */
const struct pgl_config pgl_configs[PGL_CONFIG_COUNT] =
{
    { 8, 24, 8 }, { 0, 24, 8 },
    { 8, 24, 0 }, { 0, 24, 0 },
    { 8, 16, 0 }, { 0, 16, 0 },
    { 8,  0, 0 }, { 0,  0, 0 },
};

/* D2.2: upstream's three flag sets, in its order */
static const uint32_t flag_sets[PGL_FLAG_SET_COUNT] =
{
    PGL_PFD_SUPPORT_GDI,
    PGL_PFD_DOUBLEBUFFER,
    0, /* offscreen */
};

int pgl_format_split( int format, unsigned int *config, unsigned int *flag_set )
{
    if (format < 1 || format > (int)PGL_FORMAT_COUNT) return 0;
    *config = (unsigned int)(format - 1) % PGL_CONFIG_COUNT;
    *flag_set = (unsigned int)(format - 1) / PGL_CONFIG_COUNT;
    return 1;
}

int pgl_describe_format( int format, struct pgl_pixel_format *out )
{
    const struct pgl_config *cfg;
    unsigned int config, set;
    uint32_t flags;

    if (!pgl_format_split( format, &config, &set )) return 0;
    cfg = &pgl_configs[config];
    flags = flag_sets[set];

    memset( out, 0, sizeof(*out) );
    /* as upstream's describe_egl_config for a surfaceless platform: every on-screen format can
     * draw to a window, every format to a pbuffer (and so to a bitmap) */
    out->flags = PGL_PFD_SUPPORT_OPENGL | PGL_PFD_SUPPORT_COMPOSITION | flags | PGL_PFD_DRAW_TO_BITMAP;
    if (flags) out->flags |= PGL_PFD_DRAW_TO_WINDOW;

    /* the full pixel size, as upstream (and real drivers) report cColorBits; 0xARGB order */
    out->color_bits = 32;
    out->red_bits = out->green_bits = out->blue_bits = 8;
    out->blue_shift = 0;
    out->green_shift = 8;
    out->red_shift = 16;
    out->alpha_bits = cfg->alpha_bits;
    out->alpha_shift = cfg->alpha_bits ? 24 : 0;
    out->depth_bits = cfg->depth_bits;
    out->stencil_bits = cfg->stencil_bits;

    out->draw_to_pbuffer = 1;
    out->max_pbuffer_width = PGL_PBUFFER_MAX;
    out->max_pbuffer_height = PGL_PBUFFER_MAX;
    out->max_pbuffer_pixels = PGL_PBUFFER_MAX * PGL_PBUFFER_MAX;
    out->bind_to_texture_rgb = out->bind_to_texture_rgba = 1;
    out->bind_to_texture_rectangle_rgb = out->bind_to_texture_rectangle_rgba = 1;
    return 1;
}

/* ---- D3.2 ---- */

static int version_exists( int major, int minor )
{
    switch (major)
    {
    case 1: return minor >= 0 && minor <= 5;
    case 2: return minor >= 0 && minor <= 1;
    case 3: return minor >= 0 && minor <= 3;
    case 4: return minor >= 0 && minor <= 6;
    default: return 0;
    }
}

enum pgl_attribs_result pgl_parse_context_attribs( const int *attribs, struct pgl_context_request *req )
{
    memset( req, 0, sizeof(*req) );
    req->major = 1;
    req->minor = 0;

    for (; attribs && attribs[0]; attribs += 2)
    {
        switch (attribs[0])
        {
        case PGL_WGL_CONTEXT_MAJOR_VERSION: req->major = attribs[1]; break;
        case PGL_WGL_CONTEXT_MINOR_VERSION: req->minor = attribs[1]; break;
        case PGL_WGL_CONTEXT_FLAGS: req->flags = attribs[1]; break;
        case PGL_WGL_CONTEXT_PROFILE_MASK: req->profile_mask = attribs[1]; break;
        case PGL_WGL_CONTEXT_OPENGL_NO_ERROR: req->no_error = attribs[1]; break;
        default: req->unknown++; break;
        }
    }

    if (req->profile_mask & PGL_WGL_CONTEXT_ES2_PROFILE_BIT) return PGL_ATTRIBS_BAD_PROFILE;
    if (!version_exists( req->major, req->minor )) return PGL_ATTRIBS_BAD_VERSION;
    if (req->major > 2 || (req->major == 2 && req->minor > 1)) return PGL_ATTRIBS_BAD_VERSION;
    return PGL_ATTRIBS_OK;
}

/* ---- C3.4 ---- */

static int contains_nocase( const char *s, size_t len, const char *word )
{
    size_t n = strlen( word ), i, j;

    for (i = 0; i + n <= len; i++)
    {
        for (j = 0; j < n; j++)
        {
            char c = s[i + j];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != word[j]) break;
        }
        if (j == n) return 1;
    }
    return 0;
}

static int listed( const char *list, size_t list_len, const char *name, size_t len )
{
    const char *p = list, *end = list + list_len;

    while (p < end)
    {
        const char *q = memchr( p, ' ', (size_t)(end - p) );
        size_t n = q ? (size_t)(q - p) : (size_t)(end - p);
        if (n == len && !memcmp( p, name, len )) return 1;
        p += n + 1;
    }
    return 0;
}

size_t pgl_filter_extensions( const char *in, char *out, size_t out_size )
{
    size_t limit, used = 0;

    if (!out_size) return 0;
    limit = out_size - 1;
    if (limit > PGL_EXTENSIONS_MAX - 1) limit = PGL_EXTENSIONS_MAX - 1;
    out[0] = 0;
    if (!in) return 0;

    while (*in)
    {
        const char *start;
        size_t len, need;

        while (*in == ' ') in++;
        if (!*in) break;
        start = in;
        while (*in && *in != ' ') in++;
        len = (size_t)(in - start);

        if (len >= 9 && !memcmp( start, "GL_ANGLE_", 9 )) continue;
        if (contains_nocase( start, len, "gl4es" )) continue;
        if (listed( out, used, start, len )) continue;

        need = len + (used ? 1 : 0);
        if (used + need > limit) break;
        if (used) out[used++] = ' ';
        memcpy( out + used, start, len );
        used += len;
        out[used] = 0;
    }
    return used;
}

/* ---- D5.4 ---- */

double pgl_pace_period( int mode )
{
    switch (mode)
    {
    case 1: return 1.0 / 60.0;
    case 3: return 1.0 / 30.0;
    case 4: return 1.0 / 40.0;
    default: return 0.0;   /* 0 and 2 (RAW): plain present */
    }
}

int pgl_pace_raw_skip( int mode, uint64_t now_ns, uint64_t last_ns )
{
    if (mode != 2 || !last_ns || now_ns < last_ns) return 0;
    return now_ns - last_ns < 18ull * 1000 * 1000;
}

uint64_t pgl_limiter_next( uint64_t deadline, uint64_t now, uint64_t period )
{
    uint64_t next = deadline ? deadline + period : now;
    if (!deadline || now > next + period) next = now;
    return next;
}

int pgl_acquire_logged( uint64_t n, double ms )
{
    return n < 8 || ms > 50.0;
}

/* ---- H2 ---- */

void pgl_registry_init( struct pgl_registry *reg )
{
    reg->head.prev = reg->head.next = &reg->head;
    reg->head.owner = NULL;
    reg->head.kind = PGL_OWNED_KINDS;
}

void pgl_registry_add( struct pgl_registry *reg, struct pgl_owned *item, const void *owner, enum pgl_owned_kind kind )
{
    item->owner = owner;
    item->kind = kind;
    item->next = &reg->head;
    item->prev = reg->head.prev;
    reg->head.prev->next = item;
    reg->head.prev = item;
}

void pgl_registry_remove( struct pgl_owned *item )
{
    if (!item->next) return;
    item->prev->next = item->next;
    item->next->prev = item->prev;
    item->prev = item->next = NULL;
}

void pgl_registry_take( struct pgl_registry *reg, const void *owner, struct pgl_registry *taken,
                        unsigned int counts[PGL_OWNED_KINDS] )
{
    struct pgl_owned *item, *next;
    unsigned int k;

    pgl_registry_init( taken );
    for (k = 0; k < PGL_OWNED_KINDS; k++) counts[k] = 0;
    for (item = reg->head.next; item != &reg->head; item = next)
    {
        next = item->next;
        if (item->owner != owner) continue;
        pgl_registry_remove( item );
        pgl_registry_add( taken, item, owner, item->kind );
        if (item->kind < PGL_OWNED_KINDS) counts[item->kind]++;
    }
}

struct pgl_owned *pgl_registry_first( struct pgl_registry *reg )
{
    return reg->head.next == &reg->head ? NULL : reg->head.next;
}

unsigned int pgl_registry_count( const struct pgl_registry *reg, const void *owner )
{
    const struct pgl_owned *item;
    unsigned int n = 0;

    for (item = reg->head.next; item != &reg->head; item = item->next)
        if (item->owner == owner) n++;
    return n;
}

/* ---------------------------------------------------------------- D4.2: render to texture */

int pgl_pbuffer_source_ok( unsigned int source )
{
    switch (source)
    {
    case PGL_GL_FRONT:
    case PGL_GL_BACK:
    case PGL_GL_FRONT_LEFT:
    case PGL_GL_BACK_LEFT:
    case PGL_GL_LEFT:
    case PGL_GL_FRONT_AND_BACK:
        return 1;
    default:
        return 0;
    }
}

enum pgl_copy_result pgl_pbuffer_copy( const struct pgl_copy_gl *gl, const struct pgl_pbuffer_copy *req )
{
    unsigned int fbo = 0, target = req->texture_target, status;
    int draw = 0, read = 0;

    if (req->source == PGL_GL_NONE) return PGL_COPY_DONE;
    if (!pgl_pbuffer_source_ok( req->source )) return PGL_COPY_BAD_SOURCE;
    if (!req->renderbuffer || req->width < 1 || req->height < 1) return PGL_COPY_NOTHING;
    if (target == PGL_GL_TEXTURE_CUBE_MAP)
        target = req->cube_face ? req->cube_face : PGL_GL_TEXTURE_CUBE_MAP_POSITIVE_X;

    gl->get_integerv( PGL_GL_DRAW_FRAMEBUFFER_BINDING, &draw );
    gl->get_integerv( PGL_GL_READ_FRAMEBUFFER_BINDING, &read );

    gl->gen_framebuffers( 1, &fbo );
    gl->bind_framebuffer( PGL_GL_FRAMEBUFFER, fbo );
    gl->framebuffer_renderbuffer( PGL_GL_FRAMEBUFFER, PGL_GL_COLOR_ATTACHMENT0, PGL_GL_RENDERBUFFER, req->renderbuffer );
    status = gl->check_framebuffer_status( PGL_GL_FRAMEBUFFER );
    if (status == PGL_GL_FRAMEBUFFER_COMPLETE)
        gl->copy_tex_image_2d( target, req->level, req->texture_format, 0, 0, req->width, req->height, 0 );

    /* GL_FRAMEBUFFER sets both bindings; the read one again when the program had them apart */
    gl->bind_framebuffer( PGL_GL_FRAMEBUFFER, (unsigned int)draw );
    if (read != draw) gl->bind_framebuffer( PGL_GL_READ_FRAMEBUFFER, (unsigned int)read );
    gl->delete_framebuffers( 1, &fbo );
    return status == PGL_GL_FRAMEBUFFER_COMPLETE ? PGL_COPY_DONE : PGL_COPY_INCOMPLETE;
}
