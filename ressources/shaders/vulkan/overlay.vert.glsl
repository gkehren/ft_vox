#version 450
layout(location = 0) in vec3 aPos;

#include "frame_ubo.inc.glsl"

layout(push_constant) uniform PC {
    mat4 model;
    vec4 color;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    vColor = pc.color;
    gl_Position = frame.projection * frame.view * pc.model * vec4(aPos, 1.0);
}
