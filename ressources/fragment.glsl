#version 410 core
out vec4 FragColor;

in vec2 texCoords;

uniform sampler2D textureSampler;

void main()
{
    FragColor = texture(textureSampler, texCoords);
}
