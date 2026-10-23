// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15: brings in interfaces/wine_unix_contract.h (Sections B and C: the controller store
// and its structures) from any translation unit. Inside Wine (server, ntdll) windef.h has
// already defined the handle and integer types the contract's prototypes name; anywhere else
// (the host tests) this header declares the minimal set the contract's own comment lists.
// Build with -I<repo>/interfaces.

#ifndef MYIOSDECK_IOS_HIDPAD_CONTRACT_H
#define MYIOSDECK_IOS_HIDPAD_CONTRACT_H

#include <stdint.h>

#ifndef _WINDEF_
typedef void *HWND;
typedef void *HCURSOR;
typedef unsigned int UINT;
typedef int INT;
typedef int BOOL;
typedef uint32_t DWORD;
typedef struct { int32_t left, top, right, bottom; } RECT;
#endif
struct window_rects;
struct window_surface;

#include "wine_unix_contract.h"

#endif
