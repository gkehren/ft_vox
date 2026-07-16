#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in uint aPackedData;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in uint aPackedBiomeColor;

// Must match TerrainRenderer::FrameUBO (std140)
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 projection;
    mat4 cascadeMatrix0;
    mat4 cascadeMatrix1;
    mat4 cascadeMatrix2;
    vec4 viewPos;
    vec4 lightDirection;
    vec4 fogColor;
    vec4 fogParams;       // start, end, density, heightFalloff
    vec4 lightParams;
    vec4 visualParams;
    vec4 sunDir;
    vec4 moonDir;
    vec4 skyParams;       // time, day, sunset, night
    vec4 cascadeSplits;   // xyz split ends, w = count
    vec4 moonAmbient;     // rgb + strength
    vec4 tier1Params;     // x=blockLightScale, y=emissiveScale, z=fogBaseY, w=underwater
    vec4 waterParams;     // x=wave, y=refraction, z=specular, w=foam
    vec4 postParams0;
    vec4 postParams1;
    vec4 postParams2;
    vec4 postParams3;
} frame;

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

void main()
{
    vFragPos = aPos;

    uint normalIdx = aPackedData & 0x7u;
    vNormal = NORMALS[normalIdx];
    vTextureIndex = float((aPackedData >> 3) & 0xFFu);
    vUseBiomeColor = float((aPackedData >> 11) & 0x1u);
    vAO = float((aPackedData >> 12) & 0x3u) / 3.0;
    vSkyLight = float((aPackedData >> 14) & 0xFu) / 15.0;
    vBlockLight = float((aPackedData >> 18) & 0xFu) / 15.0;

    float r = float(aPackedBiomeColor & 0xFFu) / 255.0;
    float g = float((aPackedBiomeColor >> 8) & 0xFFu) / 255.0;
    float b = float((aPackedBiomeColor >> 16) & 0xFFu) / 255.0;
    vBiomeColor = vec3(r, g, b);
    vTexCoord = aTexCoord;

    vec4 viewPos4 = frame.view * vec4(aPos, 1.0);
    vViewDepth = -viewPos4.z;
    gl_Position = frame.projection * viewPos4;
}
