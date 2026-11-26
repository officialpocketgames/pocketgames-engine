// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A15, ps_2_0: half below Shader Model 4 is left alone; the output must be what Wine's
// unchanged vkd3d 1.19 gives.

float4 main(float4 c : COLOR0) : COLOR
{
    half4 h = (half4)c;
    half k = dot(h.rgb, half3(0.3, 0.6, 0.1));
    return lerp(h * h, half4(k, k, k, 1.0), 0.5) + half4(0.5, 0.5, 0.5, 0.0);
}
