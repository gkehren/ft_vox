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
layout(location = 2) out vec2 vTexCoord;
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

    // Geometrically flat water: no vertex displacement, no per-vertex normal
    // spread. Greedy rectangles of different sizes shared the same vertices
    // only along their edges, so the old displacement broke across rectangle
    // boundaries and different-sized rectangles deformed as different waves.
    // All motion is fragment-level now (see water.frag.glsl), keyed on world
    // position so every rectangle evaluates identically at the same place.
    vec3 worldPos = vec3(chunkOrigin) + localPos;
    vFragPos = worldPos;
    vGeoNormal = baseN;
    vTexCoord = texCoord;

    gl_Position = frame.projection * frame.view * vec4(worldPos, 1.0);
}
