// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Memory primitives of the iOS win32u that need Mach headers (w32ios_mach.c includes no Wine
// header: Mach's and Wine's types clash).

#ifndef W32IOS_MACH_H
#define W32IOS_MACH_H

#include <stddef.h>

/* the system page size */
size_t w32ios_page_size( void );
/* app-lifetime anonymous read/write memory, not known to ntdll's virtual memory layer */
void *w32ios_vm_alloc( size_t size );
/* a second mapping of [src, src + size) sharing the same pages. fixed = 0: anywhere, the address
 * goes to *out; fixed = 1: replacing whatever is mapped at *out. Returns 1 on success. */
int w32ios_vm_alias( void *src, size_t size, void **out, int fixed );

#endif /* W32IOS_MACH_H */
