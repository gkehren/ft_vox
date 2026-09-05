#version 450
// Depth-only shadow pass — cascade matrix + time via push constant (wind match).

layout(location = 0) in vec3 aPos;
layout(location = 1) in uint aPackedData;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in uint aPackedBiomeColor;

layout(push_constant) uniform PC {
    mat4 lightSpace;
    float time;
    float pad0;
    float pad1;
    float pad2;
} pc;

layout(set = 0, binding = 1) uniform MaterialTable { vec4 mats[256]; } materialTable;
layout(location = 0) out vec2 vTexCoord;
layout(location = 1) flat out uint vTextureIndex;
#include "foliage_wind.inc.glsl"

void main()
{
    uint texIdx = (aPackedData >> 3u) & 0xFFu;
    vec3 pos = applyFoliageWind(aPos, texIdx, pc.time, aTexCoord);
    vTexCoord = aTexCoord;
    vTextureIndex = texIdx;
    gl_Position = pc.lightSpace * vec4(pos, 1.0);
}
