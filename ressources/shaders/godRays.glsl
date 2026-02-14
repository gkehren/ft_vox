#version 460 core
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D hdrBuffer;
uniform vec2 sunScreenPos;     // Sun position in screen space [0,1]
uniform float density   = 1.0;
uniform float weight    = 0.01;
uniform float decay     = 0.97;
uniform float godRaysExposure = 0.3;

// P3: Compile-time constant enables GPU loop unrolling
#define NUM_SAMPLES 64

// S4: Constants for readability
const vec3 LUMINANCE_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);
const float THRESHOLD_LOWER = 0.8;
const float THRESHOLD_UPPER = 1.5;

void main()
{
    vec2 deltaUV = (TexCoord - sunScreenPos);
    deltaUV *= (1.0 / float(NUM_SAMPLES)) * density;

    vec2 sampleUV = TexCoord;
    float illumination = 1.0;
    vec3 result = vec3(0.0);

    for (int i = 0; i < NUM_SAMPLES; ++i)
    {
        sampleUV -= deltaUV;

        // Clamp to valid UV range to avoid sampling outside the screen
        vec2 clampedUV = clamp(sampleUV, vec2(0.001), vec2(0.999));

        vec3 sampleColor = texture(hdrBuffer, clampedUV).rgb;

        // Extract bright parts for light shafts (threshold)
        float brightness = dot(sampleColor, LUMINANCE_WEIGHTS);
        float brightMask = smoothstep(THRESHOLD_LOWER, THRESHOLD_UPPER, brightness);

        result += sampleColor * brightMask * illumination * weight;
        illumination *= decay;
    }

    FragColor = vec4(result * godRaysExposure, 1.0);
}
