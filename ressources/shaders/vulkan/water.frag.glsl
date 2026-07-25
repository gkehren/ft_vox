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

// --- Value noise -----------------------------------------------------------
float whash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float wnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(whash(i), whash(i + vec2(1.0, 0.0)), f.x),
               mix(whash(i + vec2(0.0, 1.0)), whash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Animated multi-octave water heightfield (world XZ domain)
float waterHeight(vec2 p, float t) {
    float h = 0.0;
    h += wnoise(p * 0.30 + vec2(t * 0.10, t * 0.06)) * 0.55;
    h += wnoise(p * 0.85 + vec2(-t * 0.13, t * 0.11)) * 0.30;
    h += wnoise(p * 2.10 + vec2(t * 0.20, -t * 0.17)) * 0.15;
    return h;
}

// Fragment-level wave normal from finite differences of the heightfield
vec3 waveNormal(vec2 p, float t, float amp) {
    float e = 0.08;
    float hC = waterHeight(p, t);
    float hX = waterHeight(p + vec2(e, 0.0), t);
    float hZ = waterHeight(p + vec2(0.0, e), t);
    vec2 grad = vec2(hX - hC, hZ - hC) / e;
    return normalize(vec3(-grad.x * amp, 1.0, -grad.y * amp));
}

// Sky gradient identical to skybox.frag.glsl so reflections match the sky
vec3 skyReflection(vec3 R, float day, float sunset, float night) {
    float h = max(R.y, 0.0);
    vec3 zenith = vec3(0.06, 0.24, 0.68) * day
                + vec3(0.18, 0.08, 0.28) * sunset
                + vec3(0.002, 0.005, 0.013) * night;
    vec3 horizon = vec3(0.36, 0.62, 0.92) * day
                 + vec3(0.95, 0.35, 0.12) * sunset
                 + vec3(0.006, 0.010, 0.024) * night;
    horizon = mix(horizon, frame.fogColor.rgb, 0.12);
    float grad = pow(1.0 - h, 2.8);
    vec3 sky = mix(zenith, horizon, grad);

    // Concentrated twilight warmth toward the sun azimuth
    vec2 viewH = normalize(R.xz + vec2(0.0001));
    vec2 sunH = normalize(frame.sunDir.xz + vec2(0.0001));
    float horizonBand = pow(1.0 - h, 4.5);
    float sunsetFacing = pow(max(dot(viewH, sunH), 0.0), 3.5);
    sky += vec3(0.30, 0.09, 0.03) * sunset * horizonBand * pow(sunsetFacing, 1.4);
    // Soft daytime sun scatter near horizon
    sky += vec3(0.12, 0.18, 0.28) * day * pow(1.0 - h, 6.0) * 0.35;
    // Faint residual horizon glow toward the moon azimuth at night
    vec2 moonH = normalize(frame.moonDir.xz + vec2(0.0001));
    float moonFacing = pow(max(dot(viewH, moonH), 0.0), 4.0);
    sky += vec3(0.020, 0.030, 0.055) * night * horizonBand * moonFacing;
    return sky;
}

void main()
{
    float time = frame.skyParams.x;
    float waveStr = frame.waterParams.x;
    float refractionStr = frame.waterParams.y;
    float specularStr = frame.waterParams.z;
    float foamStr = frame.waterParams.w;
    float dayFactor = frame.skyParams.y;
    float sunsetFactor = frame.skyParams.z;
    float nightFactor = frame.skyParams.w;

    vec3 geoN = normalize(vNormal);
    vec3 V = normalize(frame.viewPos.xyz - vFragPos);

    // Fragment-level wave normals on top faces only (sides stay voxel-flat)
    float topMask = smoothstep(0.7, 0.95, geoN.y);
    vec3 N = geoN;
    if (topMask > 0.001) {
        vec3 wN = waveNormal(vFragPos.xz, time, waveStr * 4.0);
        N = normalize(mix(geoN, wN, topMask));
    }

    // Screen-space refraction of the opaque history
    vec2 screenUV = clamp(gl_FragCoord.xy * pc.invScreen, vec2(0.001), vec2(0.999));
    vec2 distort = N.xz * refractionStr * 2.0;
    vec2 refrUV = clamp(screenUV + distort, vec2(0.001), vec2(0.999));
    vec3 scene = texture(sceneColor, refrUV).rgb;

    // Water column: linearized opaque depth vs water view depth (GLM RH_ZO)
    float opaqueDepth = texture(sceneDepth, screenUV).r;
    float linOpaque = frame.projection[3][2] / (opaqueDepth + frame.projection[2][2]);
    float column = clamp(linOpaque - vViewDepth, 0.0, 64.0);

    // Beer-Lambert absorption: red dies first -> teal body
    vec3 sigma = vec3(0.42, 0.16, 0.10) * 0.35;
    vec3 absorb = exp(-column * sigma);
    float scatterAmt = 1.0 - exp(-column * 0.22);
    vec3 scatterColor = vec3(0.015, 0.14, 0.24);
    float scatterLight = dayFactor * 0.9 + sunsetFactor * 0.55 + 0.03;
    vec3 waterBody = scene * absorb + scatterColor * scatterAmt * scatterLight;

    // Fresnel + analytic sky reflection
    float F0 = 0.02;
    float fres = F0 + (1.0 - F0) * pow(1.0 - max(dot(N, V), 0.0), 5.0);
    vec3 R = reflect(-V, N);
    R.y = abs(R.y); // keep reflections above the horizon
    vec3 refl = skyReflection(R, dayFactor, sunsetFactor, nightFactor);

    vec3 color = mix(waterBody, refl, fres);

    // Sun glitter on the wave normals
    float sunVis = smoothstep(-0.04, 0.08, frame.sunDir.y);
    float RdotS = max(dot(R, frame.sunDir.xyz), 0.0);
    float sunLow = 1.0 - smoothstep(0.0, 0.35, frame.sunDir.y);
    vec3 sunTint = mix(vec3(1.0, 0.96, 0.72), vec3(1.0, 0.45, 0.12), sunLow * sunLow);
    float sunGlitter = pow(RdotS, 700.0) * specularStr * sunVis * (0.35 + 0.65 * dayFactor);
    float sunSheen = pow(RdotS, 64.0) * specularStr * sunVis * 0.08 * (0.3 + 0.7 * dayFactor);
    color += sunTint * (sunGlitter * 2.2 + sunSheen);

    // Moon glitter (cool tint, night only)
    float moonVis = smoothstep(0.02, 0.28, frame.moonDir.y);
    float RdotM = max(dot(R, frame.moonDir.xyz), 0.0);
    float moonGlitter = pow(RdotM, 700.0) * specularStr * moonVis * nightFactor;
    float moonSheen = pow(RdotM, 64.0) * specularStr * moonVis * nightFactor * 0.10;
    color += vec3(0.55, 0.68, 1.0) * (moonGlitter * 1.4 + moonSheen);

    // Foam: shore band from the real water column + wave-crest whitecaps
    float foamNoise = wnoise(vFragPos.xz * 1.8 + vec2(time * 0.35, -time * 0.25))
                    * wnoise(vFragPos.xz * 3.7 - vec2(time * 0.22, time * 0.30)) * 2.0;
    float shore = 1.0 - smoothstep(0.0, 1.6, column);
    float shoreFoam = shore * smoothstep(0.30, 0.72, foamNoise + shore * 0.25);
    float caps = smoothstep(0.80, 0.95, waterHeight(vFragPos.xz, time)) * 0.25 * topMask;
    float sideFoam = (1.0 - abs(geoN.y)) * 0.15;
    float foam = clamp((shoreFoam + caps + sideFoam) * foamStr, 0.0, 1.0);
    vec3 foamColor = vec3(0.88, 0.93, 0.96) * (0.22 + 0.78 * dayFactor);
    foamColor = mix(foamColor, vec3(1.0, 0.72, 0.50) * (0.25 + 0.75 * dayFactor), sunsetFactor * 0.45);
    foamColor *= 1.0 - nightFactor * 0.75;
    color = mix(color, foamColor, foam * 0.85);

    // Distance fog (same 0.45 cap as terrain)
    float dist = length(vFragPos - frame.viewPos.xyz);
    float fogAmt = smoothstep(frame.fogParams.x, max(frame.fogParams.x + 1.0, frame.fogParams.y), dist) * 0.45;
    color = mix(color, frame.fogColor.rgb, fogAmt);

    // Refraction is composited in-color: keep the surface nearly opaque
    float alpha = clamp(0.90 + 0.10 * fres + foam * 0.10, 0.0, 1.0);
    outColor = vec4(color, alpha);
}
