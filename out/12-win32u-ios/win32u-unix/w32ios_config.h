// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Included first in every file of the iOS win32u (build-ios.sh: -include). Spec 12 Part A3/A4:
// the Wine build's configuration, then the optional libraries this build goes without
// (fontconfig, EGL, Vulkan, GnuTLS) undefined, so their code paths fall back as upstream does
// when the library is missing, and FreeType declared present: it is linked in statically and
// freetype.c reaches it through w32ios_freetype.c instead of dlopen.

#ifndef W32IOS_CONFIG_H
#define W32IOS_CONFIG_H

#include "config.h"

#undef SONAME_LIBFONTCONFIG
#undef HAVE_FONTCONFIG_FONTCONFIG_H
#undef SONAME_LIBEGL
#undef SONAME_LIBVULKAN
#undef SONAME_LIBMOLTENVK
#undef SONAME_LIBGNUTLS

#undef SONAME_LIBFREETYPE
#define SONAME_LIBFREETYPE "libfreetype.6.dylib"   /* never opened: see w32ios_freetype.c */
#undef HAVE_FT2BUILD_H
#define HAVE_FT2BUILD_H 1

#endif /* W32IOS_CONFIG_H */
