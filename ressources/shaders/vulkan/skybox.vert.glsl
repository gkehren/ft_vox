#version 450
layout(location = 0) in vec3 aPos;

#include "frame_ubo.inc.glsl"

layout(location = 0) out vec3 vTexCoords;

void main()
{
    vTexCoords = aPos;
    mat4 viewNoTrans = mat4(mat3(frame.view));
    vec4 pos = frame.projection * viewNoTrans * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
