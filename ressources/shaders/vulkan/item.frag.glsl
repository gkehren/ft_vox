#version 450
// Dropped item shading (issue #229): the mob lighting model (ambient +
// directional + hemisphere + block fill + shared atmosphere fog) sampling
// the BLOCK atlas array per instance layer. Items RECEIVE directional
// shadows like mobs but never CAST them (no shadow-pass pipeline in v1).
#include "frame_ubo.inc.glsl"
#include "csm.inc.glsl"
#include "colorspace.inc.glsl"
#include "atmosphere_fog.inc.glsl"
#include "cutout.inc.glsl"

layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=2) in vec2 texCoord;
layout(location=3) flat in float vLayer;
layout(location=4) in float vSkyLight;
layout(location=5) in vec3 vBlockLightRGB;

layout(set=1, binding=0) uniform sampler2DArray blockAtlas;
layout(location=0) out vec4 outColor;

void main() {
    vec4 texel = texture(blockAtlas, vec3(texCoord, vLayer));
    if (texel.a < kAlphaCutoutThreshold) discard;

    vec3 norm = normalize(worldNormal);
    vec3 lightDir = normalize(frame.lightDirection.xyz);
    float ambientStrength = frame.lightParams.x;
    float diffuseIntensity = frame.lightParams.y;
    float dayFactor = frame.skyParams.y;
    float nightFactor = frame.skyParams.w;
    float sunsetFactor = frame.skyParams.z;
    float blockLightScale = frame.lightingParams.x;
    // NOTE: items deliberately do NOT read the material-grading lanes
    // (colorBoost / saturation / contrast): issue #161 scopes those UI
    // controls to terrain & mobs exactly (test_render_helpers source
    // contract). Items get the shared lighting + fog, not the artistic grade.

    float sky = clamp(vSkyLight, 0.0, 1.0);
    float sunReach = smoothstep(0.05, 0.45, sky);
    float viewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;
    float sunShadow = viewDepth < frame.cascadeSplits.z ? sampleDirectionalShadow(worldPosition, norm, lightDir, viewDepth) : 0.0;
    float shadow = sunShadow * sunReach;

    float hemisphere = 0.65 + 0.35 * clamp(norm.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 lightTint = mix(vec3(1.06, 0.98, 0.88), vec3(1.12, 0.78, 0.48), sunsetFactor * 0.72);
    lightTint = mix(lightTint, vec3(0.55, 0.68, 1.0), nightFactor);
    float dayLightFactor = clamp(dayFactor + sunsetFactor * 0.3 + nightFactor * 0.20, 0.0, 1.0);
    float diff = max(dot(norm, lightDir), 0.0) * diffuseIntensity;
    vec3 skyFill = mix(vec3(0.82, 0.90, 1.0), vec3(1.0, 0.82, 0.68), sunsetFactor * 0.4);
    vec3 outdoorAmbient = skyFill * ambientStrength * dayFactor
                        + frame.moonAmbient.rgb * frame.moonAmbient.w * nightFactor;
    vec3 caveAmbient = vec3(0.085, 0.094, 0.117);
    vec3 ambient = mix(caveAmbient, outdoorAmbient, sky) * hemisphere;
    vec3 direct = lightTint * diff * dayLightFactor * sunReach * (1.0 - shadow);

    vec3 blockFill = max(vBlockLightRGB, vec3(0.0));
    float blockPeak = max(blockFill.r, max(blockFill.g, blockFill.b));
    blockFill *= blockPeak * blockLightScale;
    vec3 result = texel.rgb * (ambient + direct + blockFill);

    float nightLum = dot(result, kRec709Luma);
    result = mix(result, vec3(nightLum) * vec3(0.62, 0.74, 1.05), nightFactor * 0.38);

    AtmosphereFog atmo = evaluateAtmosphereFog(worldPosition, sunReach);

    result = applyAtmosphereFog(result, atmo);

    outColor = vec4(result, 1.0);
}
