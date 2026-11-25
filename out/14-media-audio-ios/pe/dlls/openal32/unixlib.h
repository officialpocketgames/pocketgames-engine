// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 round 4: the unix call of openal32.dll. Every export goes through one entry with the
// function's index (openal32_funcs.h) and its arguments as 64-bit slots: integers and enums
// sign- or zero-extended, floats and doubles as their bits, pointers as addresses (a 32-bit
// caller's are guest addresses, converted on the unix side), device and context handles as
// the small ids the unix side hands out. The block has the same layout for 32- and 64-bit
// callers.

#ifndef OPENAL32_UNIXLIB_H
#define OPENAL32_UNIXLIB_H

#include <stdint.h>

#define OPENAL32_MAX_ARGS 6

struct openal32_call_params
{
    uint32_t func;                     /* enum openal32_func */
    uint32_t pad;
    uint64_t args[OPENAL32_MAX_ARGS];
    uint64_t ret;                      /* the return value's bits; a string call's needed size */
};

_Static_assert( sizeof(struct openal32_call_params) == 64, "openal32_call_params" );

enum openal32_unix_funcs
{
    unix_openal32_call,
    unix_openal32_funcs_count,
};

#endif /* OPENAL32_UNIXLIB_H */
