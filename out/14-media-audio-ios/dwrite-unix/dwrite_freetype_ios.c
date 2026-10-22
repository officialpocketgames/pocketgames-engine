// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part D: dwrite's unix side on iOS. Upstream dlls/dwrite/freetype.c (Wine 11.4,
// LGPL-2.1-or-later, copyright Nikolay Sivov for CodeWeavers) is compiled UNCHANGED by including
// it below; build-ios.sh copies it to freetype_upstream.inc. This wrapper only:
//   D1  defines HAVE_FT2BUILD_H, HAVE_FREETYPE and a SONAME_LIBFREETYPE, and maps dlopen / dlsym /
//       dlclose to functions of its own that resolve exactly the FreeType functions freetype.c
//       loads, against spec 12's static FreeType (linked once, in libwin32u_unix.a);
//   D2  provides 32-bit thunks that convert every guest pointer by rule A4 (upstream's use
//       ULongToPtr, which is only right with no guest window);
//   D3  initialises one FreeType library for the app on the first process_attach and never
//       destroys it;
//   A3  publishes the tables as dwrite_unix_call_funcs / dwrite_unix_call_wow64_funcs.

#if 0
#pragma makedep unix
#endif

#include "config.h"

#undef HAVE_FT2BUILD_H
#define HAVE_FT2BUILD_H 1
#undef HAVE_FREETYPE
#define HAVE_FREETYPE 1
#undef SONAME_LIBFREETYPE
#define SONAME_LIBFREETYPE "libfreetype (static, spec 12)"

#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>

/* D1: our own loader functions; declared before the macros so <dlfcn.h>'s prototypes keep their
 * names */
static void *dwrite_ft_dlopen( const char *name, int flags );
static void *dwrite_ft_dlsym( void *handle, const char *name );
static int dwrite_ft_dlclose( void *handle );
#define dlopen dwrite_ft_dlopen
#define dlsym dwrite_ft_dlsym
#define dlclose dwrite_ft_dlclose

/* upstream's tables and attach/detach keep their code but not their names: this file defines the
 * tables spec 09's loader looks up, with D3's attach/detach in front of upstream's */
#define __wine_unix_call_funcs dwrite_upstream_unused_funcs
#define __wine_unix_call_wow64_funcs dwrite_upstream_unused_wow64_funcs
#define process_attach dwrite_upstream_process_attach
#define process_detach dwrite_upstream_process_detach

#include "freetype_upstream.inc"

#undef process_attach
#undef process_detach
#undef dlopen
#undef dlsym
#undef dlclose

#include "ios_media_wow.h"

/* ---------------------------------------------------------------- D1: the symbol table */

static int freetype_token;

#define FT_ENTRY(f) { #f, (void *)f }
static const struct { const char *name; void *func; } freetype_functions[] =
{
    /* exactly the functions freetype.c loads */
    FT_ENTRY(FT_Activate_Size),
    FT_ENTRY(FT_Done_Face),
    FT_ENTRY(FT_Done_FreeType),
    FT_ENTRY(FT_Done_Glyph),
    FT_ENTRY(FT_Done_Size),
    FT_ENTRY(FT_Get_First_Char),
    FT_ENTRY(FT_Get_Glyph),
    FT_ENTRY(FT_Get_Kerning),
    FT_ENTRY(FT_Get_Sfnt_Table),
    FT_ENTRY(FT_Glyph_Copy),
    FT_ENTRY(FT_Glyph_Get_CBox),
    FT_ENTRY(FT_Glyph_Transform),
    FT_ENTRY(FT_Init_FreeType),
    FT_ENTRY(FT_Library_Version),
    FT_ENTRY(FT_Load_Glyph),
    FT_ENTRY(FT_Matrix_Multiply),
    FT_ENTRY(FT_MulDiv),
    FT_ENTRY(FT_New_Memory_Face),
    FT_ENTRY(FT_New_Size),
    FT_ENTRY(FT_Outline_Copy),
    FT_ENTRY(FT_Outline_Decompose),
    FT_ENTRY(FT_Outline_Done),
    FT_ENTRY(FT_Outline_EmboldenXY),
    FT_ENTRY(FT_Outline_Get_Bitmap),
    FT_ENTRY(FT_Outline_New),
    FT_ENTRY(FT_Outline_Transform),
    FT_ENTRY(FT_Outline_Translate),
    FT_ENTRY(FT_Set_Pixel_Sizes),
};
#undef FT_ENTRY

static void *dwrite_ft_dlopen( const char *name, int flags )
{
    return name && strstr( name, "freetype" ) ? &freetype_token : NULL;
}

static void *dwrite_ft_dlsym( void *handle, const char *name )
{
    size_t i;

    if (handle != &freetype_token || !name) return NULL;
    for (i = 0; i < sizeof(freetype_functions) / sizeof(freetype_functions[0]); i++)
        if (!strcmp( freetype_functions[i].name, name )) return freetype_functions[i].func;
    return NULL;   /* upstream's "can't find symbol" path fails loudly */
}

static int dwrite_ft_dlclose( void *handle )
{
    return 0;
}

