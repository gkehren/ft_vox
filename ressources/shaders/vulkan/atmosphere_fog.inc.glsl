// Shared camera-to-surface aerial perspective / air-fog contract (issue #159).
//
// One documented source for the AIR medium between the camera and any
// world-space surface: terrain (terrain.frag.glsl), water (water.frag.glsl)
// and dynamic entities (mob.frag.glsl). Material responsibilities stay
// separate — terrain material lighting, the Beer-Lambert water column
// (water_optics.inc.glsl) and entity local-light sampling are NOT part of
// this contract; only the camera-to-surface air term is shared here.
//
// The model: linear smoothstep fog from frame.fogParams.xy, exponential
// density fog with height falloff toward frame.lightingParams.z (fogBaseY),
// capped at kAtmosphereFogCap, gated by local skylight (sealed caves get no
// outdoor haze), and a day/sunset/night aerial haze color blended with
// frame.fogColor. Surface composition (desaturate + lift toward the haze) is
// shared too, so terrain, water and entities at the same world position
// converge toward the same atmosphere.
//
// The camera-underwater composite (composite.frag.glsl) remains the sole
// authority for the medium seen by a submerged camera: callers below the
// waterline must NOT apply this air term on top of it.
//
// Requires frame_ubo.inc.glsl to be included first. Keep in sync with
// src/Renderer/Lighting.hpp (kAtmosphereFog* constants and
// atmosphereFogAmount / atmosphereHazeColor / applyAerialPerspective); the
// numeric contract is unit-tested in test_render_helpers.cpp.

// Cap must match lighting::kAtmosphereFogAmountCap
const float kAtmosphereFogCap = 0.45;
// Density scale of the exponential height term — matches
// lighting::kAtmosphereFogDensityScale
const float kAtmosphereFogDensityScale = 0.0009;
// Desaturation floor at full haze: the shared aerial-perspective grade.
// Surface chroma fades toward the surface's own luminance as the atmosphere
// thickens; 1.0 at amount 0 means untouched near-field color.
const float kAtmosphereDesatMin = 0.72;

struct AtmosphereFog
{
    float amount;       // 0..kAtmosphereFogCap, already skylight-gated
    vec3 color;         // horizon-matched haze color (linear light)
    float desaturation; // 1 = untouched chroma, lower = desaturated
};

// Pure function of the world-space surface position, the frame atmosphere
// inputs (frame.fogParams / lightingParams.z / skyParams / fogColor) and the
// caller's local skylight gate. `skyReach` is the standard enclosure gate
// smoothstep(0.05, 0.45, skyLight) (lighting::sunShadowWeight) that every
// caller already computes for lighting; passing it keeps one evaluation per
// fragment and pins the shared gating policy.
AtmosphereFog evaluateAtmosphereFog(vec3 worldPos, float skyReach)
{
    AtmosphereFog f;
    float fogStart = frame.fogParams.x;
    float fogEnd = frame.fogParams.y;
    float fogDensity = frame.fogParams.z;
    float heightFalloff = frame.fogParams.w;
    float fogBaseY = frame.lightingParams.z;
    float dist = length(worldPos - frame.viewPos.xyz);
    float linearFog = smoothstep(fogStart, max(fogStart + 1.0, fogEnd), dist);
    float avgY = 0.5 * (worldPos.y + frame.viewPos.y);
    float heightTerm = exp(-heightFalloff * max(0.0, avgY - fogBaseY));
    float densityFog = 1.0 - exp(-max(0.0, dist - fogStart * 0.25) * fogDensity *
                                 kAtmosphereFogDensityScale * heightTerm);
    f.amount = clamp(max(linearFog, densityFog), 0.0, kAtmosphereFogCap) *
               clamp(skyReach, 0.0, 1.0);

    // Sky aerial color: cool blue day → warm sunset → dark-blue night (lifted
    // from near-black so night fog doesn't swallow the world)
    vec3 dayAerial = vec3(0.40, 0.60, 0.90);
    vec3 sunsetAerial = vec3(0.95, 0.55, 0.32);
    vec3 nightAerial = vec3(0.014, 0.022, 0.048);
    float sunsetFactor = frame.skyParams.z;
    float nightFactor = frame.skyParams.w;
    vec3 aerialSky = mix(dayAerial, sunsetAerial, sunsetFactor);
    aerialSky = mix(aerialSky, nightAerial, nightFactor);
    // Blend engine fogColor with aerial sky for horizon-matched haze
    vec3 fogCol = mix(frame.fogColor.rgb, aerialSky, 0.55);
    f.color = mix(fogCol, vec3(1.0, 0.72, 0.42), sunsetFactor * 0.25);

    f.desaturation = mix(1.0, kAtmosphereDesatMin, f.amount);
    return f;
}

// Shared aerial-perspective composition: desaturate toward the surface's own
// luminance, then lift toward the haze while retaining a little surface color
// so the midground stays readable. Same blend for every material family, so
// equal world positions converge on equal atmosphere. Luminance uses the Rec.
// 709 weights of colorspace.inc.glsl kRec709Luma (kept local so this include
// stays order-independent — GLSL has no include guards).
vec3 applyAtmosphereFog(vec3 color, AtmosphereFog f)
{
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    vec3 aerialLit = mix(vec3(lum), color, f.desaturation);
    vec3 fogMix = mix(f.color, aerialLit * 0.40 + f.color * 0.60, 0.22);
    return mix(aerialLit, fogMix, f.amount);
}
