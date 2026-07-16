#version 450

layout(location = 0) in vec3 vFragPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 3) in vec4 vClipPos;
layout(location = 4) in float vViewDepth;

#include "frame_ubo.inc.glsl"

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;
layout(set = 1, binding = 1) uniform sampler2DArray shadowMap;
// Opaque scene history (color) + depth history (real depth, not color)
layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;

// invScreenSize.xy — matches framebuffer / gl_FragCoord (no manual Y flip)
layout(push_constant) uniform PC {
    vec2 invScreen;
    vec2 pad;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    float time = frame.skyParams.x;
    float refractionStr = frame.waterParams.y;
    float specularStr = frame.waterParams.z;
    float foamStr = frame.waterParams.w;
    float dayFactor = frame.skyParams.y;
    float nightFactor = frame.skyParams.w;
    float sunsetFactor = frame.skyParams.z;

    vec3 N = normalize(vNormal);
    vec3 V = normalize(frame.viewPos.xyz - vFragPos);
    vec3 L = normalize(frame.lightDirection.xyz);
    vec3 H = normalize(L + V);

    // gl_FragCoord is framebuffer space (matches the copied history image under negative viewport).
    // Do NOT reconstruct from clip + flip Y — that caused mirrored/grid refraction.
    vec2 screenUV = gl_FragCoord.xy * pc.invScreen;
    screenUV = clamp(screenUV, vec2(0.001), vec2(0.999));

    // Mild wave distortion only (keep small so shore stays stable)
    vec2 distort = N.xz * refractionStr * 0.65
                 + vec2(sin(vFragPos.x * 0.4 + time * 1.3),
                        cos(vFragPos.z * 0.35 + time * 1.1)) * refractionStr * 0.35;
    vec2 refrUV = clamp(screenUV + distort, vec2(0.001), vec2(0.999));

    vec3 scene = texture(sceneColor, refrUV).rgb;
    float opaqueDepth = texture(sceneDepth, screenUV).r;
    float waterDepth = gl_FragCoord.z;

    // Shore foam: real depth history vs water fragment depth (shallow = more foam)
    float foam = 0.0;
    float depthDelta = abs(opaqueDepth - waterDepth);
    foam = (1.0 - smoothstep(0.0, 0.025, depthDelta)) * foamStr * 0.85;
    foam = max(foam, (1.0 - abs(N.y)) * 0.18 * foamStr);
    float shoreNoise = sin(vFragPos.x * 2.1 + time * 0.3) * sin(vFragPos.z * 1.7 - time * 0.2);
    foam = max(foam, smoothstep(0.65, 0.95, shoreNoise * 0.5 + 0.5) * 0.12 * foamStr);
    foam = clamp(foam, 0.0, 1.0);

    // Richer teal (pale water was reading as washed white ocean)
    vec3 deep = vec3(0.04, 0.20, 0.38);
    vec3 shallow = vec3(0.10, 0.42, 0.55);
    float depthMix = clamp(1.0 - N.y * 0.35, 0.0, 1.0);
    vec3 waterAlbedo = mix(shallow, deep, depthMix);
    waterAlbedo = mix(waterAlbedo, waterAlbedo * vec3(1.15, 0.75, 0.45), sunsetFactor * 0.30);
    waterAlbedo = mix(waterAlbedo, waterAlbedo * 0.40, nightFactor * 0.55);

    float F0 = 0.06;
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - max(dot(N, V), 0.0), 5.0);

    // Stronger sun specular (tight highlight) + broader cool ice-like glint
    float NdotH = max(dot(N, H), 0.0);
    float sunSpec = pow(NdotH, 220.0) * specularStr * (0.55 + 0.85 * dayFactor) * 1.45;
    float iceSpec = pow(NdotH, 48.0) * specularStr * 0.38;
    vec3 sunColor = mix(vec3(1.0, 0.95, 0.82), vec3(1.0, 0.55, 0.22), sunsetFactor);
    vec3 iceTint = mix(vec3(0.72, 0.88, 1.05), sunColor * 0.85, dayFactor * 0.55);

    // Prefer water color over pale scene/fog samples
    vec3 refracted = mix(waterAlbedo * 1.15, scene * waterAlbedo, 0.28);
    refracted = mix(refracted, waterAlbedo, 0.35);

    vec3 color = mix(refracted, waterAlbedo * 1.12 + frame.fogColor.rgb * 0.06, fresnel * 0.50);
    color += sunColor * sunSpec;
    color += iceTint * iceSpec * (0.65 + 0.35 * nightFactor);
    color = mix(color, vec3(0.88, 0.93, 0.96), foam * 0.65);

    float dist = length(vFragPos - frame.viewPos.xyz);
    float fogStart = frame.fogParams.x;
    float fogEnd = frame.fogParams.y;
    float fogAmt = smoothstep(fogStart, max(fogStart + 1.0, fogEnd), dist) * 0.45;
    color = mix(color, frame.fogColor.rgb * 0.85 + waterAlbedo * 0.15, fogAmt);

    float alpha = mix(0.52, 0.78, fresnel);
    alpha = mix(alpha, 0.90, foam);
    outColor = vec4(color, alpha);
}
