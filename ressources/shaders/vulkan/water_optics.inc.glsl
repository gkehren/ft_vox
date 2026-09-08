// Shared water-medium optical model (issue #144).
// One documented source for the constants used by the surface water
// (water.frag.glsl) and the camera-underwater path (composite.frag.glsl).
// Rates are per metre in linear-light HDR (issue #135 pipeline).

// Beer-Lambert absorption: red dies first -> teal body.
const vec3 WATER_SIGMA = vec3(0.42, 0.16, 0.10) * 0.35;
// In-scatter tint added back as the optical path grows.
const vec3 WATER_SCATTER_COLOR = vec3(0.015, 0.14, 0.24);
// Rate of the in-scatter buildup toward WATER_SCATTER_COLOR.
const float WATER_SCATTER_RATE = 0.22;
// Column distance used for sky/far-plane pixels so the water surface seen
// from below stays readable instead of collapsing to pure scatter color.
const float WATER_SKY_COLUMN = 28.0;

// Ambient scatter brightness from the day cycle. directVisibility is the
// shadow factor (0 = fully shadowed); the camera-underwater path passes 1.0
// (no CSM sample available in composite — the sun-elevation caustic gate
// plays the same role there).
float waterScatterAmbient(float dayFactor, float sunsetFactor, float directVisibility)
{
    return (dayFactor * 0.9 + sunsetFactor * 0.55) * (0.25 + 0.75 * directVisibility) + 0.03;
}

// --- Value noise shared by water waves and underwater caustics -------------
float whash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float wnoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(whash(i), whash(i + vec2(1.0, 0.0)), f.x),
               mix(whash(i + vec2(0.0, 1.0)), whash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Procedural sun caustics: two drifting value-noise octaves interfere into
// bright connected ridges. p is world XZ, t the pinned animation time.
float causticPattern(vec2 p, float t)
{
    float n1 = wnoise(p * 0.55 + vec2(t * 0.16, -t * 0.12));
    float n2 = wnoise(p * 1.30 - vec2(t * 0.10, t * 0.21));
    float v = n1 * 0.6 + n2 * 0.4;
    float ridge = 1.0 - abs(v * 2.0 - 1.0);
    return pow(clamp(ridge, 0.0, 1.0), 3.0) * 1.5;
}
