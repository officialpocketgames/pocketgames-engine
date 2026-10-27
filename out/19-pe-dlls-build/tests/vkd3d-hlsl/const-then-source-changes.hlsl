// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The const keeps the value its source had at the declaration (Microsoft: o0.x = 3, o0.y = v0.x).

float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    float k = 3;
    const float c = k;
    k = uv.x;
    return float4(c, k, 0, 1);
}
