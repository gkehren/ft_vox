#version 450

layout(location = 0) in uint aPackedPos;
layout(location = 1) in uint aPackedData;
layout(location = 2) in uvec2 aTexCoord;
layout(location = 3) in uint aPackedBiomeColor;

// Must match src/Renderer/FrameUBO.hpp (std140, sizeof 528)
#include "frame_ubo.inc.glsl"

// Must match materials::MaterialTableUBO — x=wind, y=emissive, z=ice, w=flags
layout(set = 0, binding = 1) uniform MaterialTable {
    vec4 mats[256];
} materialTable;

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
layout(location = 3) out float vTextureIndex;
layout(location = 4) out float vUseBiomeColor;
layout(location = 5) out vec3 vBiomeColor;
layout(location = 6) out float vAO;
layout(location = 7) out float vSkyLight;
layout(location = 8) out float vBlockLight;
layout(location = 9) out float vViewDepth;

const vec3 NORMALS[6] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(-1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, -1.0, 0.0),
    vec3(0.0, 0.0, 1.0),
    vec3(0.0, 0.0, -1.0)
);

#include "foliage_wind.inc.glsl"
#include "colorspace.inc.glsl"

void main()
{
    uint normalIdx = aPackedData & 0x7u;
    vNormal = NORMALS[normalIdx];

    // Integer extract — avoid float round-trip for texture type tests
    uint texIdx = (aPackedData >> 3u) & 0xFFu;
    vTextureIndex = float(texIdx);
    vUseBiomeColor = float((aPackedData >> 11u) & 0x1u);
    vAO = float((aPackedData >> 12u) & 0x3u) / 3.0;
    vSkyLight = float((aPackedData >> 14u) & 0xFu) / 15.0;
    vBlockLight = float((aPackedData >> 18u) & 0xFu) / 15.0;

    float r = float(aPackedBiomeColor & 0xFFu) / 255.0;
    float g = float((aPackedBiomeColor >> 8u) & 0xFFu) / 255.0;
    float b = float((aPackedBiomeColor >> 16u) & 0xFFu) / 255.0;
    // Biome tints are sRGB-authored (BiomeConfig colors are display-domain,
    // quantized to RGB8 like a texture): decode so the tint is linear-light
    // before terrain.frag mixes it with the linear-decoded albedo (issue #135).
    vBiomeColor = srgbToLinear(vec3(r, g, b));
    vTexCoord = vec2(aTexCoord);

    // Each voxel draw uses instanceCount=1, so gl_InstanceIndex equals
    // VkDrawIndexedIndirectCommand::firstInstance (requires the
    // drawIndirectFirstInstance device feature) and directly indexes the
    // per-frame VoxelDrawData table.
    ivec3 chunkOrigin = drawData[gl_InstanceIndex].worldOrigin;
    vec3 localPos = vec3(
        float(aPackedPos & 0x1FFu),
        float((aPackedPos >> 9u) & 0x3FFFu),
        float((aPackedPos >> 23u) & 0x1FFu)
    ) * (1.0 / 16.0);
    vec3 worldPos = vec3(chunkOrigin) + localPos;

    float time = frame.skyParams.x;
    vec3 pos = applyFoliageWind(worldPos, texIdx, time, vTexCoord);
    vFragPos = pos;

    vec4 viewPos4 = frame.view * vec4(pos, 1.0);
    vViewDepth = -viewPos4.z;
    gl_Position = frame.projection * viewPos4;
}
