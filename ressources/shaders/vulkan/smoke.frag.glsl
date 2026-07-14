#version 450
// Minimal fragment — used only to exercise SPIR-V loading (PR2).
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(0.2, 0.4, 0.8, 1.0);
}
