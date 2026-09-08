#version 450

layout(location = 0) in vec3 vFragPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 3) in vec4 vClipPos;
layout(location = 4) in float vViewDepth;
layout(location = 5) flat in vec3 vGeoNormal;

#include "frame_ubo.inc.glsl"
#include "sky_radiance.inc.glsl"
#include "csm.inc.glsl"
#include "water_optics.inc.glsl"

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;
// Opaque scene history (color) + depth history (real depth, not color)
layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;

// invScreenSize.xy — matches framebuffer / gl_FragCoord (no manual Y flip)
layout(push_constant) uniform PC {
    vec2 invScreen;
    vec2 pad;
} pc;

layout(location = 0) out vec4 outColor;

// Value noise comes from water_optics.inc.glsl (shared with the
// camera-underwater caustics).

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

// Depth is fetched without filtering: interpolation across a silhouette invents geometry.
float opaqueViewDepth(vec2 uv) {
    ivec2 size = textureSize(sceneDepth, 0);
    float d = texelFetch(sceneDepth, clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1), 0).r;
    return frame.projection[3][2] / (d + frame.projection[2][2]);
}

// Validate the entire bilinear color footprint, not only its nearest depth texel.
bool refractionFootprintSafe(vec2 uv, float surface, float expected, float tolerance) {
    vec2 size = vec2(textureSize(sceneDepth, 0));
    vec2 base = floor(uv * size - 0.5) + 0.5;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            float d = opaqueViewDepth((base + vec2(x, y)) / size);
            if (d <= surface + 0.02 || abs(d - expected) >= tolerance) return false;
        }
    return true;
}

bool projectWaterRay(vec3 p, out vec2 uv) {
    vec4 clip = frame.projection * vec4(p, 1.0);
    if (clip.w <= 0.0) return false;
    vec3 ndc = clip.xyz / clip.w;
    // Production uses a negative-height Vulkan viewport.
    uv = ndc.xy * vec2(0.5, -0.5) + 0.5;
    vec2 margin = pc.invScreen * 1.5;
    return ndc.z > 0.0 && ndc.z < 1.0 &&
           all(greaterThan(uv, margin)) && all(lessThan(uv, 1.0 - margin));
}

