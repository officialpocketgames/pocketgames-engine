// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part J1: host tests of pgl's pure functions (pgl_pure.c): D2's format table and
// descriptions, D3.2's attribute parsing, C3.4's extension filter, D5.4's pacing decisions,
// H2's bookkeeping on fake objects of two owners, and D4.2's pbuffer-to-texture copy (the binding
// save and restore, clarification 2.6) on a fake GL. No GL, EGL or Metal.

#include <stdio.h>
#include <string.h>

#include "pgl_pure.h"

static int checks, failures;

#define CHECK( cond ) do { checks++; if (!(cond)) { failures++; \
    printf( "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond ); } } while (0)

/* ---- D2 ---- */

static void test_formats( void )
{
    static const struct { int alpha, depth, stencil; } want[8] =
        { { 8, 24, 8 }, { 0, 24, 8 }, { 8, 24, 0 }, { 0, 24, 0 }, { 8, 16, 0 }, { 0, 16, 0 }, { 8, 0, 0 }, { 0, 0, 0 } };
    struct pgl_pixel_format f;
    unsigned int config, set, onscreen = 0;
    int i;

    CHECK( PGL_FORMAT_COUNT == 24 );
    CHECK( PGL_ONSCREEN_COUNT == 16 );
    for (i = 0; i < 8; i++)
    {
        CHECK( pgl_configs[i].alpha_bits == want[i].alpha );
        CHECK( pgl_configs[i].depth_bits == want[i].depth );
        CHECK( pgl_configs[i].stencil_bits == want[i].stencil );
    }

    CHECK( !pgl_format_split( 0, &config, &set ) );
    CHECK( !pgl_format_split( 25, &config, &set ) );
    CHECK( !pgl_format_split( -1, &config, &set ) );
    CHECK( !pgl_describe_format( 0, &f ) );
    CHECK( !pgl_describe_format( 25, &f ) );
    CHECK( pgl_format_split( 1, &config, &set ) && config == 0 && set == 0 );
    CHECK( pgl_format_split( 8, &config, &set ) && config == 7 && set == 0 );
    CHECK( pgl_format_split( 9, &config, &set ) && config == 0 && set == 1 );
    CHECK( pgl_format_split( 24, &config, &set ) && config == 7 && set == 2 );

    for (i = 1; i <= 24; i++)
    {
        unsigned int c = (unsigned int)(i - 1) % 8, s = (unsigned int)(i - 1) / 8;

        CHECK( pgl_describe_format( i, &f ) );
        CHECK( f.flags & PGL_PFD_SUPPORT_OPENGL );
        CHECK( f.flags & PGL_PFD_SUPPORT_COMPOSITION );
        CHECK( f.flags & PGL_PFD_DRAW_TO_BITMAP );            /* every format draws to a pbuffer */
        CHECK( !(f.flags & PGL_PFD_STEREO) );
        CHECK( !!(f.flags & PGL_PFD_SUPPORT_GDI) == (s == 0) );
        CHECK( !!(f.flags & PGL_PFD_DOUBLEBUFFER) == (s == 1) );
        CHECK( !!(f.flags & PGL_PFD_DRAW_TO_WINDOW) == (s < 2) );   /* only the offscreen set can't */
        if (f.flags & PGL_PFD_DRAW_TO_WINDOW) onscreen++;
        CHECK( f.color_bits == 32 );
        CHECK( f.red_bits == 8 && f.green_bits == 8 && f.blue_bits == 8 );
        CHECK( f.blue_shift == 0 && f.green_shift == 8 && f.red_shift == 16 );
        CHECK( f.alpha_bits == want[c].alpha );
        CHECK( f.alpha_shift == (want[c].alpha ? 24 : 0) );
        CHECK( f.depth_bits == want[c].depth && f.stencil_bits == want[c].stencil );
        CHECK( f.draw_to_pbuffer == 1 );
        CHECK( f.max_pbuffer_width == 4096 && f.max_pbuffer_height == 4096 );
        CHECK( f.max_pbuffer_pixels == 4096 * 4096 );
        CHECK( f.bind_to_texture_rgb && f.bind_to_texture_rgba );
        CHECK( f.bind_to_texture_rectangle_rgb && f.bind_to_texture_rectangle_rgba );
    }
    CHECK( onscreen == PGL_ONSCREEN_COUNT );
}

/* ---- D3.2 ---- */

static enum pgl_attribs_result parse( const int *attribs, struct pgl_context_request *req )
{
    return pgl_parse_context_attribs( attribs, req );
}

static void test_attribs( void )
{
    struct pgl_context_request req;

    /* no list: 1.0 */
    CHECK( parse( NULL, &req ) == PGL_ATTRIBS_OK && req.major == 1 && req.minor == 0 );
    {
        static const int empty[] = { 0 };
        CHECK( parse( empty, &req ) == PGL_ATTRIBS_OK && req.major == 1 && req.minor == 0 );
    }
    /* what wglCreateContext asks for */
    {
        static const int compat[] = { PGL_WGL_CONTEXT_PROFILE_MASK, PGL_WGL_CONTEXT_COMPAT_PROFILE_BIT, 0, 0 };
        CHECK( parse( compat, &req ) == PGL_ATTRIBS_OK && req.profile_mask == PGL_WGL_CONTEXT_COMPAT_PROFILE_BIT );
    }
    /* every version up to 2.1, with any profile bit (ignored below 3.2) */
    {
        static const int v[][2] = { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 1, 3 }, { 1, 4 }, { 1, 5 }, { 2, 0 }, { 2, 1 } };
        unsigned int i;
        for (i = 0; i < sizeof(v) / sizeof(v[0]); i++)
        {
            int a[] = { PGL_WGL_CONTEXT_MAJOR_VERSION, v[i][0], PGL_WGL_CONTEXT_MINOR_VERSION, v[i][1],
                        PGL_WGL_CONTEXT_PROFILE_MASK, PGL_WGL_CONTEXT_CORE_PROFILE_BIT, 0 };
            CHECK( parse( a, &req ) == PGL_ATTRIBS_OK && req.major == v[i][0] && req.minor == v[i][1] );
        }
    }
    /* above 2.1, and versions that don't exist */
    {
        static const int v[][2] = { { 2, 2 }, { 3, 0 }, { 3, 3 }, { 4, 6 }, { 1, 6 }, { 0, 0 }, { 5, 0 }, { 2, -1 } };
        unsigned int i;
        for (i = 0; i < sizeof(v) / sizeof(v[0]); i++)
        {
            int a[] = { PGL_WGL_CONTEXT_MAJOR_VERSION, v[i][0], PGL_WGL_CONTEXT_MINOR_VERSION, v[i][1], 0 };
            CHECK( parse( a, &req ) == PGL_ATTRIBS_BAD_VERSION );
        }
    }
    /* 3.3 core: the version error (J4's last program) */
    {
        static const int core33[] = { PGL_WGL_CONTEXT_MAJOR_VERSION, 3, PGL_WGL_CONTEXT_MINOR_VERSION, 3,
                                      PGL_WGL_CONTEXT_PROFILE_MASK, PGL_WGL_CONTEXT_CORE_PROFILE_BIT, 0 };
        CHECK( parse( core33, &req ) == PGL_ATTRIBS_BAD_VERSION && req.major == 3 && req.minor == 3 );
    }
    /* the ES2 profile bit fails whatever the version */
    {
        static const int es2[] = { PGL_WGL_CONTEXT_MAJOR_VERSION, 2, PGL_WGL_CONTEXT_PROFILE_MASK, PGL_WGL_CONTEXT_ES2_PROFILE_BIT, 0 };
        static const int es3[] = { PGL_WGL_CONTEXT_PROFILE_MASK, PGL_WGL_CONTEXT_ES2_PROFILE_BIT, PGL_WGL_CONTEXT_MAJOR_VERSION, 3, 0 };
        CHECK( parse( es2, &req ) == PGL_ATTRIBS_BAD_PROFILE );
        CHECK( parse( es3, &req ) == PGL_ATTRIBS_BAD_PROFILE );
    }
    /* debug and forward-compatible accepted (ignored at <= 2.1), no-error kept, unknown counted */
    {
        static const int flags[] = { PGL_WGL_CONTEXT_FLAGS, PGL_WGL_CONTEXT_DEBUG_BIT | PGL_WGL_CONTEXT_FORWARD_COMPAT_BIT,
                                     PGL_WGL_CONTEXT_OPENGL_NO_ERROR, 1, PGL_WGL_CONTEXT_LAYER_PLANE, 0, 0x1234, 5, 0 };
        CHECK( parse( flags, &req ) == PGL_ATTRIBS_OK );
        CHECK( req.flags == (PGL_WGL_CONTEXT_DEBUG_BIT | PGL_WGL_CONTEXT_FORWARD_COMPAT_BIT) );
        CHECK( req.no_error == 1 && req.unknown == 2 );
    }
    /* a later value replaces an earlier one */
    {
        static const int twice[] = { PGL_WGL_CONTEXT_MAJOR_VERSION, 4, PGL_WGL_CONTEXT_MAJOR_VERSION, 2, 0 };
        CHECK( parse( twice, &req ) == PGL_ATTRIBS_OK && req.major == 2 && req.minor == 0 );
    }
}

