// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
// Spec 19 A24: a global output struct whose first element is written only in part (rgb) and only
// in a branch for the second: their first values are read, so the global stays a constant
// (c0 and c1, count 2) and the program reads it.
struct Out { float4 c[3] : COLOR0; };
Out o;

Out main(float4 t : TEXCOORD0) : COLOR
{
    o.c[0].rgb = t.rgb;
    if (t.y > 0.5)
        o.c[1] = t;
    o.c[2] = t;
    return o;
}
