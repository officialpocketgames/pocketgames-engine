// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A15, vs_5_0: half in a cbuffer (half4x4, half4), vector<half, 3>, a typedef of half, mul, dot,
// lerp, multiplication and addition. float-vs50.hlsl is the same with float; same output.

typedef half3 hvec;

cbuffer constants
{
    half4x4 transform;
    half4 tint;
};

struct vs_out
{
    float4 pos : SV_Position;
    half4 color : COLOR0;
};

vs_out main(float4 pos : POSITION, float3 normal : NORMAL)
{
    vs_out o;
    vector<half, 3> n = (half3)normal;
    hvec up = hvec(0.0, 1.0, 0.0);
    half shade = dot(n, up) * 0.5 + 0.5;
    o.pos = mul((half4)pos, transform);
    o.color = lerp(tint, half4(shade, shade, shade, 1.0), shade) * tint + half4(0.0, 0.0, 0.0, 0.25);
    return o;
}
