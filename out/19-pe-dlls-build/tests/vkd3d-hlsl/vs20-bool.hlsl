// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A22, vs_2_0: static flow control exists (if bN) but break doesn't, so a bool reserved in b2
// goes in b2 while the rest compiles as before.

bool bSkin : register(b2);
float4x4 wvp;
float4 offset;

float4 main(float4 p : POSITION) : POSITION
{
    float4 q = p;

    if (bSkin)
        q += offset;
    return mul(q, wvp);
}
