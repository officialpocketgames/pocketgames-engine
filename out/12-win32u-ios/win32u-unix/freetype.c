// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Wrapper around upstream dlls/win32u/freetype.c (Wine 11.4, LGPL-2.1-or-later). build-ios.sh
// keeps the upstream file unchanged as freetype_upstream.inc; this file compiles it with:
//
//  - Part A4: dlopen/dlsym/dlclose mapped to w32ios_freetype.c, which answers for the FreeType
//    linked into the app (opening a name that contains "freetype" succeeds, a symbol is looked
//    up in a table of every FreeType function freetype.c asks for);
//  - Part A5: CoreText's list of the system's fonts reported as unavailable, so only the
//    prefix's C:\windows\fonts and Wine's own font directory are scanned. (On iOS, CoreText
//    handed over a system font FreeType can't parse.)

#include <dlfcn.h>
#ifdef __APPLE__
#include <CoreText/CoreText.h>
#endif
#include "w32ios_freetype.h"

#define dlopen   w32ios_ft_dlopen
#define dlsym    w32ios_ft_dlsym
#define dlclose  w32ios_ft_dlclose

#ifdef __APPLE__
#define CTFontCollectionCreateFromAvailableFonts(options) ((void)(options), (CTFontCollectionRef)NULL)
#endif

#include "freetype_upstream.inc"
