// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part E2: Wine threads never drain an autorelease pool (spec 05 B9), and MoltenVK
// autoreleases Metal objects (each frame's CAMetalDrawable first of all) in the calls the layer
// wraps. Each such driver call runs inside its own pool (Apple: Objective-C @autoreleasepool),
// drained when it returns. Built on iOS only; ios_vk_pool.c is the plain call elsewhere.

#include "ios_vk_internal.h"

void ios_vk_pooled( void (*fn)( void *ctx ), void *ctx )
{
    @autoreleasepool
    {
        fn( ctx );
    }
}
