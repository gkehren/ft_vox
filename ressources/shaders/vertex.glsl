#version 460 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in float aTextureIndex;
layout (location = 4) in float aUseBiomeColor;
layout (location = 5) in vec3 aBiomeColor;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;
out float TextureIndex;
out float UseBiomeColor;
out vec3 BiomeColor;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main()
{
    FragPos = aPos;
    Normal = normalize(aNormal);
    TexCoord = aTexCoord;
    TextureIndex = aTextureIndex;
    UseBiomeColor = aUseBiomeColor;
    BiomeColor = aBiomeColor;

    gl_Position = projection * view * vec4(FragPos, 1.0);
}