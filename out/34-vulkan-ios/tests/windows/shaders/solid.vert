// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
// spec 34 M6, vktest: the smoke tests' vertex shader (one triangle, no vertex buffer)
#version 450
void main()
{
    vec2 p[3] = vec2[](vec2(-0.5, 0.5), vec2(0.5, 0.5), vec2(0.0, -0.5));
    gl_Position = vec4(p[gl_VertexIndex], 0.5, 1.0);
}
