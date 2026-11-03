// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
// spec 34 M6, vktest: the cube's vertex shader
#version 450
layout(push_constant) uniform Push { mat4 mvp; } pc;
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in float in_tex;
layout(location = 0) out vec2 uv;
layout(location = 1) flat out float tex;
void main()
{
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    uv = in_uv;
    tex = in_tex;
}
