#version 450

layout(location = 0) in vec3 vFragPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 3) in float vTextureIndex;
layout(location = 4) in float vUseBiomeColor;
layout(location = 5) in vec3 vBiomeColor;
layout(location = 6) in float vAO;
layout(location = 7) in float vSkyLight;
layout(location = 8) in float vBlockLight;
layout(location = 9) in float vViewDepth;

#include "frame_ubo.inc.glsl"
#include "colorspace.inc.glsl"

// x=wind, y=emissive, z=iceSpec, w=flags — materials::MaterialTableUBO
layout(set = 0, binding = 1) uniform MaterialTable {
    vec4 mats[256];
} materialTable;

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;
layout(set = 1, binding = 1) uniform sampler2DArray shadowMap;

layout(location = 0) out vec4 outColor;

const vec2 POISSON[12] = vec2[](
    vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696,  0.457),
    vec2(-0.203,  0.621), vec2( 0.962, -0.195), vec2( 0.473, -0.480),
    vec2( 0.519,  0.767), vec2( 0.185, -0.893), vec2( 0.507,  0.064),
    vec2( 0.896,  0.412), vec2(-0.322, -0.932), vec2(-0.792, -0.598)
);

vec4 materialFor(float texIdx)
{
    uint t = uint(texIdx + 0.5);
    return materialTable.mats[t];
}

mat4 cascadeMatrix(int c)
{
    if (c == 0) return frame.cascadeMatrix0;
    if (c == 1) return frame.cascadeMatrix1;
    return frame.cascadeMatrix2;
}

// Soft sample one cascade; out-of-bounds UV taps are discarded (unshadowed), not garbage.
float sampleCascadeShadow(vec3 fragPos, vec3 normal, vec3 lightDir, int cascade)
{
    vec4 fragPosLS = cascadeMatrix(cascade) * vec4(fragPos, 1.0);
    vec3 projCoords = fragPosLS.xyz / max(fragPosLS.w, 1e-6);
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    if (projCoords.z < 0.0 || projCoords.z > 1.0 ||
        projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0)
        return 0.0;

    float currentDepth = projCoords.z;
    float nDotL = max(dot(normal, lightDir), 0.0);
    float bias = max(0.012 * (1.0 - nDotL), 0.0035);
    float radius = (1.5 + float(cascade) * 1.0) / 1024.0;

    float shadow = 0.0;
    float taps = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        vec2 uv = projCoords.xy + POISSON[i] * radius * 2.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
            continue;
        float pcfDepth = texture(shadowMap, vec3(uv, float(cascade))).r;
        shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        taps += 1.0;
    }
    if (taps < 0.5)
        return 0.0;
    return shadow / taps;
}

float ShadowCalculation(vec3 fragPos, vec3 normal, vec3 lightDir, float viewDepth)
{
    float s0 = frame.cascadeSplits.x;
    float s1 = frame.cascadeSplits.y;
    float s2 = frame.cascadeSplits.z;

    int cascade = 2;
    float splitStart = s1;
    float splitEnd = s2;
    if (viewDepth < s0) {
        cascade = 0;
        splitStart = 0.1;
        splitEnd = s0;
    } else if (viewDepth < s1) {
        cascade = 1;
        splitStart = s0;
        splitEnd = s1;
    }

    float shadow = sampleCascadeShadow(fragPos, normal, lightDir, cascade);

    if (cascade < 2) {
        float gap = max(splitEnd - splitStart, 1.0);
        float band = gap * 0.12;
        float edge = splitEnd - band;
        if (viewDepth > edge) {
            float w = clamp((viewDepth - edge) / max(band, 1e-3), 0.0, 1.0);
            float shadowNext = sampleCascadeShadow(fragPos, normal, lightDir, cascade + 1);
            shadow = mix(shadow, shadowNext, w);
        }
    }
    return shadow;
}

void main()
{
    vec4 texColor = texture(textureArray, vec3(vTexCoord, vTextureIndex));
    if (texColor.a < 0.01)
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
    float sunShadow = ShadowCalculation(vFragPos, norm, lightDir, vViewDepth);
    float shadow = sunShadow * sunReach;
    float hemisphere = 0.65 + 0.35 * clamp(norm.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 lightTint = mix(vec3(1.06, 0.98, 0.88), vec3(1.12, 0.78, 0.48), sunsetFactor * 0.72);
    lightTint = mix(lightTint, vec3(0.55, 0.68, 1.0), nightFactor);
    float dayLightFactor = clamp(dayFactor + sunsetFactor * 0.3 + nightFactor * 0.20, 0.0, 1.0);
    float diff = max(dot(norm, lightDir), 0.0) * diffuseIntensity;
    vec3 skyFill = mix(vec3(0.82, 0.90, 1.0), vec3(1.0, 0.82, 0.68), sunsetFactor * 0.4);
    vec3 outdoorAmbient = skyFill * ambientStrength * dayFactor
                        + frame.moonAmbient.rgb * frame.moonAmbient.w * nightFactor;
    // Enclosed rock gets a small phase-independent floor; no sun-oriented
    // top-face bonus underground and no night-time multiplier on cave fill.
    vec3 caveAmbient = vec3(0.085, 0.094, 0.117);
    vec3 ambient = mix(caveAmbient, outdoorAmbient, sky) * hemisphere;
    vec3 direct = lightTint * diff * dayLightFactor * sunReach * (1.0 - shadow);
    vec3 blockFill = vec3(1.0, 0.72, 0.46) * max(vBlockLight * blockLightScale, 0.0);
    vec3 result = color * (ambient + direct + blockFill) * colorBoost;

    vec4 mat = materialFor(vTextureIndex);
    float em = mat.y * emissiveScale;
    result += color * em * (1.2 + vBlockLight);

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
    result = mix(result, vec3(nightLum) * vec3(0.62, 0.74, 1.05), nightFactor * 0.38);

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
