#version 460 core
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D image;
uniform bool horizontal;
uniform vec2 texelSize;

// Bilinear offsets and weights for 9-tap equivalent using 5 taps
const float offsets[3] = float[](0.0, 1.38461538, 3.23076923);
const float weights[3] = float[](0.22702702, 0.31621622, 0.07027027);

void main()
{
    vec3 result = texture(image, TexCoord).rgb * weights[0];

    if (horizontal)
    {
        for (int i = 1; i < 3; ++i)
        {
            result += texture(image, TexCoord + vec2(texelSize.x * offsets[i], 0.0)).rgb * weights[i];
            result += texture(image, TexCoord - vec2(texelSize.x * offsets[i], 0.0)).rgb * weights[i];
        }
    }
    else
    {
        for (int i = 1; i < 3; ++i)
        {
            result += texture(image, TexCoord + vec2(0.0, texelSize.y * offsets[i])).rgb * weights[i];
            result += texture(image, TexCoord - vec2(0.0, texelSize.y * offsets[i])).rgb * weights[i];
        }
    }

    FragColor = vec4(result, 1.0);
}
