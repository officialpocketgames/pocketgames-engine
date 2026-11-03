// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35: the parts of pgl that are pure functions (no EGL, GL, Metal or Wine types), so the
// host tests run them on Linux: the pixel formats (D2), the context attributes (D3.2), the
// strings a program reads (C3.4), the pacing decisions (D5.4) and the per-program bookkeeping
// (H2). pgl.c, pgl_present.m and win32u's opengl_ios.c use them.

#ifndef PGL_PURE_H
#define PGL_PURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- D2: pixel formats ---- */

/* PIXELFORMATDESCRIPTOR flags (Microsoft's wingdi.h values); opengl_ios.c checks them against
 * Wine's with _Static_assert. */
#define PGL_PFD_DOUBLEBUFFER         0x00000001
#define PGL_PFD_STEREO               0x00000002
#define PGL_PFD_DRAW_TO_WINDOW       0x00000004
#define PGL_PFD_DRAW_TO_BITMAP       0x00000008
#define PGL_PFD_SUPPORT_GDI          0x00000010
#define PGL_PFD_SUPPORT_OPENGL       0x00000020
#define PGL_PFD_SUPPORT_COMPOSITION  0x00008000

/* One configuration: colour 8/8/8 with or without 8 bits of alpha, a depth/stencil pair. */
struct pgl_config
{
    uint8_t alpha_bits;
    uint8_t depth_bits;
    uint8_t stencil_bits;
};

#define PGL_CONFIG_COUNT    8u   /* D2.1, best first */
#define PGL_FLAG_SET_COUNT  3u   /* D2.2: PFD_SUPPORT_GDI, PFD_DOUBLEBUFFER, offscreen */
#define PGL_FORMAT_COUNT    (PGL_CONFIG_COUNT * PGL_FLAG_SET_COUNT)
#define PGL_ONSCREEN_COUNT  (PGL_CONFIG_COUNT * (PGL_FLAG_SET_COUNT - 1))

#define PGL_PBUFFER_MAX     4096 /* D2.3: pbuffer limits, and D4.2's "largest" */

extern const struct pgl_config pgl_configs[PGL_CONFIG_COUNT];

/* A pixel format as WGL describes it, the fields of PIXELFORMATDESCRIPTOR and Wine's
 * struct wgl_pixel_format that D2 fills; every other field is 0. */
struct pgl_pixel_format
{
    uint32_t flags;               /* PGL_PFD_* */
    uint8_t color_bits;
    uint8_t red_bits, red_shift;
    uint8_t green_bits, green_shift;
    uint8_t blue_bits, blue_shift;
    uint8_t alpha_bits, alpha_shift;
    uint8_t depth_bits, stencil_bits;
    int draw_to_pbuffer;
    int max_pbuffer_width, max_pbuffer_height, max_pbuffer_pixels;
    int bind_to_texture_rgb, bind_to_texture_rgba;
    int bind_to_texture_rectangle_rgb, bind_to_texture_rectangle_rgba;
};

/* format is 1-based, as WGL numbers them. Returns 0 for a number outside 1..PGL_FORMAT_COUNT;
 * otherwise sets the configuration index and the flag set (upstream egldrv's layout: format f
 * is configuration (f - 1) % 8 with flag set (f - 1) / 8). */
int pgl_format_split( int format, unsigned int *config, unsigned int *flag_set );

/* D2.3: fills *out for format (1-based). Returns 0 for an invalid number. */
int pgl_describe_format( int format, struct pgl_pixel_format *out );

/* ---- D3.2: context attributes ---- */

/* WGL_ARB_create_context(_profile), WGL_ARB_create_context_no_error and
 * WGL_EXT_create_context_es2_profile values (Khronos registry) */
#define PGL_WGL_CONTEXT_MAJOR_VERSION      0x2091
#define PGL_WGL_CONTEXT_MINOR_VERSION      0x2092
#define PGL_WGL_CONTEXT_LAYER_PLANE        0x2093
#define PGL_WGL_CONTEXT_FLAGS              0x2094
#define PGL_WGL_CONTEXT_PROFILE_MASK       0x9126
#define PGL_WGL_CONTEXT_OPENGL_NO_ERROR    0x31B3
#define PGL_WGL_CONTEXT_CORE_PROFILE_BIT   0x1
#define PGL_WGL_CONTEXT_COMPAT_PROFILE_BIT 0x2
#define PGL_WGL_CONTEXT_ES2_PROFILE_BIT    0x4
#define PGL_WGL_CONTEXT_DEBUG_BIT          0x1
#define PGL_WGL_CONTEXT_FORWARD_COMPAT_BIT 0x2
#define PGL_ERROR_INVALID_VERSION_ARB      0x2095
#define PGL_ERROR_INVALID_PROFILE_ARB      0x2096

