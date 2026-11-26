// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A17, ps_4_0: arrays sized by static const int expressions (a cast, arithmetic) and an
// [unroll] count from one.

static const int TAPS = 3;
static const float SCALE = 2.0;

cbuffer weights
{
    float4 w[TAPS * 2 + 1];         // 7 registers
};

float4 main(float4 c : TEXCOORD0) : SV_Target
{
    float4 acc[(int)SCALE + 1];
    float4 r = 0;
    int i;

    [unroll(TAPS * 2 + 1)] for (i = 0; i < TAPS * 2 + 1; ++i)
        r += c * w[i];
    [unroll] for (i = 0; i < (int)SCALE + 1; ++i)
        acc[i] = r * i;
    return acc[2];
}
