// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
// spec 34 clarification 1, item 6: passes the triangle on and captures each vertex's value plus
// (100, 0, 0, 0) into transform feedback buffer 0, 16 bytes a vertex
#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec4 v_value[];
layout(location = 0) out vec4 g_color;
layout(location = 1, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 16) out vec4 g_captured;
void main()
{
    for (int i = 0; i < 3; i++)
    {
        gl_Position = gl_in[i].gl_Position;
        g_color = vec4(0.0, 1.0, 0.0, 1.0);
        g_captured = v_value[i] + vec4(100.0, 0.0, 0.0, 0.0);
        EmitVertex();
    }
    EndPrimitive();
}