struct pgl_context_request
{
    int major, minor;        /* default 1.0 */
    int profile_mask;        /* as given (ignored below 3.2) */
    int flags;               /* as given (debug and forward-compatible ignored at <= 2.1) */
    int no_error;
    int unknown;             /* attributes that were neither of the above (ignored) */
};

enum pgl_attribs_result
{
    PGL_ATTRIBS_OK = 0,
    PGL_ATTRIBS_BAD_VERSION,  /* -> ERROR_INVALID_VERSION_ARB */
    PGL_ATTRIBS_BAD_PROFILE,  /* -> ERROR_INVALID_PROFILE_ARB */
};

/* Parses a 0-terminated WGL attribute list (NULL = none). Accepts OpenGL 1.0-1.5, 2.0 and 2.1
 * with any profile bit but the ES2 one; a later value of an attribute replaces an earlier one,
 * as WGL_ARB_create_context allows. */
enum pgl_attribs_result pgl_parse_context_attribs( const int *attribs, struct pgl_context_request *req );

/* ---- C3.4: the strings a program reads ---- */

#define PGL_GL_VENDOR_VALUE    "Apple"
#define PGL_GL_VERSION_VALUE   "2.1.0"
#define PGL_GLSL_VERSION_VALUE "1.20"
#define PGL_EXTENSIONS_MAX     4000u   /* some old games copy the string into 4096 bytes */

/* Copies the space-separated extension list in to out (at most out_size bytes with the
 * terminator, and never more than PGL_EXTENSIONS_MAX - 1 characters), dropping the names that
 * would show a library (GL_ANGLE_*, and any containing "gl4es" in any case) and repeated
 * names; a name that doesn't fit ends the list. Returns the length written. */
size_t pgl_filter_extensions( const char *in, char *out, size_t out_size );

/* ---- D5.4: pacing (spec 05 E3's modes) ---- */

/* The minimum duration between presents for a pacing mode, in seconds (0: plain present). */
double pgl_pace_period( int mode );

/* RAW mode (2): 1 when no drawable may be taken, because less than 18 ms passed since the last
 * real acquire at last_ns (0: none yet). */
int pgl_pace_raw_skip( int mode, uint64_t now_ns, uint64_t last_ns );

/* The precise limiter (DXMT_PRESENT_LIMITER=1): the next deadline after `deadline` (0: none),
 * one period later; from `now` again when more than one period late. */
uint64_t pgl_limiter_next( uint64_t deadline, uint64_t now, uint64_t period );

/* nextDrawable timing log (spec 05 E3): 1 when acquire number n (0-based) taking ms should be
 * logged: the first eight, and every one over 50 ms (the caller rate-limits those). */
int pgl_acquire_logged( uint64_t n, double ms );

/* ---- H2: what each program owns ---- */

enum pgl_owned_kind
{
    PGL_OWNED_CONTEXT = 0,
    PGL_OWNED_DRAWABLE,
    PGL_OWNED_KINDS
};

struct pgl_owned
{
    struct pgl_owned *prev, *next;
    const void *owner;          /* the creating pseudo-process (its PEB) */
    enum pgl_owned_kind kind;
};

struct pgl_registry
{
    struct pgl_owned head;      /* circular list */
};

void pgl_registry_init( struct pgl_registry *reg );
void pgl_registry_add( struct pgl_registry *reg, struct pgl_owned *item, const void *owner, enum pgl_owned_kind kind );
void pgl_registry_remove( struct pgl_owned *item );   /* an item not in a list is left alone */
/* Moves every item of owner into *taken (initialised here) and counts them per kind. */
void pgl_registry_take( struct pgl_registry *reg, const void *owner, struct pgl_registry *taken,
                        unsigned int counts[PGL_OWNED_KINDS] );
struct pgl_owned *pgl_registry_first( struct pgl_registry *reg );   /* NULL when empty */
unsigned int pgl_registry_count( const struct pgl_registry *reg, const void *owner );

