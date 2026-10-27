// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// A const local initialised from a static that the shader changed first (Microsoft:
// mov o0.xy, v0.xyxx). Upstream vkd3d (2.1 too) still initialises it at program start, to 0;
// A14 counts a static as a compile-time constant only when it is also const.

static float2 g = {0, 0};

float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    g = uv;
    const float2 c = g;
    return float4(c, 0, 1);
}
