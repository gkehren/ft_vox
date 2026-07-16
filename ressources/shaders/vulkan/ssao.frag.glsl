#version 450
// Half-res SSAO from depth — mild false-occlusion controls for outdoor look.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D depthBuffer;

layout(push_constant) uniform PC {
    vec4 p0; // xy = inv resolution (full-res), z = radius, w = bias
    vec4 p1; // x = intensity, y = near, z = far, w = unused
    mat4 invProj;
} pc;

float linearDepth(float d)
{
    float n = pc.p1.y;
    float f = pc.p1.z;
    float z = d;
    return (n * f) / max(f - z * (f - n), 1e-5);
}

vec3 viewPosFromDepth(vec2 uv, float depth)
{
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = pc.invProj * clip;
    return view.xyz / max(view.w, 1e-5);
}

void main()
{
    float depth = texture(depthBuffer, vUV).r;
    // Sky / far plane: no occlusion (prevents milky veil on sky and distant haze)
    if (depth >= 0.9990)
    {
        outColor = vec4(1.0);
        return;
    }

    vec3 pos = viewPosFromDepth(vUV, depth);
    // Reject invalid reconstructions (behind camera / explosion)
    if (pos.z > -0.05 || any(isnan(pos)))
    {
        outColor = vec4(1.0);
        return;
    }

    vec2 texel = pc.p0.xy;
    float dR = texture(depthBuffer, vUV + vec2(texel.x, 0.0)).r;
    float dU = texture(depthBuffer, vUV + vec2(0.0, texel.y)).r;
    // Skip AO on depth discontinuities (foliage silhouettes, cliff edges)
    if (abs(dR - depth) > 0.02 || abs(dU - depth) > 0.02)
    {
        outColor = vec4(1.0);
        return;
    }

    vec3 pR = viewPosFromDepth(vUV + vec2(texel.x, 0.0), dR);
    vec3 pU = viewPosFromDepth(vUV + vec2(0.0, texel.y), dU);
    vec3 N = normalize(cross(pR - pos, pU - pos));

    float radius = pc.p0.z;
    float bias = pc.p0.w;
    // Milder intensity: composite also clamps; keep room without full-frame grey
    float intensity = min(pc.p1.x, 0.85);

    // Fewer taps + smaller effective radius → less soft veil on open slopes
    const vec3 kKernel[8] = vec3[](
        vec3( 0.15,  0.22, 0.55), vec3(-0.28,  0.12, 0.62),
        vec3( 0.05, -0.31, 0.48), vec3( 0.33,  0.08, 0.71),
        vec3(-0.18, -0.24, 0.58), vec3( 0.41, -0.19, 0.44),
        vec3(-0.39,  0.27, 0.52), vec3( 0.11,  0.42, 0.39)
    );

    float noise = fract(sin(dot(vUV * 800.0, vec2(12.9898, 78.233))) * 43758.5453);
    float a = noise * 6.2831853;
    mat2 rot = mat2(cos(a), -sin(a), sin(a), cos(a));

    float occlusion = 0.0;
    float posLin = linearDepth(depth);
    for (int i = 0; i < 8; ++i)
    {
        vec3 samp = kKernel[i];
        samp.xy = rot * samp.xy;
        float invZ = 1.0 / max(-pos.z, 0.1);
        vec2 offset = samp.xy * radius * invZ * 0.35;
        vec2 sampleUV = clamp(vUV + offset, vec2(0.0), vec2(1.0));
        float sampleDepth = texture(depthBuffer, sampleUV).r;
        if (sampleDepth >= 0.9990)
            continue; // sky sample does not occlude
        float sampleLin = linearDepth(sampleDepth);
        float rangeCheck = smoothstep(0.0, 1.0, radius / max(abs(posLin - sampleLin), 1e-3));
        occlusion += (sampleLin >= posLin + bias ? 0.0 : 1.0) * rangeCheck;
    }
    // Lift AO floor so open slopes never go full grey (min ~0.55 at intensity 1)
    float ao = 1.0 - (occlusion / 8.0) * intensity * 0.65;
    ao = mix(0.55, 1.0, clamp(ao, 0.0, 1.0));
    outColor = vec4(vec3(ao), 1.0);
}
