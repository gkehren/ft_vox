#version 460 core
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D image;
uniform bool horizontal;

// 9-tap Gaussian weights (sigma ~= 4.0)
const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main()
{
    vec2 texelSize = 1.0 / textureSize(image, 0);
    vec3 result = texture(image, TexCoord).rgb * weights[0];

    if (horizontal)
    {
        for (int i = 1; i < 5; ++i)
        {
            result += texture(image, TexCoord + vec2(texelSize.x * i, 0.0)).rgb * weights[i];
            result += texture(image, TexCoord - vec2(texelSize.x * i, 0.0)).rgb * weights[i];
        }
    }
    else
    {
        for (int i = 1; i < 5; ++i)
        {
            result += texture(image, TexCoord + vec2(0.0, texelSize.y * i)).rgb * weights[i];
            result += texture(image, TexCoord - vec2(0.0, texelSize.y * i)).rgb * weights[i];
        }
    }

    FragColor = vec4(result, 1.0);
}
