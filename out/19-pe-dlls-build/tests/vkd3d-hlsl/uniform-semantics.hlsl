// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
// Spec 19 A24: uniforms declared with a semantic and a register keep the register and are in the
// constant table; a uniform with neither takes the first free register, as the global output
// struct (written before it is read) takes none.
shared uniform float4 fogColor : FOGCOLOR : register(c0);
shared uniform float4 fogParams : FOGPARAMS : register(c1);
uniform float4 tint : TINT : register(c5);
uniform float4 scale;

struct VIn { float4 tc : TEXCOORD0; float fog : TEXCOORD1; };
struct POut { float4 color[3] : COLOR0; };
VIn vin;
POut pout;
sampler2D tex : register(s0);

void fog()
{
    pout.color[0].rgb = lerp(fogColor.rgb, pout.color[0].rgb, saturate(vin.fog * fogParams.x));
}

POut main(VIn i)
{
    vin = i;
    pout.color[0] = tex2D(tex, vin.tc.xy) * tint * scale;
    fog();
    pout.color[1] = vin.tc;
    pout.color[2] = vin.tc.z;
    return pout;
}
