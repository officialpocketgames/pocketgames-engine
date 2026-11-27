// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
// Spec 19 A24: a non-static global written only in a function the entry function calls and read
// only in the entry function. Correct: the output is t * 2 + t, and the global is no constant.
float4 g;

void set(float4 v)
{
    g = v * 2;
}

float4 main(float4 t : TEXCOORD0) : COLOR
{
    set(t);
    return g + t;
}
