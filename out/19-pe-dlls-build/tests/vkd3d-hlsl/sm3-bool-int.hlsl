// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A22: bool and int uniforms in b and i registers (compiled for vs_3_0). bLight is reserved in
// b3 and switches a lighting term; n, reserved in i1, counts a loop; bFog, not annotated, is
// used only in a branch (next free b register, b0); bTint, not annotated, is used in arithmetic
// (stays a float c register); count2, not annotated, only bounds a loop (next free i register).

bool bLight : register(b3);
int n : register(i1);
bool bFog;
bool bTint;
int count2;
float4 lightColor;
float4 step;
float4x4 wvp;

struct vs_out
{
    float4 pos : POSITION;
    float4 col : COLOR0;
};

vs_out main(float4 p : POSITION, float3 normal : NORMAL)
{
    vs_out o;
    float4 acc = 0;
    int i;

    o.pos = mul(p, wvp);
    if (bLight)
    {
        acc = lightColor * saturate(dot(normal, float3(0, 1, 0)));
        acc.rgb = normalize(acc.rgb + 0.001) * length(acc.rgb);
    }
    for (i = 0; i < n; i++)
        acc += step;
    for (i = 0; i < count2; i++)
        acc *= 0.5;
    if (bFog)
    {
        acc.rgb = lerp(acc.rgb, float3(0.5, 0.5, 0.5), p.z * 0.01);
        acc.a = sqrt(acc.a);
    }
    o.col = acc + bTint * 0.25;
    return o;
}
