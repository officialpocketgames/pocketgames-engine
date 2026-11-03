// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part E4: the one Objective-C helper opengl32's generated wrappers call, so every
// opengl32 unix entry runs inside an autorelease pool. ANGLE's Metal work on a Windows thread
// otherwise autoreleases into a pool nobody drains. The call also holds the thread's share
// group while it runs, when its context shares lists with another (C3.1, pgl_internal.h).

#import <Foundation/Foundation.h>

#include "pgl_internal.h"

int32_t pgl_call_in_pool( int32_t (*fn)( void *args ), void *args )
{
    struct pgl_group *group = pgl_group_enter();
    int32_t status;

    @autoreleasepool
    {
        status = fn( args );
    }
    pgl_group_leave( group );
    return status;
}