/* ---- C3.4 ---- */

static void test_extensions( void )
{
    static char big_in[20000], out[PGL_EXTENSIONS_MAX + 64];
    size_t n, i, len;

    n = pgl_filter_extensions( "GL_ARB_multitexture  GL_ANGLE_texture_usage GL_GL4ES_hint gl4es_x "
                               "GL_ARB_texture_rectangle GL_ARB_multitexture GL_EXT_texture_compression_s3tc",
                               out, sizeof(out) );
    CHECK( !strcmp( out, "GL_ARB_multitexture GL_ARB_texture_rectangle GL_EXT_texture_compression_s3tc" ) );
    CHECK( n == strlen( out ) );
    CHECK( pgl_filter_extensions( NULL, out, sizeof(out) ) == 0 && !out[0] );
    CHECK( pgl_filter_extensions( "", out, sizeof(out) ) == 0 && !out[0] );
    CHECK( pgl_filter_extensions( "GL_A", out, 0 ) == 0 );
    CHECK( pgl_filter_extensions( "GL_ABCDEF GL_X", out, 8 ) == 0 && !out[0] );   /* the first name doesn't fit */
    CHECK( pgl_filter_extensions( "GL_ABC GL_XYZ", out, 8 ) == 6 && !strcmp( out, "GL_ABC" ) );

    /* a long list stays under 4,000 bytes and ends at a whole name */
    for (i = 0, len = 0; len + 32 < sizeof(big_in); i++)
        len += (size_t)snprintf( big_in + len, sizeof(big_in) - len, "GL_EXT_name_number_%04u ", (unsigned int)i );
    n = pgl_filter_extensions( big_in, out, sizeof(out) );
    CHECK( n == strlen( out ) );
    CHECK( n < PGL_EXTENSIONS_MAX );
    CHECK( n > PGL_EXTENSIONS_MAX - 40 );
    CHECK( out[n - 1] != ' ' );
    CHECK( !strncmp( out + n - 23, "GL_EXT_name_number_", 19 ) );
}

