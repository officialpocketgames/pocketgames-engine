// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
// Spec 19 A24: a non-static global (an implicit uniform) written by the entry function and read and
// rewritten by a function it calls. Correct: o.c[0].rgb = t.rgb * t.x, o.c[0].a = t.a.
struct Out { float4 c[3] : COLOR0; };
Out o;
float4 g;
void stage() { o.c[0].rgb = lerp(float3(0, 0, 0), o.c[0].rgb, g.x); }
Out main(float4 t : TEXCOORD0) : COLOR { g = t; o.c[0] = t; stage(); o.c[1] = t; o.c[2] = t; return o; }
