// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
// spec 34 M6, vktest: three faces show the BC1 texture, three the BC7 one
#version 450
layout(set = 0, binding = 0) uniform sampler2D tex_bc1;
layout(set = 0, binding = 1) uniform sampler2D tex_bc7;
layout(location = 0) in vec2 uv;
layout(location = 1) flat in float tex;
layout(location = 0) out vec4 color;
void main()
{
    color = tex < 0.5 ? texture(tex_bc1, uv) : texture(tex_bc7, uv);
}
