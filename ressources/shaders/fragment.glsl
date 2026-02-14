#version 460 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;
in float TextureIndex;
in float UseBiomeColor;
in vec3 BiomeColor;
in float AO;
in vec4 FragPosLightSpace;

uniform sampler2DArray textureArray;
layout (binding = 2) uniform sampler2D shadowMap;
uniform vec3 sunDirection;
uniform vec3 viewPos;

// Fog parameters
uniform float fogEnd = 480.0;
uniform vec3 fogColor = vec3(0.75, 0.85, 1.0);
uniform float fogDensity = 0.8;

// Visual parameters
uniform float ambientStrength = 0.2;
uniform float diffuseIntensity = 0.7;
uniform float lightLevels = 5.0;
uniform float colorBoost = 1.0;

float ShadowCalculation(vec4 fragPosLightSpace, vec3 normal, vec3 lightDir)
{
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    float currentDepth = projCoords.z;
    float bias = max(0.005 * (1.0 - dot(normal, lightDir)), 0.0005);
    
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
    for(int x = -1; x <= 1; ++x)
    {
        for(int y = -1; y <= 1; ++y)
        {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r; 
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;        
        }    
    }
    shadow /= 9.0;
    
    // If we're outside the shadow map's ortho box, don't shadow (or use simpler logic)
    if(projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0)
        shadow = 0.0;
        
    return shadow;
}

void main()
{
    // Retrieve texture color from texture array
    vec4 texColor = texture(textureArray, vec3(TexCoord, TextureIndex));

    // Reduce alpha threshold to allow partially transparent textures to display
    // Do not reject completely transparent fragments for specific types
    if (texColor.a < 0.01) discard;

    vec3 color = texColor.rgb;

    // Apply biome coloring to grayscale parts of textures
    if (UseBiomeColor > 0.5) {
        // Calculate luminance (grayscale level)
        float luminance = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));

        // Determine if the pixel is gray (R≈G≈B)
        // Improved method to detect gray parts
        float maxChannel = max(max(texColor.r, texColor.g), texColor.b);
        float minChannel = min(min(texColor.r, texColor.g), texColor.b);
        float colorfulnessRatio = (maxChannel - minChannel) / max(0.001, maxChannel);

        // Lower the ratio, more "gray" the color
        float grayscaleFactor = 1.0 - min(colorfulnessRatio * 4.0, 1.0);

        // Blend original texture with biome color
        // applied only to gray parts
        vec3 coloredPart = luminance * BiomeColor * 1.5; // Slightly intensify
        vec3 biomeColored = mix(texColor.rgb, coloredPart, grayscaleFactor * 0.8);
        color = biomeColored;
    }

    // Apply Ambient Occlusion
    color *= (AO * 0.7 + 0.3);

    vec3 norm = normalize(Normal);

    // Light direction
    vec3 lightDir = normalize(sunDirection);

    // Shadow
    float shadow = ShadowCalculation(FragPosLightSpace, norm, lightDir);

    // Ambient lighting
    vec3 ambient = ambientStrength * color;

    // Diffuse lighting
    float diff = max(dot(norm, lightDir), 0.0) * diffuseIntensity;

    // Minecraft-style "blocky lighting"
    diff = floor(diff * lightLevels) / lightLevels;
    vec3 diffuse = diff * color;

    // Further accentuate directional lighting based on face
    float topLight = 0.0;
    if(norm.y > 0.9) { // Top face
        topLight = 0.3;
    } else if(norm.y < -0.9) { // Bottom face
        topLight = -0.15;
    } else if(abs(norm.x) > 0.9) { // Side faces (east/west)
        topLight = 0.05;
    }

    // Combined result
    vec3 result = ambient + (1.0 - shadow) * (diffuse + topLight * color);

    // Color boost
    result *= colorBoost;

    // Exponential squared fog
    float dist = length(FragPos - viewPos);
    float fogFactor = exp(-pow(dist * fogDensity * 0.003, 2.0));
    fogFactor = clamp(fogFactor, 0.0, 1.0);

    result = mix(fogColor, result, fogFactor);

    FragColor = vec4(result, texColor.a);
}