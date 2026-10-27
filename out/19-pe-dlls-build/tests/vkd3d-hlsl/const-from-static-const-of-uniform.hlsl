// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// A static const initialised from a uniform (Microsoft: mad o0.x, cb0[0].x, l(2), l(1)).

float u;
static const float x = u * 2;

float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    const float c = x + 1;
    return float4(c, uv, 1);
}
