// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A17, ps_4_0: a struct field named "pixel", written and read.

struct shade
{
    float4 pixel;
    float weight;
};

float4 main(float4 c : COLOR0) : SV_Target
{
    shade s;

    s.pixel = c * 2.0;
    s.weight = 0.5;
    return s.pixel * s.weight;
}
