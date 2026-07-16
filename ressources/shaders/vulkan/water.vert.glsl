#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in uint aPackedData;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in uint aPackedBiomeColor;

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

layout(location = 0) out vec3 vFragPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vTexCoord;
layout(location = 3) out vec4 vClipPos;
layout(location = 4) out float vViewDepth;

const vec3 NORMALS[6] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(-1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, -1.0, 0.0),
    vec3(0.0, 0.0, 1.0),
    vec3(0.0, 0.0, -1.0)
);

void main()
{
    float time = frame.skyParams.x;
    float wave = frame.waterParams.x;

    uint normalIdx = aPackedData & 0x7u;
    vec3 baseN = NORMALS[normalIdx];

    // Animate top faces primarily
    vec3 pos = aPos;
    float top = step(0.9, baseN.y);
    float w1 = sin(pos.x * 0.35 + time * 1.6) * cos(pos.z * 0.28 + time * 1.1);
    float w2 = sin(pos.x * 0.12 + pos.z * 0.18 + time * 0.7);
    pos.y += top * wave * (w1 * 0.55 + w2 * 0.35);

    // Perturb normal for specular / Fresnel
    vec3 n = baseN;
    if (top > 0.5) {
        n.x += wave * 2.2 * cos(pos.x * 0.35 + time * 1.6);
        n.z += wave * 2.2 * (-sin(pos.z * 0.28 + time * 1.1));
        n = normalize(n);
    }

    vFragPos = pos;
    vNormal = n;
    vTexCoord = aTexCoord + vec2(time * 0.02, time * 0.015);

    vec4 viewPos4 = frame.view * vec4(pos, 1.0);
    vViewDepth = -viewPos4.z;
    vClipPos = frame.projection * viewPos4;
    gl_Position = vClipPos;
}
