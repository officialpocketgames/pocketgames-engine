// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A17, ps_4_0: texel offsets given as a static const int2, a constructor, unary minus and
// arithmetic of static consts, for Sample and Load.

Texture2D tex;
SamplerState smp;

static const int2 left_up = int2(-1, 1);
static const int R = 2;

float4 main(float2 uv : TEXCOORD0, float4 pos : SV_Position) : SV_Target
{
    float4 r = tex.Sample(smp, uv, left_up);

    r += tex.Sample(smp, uv, int2(-1, 1));
    r += tex.Sample(smp, uv, -int2(R, -R) + 1);
    r += tex.Load(int3(pos.xy, 0), left_up);
    r += tex.Load(int3(pos.xy, 0), int2(R * 3, -R * 4));
    return r;
}
