// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A16, vs_4_0: a float SV_ClipDistance0 and a float2 SV_ClipDistance1, with a float
// TEXCOORD0 that must not be packed beside either of them.

cbuffer planes : register(b0)
{
    float4x4 mvp;
    float4 plane0, plane1, plane2;
};

struct vs_out
{
    float4 pos : SV_Position;
    float clip0 : SV_ClipDistance0;
    float2 clip1 : SV_ClipDistance1;
    float fog : TEXCOORD0;
};

vs_out main(float4 p : POSITION)
{
    vs_out o;

    o.pos = mul(p, mvp);
    o.clip0 = dot(p, plane0);
    o.clip1 = float2(dot(p, plane1), dot(p, plane2));
    o.fog = o.pos.z;
    return o;
}
