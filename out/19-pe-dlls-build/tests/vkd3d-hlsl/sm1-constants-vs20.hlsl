// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A19, vs_2_0: the constant table of a typical Direct3D 9 vertex shader: a reserved light
// direction, a column-major matrix, an array of bone matrices read with a relative index, a
// vector with a default value, and a declared but unused uniform (left out of the table).

float4x4 WorldViewProj;
float4 LightDir : register(c20);
float4x3 Bones[8];
float4 Tint = {1, 0.5, 0.25, 1};
float4 Unused;

struct VSO { float4 pos : POSITION; float4 col : COLOR0; };

VSO main(float4 p : POSITION, float3 n : NORMAL, float4 idx : BLENDINDICES)
{
    VSO o;
    float3 w = mul(p, Bones[idx.x]);

    o.pos = mul(float4(w, 1), WorldViewProj);
    o.col = Tint * saturate(dot(n, LightDir.xyz));
    return o;
}
