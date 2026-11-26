// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A16, vs_4_0: eight clip distances as two float4 outputs, SV_ClipDistance0 and
// SV_ClipDistance1, written as out parameters.

cbuffer planes : register(b0)
{
    float4 planes[8];
};

void main(float4 p : POSITION, out float4 pos : SV_Position,
        out float4 clip0 : SV_ClipDistance0, out float4 clip1 : SV_ClipDistance1)
{
    pos = p;
    clip0 = float4(dot(p, planes[0]), dot(p, planes[1]), dot(p, planes[2]), dot(p, planes[3]));
    clip1 = float4(dot(p, planes[4]), dot(p, planes[5]), dot(p, planes[6]), dot(p, planes[7]));
}
