// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A19, ps_2_0 and ps_1_1 with the backwards-compatibility flag: samplers in declaration order
// and one reserved, a row-major matrix, and an if that ps_2_0 has to flatten (cmp).

sampler2D DiffuseMap;
sampler2D NormalMap;
sampler2D SpecMap : register(s5);
row_major float3x4 ColorMatrix;
float Threshold;

float4 main(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR
{
    float4 d = tex2D(DiffuseMap, uv) * tex2D(NormalMap, uv) + tex2D(SpecMap, uv);
    float4 r;

    if (d.a < Threshold)
        r = col;
    else
        r = float4(mul(ColorMatrix, d), 1);
    return r;
}
