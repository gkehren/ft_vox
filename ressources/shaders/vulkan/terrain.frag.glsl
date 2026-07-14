#version 450

layout(location = 0) in vec3 vFragPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 3) in float vTextureIndex;
layout(location = 4) in float vUseBiomeColor;
layout(location = 5) in vec3 vBiomeColor;
layout(location = 6) in float vAO;
layout(location = 7) in vec4 vFragPosLightSpace;

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 viewPos;
    vec4 lightDirection;
    vec4 fogColor;
    vec4 fogParams;
    vec4 lightParams;
    vec4 visualParams;
    vec4 sunDir;
    vec4 moonDir;
    vec4 skyParams;
    vec4 postParams0;
    vec4 postParams1;
    vec4 postParams2;
    vec4 postParams3;
} frame;

layout(set = 1, binding = 0) uniform sampler2DArray textureArray;
layout(set = 1, binding = 1) uniform sampler2D shadowMap;

layout(location = 0) out vec4 outColor;

// Manual 3x3 PCF (matches old OpenGL path; shadow map is 2048²)
float ShadowCalculation(vec4 fragPosLightSpace, vec3 normal, vec3 lightDir)
{
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    // Clip XY [-1,1] → UV [0,1]; Z already [0,1] with GLM_FORCE_DEPTH_ZERO_TO_ONE
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0)
        return 0.0;

    float currentDepth = projCoords.z;
    float bias = max(0.005 * (1.0 - dot(normal, lightDir)), 0.0005);

    float shadow = 0.0;
    const float texelSize = 1.0 / 2048.0;
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(float(x), float(y)) * texelSize).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

void main()
{
    vec4 texColor = texture(textureArray, vec3(vTexCoord, vTextureIndex));
    if (texColor.a < 0.01)
        discard;

    vec3 color = texColor.rgb;

    // Biome tint: blend biome hue into greyscale-ish grass/leaves (moderate chroma)
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

    // Softer AO so corners stay readable without grey crush
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
    // colorBoost is packed in lightParams.w (also mirrored in visualParams.z)
    float colorBoost = frame.lightParams.w;
    float saturationLevel = frame.visualParams.x;
    float contrastLevel = frame.visualParams.y;

    float shadow = ShadowCalculation(vFragPosLightSpace, norm, lightDir);

    // Subtle warm key (not heavy orange cast)
    vec3 lightTint = mix(vec3(1.0, 0.98, 0.94), vec3(1.0), 0.55);
    vec3 ambient = ambientStrength * color;
    float diff = max(dot(norm, lightDir), 0.0) * diffuseIntensity;
    diff = floor(diff * lightLevels + 0.001) / lightLevels;
    float shadowTerm = mix(0.28, 1.0, 1.0 - shadow);
    vec3 diffuse = diff * color * lightTint;

    float topLight = 0.0;
    if (norm.y > 0.9) topLight = 0.28;
    else if (norm.y < -0.9) topLight = -0.08;
    else if (abs(norm.x) > 0.9) topLight = 0.06;
    float dayLightFactor = clamp(diffuseIntensity / 0.75, 0.0, 1.0);

    vec3 result = ambient + shadowTerm * (diffuse + topLight * color * dayLightFactor * lightTint);
    result *= colorBoost;

    // Mild midtone contrast before fog
    result = (result - vec3(0.5)) * contrastLevel + vec3(0.5);
    result = max(result, vec3(0.0));

    float fogStart = frame.fogParams.x;
    float fogEnd = frame.fogParams.y;
    float fogDensity = frame.fogParams.z;
    float dist = length(vFragPos - frame.viewPos.xyz);
    float linearFog = smoothstep(fogStart, max(fogStart + 1.0, fogEnd), dist);
    float densityDistance = max(0.0, dist - fogStart * 0.4);
    float densityFog = 1.0 - exp(-pow(densityDistance * fogDensity * 0.0012, 2.0));
    float relativeHeight = clamp((vFragPos.y - frame.viewPos.y + 48.0) / 160.0, 0.0, 1.0);
    float heightFog = mix(1.05, 0.82, relativeHeight);
    // Cap fog lower so far terrain does not become a white/grey slab
    float fogAmount = clamp(max(linearFog, densityFog) * heightFog, 0.0, 0.78);

    float lum = dot(result, vec3(0.299, 0.587, 0.114));
    result = mix(vec3(lum), result, saturationLevel);
    // Fog retains a hint of the lit color so distance is not dead grey
    vec3 fogMix = mix(frame.fogColor.rgb, result * 0.35 + frame.fogColor.rgb * 0.65, 0.15);
    result = mix(result, fogMix, fogAmount);

    outColor = vec4(result, texColor.a);
}
