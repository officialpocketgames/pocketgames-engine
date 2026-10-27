// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// An array size from a static that is not const: Microsoft refuses it (X3058, "array dimensions
// must be literal scalar expressions"), and with A14 so does vkd3d.

static int n = 2;

float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    float a[n];
    a[0] = uv.x;
    a[1] = uv.y;
    return float4(a[0], a[1], 0, 1);
}
