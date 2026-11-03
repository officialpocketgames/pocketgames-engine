// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part E2 on hosts without Objective-C: the pool helper is a plain call (ios_vk_pool.m
// is the iOS one).

#include "ios_vk_internal.h"

void ios_vk_pooled( void (*fn)( void *ctx ), void *ctx )
{
    fn( ctx );
}
