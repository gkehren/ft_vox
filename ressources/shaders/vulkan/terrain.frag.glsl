#version 450

layout(location = 0) in vec3 vFragPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 3) in float vTextureIndex;
layout(location = 4) in float vUseBiomeColor;
layout(location = 5) in vec3 vBiomeColor;
layout(location = 6) in float vAO;
layout(location = 7) in float vSkyLight;
layout(location = 8) in vec3 vBlockLightRGB;
layout(location = 9) in float vViewDepth;

#include "frame_ubo.inc.glsl"
#include "csm.inc.glsl"
#include "colorspace.inc.glsl"
#include "cutout.inc.glsl"

// x=wind, y=emissive, z=iceSpec, w=flags — materials::MaterialTableUBO
layout(set = 0, binding = 1) uniform MaterialTable {
    vec4 mats[256];
} materialTable;

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;

layout(location = 0) out vec4 outColor;

vec4 materialFor(float texIdx)
{
    uint t = uint(texIdx + 0.5);
    return materialTable.mats[t];
}

void main()
{
    vec4 mat = materialFor(vTextureIndex);
    bool lavaSurface = (uint(mat.w + 0.5) & 16u) != 0u;
    vec2 surfaceUV = vTexCoord;
    if (lavaSurface) {
        // Slow coherent advection: molten rock, not water-speed ripples.
        float t = frame.skyParams.x;
        surfaceUV += vec2(t * 0.018, t * -0.012);
        surfaceUV += 0.025 * sin(vTexCoord.yx * 3.0 + vec2(t * 0.35, -t * 0.28));
    }
    vec4 texColor = texture(textureArray, vec3(surfaceUV, vTextureIndex));
    if (texColor.a < kAlphaCutoutThreshold)
        discard;

    vec3 color = texColor.rgb;

    if (vUseBiomeColor > 0.5) {
        // Tint grayscale resource-pack vegetation in linear light. Preserve
        // authored texture contrast; lighting supplies readability, not a
        // per-material gamma lift or a 2.9x chroma multiplier.
        float luminance = dot(texColor.rgb, kRec709Luma);
        float hi = max(texColor.r, max(texColor.g, texColor.b));
        float lo = min(texColor.r, min(texColor.g, texColor.b));
        float grayscaleFactor = 1.0 - clamp((hi - lo) / max(hi, 0.001) * 4.0, 0.0, 1.0);
        vec3 tint = mix(vec3(dot(vBiomeColor, kRec709Luma)), vBiomeColor, 0.85);
        color = mix(texColor.rgb, luminance * tint * 1.65, grayscaleFactor);

    }

    if (abs(vTextureIndex - 13.0) < 0.5) {
        color *= mix(0.72, 1.15, vAO);
    } else {
        color *= mix(0.58, 1.0, vAO);
    }

    vec3 norm = normalize(vNormal);
    vec3 lightDir = normalize(frame.lightDirection.xyz);
    float ambientStrength = frame.lightParams.x;
    float diffuseIntensity = frame.lightParams.y;
    float colorBoost = frame.lightParams.w;
    float saturationLevel = frame.visualParams.x;
    float contrastLevel = frame.visualParams.y;
    float dayFactor = frame.skyParams.y;
    float nightFactor = frame.skyParams.w;
    float sunsetFactor = frame.skyParams.z;
    float blockLightScale = frame.lightingParams.x;
    float emissiveScale = frame.lightingParams.y;
    float fogBaseY = frame.lightingParams.z;

    // Raw skylight describes enclosure, independent of the time of day.
    // Only direct celestial light uses the CSM; sky bounce remains in shade.
    float sky = clamp(vSkyLight, 0.0, 1.0);
    float sunReach = smoothstep(0.05, 0.45, sky);
    float sunShadow = sampleDirectionalShadow(vFragPos, norm, lightDir, vViewDepth);
    float shadow = sunShadow * sunReach;

    // Shadow debug visualization (issue #137): frame.visualParams.w selects
    // the mode. 1 = cascade index color, 2 = cascade blend band (only
    // cascades 0/1 blend — cascade 2 has no next), 3 = shadow texel density
    // (relative footprint, non-periodic), 4 = receiver light-space depth.
    // Terrain-only tool; mobs share the sampling path but not the output.
    int shadowDebugMode = int(frame.visualParams.w + 0.5);
    if (shadowDebugMode > 0)
    {
        float s0 = frame.cascadeSplits.x, s1 = frame.cascadeSplits.y, s2 = frame.cascadeSplits.z;
        int cascade = vViewDepth < s0 ? 0 : (vViewDepth < s1 ? 1 : 2);
        if (shadowDebugMode == 1)
        {
            outColor = vec4(cascade == 0 ? vec3(1.0, 0.25, 0.25)
                          : cascade == 1 ? vec3(0.25, 1.0, 0.25)
                                         : vec3(0.30, 0.45, 1.0),
                            1.0);
            return;
        }
        if (shadowDebugMode == 2)
        {
            float bandW = 0.0;
            if (cascade < 2)
            {
                float splitEnd = cascade == 0 ? s0 : s1;
                float splitStart = cascade == 0 ? 0.1 : s0;
                float band = max(splitEnd - splitStart, 1.0) * 0.12;
                bandW = clamp((vViewDepth - (splitEnd - band)) / max(band, 1e-3), 0.0, 1.0);
            }
            outColor = vec4(mix(vec3(0.05), vec3(1.0, 0.6, 0.1), bandW), 1.0);
            return;
        }
        if (shadowDebugMode == 3)
        {
            // Real world-units-per-texel relative to cascade 0, on a log
            // scale: 0 = same texel density as cascade 0, 1 = 16x coarser.
            float density = frame.cascadeTexelWorldSizes[cascade] /
                            max(frame.cascadeTexelWorldSizes.x, 1e-8);
            float t = clamp(log2(max(density, 1.0)) / 4.0, 0.0, 1.0);
            outColor = vec4(vec3(t), 1.0);
            return;
        }
        if (shadowDebugMode == 4)
        {
            // Receiver depth in the light-space ortho projection — already
            // [0,1] with GLM_FORCE_DEPTH_ZERO_TO_ONE (this is the receiver's
            // projected depth, not the shadow-map content; reading the map
            // back would need a separate non-comparison debug sampler).
            vec4 ls = csmCascadeMatrix(cascade) * vec4(vFragPos, 1.0);
            vec3 p = ls.xyz / max(ls.w, 1e-6);
            outColor = vec4(vec3(clamp(p.z, 0.0, 1.0)), 1.0);
            return;
        }
    }
    float hemisphere = 0.65 + 0.35 * clamp(norm.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 lightTint = mix(vec3(1.06, 0.98, 0.88), vec3(1.12, 0.78, 0.48), sunsetFactor * 0.72);
    lightTint = mix(lightTint, vec3(0.55, 0.68, 1.0), nightFactor);
    float dayLightFactor = clamp(dayFactor + sunsetFactor * 0.3 + nightFactor * 0.20, 0.0, 1.0);
    float diff = max(dot(norm, lightDir), 0.0) * diffuseIntensity;
    vec3 skyFill = mix(vec3(0.82, 0.90, 1.0), vec3(1.0, 0.82, 0.68), sunsetFactor * 0.4);
    vec3 outdoorAmbient = skyFill * ambientStrength * dayFactor
                        + frame.moonAmbient.rgb * frame.moonAmbient.w * nightFactor;
    // Enclosed rock gets a small time-of-day-independent floor, shaped only
    // by the shared hemisphere term; no night-time multiplier on cave fill.
    vec3 caveAmbient = vec3(0.085, 0.094, 0.117);
    vec3 ambient = mix(caveAmbient, outdoorAmbient, sky) * hemisphere;
    vec3 direct = lightTint * diff * dayLightFactor * sunReach * (1.0 - shadow);
    // Colored block light (issue #141): per-source linear RGB propagated by
    // the voxel BFS, illuminating albedo in place of the fixed warm scalar
    // tint. Sky/sun/moon terms above stay untouched.
    vec3 blockFill = max(vBlockLightRGB, vec3(0.0));
    // The flood stores linearly decremented levels, not irradiance. A
    // quadratic falloff localizes the bounce while preserving its RGB hue.
    float blockPeak = max(blockFill.r, max(blockFill.g, blockFill.b));
    blockFill *= blockPeak * blockLightScale;
    vec3 result = color * (ambient + direct + blockFill) * colorBoost;

    float em = mat.y * emissiveScale;
    float blockLuma = max(vBlockLightRGB.r, max(vBlockLightRGB.g, vBlockLightRGB.b));
    if (lavaSurface) {
        // Self emission is independent of AO and its own propagated light.
        // Avoid adding that light a second time: it bleaches the hot texels.
        float heat = smoothstep(0.15, 0.85, max(texColor.r, max(texColor.g, texColor.b)));
        float pulse = 0.96 + 0.04 * sin(frame.skyParams.x * 0.8 + vFragPos.x * 0.7 + vFragPos.z * 0.5);
        result = texColor.rgb * (ambient + direct) * 0.15
               + texColor.rgb * em * mix(1.0, 1.8, heat) * pulse;
    } else {
        result += color * em * (1.2 + blockLuma);
    }

    // Ice/snow specular from material table (IceSpec flag bit 2)
    if ((uint(mat.w + 0.5) & 4u) != 0u && mat.z > 0.0)
    {
        vec3 V = normalize(frame.viewPos.xyz - vFragPos);
        vec3 H = normalize(lightDir + V);
        float iceSpec = pow(max(dot(norm, H), 0.0), 96.0)
                      * dayLightFactor * (1.0 - shadow * 0.85) * sunReach;
        result += vec3(0.82, 0.92, 1.05) * iceSpec * mat.z;
    }

    result = gradeContrast(result, contrastLevel);

    // Scotopic night vision: mild desat toward cool blue — kept light so the
    // night stays colorful enough to navigate (playability-first baseline)
    float nightLum = dot(result, kRec709Luma);
    result = mix(result, vec3(nightLum) * vec3(0.62, 0.74, 1.05),
                 nightFactor * 0.38 * (lavaSurface ? 0.0 : 1.0));

    // Fog + aerial perspective (distance desat toward sky-tinted haze)
    float fogStart = frame.fogParams.x;
    float fogEnd = frame.fogParams.y;
    float fogDensity = frame.fogParams.z;
    float heightFalloff = frame.fogParams.w;
    float dist = length(vFragPos - frame.viewPos.xyz);
    float linearFog = smoothstep(fogStart, max(fogStart + 1.0, fogEnd), dist);
    float avgY = 0.5 * (vFragPos.y + frame.viewPos.y);
    float heightTerm = exp(-heightFalloff * max(0.0, avgY - fogBaseY));
    float densityFog = 1.0 - exp(-max(0.0, dist - fogStart * 0.25) * fogDensity * 0.0009 * heightTerm);
    // Cap must match lighting::kTerrainFogAmountCap
    float fogAmount = clamp(max(linearFog, densityFog), 0.0, 0.45) * sunReach;

    // Sky aerial color: cool blue day → warm sunset → dark-blue night (lifted
    // from near-black so night fog doesn't swallow the terrain)
    vec3 dayAerial = vec3(0.40, 0.60, 0.90);
    vec3 sunsetAerial = vec3(0.95, 0.55, 0.32);
    vec3 nightAerial = vec3(0.014, 0.022, 0.048);
    vec3 aerialSky = mix(dayAerial, sunsetAerial, sunsetFactor);
    aerialSky = mix(aerialSky, nightAerial, nightFactor);
    // Blend engine fogColor with aerial sky for horizon-matched haze
    vec3 fogCol = mix(frame.fogColor.rgb, aerialSky, 0.55);
    fogCol = mix(fogCol, vec3(1.0, 0.72, 0.42), sunsetFactor * 0.25);

    float lum = dot(result, kRec709Luma);
    result = gradeSaturation(result, saturationLevel);

    // Aerial perspective: desaturate + lift toward sky with distance (not pure wash)
    float aerial = fogAmount;
    float desat = mix(1.0, 0.72, aerial);
    vec3 aerialLit = mix(vec3(lum), result, desat);
    // Retain a bit of surface color so midground stays readable
    vec3 fogMix = mix(fogCol, aerialLit * 0.40 + fogCol * 0.60, 0.22);
    result = mix(aerialLit, fogMix, aerial);

    outColor = vec4(result, texColor.a);
}
