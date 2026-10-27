// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// As const-from-changed-static, for a static without an initialiser (Microsoft: mov o0.x, v0.x).

static float s;

float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    s = uv.x;
    const float c = s;
    return float4(c, 0, 0, 1);
}
