#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D image;
layout(push_constant) uniform PC {
    vec4 data; // xy = texelSize, z = horizontal (0/1), w = unused
} pc;

const float offsets[3] = float[](0.0, 1.38461538, 3.23076923);
const float weights[3] = float[](0.22702702, 0.31621622, 0.07027027);

void main()
{
    vec2 texelSize = pc.data.xy;
    bool horizontal = pc.data.z > 0.5;
    vec3 result = texture(image, vUV).rgb * weights[0];
    if (horizontal)
    {
        for (int i = 1; i < 3; ++i)
        {
            result += texture(image, vUV + vec2(texelSize.x * offsets[i], 0.0)).rgb * weights[i];
            result += texture(image, vUV - vec2(texelSize.x * offsets[i], 0.0)).rgb * weights[i];
        }
    }
    else
    {
        for (int i = 1; i < 3; ++i)
        {
            result += texture(image, vUV + vec2(0.0, texelSize.y * offsets[i])).rgb * weights[i];
            result += texture(image, vUV - vec2(0.0, texelSize.y * offsets[i])).rgb * weights[i];
        }
    }
    outColor = vec4(result, 1.0);
}
