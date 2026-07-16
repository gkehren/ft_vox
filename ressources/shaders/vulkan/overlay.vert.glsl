#version 450
layout(location = 0) in vec3 aPos;

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 projection;
    mat4 cascadeMatrix0;
    mat4 cascadeMatrix1;
    mat4 cascadeMatrix2;
    vec4 viewPos;
    vec4 lightDirection;
    vec4 fogColor;
    vec4 fogParams;
    vec4 lightParams;
    vec4 visualParams;
    vec4 sunDir;
    vec4 moonDir;
    vec4 skyParams;
    vec4 cascadeSplits;
    vec4 moonAmbient;
    vec4 lightingParams;
    vec4 waterParams;
} frame;

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
