#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in uint aPackedData;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in uint aPackedBiomeColor;

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
    vec4 sunDir;
    vec4 moonDir;
    vec4 skyParams;
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
layout(location = 7) out vec4 vFragPosLightSpace;

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

    float r = float(aPackedBiomeColor & 0xFFu) / 255.0;
    float g = float((aPackedBiomeColor >> 8) & 0xFFu) / 255.0;
    float b = float((aPackedBiomeColor >> 16) & 0xFFu) / 255.0;
    vBiomeColor = vec3(r, g, b);
    vTexCoord = aTexCoord;

    vFragPosLightSpace = frame.lightSpaceMatrix * vec4(aPos, 1.0);
    gl_Position = frame.projection * frame.view * vec4(aPos, 1.0);
}