/* ---- D5.4 ---- */

static void test_pacing( void )
{
    const uint64_t ms = 1000000;

    CHECK( pgl_pace_period( 0 ) == 0.0 );
    CHECK( pgl_pace_period( 1 ) == 1.0 / 60.0 );
    CHECK( pgl_pace_period( 2 ) == 0.0 );
    CHECK( pgl_pace_period( 3 ) == 1.0 / 30.0 );
    CHECK( pgl_pace_period( 4 ) == 1.0 / 40.0 );
    CHECK( pgl_pace_period( 7 ) == 0.0 );

    CHECK( !pgl_pace_raw_skip( 2, 100 * ms, 0 ) );            /* no acquire yet */
    CHECK( pgl_pace_raw_skip( 2, 117 * ms, 100 * ms ) );      /* 17 ms */
    CHECK( !pgl_pace_raw_skip( 2, 118 * ms, 100 * ms ) );     /* 18 ms */
    CHECK( !pgl_pace_raw_skip( 1, 101 * ms, 100 * ms ) );     /* only RAW skips */
    CHECK( !pgl_pace_raw_skip( 2, 99 * ms, 100 * ms ) );      /* a clock going back never skips */

    /* the limiter: from now at first, then one period after the last deadline, from now again
     * when more than one period late */
    CHECK( pgl_limiter_next( 0, 500, 16 ) == 500 );
    CHECK( pgl_limiter_next( 500, 505, 16 ) == 516 );
    CHECK( pgl_limiter_next( 516, 540, 16 ) == 532 );
    CHECK( pgl_limiter_next( 532, 565, 16 ) == 565 );

    CHECK( pgl_acquire_logged( 0, 1.0 ) && pgl_acquire_logged( 7, 1.0 ) );
    CHECK( !pgl_acquire_logged( 8, 49.0 ) && pgl_acquire_logged( 8, 51.0 ) );

    CHECK( pgl_ring_next( 0 ) == 1 && pgl_ring_next( 1 ) == 2 && pgl_ring_next( 2 ) == 0 );
}

