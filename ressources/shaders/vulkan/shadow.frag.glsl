#version 450
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) flat in uint vTextureIndex;
layout(set = 1, binding = 0) uniform sampler2DArray textureArray;
void main()
{
    if (texture(textureArray, vec3(vTexCoord, float(vTextureIndex))).a < 0.01)
        discard;
}
