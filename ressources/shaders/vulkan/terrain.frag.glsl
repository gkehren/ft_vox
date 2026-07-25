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
        float luminance = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        float maxChannel = max(max(texColor.r, texColor.g), texColor.b);
        float minChannel = min(min(texColor.r, texColor.g), texColor.b);
        float colorfulnessRatio = (maxChannel - minChannel) / max(0.001, maxChannel);
        float grayscaleFactor = 1.0 - min(colorfulnessRatio * 4.0, 1.0);
        vec3 biome = max(vBiomeColor, vec3(0.05));
        vec3 coloredPart = luminance * biome * 1.45;
        color = mix(texColor.rgb, coloredPart, grayscaleFactor * 0.82);
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
    float lightLevels = max(frame.lightParams.z, 1.0);
    float colorBoost = frame.lightParams.w;
    float saturationLevel = frame.visualParams.x;
    float contrastLevel = frame.visualParams.y;
    float dayFactor = frame.skyParams.y;
    float nightFactor = frame.skyParams.w;
    float sunsetFactor = frame.skyParams.z;
    float blockLightScale = frame.lightingParams.x;
    float emissiveScale = frame.lightingParams.y;
    float fogBaseY = frame.lightingParams.z;

    // Must match lighting::localLightScale (raised cave floor + smooth light curve).
    float skyL = vSkyLight * clamp(dayFactor + 0.12 * (1.0 - nightFactor), 0.0, 1.0);
    float blkL = vBlockLight * blockLightScale;
    float combined = clamp(max(skyL, blkL), 0.0, 1.0);
    // Hermite curve: mid light levels more readable; floor stays dark-but-shaped
    float lightCurve = combined * combined * (3.0 - 2.0 * combined);
    const float kCaveFloor = 0.22;
    float localLight = mix(kCaveFloor, 1.0, lightCurve);

    // CSM wherever raw sky light reaches — active for the moon at night too
    // (caves keep shadowTerm=1, full ambient path). Matches lighting::sunShadowWeight.
    float sunShadow = ShadowCalculation(vFragPos, norm, lightDir, vViewDepth);
    float sunReach = smoothstep(0.05, 0.45, vSkyLight);
    float shadow = sunShadow * sunReach;

    // Golden day light → amber sunset → cool blue moonlight
    vec3 lightTint = mix(vec3(1.06, 0.98, 0.88), vec3(1.12, 0.78, 0.48), sunsetFactor * 0.72);
    lightTint = mix(lightTint, vec3(0.55, 0.68, 1.0), nightFactor);

    // Ambient: warm sky fill by day, near-dark cool fill at night (cinematic night)
    vec3 moonFill = frame.moonAmbient.rgb * frame.moonAmbient.w * nightFactor;
    vec3 ambient = (ambientStrength * (dayFactor + 0.6 * sunsetFactor) + 0.05 * nightFactor) * color
                 + moonFill * color;
    // Sunset-tinted ambient fill (warm bounce under golden hour)
    ambient *= mix(vec3(1.0), vec3(1.18, 0.88, 0.62), sunsetFactor * 0.50);

    float diff = max(dot(norm, lightDir), 0.0) * diffuseIntensity;
    diff = floor(diff * lightLevels + 0.001) / lightLevels;
    // Higher unshadowed floor underground (sunReach=0 → shadowTerm=1, full ambient path)
    float shadowTerm = mix(0.22, 1.0, 1.0 - shadow);
    // Phase exposure: full sun by day, dimmed golden hour, soft directional moonlight
    float dayLightFactor = clamp(diffuseIntensity / 0.75, 0.0, 1.0)
                         * clamp(dayFactor + sunsetFactor * 0.5 + nightFactor * 0.20, 0.05, 1.0);

    vec3 diffuse = diff * color * lightTint * dayLightFactor;

    float topLight = 0.0;
    if (norm.y > 0.9) topLight = 0.16;
    else if (norm.y < -0.9) topLight = -0.08;
    else if (abs(norm.x) > 0.9) topLight = 0.06;

    vec3 result = ambient + shadowTerm * (diffuse + topLight * color * dayLightFactor * lightTint);

    // Cool shadow tint — outdoor umbra only (gated by sunReach)
    float shadowAmt = clamp(shadow, 0.0, 1.0);
    vec3 coolShadow = mix(vec3(0.78, 0.86, 1.05), vec3(0.55, 0.62, 0.95), nightFactor * 0.4);
    result = mix(result, result * coolShadow,
                 shadowAmt * sunReach * (0.38 * dayFactor + 0.18 * sunsetFactor + 0.22));

    result *= colorBoost;
    result *= localLight;

    // Soft cave fill (not multiplied by localLight floor) — cool slate so shapes stay readable
    float caveAmt = 1.0 - combined;
    vec3 caveFill = color * vec3(0.065, 0.072, 0.090) * caveAmt;
    // Slight face bias so walls/ceilings separate without looking lit
    caveFill *= mix(0.85, 1.15, clamp(0.5 + 0.5 * norm.y + topLight, 0.0, 1.0));
    // Dimmer fill outdoors at night (keeps caves readable but nights dark)
    caveFill *= mix(1.0, 0.55, nightFactor);
    result += caveFill;

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

    // Soften contrast in unlit caves so blacks aren't crushed to pure #000
    float contrastEff = mix(mix(1.0, contrastLevel, 0.35), contrastLevel, combined);
    result = (result - vec3(0.5)) * contrastEff + vec3(0.5);
    result = max(result, vec3(0.0));

    // Scotopic night vision: desaturate toward a cool blue-grey (cinematic night)
    float nightLum = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(result, vec3(nightLum) * vec3(0.62, 0.74, 1.05), nightFactor * 0.55);

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
    float fogAmount = clamp(max(linearFog, densityFog), 0.0, 0.45);

    // Sky aerial color: cool blue day → warm sunset → near-black night
    vec3 dayAerial = vec3(0.40, 0.60, 0.90);
    vec3 sunsetAerial = vec3(0.95, 0.55, 0.32);
    vec3 nightAerial = vec3(0.006, 0.010, 0.024);
    vec3 aerialSky = mix(dayAerial, sunsetAerial, sunsetFactor);
    aerialSky = mix(aerialSky, nightAerial, nightFactor);
    // Blend engine fogColor with aerial sky for horizon-matched haze
    vec3 fogCol = mix(frame.fogColor.rgb, aerialSky, 0.55);
    fogCol = mix(fogCol, vec3(1.0, 0.72, 0.42), sunsetFactor * 0.25);

    float lum = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(lum), result, saturationLevel);

    // Aerial perspective: desaturate + lift toward sky with distance (not pure wash)
    float aerial = fogAmount;
    float desat = mix(1.0, 0.72, aerial);
    vec3 aerialLit = mix(vec3(lum), result, desat);
    // Retain a bit of surface color so midground stays readable
    vec3 fogMix = mix(fogCol, aerialLit * 0.40 + fogCol * 0.60, 0.22);
    result = mix(aerialLit, fogMix, aerial);

    outColor = vec4(result, texColor.a);
}