/* ---- H2 ---- */

struct fake_object
{
    struct pgl_owned owned;
    int id;
};

static void test_registry( void )
{
    static char owner_a, owner_b;
    struct fake_object obj[6];
    struct pgl_registry reg, taken;
    unsigned int counts[PGL_OWNED_KINDS], n;
    struct pgl_owned *item;
    int i, seen[6] = { 0 };

    pgl_registry_init( &reg );
    CHECK( !pgl_registry_first( &reg ) );
    for (i = 0; i < 6; i++)
    {
        obj[i].id = i;
        /* a: contexts 0, 2 and drawable 4; b: context 1, drawables 3 and 5 */
        pgl_registry_add( &reg, &obj[i].owned, (i % 2) ? &owner_b : &owner_a,
                          (i < 3) ? PGL_OWNED_CONTEXT : PGL_OWNED_DRAWABLE );
    }
    CHECK( pgl_registry_count( &reg, &owner_a ) == 3 );
    CHECK( pgl_registry_count( &reg, &owner_b ) == 3 );

    /* a releases context 2 itself before it ends */
    pgl_registry_remove( &obj[2].owned );
    pgl_registry_remove( &obj[2].owned );   /* twice: nothing */
    CHECK( pgl_registry_count( &reg, &owner_a ) == 2 );

    /* a ends: its two objects are taken, b's stay */
    pgl_registry_take( &reg, &owner_a, &taken, counts );
    CHECK( counts[PGL_OWNED_CONTEXT] == 1 && counts[PGL_OWNED_DRAWABLE] == 1 );
    CHECK( pgl_registry_count( &reg, &owner_a ) == 0 );
    CHECK( pgl_registry_count( &reg, &owner_b ) == 3 );
    CHECK( pgl_registry_count( &taken, &owner_a ) == 2 );
    n = 0;
    while ((item = pgl_registry_first( &taken )))
    {
        struct fake_object *o = (struct fake_object *)item;
        pgl_registry_remove( item );
        CHECK( o->owned.owner == &owner_a );
        seen[o->id]++;
        n++;
    }
    CHECK( n == 2 && seen[0] == 1 && seen[4] == 1 && !seen[2] );
    /* a released object of an ended program is left alone */
    pgl_registry_remove( &obj[0].owned );

    /* a program with nothing */
    pgl_registry_take( &reg, &n, &taken, counts );
    CHECK( counts[0] == 0 && counts[1] == 0 && !pgl_registry_first( &taken ) );

    /* b ends */
    pgl_registry_take( &reg, &owner_b, &taken, counts );
    CHECK( counts[PGL_OWNED_CONTEXT] == 1 && counts[PGL_OWNED_DRAWABLE] == 2 );
    CHECK( !pgl_registry_first( &reg ) );
}

/* ---- D4.2, clarification 2.6: a fake GL with draw and read bindings, framebuffer objects with
 * one colour attachment, and a log of the copies */
static struct
{
    unsigned int draw, read;              /* the bindings */
    unsigned int next_name;
    unsigned int live[8];                 /* framebuffers alive, 0: free slot */
    unsigned int attached[8];             /* their colour renderbuffer */
    unsigned int complete_rb;             /* the renderbuffer a framebuffer is complete with; 0: any */
    int copies;
    unsigned int copy_target, copy_format, copy_read_fbo, copy_rb;
    int copy_level, copy_w, copy_h, other_calls;
} fake;

