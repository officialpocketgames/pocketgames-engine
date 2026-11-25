// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 4: the real OpenAL Soft functions. The app links libopenal.1.dylib dynamically
// (never statically, LGPL relinking); these references are resolved by the dynamic linker.

#include "openal_api.h"

static const struct oa_api real_api =
{
#define OA(name, flags, ptrmask) name,
    OPENAL32_FUNCS
#undef OA
};

const struct oa_api *oa_api = &real_api;
