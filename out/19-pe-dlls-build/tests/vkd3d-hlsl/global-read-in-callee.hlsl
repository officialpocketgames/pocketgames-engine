// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
// Spec 19 A24: a non-static global written only in the entry function and read only in a function
// it calls. Correct: the output is t * 2, and the global is no constant.
float4 g;

float4 shade()
{
    return g * 2;
}

float4 main(float4 t : TEXCOORD0) : COLOR
{
    g = t;
    return shade();
}
