// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part A4: FreeType is linked into the app statically (build-ios.sh builds
// refs/freetype-2.13.3 unchanged and merges it into libwin32u_unix.a). Upstream freetype.c loads
// FreeType with dlopen/dlsym; the wrapper freetype.c maps those two (and dlclose) to these.

#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H
#include FT_OUTLINE_H
#include FT_TRIGONOMETRY_H
#include FT_MODULE_H
#include FT_WINFONTS_H
#include FT_LCD_FILTER_H
#include "w32ios_freetype.h"

/* the address handed out as the library handle */
static char ft_token;

/* every function upstream freetype.c asks for (its MAKE_FUNCPTR / LOAD_FUNCPTR list) */
static const struct { const char *name; void *addr; } ft_symbols[] =
{
#define FT_SYM(f) { #f, (void *)f }
    FT_SYM( FT_Done_Face ),
    FT_SYM( FT_Get_Char_Index ),
    FT_SYM( FT_Get_First_Char ),
    FT_SYM( FT_Get_Next_Char ),
    FT_SYM( FT_Get_Sfnt_Name ),
    FT_SYM( FT_Get_Sfnt_Name_Count ),
    FT_SYM( FT_Get_Sfnt_Table ),
    FT_SYM( FT_Get_TrueType_Engine_Type ),
    FT_SYM( FT_Get_WinFNT_Header ),
    FT_SYM( FT_Init_FreeType ),
    FT_SYM( FT_Library_SetLcdFilter ),
    FT_SYM( FT_Library_Version ),
    FT_SYM( FT_Load_Glyph ),
    FT_SYM( FT_Load_Sfnt_Table ),
    FT_SYM( FT_Matrix_Multiply ),
    FT_SYM( FT_MulDiv ),
#ifndef FT_MULFIX_INLINED   /* same condition as freetype.c's LOAD_FUNCPTR( FT_MulFix ) */
    FT_SYM( FT_MulFix ),
#endif
    FT_SYM( FT_New_Face ),
    FT_SYM( FT_New_Memory_Face ),
    FT_SYM( FT_Outline_Embolden ),
    FT_SYM( FT_Outline_Get_Bitmap ),
    FT_SYM( FT_Outline_Get_CBox ),
    FT_SYM( FT_Outline_Transform ),
    FT_SYM( FT_Outline_Translate ),
    FT_SYM( FT_Property_Set ),
    FT_SYM( FT_Render_Glyph ),
    FT_SYM( FT_Set_Charmap ),
    FT_SYM( FT_Set_Pixel_Sizes ),
    FT_SYM( FT_Vector_Length ),
    FT_SYM( FT_Vector_Transform ),
    FT_SYM( FT_Vector_Unit ),
#undef FT_SYM
};

void *w32ios_ft_dlopen( const char *name, int flags )
{
    (void)flags;
    if (name && strstr( name, "freetype" )) return &ft_token;
    return NULL;
}

void *w32ios_ft_dlsym( void *handle, const char *symbol )
{
    size_t i;

    if (handle != &ft_token || !symbol) return NULL;
    for (i = 0; i < sizeof(ft_symbols) / sizeof(ft_symbols[0]); i++)
        if (!strcmp( ft_symbols[i].name, symbol )) return ft_symbols[i].addr;
    return NULL;
}

int w32ios_ft_dlclose( void *handle )
{
    (void)handle;
    return 0;
}
