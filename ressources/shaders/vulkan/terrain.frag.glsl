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

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 projection;
    mat4 cascadeMatrix0;
    mat4 cascadeMatrix1;
    mat4 cascadeMatrix2;
    vec4 viewPos;
    vec4 lightDirection;
    vec4 fogColor;
    vec4 fogParams;
    vec4 lightParams;
    vec4 visualParams;
    vec4 sunDir;
    vec4 moonDir;
    vec4 skyParams;
    vec4 cascadeSplits;
    vec4 moonAmbient;
    vec4 tier1Params;
    vec4 waterParams;
    vec4 postParams0;
    vec4 postParams1;
    vec4 postParams2;
    vec4 postParams3;
} frame;

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;
layout(set = 1, binding = 1) uniform sampler2DArray shadowMap;

layout(location = 0) out vec4 outColor;

const vec2 POISSON[12] = vec2[](
    vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696,  0.457),
    vec2(-0.203,  0.621), vec2( 0.962, -0.195), vec2( 0.473, -0.480),
    vec2( 0.519,  0.767), vec2( 0.185, -0.893), vec2( 0.507,  0.064),
    vec2( 0.896,  0.412), vec2(-0.322, -0.932), vec2(-0.792, -0.598)
);

float emissiveForIndex(float texIdx)
{
    // Matches TextureType enum in utils.hpp
    int t = int(texIdx + 0.5);
    if (t == 23) return 0.90; // REDSTONE_ORE
    if (t == 22) return 0.45; // LAPIS_ORE
    if (t == 18) return 0.25; // DIAMOND_ORE
    if (t == 19) return 0.20; // EMERALD_ORE
    if (t == 20) return 0.12; // GOLD_ORE
    return 0.0;
}

mat4 cascadeMatrix(int c)
{
    if (c == 0) return frame.cascadeMatrix0;
    if (c == 1) return frame.cascadeMatrix1;
    return frame.cascadeMatrix2;
}

// Soft sample one cascade; out-of-bounds UV taps are discarded (unshadowed), not garbage.
// Matches tier1::shadowDepthBias.
float sampleCascadeShadow(vec3 fragPos, vec3 normal, vec3 lightDir, int cascade)
{
    vec4 fragPosLS = cascadeMatrix(cascade) * vec4(fragPos, 1.0);
    vec3 projCoords = fragPosLS.xyz / max(fragPosLS.w, 1e-6);
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // Outside cascade map → unshadowed (not black)
    if (projCoords.z < 0.0 || projCoords.z > 1.0 ||
        projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0)
        return 0.0;

    float currentDepth = projCoords.z;
    float nDotL = max(dot(normal, lightDir), 0.0);
    // Voxel-friendly bias (was too low → banded acne / false black regions)
    float bias = max(0.012 * (1.0 - nDotL), 0.0035);
    // Slightly larger filter on far cascades
    float radius = (1.5 + float(cascade) * 1.0) / 1024.0;

    float shadow = 0.0;
    float taps = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        vec2 uv = projCoords.xy + POISSON[i] * radius * 2.5;
        // Discard OOB taps — do not sample border as inverted shadow
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
    // Select primary cascade + soft blend into next near split boundaries
    // (avoids hard cascade cuts that read as black bands on slopes)
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

    // Blend toward next cascade near the far edge of this split
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
    float blockLightScale = frame.tier1Params.x;
    float emissiveScale = frame.tier1Params.y;
    float fogBaseY = frame.tier1Params.z;

    float shadow = ShadowCalculation(vFragPos, norm, lightDir, vViewDepth);

    // Must match tier1::localLightScale (no zero→full-light fallback: caves stay dark).
    float skyL = vSkyLight * clamp(dayFactor + 0.15 * (1.0 - nightFactor), 0.0, 1.0);
    float blkL = vBlockLight * blockLightScale;
    float localLight = mix(0.12, 1.0, clamp(max(skyL, blkL), 0.0, 1.0));

    vec3 lightTint = mix(vec3(1.0, 0.98, 0.94), vec3(1.0), 0.55);
    vec3 moonFill = frame.moonAmbient.rgb * frame.moonAmbient.w * nightFactor;
    vec3 ambient = (ambientStrength * max(dayFactor, 0.2) + length(moonFill) * 0.85) * color
                 + moonFill * color;

    float diff = max(dot(norm, lightDir), 0.0) * diffuseIntensity;
    diff = floor(diff * lightLevels + 0.001) / lightLevels;
    float shadowTerm = mix(0.28, 1.0, 1.0 - shadow);
    float dayLightFactor = clamp(diffuseIntensity / 0.75, 0.0, 1.0)
                         * clamp(dayFactor + sunsetFactor * 0.5, 0.05, 1.0);

    vec3 diffuse = diff * color * lightTint * dayLightFactor;

    float topLight = 0.0;
    if (norm.y > 0.9) topLight = 0.28;
    else if (norm.y < -0.9) topLight = -0.08;
    else if (abs(norm.x) > 0.9) topLight = 0.06;

    vec3 result = ambient + shadowTerm * (diffuse + topLight * color * dayLightFactor * lightTint);
    result *= colorBoost;
    result *= localLight;

    float em = emissiveForIndex(vTextureIndex) * emissiveScale;
    result += color * em * (1.2 + vBlockLight);

    result = (result - vec3(0.5)) * contrastLevel + vec3(0.5);
    result = max(result, vec3(0.0));

    // Fog — matches tier1::terrainFogAmount (lower density scale + cap for outdoor chroma)
    float fogStart = frame.fogParams.x;
    float fogEnd = frame.fogParams.y;
    float fogDensity = frame.fogParams.z;
    float heightFalloff = frame.fogParams.w;
    float dist = length(vFragPos - frame.viewPos.xyz);
    float linearFog = smoothstep(fogStart, max(fogStart + 1.0, fogEnd), dist);
    float avgY = 0.5 * (vFragPos.y + frame.viewPos.y);
    float heightTerm = exp(-heightFalloff * max(0.0, avgY - fogBaseY));
    // density scale 0.0009 (was 0.0018); cap 0.55 (was 0.82) — midground keeps color
    float densityFog = 1.0 - exp(-max(0.0, dist - fogStart * 0.25) * fogDensity * 0.0009 * heightTerm);
    float fogAmount = clamp(max(linearFog, densityFog), 0.0, 0.55);

    vec3 sunTint = mix(frame.fogColor.rgb, vec3(1.0, 0.72, 0.42), sunsetFactor * 0.55);
    vec3 fogCol = mix(sunTint, vec3(0.08, 0.10, 0.18), nightFactor);

    float lum = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(lum), result, saturationLevel);
    // Retain more lit color in fog mix so forests don't go pastel
    vec3 fogMix = mix(fogCol, result * 0.45 + fogCol * 0.55, 0.18);
    result = mix(result, fogMix, fogAmount);

    outColor = vec4(result, texColor.a);
}
