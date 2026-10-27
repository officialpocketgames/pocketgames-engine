// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Static consts stay compile-time constants (Microsoft: o0.x = 6).

static const float k = 3;

float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    const float c = k * 2;
    return float4(c, uv, 1);
}
