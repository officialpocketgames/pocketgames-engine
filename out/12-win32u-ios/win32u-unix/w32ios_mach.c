// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// See w32ios_mach.h. Apple SDK: mmap(2), getpagesize(3), vm_remap (mach/vm_map.h).

#include <mach/mach.h>
#include <mach/vm_map.h>
#include <sys/mman.h>
#include <unistd.h>
#include "w32ios_mach.h"

size_t w32ios_page_size( void )
{
    return (size_t)getpagesize();
}

void *w32ios_vm_alloc( size_t size )
{
    void *ptr = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    return ptr == MAP_FAILED ? NULL : ptr;
}

int w32ios_vm_alias( void *src, size_t size, void **out, int fixed )
{
    vm_address_t target = fixed ? (vm_address_t)*out : 0;
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr;

    kr = vm_remap( mach_task_self(), &target, (vm_size_t)size, 0,
                   fixed ? (VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) : VM_FLAGS_ANYWHERE,
                   mach_task_self(), (vm_address_t)src, FALSE /* share, don't copy */,
                   &cur, &max, VM_INHERIT_NONE );
    if (kr != KERN_SUCCESS) return 0;
    *out = (void *)target;
    return 1;
}
