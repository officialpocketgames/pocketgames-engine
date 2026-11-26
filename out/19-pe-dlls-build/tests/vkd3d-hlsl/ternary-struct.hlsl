// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A20: "?:" whose operands are structs selects the whole struct (compiled for ps_4_0 and ps_2_0).

struct light
{
    float4 color;
    float3 dir;
    float power;
};

light day, night;

float4 main(float3 n : TEXCOORD0, float t : TEXCOORD1) : COLOR
{
    light l = t > 0.5 ? day : night;

    return l.color * saturate(dot(n, l.dir)) * l.power;
}
