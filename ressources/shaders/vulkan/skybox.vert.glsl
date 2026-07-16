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
    vec4 tier1Params;
    vec4 waterParams;
    vec4 postParams0;
    vec4 postParams1;
    vec4 postParams2;
    vec4 postParams3;
} frame;

layout(location = 0) out vec3 vTexCoords;

void main()
{
    vTexCoords = aPos;
    mat4 viewNoTrans = mat4(mat3(frame.view));
    vec4 pos = frame.projection * viewNoTrans * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