/* ---------------------------------------------------------------- D3: one FreeType library */

static pthread_mutex_t ft_lock = PTHREAD_MUTEX_INITIALIZER;
static int ft_ready;

static NTSTATUS ios_process_attach( void *args )
{
    NTSTATUS status = STATUS_SUCCESS;

    pthread_mutex_lock( &ft_lock );
    if (!ft_ready)
    {
        status = dwrite_upstream_process_attach( args );
        if (!status) ft_ready = 1;
    }
    pthread_mutex_unlock( &ft_lock );
    return status;
}

static NTSTATUS ios_process_detach( void *args )
{
    /* never destroyed: another pseudo-process may be using it */
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- A3: the tables */

#define EXPORTED_TABLE __attribute__((used, visibility("default")))

const unixlib_entry_t dwrite_unix_call_funcs[] EXPORTED_TABLE =
{
    [unix_process_attach] = ios_process_attach,
    [unix_process_detach] = ios_process_detach,
    [unix_create_font_object] = create_font_object,
    [unix_release_font_object] = release_font_object,
    [unix_get_glyph_outline] = get_glyph_outline,
    [unix_get_glyph_count] = get_glyph_count,
    [unix_get_glyph_advance] = get_glyph_advance,
    [unix_get_glyph_bbox] = get_glyph_bbox,
    [unix_get_glyph_bitmap] = get_glyph_bitmap,
    [unix_get_design_glyph_metrics] = get_design_glyph_metrics,
};
_Static_assert( sizeof(dwrite_unix_call_funcs) / sizeof(dwrite_unix_call_funcs[0]) == unix_funcs_count,
                "dwrite_unix_call_funcs" );

/* ---------------------------------------------------------------- D2: 32-bit callers */

typedef ULONG PTR32_IOS;

struct create_font_object_params32
{
    PTR32_IOS data;
    UINT64 size;
    ULONG index;
    PTR32_IOS object;
};
_Static_assert( offsetof(struct create_font_object_params32, size) == 8, "create32" );
_Static_assert( offsetof(struct create_font_object_params32, object) == 20, "create32" );
_Static_assert( sizeof(struct create_font_object_params32) == 24, "create32" );

static NTSTATUS ios_wow64_create_font_object( void *args )
{
    const struct create_font_object_params32 *params32 = args;
    struct create_font_object_params params =
    {
        iom_guest_ptr( params32->data ),
        params32->size,
        params32->index,
        iom_guest_ptr( params32->object ),   /* a UINT64 handle slot; the handle itself is not converted */
    };

    return create_font_object( &params );
}

struct dwrite_outline32_ios
{
    struct
    {
        PTR32_IOS values;
        ULONG count;
        ULONG size;
    } tags;
    struct
    {
        PTR32_IOS values;
        ULONG count;
        ULONG size;
    } points;
};
_Static_assert( sizeof(struct dwrite_outline32_ios) == 24, "outline32" );

struct get_glyph_outline_params32
{
    UINT64 object;
    ULONG simulations;
    ULONG glyph;
    float emsize;
    PTR32_IOS outline;
};
_Static_assert( offsetof(struct get_glyph_outline_params32, outline) == 20, "glyph_outline32" );
_Static_assert( sizeof(struct get_glyph_outline_params32) == 24, "glyph_outline32" );

static NTSTATUS ios_wow64_get_glyph_outline( void *args )
{
    const struct get_glyph_outline_params32 *params32 = args;
    struct dwrite_outline32_ios *outline32 = iom_guest_ptr( params32->outline );
    struct dwrite_outline outline;
    struct get_glyph_outline_params params;
    NTSTATUS status;

    if (!outline32) return STATUS_INVALID_PARAMETER;
    memset( &outline, 0, sizeof(outline) );
    outline.tags.values = iom_guest_ptr( outline32->tags.values );
    outline.tags.count = outline32->tags.count;
    outline.tags.size = outline32->tags.size;
    outline.points.values = iom_guest_ptr( outline32->points.values );
    outline.points.count = outline32->points.count;
    outline.points.size = outline32->points.size;

    params.object = params32->object;
    params.simulations = params32->simulations;
    params.glyph = params32->glyph;
    params.emsize = params32->emsize;
    params.outline = &outline;
    status = get_glyph_outline( &params );
    outline32->points.count = (ULONG)outline.points.count;
    outline32->tags.count = (ULONG)outline.tags.count;
    return status;
}

struct get_glyph_count_params32
{
    UINT64 object;
    PTR32_IOS count;
};
_Static_assert( sizeof(struct get_glyph_count_params32) == 16, "glyph_count32" );

static NTSTATUS ios_wow64_get_glyph_count( void *args )
{
    const struct get_glyph_count_params32 *params32 = args;
    struct get_glyph_count_params params = { params32->object, iom_guest_ptr( params32->count ) };

    return get_glyph_count( &params );
}

struct get_glyph_advance_params32
{
    UINT64 object;
    ULONG glyph;
    ULONG mode;
    float emsize;
    PTR32_IOS advance;
    PTR32_IOS has_contours;
};
_Static_assert( offsetof(struct get_glyph_advance_params32, advance) == 20, "glyph_advance32" );
_Static_assert( offsetof(struct get_glyph_advance_params32, has_contours) == 24, "glyph_advance32" );
_Static_assert( sizeof(struct get_glyph_advance_params32) == 32, "glyph_advance32" );

static NTSTATUS ios_wow64_get_glyph_advance( void *args )
{
    const struct get_glyph_advance_params32 *params32 = args;
    struct get_glyph_advance_params params =
    {
        params32->object,
        params32->glyph,
        params32->mode,
        params32->emsize,
        iom_guest_ptr( params32->advance ),
        iom_guest_ptr( params32->has_contours ),
    };

    return get_glyph_advance( &params );
}

struct get_glyph_bbox_params32
{
    UINT64 object;
    ULONG simulations;
    ULONG glyph;
    float emsize;
    MATRIX_2X2 m;
    PTR32_IOS bbox;
};
_Static_assert( offsetof(struct get_glyph_bbox_params32, m) == 20, "glyph_bbox32" );
_Static_assert( offsetof(struct get_glyph_bbox_params32, bbox) == 36, "glyph_bbox32" );
_Static_assert( sizeof(struct get_glyph_bbox_params32) == 40, "glyph_bbox32" );

static NTSTATUS ios_wow64_get_glyph_bbox( void *args )
{
    const struct get_glyph_bbox_params32 *params32 = args;
    struct get_glyph_bbox_params params =
    {
        params32->object,
        params32->simulations,
        params32->glyph,
        params32->emsize,
        params32->m,
        iom_guest_ptr( params32->bbox ),
    };

    return get_glyph_bbox( &params );
}

struct get_glyph_bitmap_params32
{
    UINT64 object;
    ULONG simulations;
    ULONG glyph;
    ULONG mode;
    float emsize;
    MATRIX_2X2 m;
    RECT bbox;
    int pitch;
    PTR32_IOS bitmap;
    PTR32_IOS is_1bpp;
};
_Static_assert( offsetof(struct get_glyph_bitmap_params32, m) == 24, "glyph_bitmap32" );
_Static_assert( offsetof(struct get_glyph_bitmap_params32, bbox) == 40, "glyph_bitmap32" );
_Static_assert( offsetof(struct get_glyph_bitmap_params32, bitmap) == 60, "glyph_bitmap32" );
_Static_assert( offsetof(struct get_glyph_bitmap_params32, is_1bpp) == 64, "glyph_bitmap32" );
_Static_assert( sizeof(struct get_glyph_bitmap_params32) == 72, "glyph_bitmap32" );

static NTSTATUS ios_wow64_get_glyph_bitmap( void *args )
{
    const struct get_glyph_bitmap_params32 *params32 = args;
    struct get_glyph_bitmap_params params =
    {
        params32->object,
        params32->simulations,
        params32->glyph,
        params32->mode,
        params32->emsize,
        params32->m,
        params32->bbox,
        params32->pitch,
        iom_guest_ptr( params32->bitmap ),
        iom_guest_ptr( params32->is_1bpp ),
    };

    return get_glyph_bitmap( &params );
}

struct get_design_glyph_metrics_params32
{
    UINT64 object;
    ULONG simulations;
    ULONG glyph;
    ULONG upem;
    ULONG ascent;
    PTR32_IOS metrics;
};
_Static_assert( offsetof(struct get_design_glyph_metrics_params32, metrics) == 24, "metrics32" );
_Static_assert( sizeof(struct get_design_glyph_metrics_params32) == 32, "metrics32" );

static NTSTATUS ios_wow64_get_design_glyph_metrics( void *args )
{
    const struct get_design_glyph_metrics_params32 *params32 = args;
    struct get_design_glyph_metrics_params params =
    {
        params32->object,
        params32->simulations,
        params32->glyph,
        params32->upem,
        params32->ascent,
        iom_guest_ptr( params32->metrics ),
    };

    return get_design_glyph_metrics( &params );
}

_Static_assert( sizeof(struct release_font_object_params) == 8, "release32 shares the 64-bit entry" );

const unixlib_entry_t dwrite_unix_call_wow64_funcs[] EXPORTED_TABLE =
{
    [unix_process_attach] = ios_process_attach,
    [unix_process_detach] = ios_process_detach,
    [unix_create_font_object] = ios_wow64_create_font_object,
    [unix_release_font_object] = release_font_object,
    [unix_get_glyph_outline] = ios_wow64_get_glyph_outline,
    [unix_get_glyph_count] = ios_wow64_get_glyph_count,
    [unix_get_glyph_advance] = ios_wow64_get_glyph_advance,
    [unix_get_glyph_bbox] = ios_wow64_get_glyph_bbox,
    [unix_get_glyph_bitmap] = ios_wow64_get_glyph_bitmap,
    [unix_get_design_glyph_metrics] = ios_wow64_get_design_glyph_metrics,
};
_Static_assert( sizeof(dwrite_unix_call_wow64_funcs) / sizeof(dwrite_unix_call_wow64_funcs[0]) == unix_funcs_count,
                "dwrite_unix_call_wow64_funcs" );
