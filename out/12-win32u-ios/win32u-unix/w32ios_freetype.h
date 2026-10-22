// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part A4: what upstream freetype.c gets for dlopen/dlsym/dlclose (see freetype.c).

#ifndef W32IOS_FREETYPE_H
#define W32IOS_FREETYPE_H

void *w32ios_ft_dlopen( const char *name, int flags );
void *w32ios_ft_dlsym( void *handle, const char *symbol );
int   w32ios_ft_dlclose( void *handle );

#endif /* W32IOS_FREETYPE_H */
