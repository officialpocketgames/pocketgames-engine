// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A22, pixel shaders: bool bLight : register(b3) (a b register for ps_2_a and ps_3_0; a float
// c register for ps_2_0, which has no static flow control), and two colour outputs, COLOR0 and
// COLOR1 (oC0 and oC1), as a deferred renderer writes colour and normal.

bool bLight : register(b3);
float4 lightColor;
sampler2D albedo;

struct ps_out
{
    float4 color : COLOR0;
    float4 normal : COLOR1;
};

ps_out main(float2 uv : TEXCOORD0, float3 n : TEXCOORD1)
{
    ps_out o;
    float4 c = tex2D(albedo, uv);

    o.color = c * (bLight ? lightColor : 0.0);
    o.normal = float4(n * 0.5 + 0.5, 1.0);
    return o;
}
