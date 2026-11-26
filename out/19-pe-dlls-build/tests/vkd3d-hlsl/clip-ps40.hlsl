// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// A16, ps_4_0: a pixel shader reading SV_ClipDistance0 as an input.

float4 main(float4 pos : SV_Position, float clip : SV_ClipDistance0) : SV_Target
{
    return float4(clip, clip * 0.5, 0.0, 1.0);
}