vec3 sceneReflection(vec3 R, vec3 fallback) {
    int steps = int(frame.waterQuality.x);
    // A top surface cannot reflect the submerged floor. Below-horizon rays
    // retain the unmodified direction and use the horizon fallback.
    if (steps == 0 || R.y <= 0.0) return fallback;
    vec3 origin = (frame.view * vec4(vFragPos + normalize(vNormal) * 0.08, 1.0)).xyz;
    vec3 direction = mat3(frame.view) * R;
    float previousT = 0.0;
    float previousDelta = -1.0;
    float range = frame.waterQuality.y;
    float thickness = frame.waterQuality.z; // world/view-space metres, not device depth
    for (int i = 1; i <= 48; ++i) {
        if (i > steps) break;
        float f = float(i) / float(steps);
        float t = 0.12 + range * f * f;
        vec3 ray = origin + direction * t;
        vec2 uv;
        if (!projectWaterRay(ray, uv)) break;
        float delta = -ray.z - opaqueViewDepth(uv);
        if (delta >= 0.0 && previousDelta < 0.0) {
            float lo = previousT, hi = t;
            for (int j = 0; j < 5; ++j) {
                float mid = (lo + hi) * 0.5;
                vec3 probe = origin + direction * mid;
                vec2 probeUV;
                if (!projectWaterRay(probe, probeUV)) return fallback;
                if (-probe.z > opaqueViewDepth(probeUV)) hi = mid; else lo = mid;
            }
            ray = origin + direction * hi;
            if (!projectWaterRay(ray, uv)) return fallback;
            delta = -ray.z - opaqueViewDepth(uv);
            if (delta >= 0.0 && delta < thickness) {
                vec2 edge = min(uv, 1.0 - uv);
                float confidence = smoothstep(0.0, 0.08, min(edge.x, edge.y));
                confidence *= 1.0 - smoothstep(range * 0.5, range, hi);
                confidence *= 1.0 - smoothstep(thickness * 0.5, thickness, delta);
                confidence *= smoothstep(0.15, 0.6, hi);
                confidence *= smoothstep(0.0, 0.1, R.y);
                return mix(fallback, texture(sceneColor, uv).rgb, confidence);
            }
        }
        previousT = t;
        previousDelta = delta;
    }
    return fallback;
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

    // Geometric face normal, flat from the vertex stage. Gating (top-face
    // classification, CSM receiver bias) must key on this, not on the
    // wave-perturbed shading normal: the shading normal oscillates with
    // wave strength and phase, which would spatially/temporally toggle SSR
    // and shadow reception on true horizontal faces.
    vec3 geoN = normalize(vGeoNormal);
    vec3 V = normalize(frame.viewPos.xyz - vFragPos);

    // Fragment-level wave normals on top faces only (sides stay voxel-flat)
    float topMask = smoothstep(0.7, 0.95, geoN.y);
    vec3 N = geoN;
    if (topMask > 0.001) {
        vec3 wN = waveNormal(vFragPos.xz, time, waveStr * 4.0);
        N = normalize(mix(geoN, wN, topMask));
    }

    // Screen-space refraction of the opaque history
    vec2 margin = pc.invScreen * 1.5;
    vec2 screenUV = clamp(gl_FragCoord.xy * pc.invScreen, margin, 1.0 - margin);
    float surfaceDepth = -(frame.view * vec4(vFragPos, 1.0)).z;
    float linOpaque = opaqueViewDepth(screenUV);
    float column = clamp(linOpaque - surfaceDepth, 0.0, 64.0);
    vec2 edge = min(screenUV, 1.0 - screenUV);
    float edgeFade = smoothstep(0.0, 0.04, min(edge.x, edge.y));
    vec2 distort = N.xz * refractionStr * 2.0 * edgeFade * smoothstep(0.0, 0.8, column);
    vec2 refrUV = screenUV;
    // Reject foreground and disocclusion jumps. A bounded backoff preserves shorelines.
    for (int i = 0; i < 4; ++i) {
        vec2 candidate = clamp(screenUV + distort, margin, 1.0 - margin);
        if (refractionFootprintSafe(candidate, surfaceDepth, linOpaque, max(0.5, column * 0.25))) {
            refrUV = candidate;
            break;
        }
        distort *= 0.5;
    }
    vec3 scene = texture(sceneColor, refrUV).rgb;
    float directVisibility = 1.0;
    if (topMask > 0.001 && frame.waterQuality.w > 0.5)
        directVisibility -= sampleDirectionalShadow(vFragPos, geoN,
                            normalize(frame.lightDirection.xyz), surfaceDepth);

    // Beer-Lambert absorption: red dies first -> teal body (shared constants)
    vec3 absorb = exp(-column * WATER_SIGMA);
    float scatterAmt = 1.0 - exp(-column * WATER_SCATTER_RATE);
    float scatterLight = waterScatterAmbient(dayFactor, sunsetFactor, directVisibility);
    vec3 waterBody = scene * absorb + WATER_SCATTER_COLOR * scatterAmt * scatterLight;

    // Fresnel + analytic sky reflection
    float F0 = 0.02;
    float fres = F0 + (1.0 - F0) * pow(1.0 - max(dot(N, V), 0.0), 5.0);
    vec3 R = reflect(-V, N);
    vec3 refl = analyticSkyRadiance(R, dayFactor, sunsetFactor, nightFactor);

    if (topMask > 0.95 && frame.lightingParams.w < 0.5)
        refl = sceneReflection(R, refl);
    vec3 color = mix(waterBody, refl, fres);

    // Sun glitter on the wave normals
    float sunVis = smoothstep(-0.04, 0.08, frame.sunDir.y);
    float RdotS = max(dot(R, frame.sunDir.xyz), 0.0);
    float sunLow = 1.0 - smoothstep(0.0, 0.35, frame.sunDir.y);
    vec3 sunTint = mix(vec3(1.0, 0.96, 0.72), vec3(1.0, 0.45, 0.12), sunLow * sunLow);
    float sunGlitter = pow(RdotS, 700.0) * specularStr * sunVis * (0.35 + 0.65 * dayFactor);
    float sunSheen = pow(RdotS, 64.0) * specularStr * sunVis * 0.08 * (0.3 + 0.7 * dayFactor);
    color += sunTint * (sunGlitter * 2.2 + sunSheen) * directVisibility;

    // Moon glitter (cool tint, night only)
    float moonVis = smoothstep(0.02, 0.28, frame.moonDir.y);
    float RdotM = max(dot(R, frame.moonDir.xyz), 0.0);
    float moonGlitter = pow(RdotM, 700.0) * specularStr * moonVis * nightFactor;
    float moonSheen = pow(RdotM, 64.0) * specularStr * moonVis * nightFactor * 0.10;
    color += vec3(0.55, 0.68, 1.0) * (moonGlitter * 1.4 + moonSheen) * directVisibility;

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
