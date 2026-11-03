// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
// spec 34 clarification 1, item 6: a tiny triangle at the centre, for the geometry shader to grow
#version 450
void main()
{
    vec2 p[3] = vec2[](vec2(-0.05, -0.025), vec2(0.05, -0.025), vec2(0.0, 0.05));
    gl_Position = vec4(p[gl_VertexIndex], 0.5, 1.0);
}
