#version 460 core
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D sourceBuffer;
uniform vec2 sunScreenPos;     // Sun position in screen space [0,1]
uniform float density   = 1.0;
uniform float weight    = 0.01;
uniform float decay     = 0.97;
uniform float godRaysExposure = 0.3;
uniform float sunVisibility = 1.0;
uniform float time = 0.0;
uniform bool dynamicBoostEnabled = true;
uniform bool boostPreview = false;
uniform float dramaticBoost = 2.4;

// P3: Compile-time constant enables GPU loop unrolling
#define NUM_SAMPLES 64

void main()
{
    if (sunVisibility <= 0.001)
    {
        FragColor = vec4(0.0);
        return;
    }

    vec2 radialOffset = TexCoord - sunScreenPos;
    float radialDistance = length(radialOffset);
    float rayReach = mix(0.18, 1.20, clamp(density / 3.0, 0.0, 1.0));
    float slowVariation = 0.5 + 0.5 * sin(time * 0.11 + sin(time * 0.037) * 2.0);
    float dramaticWindow = smoothstep(0.64, 0.94, slowVariation);
    if (boostPreview)
        dramaticWindow = 1.0;

    float boost = dynamicBoostEnabled ? mix(1.0, dramaticBoost, dramaticWindow) : 1.0;
    float horizonBoost = mix(1.35, 1.0, smoothstep(0.22, 1.0, sunVisibility));
    rayReach *= mix(1.0, 1.35, dramaticWindow);
    float distanceFade = 1.0 - smoothstep(rayReach * 0.72, rayReach, radialDistance);

    // Always reach the solar source. Density controls the visible reach of
    // the shafts instead of stopping sampling before the sun is reached.
    vec2 deltaUV = radialOffset / float(NUM_SAMPLES - 1);

    vec2 sampleUV = TexCoord;
    float illumination = 1.0;
    vec3 result = vec3(0.0);

    for (int i = 0; i < NUM_SAMPLES; ++i)
    {
        sampleUV -= deltaUV;

        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0)
            continue;

        vec3 sampleColor = texture(sourceBuffer, sampleUV).rgb;
        float progress = float(i + 1) / float(NUM_SAMPLES);
        float sourceBias = mix(0.35, 1.0, progress);
        result += sampleColor * illumination * sourceBias * weight;
        illumination *= decay;
    }

    FragColor = vec4(result * distanceFade * godRaysExposure * sunVisibility * boost * horizonBoost, 1.0);
}
