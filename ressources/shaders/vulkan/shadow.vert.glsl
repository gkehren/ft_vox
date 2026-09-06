#version 450
// Depth-only shadow pass — cascade matrix + time via push constant (wind match).

layout(location = 0) in uint aPackedPos;
layout(location = 1) in uint aPackedData;
layout(location = 2) in uvec2 aTexCoord;
layout(location = 3) in uint aPackedBiomeColor;

layout(push_constant) uniform PC {
    mat4 lightSpace;
    float time;
    float pad0;
    float pad1;
    float pad2;
} pc;

layout(set = 0, binding = 1) uniform MaterialTable { vec4 mats[256]; } materialTable;

struct VoxelDrawData {
    ivec3 worldOrigin;
    uint flags;
};

layout(std430, set = 0, binding = 2) readonly buffer DrawDataTable {
    VoxelDrawData drawData[];
};

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) flat out uint vTextureIndex;
#include "foliage_wind.inc.glsl"

void main()
{
    uint texIdx = (aPackedData >> 3u) & 0xFFu;
    vec2 texCoord = vec2(aTexCoord);

    ivec3 chunkOrigin = drawData[gl_InstanceIndex].worldOrigin;
    vec3 localPos = vec3(
        float(aPackedPos & 0x1FFu),
        float((aPackedPos >> 9u) & 0x3FFFu),
        float((aPackedPos >> 23u) & 0x1FFu)
    ) * (1.0 / 16.0);
    vec3 worldPos = vec3(chunkOrigin) + localPos;

    vec3 pos = applyFoliageWind(worldPos, texIdx, pc.time, texCoord);
    vTexCoord = texCoord;
    vTextureIndex = texIdx;
    gl_Position = pc.lightSpace * vec4(pos, 1.0);
}
