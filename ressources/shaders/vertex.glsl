#version 460 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in uint aPackedData;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in uint aPackedBiomeColor;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;
out float TextureIndex;
out float UseBiomeColor;
out vec3 BiomeColor;
out float AO;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

const vec3 NORMALS[6] = vec3[](
    vec3(1.0, 0.0, 0.0),  // 0: +X
    vec3(-1.0, 0.0, 0.0), // 1: -X
    vec3(0.0, 1.0, 0.0),  // 2: +Y
    vec3(0.0, -1.0, 0.0), // 3: -Y
    vec3(0.0, 0.0, 1.0),  // 4: +Z
    vec3(0.0, 0.0, -1.0)  // 5: -Z
);

void main()
{
    FragPos = aPos;
    
    // Unpack normal
    uint normalIdx = aPackedData & 0x7u;
    Normal = NORMALS[normalIdx];
    
    // Unpack texture index
    TextureIndex = float((aPackedData >> 3) & 0xFFu);
    
    // Unpack biome flag
    UseBiomeColor = float((aPackedData >> 11) & 0x1u);

    // Unpack AO
    AO = float((aPackedData >> 12) & 0x3u) / 3.0;
    
    // Unpack biome color
    float r = float(aPackedBiomeColor & 0xFFu) / 255.0;
    float g = float((aPackedBiomeColor >> 8) & 0xFFu) / 255.0;
    float b = float((aPackedBiomeColor >> 16) & 0xFFu) / 255.0;
    BiomeColor = vec3(r, g, b);

    TexCoord = aTexCoord;

    gl_Position = projection * view * vec4(FragPos, 1.0);
}