// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A16, vs_5_0: a float3 SV_CullDistance0 beside a float SV_ClipDistance0.

float4 plane;

struct vs_out
{
    float4 pos : SV_Position;
    float3 cull : SV_CullDistance0;
    float clip : SV_ClipDistance0;
};

vs_out main(float4 p : POSITION)
{
    vs_out o;

    o.pos = p;
    o.cull = p.xyz - 0.5;
    o.clip = dot(p, plane);
    return o;
}
