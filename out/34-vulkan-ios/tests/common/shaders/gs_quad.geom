// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
// spec 34 clarification 1, item 6: one triangle in, a green quad from -0.5 to 0.5 around its
// centre out (a strip of four vertices: two triangles)
#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) out vec4 g_color;
void main()
{
    vec4 c = (gl_in[0].gl_Position + gl_in[1].gl_Position + gl_in[2].gl_Position) / 3.0;
    vec2 corner[4] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(-0.5, 0.5), vec2(0.5, 0.5));
    for (int i = 0; i < 4; i++)
    {
        gl_Position = vec4(c.xy + corner[i], c.z, 1.0);
        g_color = vec4(0.0, 1.0, 0.0, 1.0);
        EmitVertex();
    }
    EndPrimitive();
}