static int fake_slot( unsigned int name )
{
    int i;
    for (i = 0; i < 8; i++) if (name && fake.live[i] == name) return i;
    return -1;
}
static void fake_get_integerv( unsigned int pname, int *value )
{
    if (pname == PGL_GL_DRAW_FRAMEBUFFER_BINDING) *value = (int)fake.draw;
    else if (pname == PGL_GL_READ_FRAMEBUFFER_BINDING) *value = (int)fake.read;
    else fake.other_calls++;
}
static void fake_gen_framebuffers( int n, unsigned int *names )
{
    int i, k;
    for (k = 0; k < n; k++)
        for (i = 0; i < 8; i++)
            if (!fake.live[i]) { fake.live[i] = names[k] = ++fake.next_name; fake.attached[i] = 0; break; }
}
static void fake_delete_framebuffers( int n, const unsigned int *names )
{
    int k, i;
    for (k = 0; k < n; k++)
    {
        if ((i = fake_slot( names[k] )) < 0) { fake.other_calls++; continue; }
        fake.live[i] = 0;
        if (fake.draw == names[k]) fake.draw = 0;   /* GL: deleting a bound framebuffer binds 0 */
        if (fake.read == names[k]) fake.read = 0;
    }
}
static void fake_bind_framebuffer( unsigned int target, unsigned int name )
{
    if (target == PGL_GL_FRAMEBUFFER) fake.draw = fake.read = name;
    else if (target == PGL_GL_READ_FRAMEBUFFER) fake.read = name;
    else fake.other_calls++;
}
static void fake_framebuffer_renderbuffer( unsigned int target, unsigned int attachment, unsigned int rb_target,
                                           unsigned int rb )
{
    int i = fake_slot( fake.draw );
    if (target != PGL_GL_FRAMEBUFFER || attachment != PGL_GL_COLOR_ATTACHMENT0 || rb_target != PGL_GL_RENDERBUFFER || i < 0)
        fake.other_calls++;
    else fake.attached[i] = rb;
}
static unsigned int fake_check_framebuffer_status( unsigned int target )
{
    int i = fake_slot( fake.draw );
    (void)target;
    if (i < 0 || !fake.attached[i]) return 0x8CD6;   /* GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT */
    return (!fake.complete_rb || fake.attached[i] == fake.complete_rb) ? PGL_GL_FRAMEBUFFER_COMPLETE : 0x8CD6;
}
static void fake_copy_tex_image_2d( unsigned int target, int level, unsigned int format, int x, int y,
                                    int w, int h, int border )
{
    int i = fake_slot( fake.read );
    fake.copies++;
    fake.copy_target = target;
    fake.copy_level = level;
    fake.copy_format = format;
    fake.copy_read_fbo = fake.read;
    fake.copy_rb = i < 0 ? 0 : fake.attached[i];
    fake.copy_w = w;
    fake.copy_h = h;
    if (x || y || border) fake.other_calls++;
}

static const struct pgl_copy_gl fake_gl =
{
    fake_get_integerv, fake_gen_framebuffers, fake_delete_framebuffers, fake_bind_framebuffer,
    fake_framebuffer_renderbuffer, fake_check_framebuffer_status, fake_copy_tex_image_2d,
};

static void fake_reset( unsigned int draw, unsigned int read )
{
    int i;
    memset( &fake, 0, sizeof(fake) );
    fake.next_name = 100;
    /* the program's own framebuffers */
    fake.live[0] = 7;
    fake.live[1] = 9;
    fake.draw = draw;
    fake.read = read;
    for (i = 2; i < 8; i++) fake.live[i] = 0;
}

static int fake_live_count( void )
{
    int i, n = 0;
    for (i = 0; i < 8; i++) if (fake.live[i]) n++;
    return n;
}

