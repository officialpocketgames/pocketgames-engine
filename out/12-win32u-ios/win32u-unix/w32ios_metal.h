// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// See w32ios_metal.c (no Wine types).

#ifndef W32IOS_METAL_H
#define W32IOS_METAL_H

#include <stddef.h>
#include <stdint.h>

/* MTLDevice.registryID of the system default device; 0 when there is none */
int w32ios_metal_registry_id( uint64_t *id );

#endif /* W32IOS_METAL_H */
