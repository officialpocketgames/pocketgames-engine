// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A20, ps_4_0: << and >> in #if and #elif (precedence between + and <, as in C), a macro using
// them in both an #if and the code, and the shift operators of the code itself left alone.

#define FLAG_FOG    (1 << 4)
#define FLAGS       (FLAG_FOG | 1)
#define HALF(x)     ((x) >> 1)

#if (FLAGS & (1 << 4)) && HALF(8) == 4 && 1 << 2 + 1 == 8 && 256 >> 4 > 15
static const float fog = 1.0;
#else
#error "shifts in #if"
#endif

#if 1 << 31 >> 31 != 1 || 1 << 32 != 0
#error "shift width"
#elif (3 << 1) == 6
static const float six = 6.0;
#else
#error "shift in #elif"
#endif

float4 main(uint4 v : TEXCOORD0) : SV_Target
{
    uint a = v.x << 2;
    uint b = HALF(v.y);

    a <<= 1;
    b >>= 1;
    return float4(a, b, fog, six);
}
