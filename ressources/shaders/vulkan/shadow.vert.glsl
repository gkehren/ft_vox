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

// Shadow has no material UBO; match materials::infoFor(OAK_LEAVES) wind policy.
// OAK_LEAVES TextureType ordinal + wind strength from MaterialTable.hpp
const uint TEX_OAK_LEAVES = 8u;
const float kLeafWind = 0.14;

vec3 applyFoliageWind(vec3 pos, uint texIdx, float time)
{
    if (texIdx != TEX_OAK_LEAVES)
        return pos;

    const float wind = kLeafWind;
    float h = fract(sin(dot(pos.xz, vec2(12.9898, 78.233))) * 43758.5453);
    float phase = pos.x * 0.65 + pos.z * 0.55 + h * 6.2831853;
    float s = sin(time * 1.7 + phase) * 0.65 + sin(time * 2.35 + phase * 1.3) * 0.35;
    float c = cos(time * 1.4 + phase * 0.9);
    float heightBoost = 0.70 + 0.30 * clamp((pos.y - 60.0) / 40.0, 0.0, 1.0);
    pos.x += s * wind * heightBoost;
    pos.z += c * wind * 0.55 * heightBoost;
    return pos;
}

void main()
{
    uint texIdx = (aPackedData >> 3u) & 0xFFu;
    vec3 pos = applyFoliageWind(aPos, texIdx, pc.time);
    gl_Position = pc.lightSpace * vec4(pos, 1.0);
}
