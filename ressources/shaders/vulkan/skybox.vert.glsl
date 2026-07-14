#version 450
layout(location = 0) in vec3 aPos;

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 viewPos;
    vec4 lightDirection;
    vec4 fogColor;
    vec4 fogParams;
    vec4 lightParams;
    vec4 visualParams;
    vec4 sunDir;      // xyz + pad
    vec4 moonDir;     // xyz + pad
    vec4 skyParams;   // x=time, y=day, z=sunset, w=night
    vec4 postParams0; // exposure, bloomIntensity, gamma, bloomThreshold
    vec4 postParams1; // sunScreen.xy, sunVisibility, flags (bit packed as float bits)
    vec4 postParams2; // godRays density, weight, decay, exposure
    vec4 postParams3; // dramaticBoost, toneMapper, bloomOn, fxaaOn (as floats)
} frame;

layout(location = 0) out vec3 vTexCoords;

void main()
{
    vTexCoords = aPos;
    mat4 viewNoTrans = mat4(mat3(frame.view));
    vec4 pos = frame.projection * viewNoTrans * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
