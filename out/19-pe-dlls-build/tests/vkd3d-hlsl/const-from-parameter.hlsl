// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// A const local initialised from a parameter (Microsoft: mul o0.xy, v0.xyxx, l(2, 2)).
// vkd3d 1.19 initialised it at program start and read an unwritten register.

float4 f(float2 p)
{
    const float2 c = p * 2;
    return float4(c, 0, 1);
}

float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    return f(uv);
}
