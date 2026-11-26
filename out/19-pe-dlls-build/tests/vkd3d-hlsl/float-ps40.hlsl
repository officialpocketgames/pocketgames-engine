// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A15, ps_4_0: half-ps40.hlsl written with float (the reference output).

float4 main(float4 color : COLOR0, float3 normal : NORMAL0) : SV_Target
{
    float3 n = (float3)normal;
    float3 light = float3(0.25, 0.5, 0.75);
    float k = dot(n, light);
    float3 lit = (float3)color.rgb * k + light * 0.5;
    float4 mixed = lerp(float4(lit, 1.0), (float4)color, k);
    return mixed * 2.0 + float4(0.1, 0.1, 0.1, 0.0);
}
