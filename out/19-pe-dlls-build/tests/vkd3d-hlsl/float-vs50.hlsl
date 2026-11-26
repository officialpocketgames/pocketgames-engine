// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A15, vs_5_0: half-vs50.hlsl written with float (the reference output).

typedef float3 hvec;

cbuffer constants
{
    float4x4 transform;
    float4 tint;
};

struct vs_out
{
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

vs_out main(float4 pos : POSITION, float3 normal : NORMAL)
{
    vs_out o;
    vector<float, 3> n = (float3)normal;
    hvec up = hvec(0.0, 1.0, 0.0);
    float shade = dot(n, up) * 0.5 + 0.5;
    o.pos = mul((float4)pos, transform);
    o.color = lerp(tint, float4(shade, shade, shade, 1.0), shade) * tint + float4(0.0, 0.0, 0.0, 0.25);
    return o;
}