/* ---- D5.2: the present ring ---- */

#define PGL_RING_SIZE 3u

static inline unsigned int pgl_ring_next( unsigned int slot )
{
    return (slot + 1) % PGL_RING_SIZE;
}

/* ---- D4.2, clarification 2.6: WGL_ARB_render_texture on an FBO pbuffer ---- */

#define PGL_GL_NONE                         0x0000
#define PGL_GL_FRONT_LEFT                   0x0400
#define PGL_GL_FRONT_RIGHT                  0x0401
#define PGL_GL_BACK_LEFT                    0x0402
#define PGL_GL_BACK_RIGHT                   0x0403
#define PGL_GL_FRONT                        0x0404
#define PGL_GL_BACK                         0x0405
#define PGL_GL_LEFT                         0x0406
#define PGL_GL_RIGHT                        0x0407
#define PGL_GL_FRONT_AND_BACK               0x0408
#define PGL_GL_AUX0                         0x0409
#define PGL_GL_TEXTURE_2D                   0x0DE1
#define PGL_GL_TEXTURE_CUBE_MAP             0x8513
#define PGL_GL_TEXTURE_CUBE_MAP_POSITIVE_X  0x8515
#define PGL_GL_DRAW_FRAMEBUFFER_BINDING     0x8CA6   /* = GL_FRAMEBUFFER_BINDING */
#define PGL_GL_READ_FRAMEBUFFER             0x8CA8
#define PGL_GL_READ_FRAMEBUFFER_BINDING     0x8CAA
#define PGL_GL_FRAMEBUFFER_COMPLETE         0x8CD5
#define PGL_GL_COLOR_ATTACHMENT0            0x8CE0
#define PGL_GL_FRAMEBUFFER                  0x8D40
#define PGL_GL_RENDERBUFFER                 0x8D41

/* the GL calls of the copy, made in the program's current context (gl4es's entry points) */
struct pgl_copy_gl
{
    void (*get_integerv)( unsigned int pname, int *value );
    void (*gen_framebuffers)( int n, unsigned int *names );
    void (*delete_framebuffers)( int n, const unsigned int *names );
    void (*bind_framebuffer)( unsigned int target, unsigned int name );
    void (*framebuffer_renderbuffer)( unsigned int target, unsigned int attachment, unsigned int rb_target,
                                      unsigned int renderbuffer );
    unsigned int (*check_framebuffer_status)( unsigned int target );
    void (*copy_tex_image_2d)( unsigned int target, int level, unsigned int internal_format, int x, int y,
                               int width, int height, int border );
};

struct pgl_pbuffer_copy
{
    unsigned int source;          /* from wglBindTexImageARB (win32u's GL_FRONT, GL_BACK, ...); GL_NONE: release */
    unsigned int renderbuffer;    /* the pbuffer's colour renderbuffer, shared with the context; 0: none yet */
    int width, height;            /* its size */
    unsigned int texture_target;  /* the pbuffer's texture target: GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, ... */
    unsigned int texture_format;  /* GL_RGB or GL_RGBA */
    unsigned int cube_face;       /* the face for a cube map (wglSetPbufferAttribARB); 0: positive X */
    int level;                    /* the mipmap level (wglSetPbufferAttribARB) */
};

enum pgl_copy_result
{
    PGL_COPY_DONE,                /* copied, or a release (nothing stays bound: the copy is the binding) */
    PGL_COPY_NOTHING,             /* the pbuffer was never drawn to: the texture is left as it is */
    PGL_COPY_BAD_SOURCE,          /* right or aux buffers: a pbuffer has none (clarification 2.8) */
    PGL_COPY_INCOMPLETE,          /* the temporary framebuffer was not complete: nothing copied */
};

/* front, back, left and front-and-back are a pbuffer's one colour buffer */
int pgl_pbuffer_source_ok( unsigned int source );

/* A temporary framebuffer on the pbuffer's colour renderbuffer, glCopyTexImage2D into the texture
 * bound to the target, then the program's draw and read framebuffer bindings back and the
 * temporary framebuffer deleted. No other binding changes. */
enum pgl_copy_result pgl_pbuffer_copy( const struct pgl_copy_gl *gl, const struct pgl_pbuffer_copy *req );

#ifdef __cplusplus
}
#endif

#endif /* PGL_PURE_H */
