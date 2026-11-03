// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
// spec 34 clarification 1, item 6: the geometry shader's colour
#version 450
layout(location = 0) in vec4 g_color;
layout(location = 0) out vec4 o_color;
void main()
{
    o_color = g_color;
}
