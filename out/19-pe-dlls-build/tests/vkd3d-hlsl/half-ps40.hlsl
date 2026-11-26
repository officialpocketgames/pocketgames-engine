// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A15, ps_4_0: half, half3 and half4 multiplied and added, through dot and lerp, returned.
// float-ps40.hlsl is the same shader with float; the output must be the same.

half4 main(float4 color : COLOR0, float3 normal : NORMAL0) : SV_Target
{
    half3 n = (half3)normal;
    half3 light = half3(0.25, 0.5, 0.75);
    half k = dot(n, light);
    half3 lit = (half3)color.rgb * k + light * 0.5;
    half4 mixed = lerp(half4(lit, 1.0), (half4)color, k);
    return mixed * 2.0 + half4(0.1, 0.1, 0.1, 0.0);
}
