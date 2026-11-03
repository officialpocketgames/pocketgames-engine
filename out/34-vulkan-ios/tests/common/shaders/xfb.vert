// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
// spec 34 clarification 1, item 6: three vertices, each with the value (i, 2i, 3i, 1)
#version 450
layout(location = 0) out vec4 v_value;
void main()
{
    vec2 p[3] = vec2[](vec2(-0.5, 0.5), vec2(0.5, 0.5), vec2(0.0, -0.5));
    float i = float(gl_VertexIndex);
    gl_Position = vec4(p[gl_VertexIndex], 0.5, 1.0);
    v_value = vec4(i, 2.0 * i, 3.0 * i, 1.0);
}
