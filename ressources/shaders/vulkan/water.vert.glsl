#version 450

layout(location = 0) in uint aPackedPos;
layout(location = 1) in uint aPackedData;
layout(location = 2) in uvec2 aTexCoord;
layout(location = 3) in uint aPackedBiomeColor;

#include "frame_ubo.inc.glsl"

struct VoxelDrawData {
    ivec3 worldOrigin;
    uint flags;
};

layout(std430, set = 0, binding = 2) readonly buffer DrawDataTable {
    VoxelDrawData drawData[];
};

layout(location = 0) out vec3 vFragPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vTexCoord;
layout(location = 3) out vec4 vClipPos;
layout(location = 4) out float vViewDepth;
layout(location = 5) flat out vec3 vGeoNormal;
layout(location = 6) out float vSkyLight;
layout(location = 7) out vec3 vBlockLightRGB;

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
    vSkyLight = float((aPackedData >> 14u) & 0xFu) / 15.0;
    vBlockLightRGB = vec3(float((aPackedData >> 18u) & 0xFu),
                         float((aPackedData >> 22u) & 0xFu),
                         float((aPackedData >> 26u) & 0xFu)) / 15.0;
    vec3 baseN = NORMALS[normalIdx];

    vec2 texCoord = vec2(aTexCoord);
    // instanceCount=1 per draw: gl_InstanceIndex == firstInstance (see terrain.vert).
    ivec3 chunkOrigin = drawData[gl_InstanceIndex].worldOrigin;
    vec3 localPos = vec3(
        float(aPackedPos & 0x1FFu),
        float((aPackedPos >> 9u) & 0x3FFFu),
        float((aPackedPos >> 23u) & 0x1FFu)
    ) * (1.0 / 16.0);
    vec3 worldPos = vec3(chunkOrigin) + localPos;

    // Animate top faces primarily
    vec3 pos = worldPos;
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
    vGeoNormal = baseN;
    vTexCoord = texCoord + vec2(time * 0.02, time * 0.015);

    vec4 viewPos4 = frame.view * vec4(pos, 1.0);
    vViewDepth = -viewPos4.z;
    vClipPos = frame.projection * viewPos4;
    gl_Position = vClipPos;
}