static void test_pbuffer_copy( void )
{
    struct pgl_pbuffer_copy req = { PGL_GL_BACK, 42, 256, 128, PGL_GL_TEXTURE_2D, 0x1908 /* GL_RGBA */, 0, 0 };
    static const unsigned int good[] = { PGL_GL_FRONT, PGL_GL_BACK, PGL_GL_FRONT_LEFT, PGL_GL_BACK_LEFT,
                                         PGL_GL_LEFT, PGL_GL_FRONT_AND_BACK };
    static const unsigned int bad[] = { PGL_GL_FRONT_RIGHT, PGL_GL_BACK_RIGHT, PGL_GL_RIGHT, PGL_GL_AUX0,
                                        PGL_GL_AUX0 + 3, 0x1234 };
    unsigned int i;

    /* which buffers a pbuffer has (clarification 2.8) */
    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) CHECK( pgl_pbuffer_source_ok( good[i] ) );
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK( !pgl_pbuffer_source_ok( bad[i] ) );
    CHECK( !pgl_pbuffer_source_ok( PGL_GL_NONE ) );

    /* the copy, with the program on framebuffer 7 for both: everything back as it was */
    fake_reset( 7, 7 );
    CHECK( pgl_pbuffer_copy( &fake_gl, &req ) == PGL_COPY_DONE );
    CHECK( fake.copies == 1 );
    CHECK( fake.copy_target == PGL_GL_TEXTURE_2D && fake.copy_level == 0 && fake.copy_format == 0x1908 );
    CHECK( fake.copy_w == 256 && fake.copy_h == 128 );
    CHECK( fake.copy_rb == 42 );                                   /* read from the pbuffer's colour */
    CHECK( fake.copy_read_fbo != 7 && fake.copy_read_fbo != 9 && fake.copy_read_fbo != 0 );   /* a temporary one */
    CHECK( fake.draw == 7 && fake.read == 7 );                     /* the bindings back */
    CHECK( fake_live_count() == 2 && fake_slot( 7 ) >= 0 && fake_slot( 9 ) >= 0 );   /* temporary deleted */
    CHECK( fake.other_calls == 0 );

    /* draw and read apart (glBindFramebuffer(GL_READ_FRAMEBUFFER, ...)): both back */
    fake_reset( 7, 9 );
    CHECK( pgl_pbuffer_copy( &fake_gl, &req ) == PGL_COPY_DONE );
    CHECK( fake.draw == 7 && fake.read == 9 && fake_live_count() == 2 && fake.other_calls == 0 );

    /* the window's framebuffer (0) */
    fake_reset( 0, 0 );
    CHECK( pgl_pbuffer_copy( &fake_gl, &req ) == PGL_COPY_DONE );
    CHECK( fake.draw == 0 && fake.read == 0 && fake_live_count() == 2 && fake.copies == 1 );

    /* every source of a pbuffer's one colour buffer copies the same */
    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++)
    {
        fake_reset( 9, 7 );
        req.source = good[i];
        CHECK( pgl_pbuffer_copy( &fake_gl, &req ) == PGL_COPY_DONE && fake.copies == 1 && fake.copy_rb == 42 );
        CHECK( fake.draw == 9 && fake.read == 7 && fake_live_count() == 2 );
    }

    /* right and aux: refused, nothing touched */
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        fake_reset( 7, 9 );
        req.source = bad[i];
        CHECK( pgl_pbuffer_copy( &fake_gl, &req ) == PGL_COPY_BAD_SOURCE );
        CHECK( fake.copies == 0 && fake.draw == 7 && fake.read == 9 && fake.next_name == 100 );
    }

    /* release: nothing to undo, nothing touched */
    fake_reset( 7, 7 );
    req.source = PGL_GL_NONE;
    CHECK( pgl_pbuffer_copy( &fake_gl, &req ) == PGL_COPY_DONE && fake.copies == 0 && fake.next_name == 100 );
    req.source = PGL_GL_BACK;

    /* a pbuffer never drawn to (no attachments yet): the texture is left alone */
    fake_reset( 7, 7 );
    req.renderbuffer = 0;
    CHECK( pgl_pbuffer_copy( &fake_gl, &req ) == PGL_COPY_NOTHING && fake.copies == 0 && fake.next_name == 100 );
    req.renderbuffer = 42;
    req.width = 0;
    CHECK( pgl_pbuffer_copy( &fake_gl, &req ) == PGL_COPY_NOTHING && fake.copies == 0 );
    req.width = 256;

    /* an incomplete temporary framebuffer: no copy, but the bindings back and nothing left over */
    fake_reset( 9, 7 );
    fake.complete_rb = 43;
    CHECK( pgl_pbuffer_copy( &fake_gl, &req ) == PGL_COPY_INCOMPLETE );
    CHECK( fake.copies == 0 && fake.draw == 9 && fake.read == 7 && fake_live_count() == 2 );

    /* a cube map: the face set with wglSetPbufferAttribARB, positive X when none; the level */
    fake_reset( 7, 7 );
    req.texture_target = PGL_GL_TEXTURE_CUBE_MAP;
    req.cube_face = PGL_GL_TEXTURE_CUBE_MAP_POSITIVE_X + 3;
    req.level = 2;
    CHECK( pgl_pbuffer_copy( &fake_gl, &req ) == PGL_COPY_DONE );
    CHECK( fake.copy_target == PGL_GL_TEXTURE_CUBE_MAP_POSITIVE_X + 3 && fake.copy_level == 2 );
    fake_reset( 7, 7 );
    req.cube_face = 0;
    CHECK( pgl_pbuffer_copy( &fake_gl, &req ) == PGL_COPY_DONE && fake.copy_target == PGL_GL_TEXTURE_CUBE_MAP_POSITIVE_X );
}

int main( void )
{
    test_formats();
    test_attribs();
    test_extensions();
    test_pacing();
    test_registry();
    test_pbuffer_copy();
    printf( "test_pgl_pure: %d checks, %d failures\n", checks, failures );
    return failures != 0;
}
