// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A17, ps_4_0 with the backwards-compatibility flag: Direct3D 9 style samplers read with the
// legacy intrinsics texCUBElod, tex2Dlod, tex2Dbias and tex2Dproj (each sampler binds a texture
// and a sampler state), and a COLOR output.

samplerCUBE env : register(s0);
sampler2D diffuse : register(s1);

float4 main(float3 n : TEXCOORD0, float4 uv : TEXCOORD1) : COLOR
{
    float4 r = texCUBElod(env, float4(n, 3.0));

    r += tex2Dlod(diffuse, float4(uv.xy, 0.0, 1.0));
    r += tex2Dbias(diffuse, float4(uv.xy, 0.0, -0.5));
    r += tex2Dproj(diffuse, uv);
    return r;
}
