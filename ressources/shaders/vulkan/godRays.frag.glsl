#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D sourceBuffer;
// All floats — avoids int/float packing mismatch with C++
layout(push_constant) uniform PC {
    vec4 p0; // xy=sunScreen, z=density, w=weight
    vec4 p1; // x=decay, y=exposure, z=sunVisibility, w=time
    vec4 p2; // x=dramaticBoost, y=dynamicBoost, z=boostPreview, w=unused
} pc;

#define NUM_SAMPLES 64

void main()
{
    float sunVisibility = pc.p1.z;
    if (sunVisibility <= 0.001)
    {
        outColor = vec4(0.0);
        return;
    }

    vec2 sunScreenPos = pc.p0.xy;
    float density = pc.p0.z;
    float weight = pc.p0.w;
    float decay = pc.p1.x;
    float godRaysExposure = pc.p1.y;
    float time = pc.p1.w;
    float dramaticBoost = pc.p2.x;
    bool dynamicBoostEnabled = pc.p2.y > 0.5;
    bool boostPreview = pc.p2.z > 0.5;

    vec2 radialOffset = vUV - sunScreenPos;
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

    vec2 deltaUV = radialOffset / float(NUM_SAMPLES - 1);
    vec2 sampleUV = vUV;
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

    outColor = vec4(result * distanceFade * godRaysExposure * sunVisibility * boost * horizonBoost, 1.0);
}
