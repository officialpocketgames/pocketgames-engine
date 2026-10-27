// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// A const local initialised from a local variable (Microsoft: mov o0.x, v0.x).

float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    float a = uv.x;
    const float c = a;
    return float4(c, 0, 0, 1);
}
