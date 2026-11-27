// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
// Spec 19 A25: a shadow filter. SampleCmpLevelZero with offsets from an unrolled loop's counters
// and from a helper's literal arguments (inlined three times), and SampleCmp with the extreme
// offsets; the compare value is the interpolated depth, never a literal.
Texture2D shadowMap : register(t0);
SamplerComparisonState cmp : register(s0);

float tap(float2 uv, float z, int2 o)
{
    return shadowMap.SampleCmpLevelZero(cmp, uv, z, o);
}

float4 main(float4 pos : SV_Position, float3 uvz : TEXCOORD0) : SV_Target
{
    float s = 0;

    [unroll] for (int y = -1; y <= 1; y += 2)
        [unroll] for (int x = -1; x <= 1; x += 2)
            s += shadowMap.SampleCmpLevelZero(cmp, uvz.xy, uvz.z, int2(x, y));
    s += tap(uvz.xy, uvz.z, int2(-2, 0)) + tap(uvz.xy, uvz.z, int2(2, 0)) + tap(uvz.xy, uvz.z, int2(0, 3));
    s += shadowMap.SampleCmp(cmp, uvz.xy, uvz.z, int2(7, -8));
    return s / 8;
}
