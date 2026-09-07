// Shared cascaded-shadow-map receiver sampling (terrain + mobs) — issue #137.
//
// BIAS POLICY
// -----------
// frame.cascadeBiasScales[c] = (2*halfExtent(c)/mapSize) / lightDepthSpan(c)
// — the receiver's normalized-depth shift per world-unit texel footprint.
// The shader multiplies it by the DIMENSIONLESS factors
// shadow::kReceiverBiasSlope/kReceiverBiasBase (mirrored here): the bias
// therefore halves when the shadow map resolution doubles and grows with
// the cascade footprint. The SENDER raster depth bias (constant 1.75,
// slope 2.5, front-face culling) handles caster-side depth precision and
// aliasing on its own and stays independent of the receiver bias; the two
// are tuned as ONE system — adjust them together, never in isolation.
//
// Sampling goes through the hardware COMPARISON sampler
// (LESS_OR_EQUAL, CLAMP_TO_BORDER with an opaque-white border = lit):
// texture() returns the 2x2 hardware-PCF-filtered lit fraction directly —
// accumulate it as-is, never re-binarize. Out-of-frustum UV/z reads the
// opaque-white border, so out-of-frustum is lit — one unified
// no-special-case policy.

layout(set = 1, binding = 1) uniform sampler2DArrayShadow shadowMap;

// Keep in sync with shadow::kReceiverBiasSlope / kReceiverBiasBase.
const float CSM_BIAS_SLOPE = 22.0; // dimensionless: * normalized-depth-per-texel
const float CSM_BIAS_BASE = 6.5;   // dimensionless floor factor

// 12-tap Poisson disk (unit radius)
const vec2 CSM_POISSON[12] = vec2[](
    vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696,  0.457),
    vec2(-0.203,  0.621), vec2( 0.962, -0.195), vec2( 0.473, -0.480),
    vec2( 0.519,  0.767), vec2( 0.185, -0.893), vec2( 0.507,  0.064),
    vec2( 0.896,  0.412), vec2(-0.322, -0.932), vec2(-0.792, -0.598)
);

// Stable per-shadow-map-texel rotation angle in [0, 2π): hashed from the
// texel-snapped light-space position (+cascade), NOT gl_FragCoord, so the
// disk orientation follows the world through camera translation/rotation
// instead of crawling across the surface.
float csmTexelRotationAngle(ivec2 texel, int cascade)
{
    uint h = uint(texel.x) * 73856093u ^ uint(texel.y) * 19349663u ^ uint(cascade) * 83492791u;
    h = (h ^ (h >> 16u)) * 0x45d9f3bu;
    h = (h ^ (h >> 16u)) * 0x45d9f3bu;
    h ^= h >> 16u;
    return 6.2831853 * float(h & 0xFFFFFFu) / float(0xFFFFFFu);
}

mat4 csmCascadeMatrix(int c)
{
    if (c == 0) return frame.cascadeMatrix0;
    if (c == 1) return frame.cascadeMatrix1;
    return frame.cascadeMatrix2;
}

// LIT fraction (0.0 = fully shadowed, 1.0 = fully lit) for one cascade.
float csmSampleCascadeLit(vec3 worldPos, vec3 normal, vec3 lightDir, int cascade)
{
    vec4 fragPosLS = csmCascadeMatrix(cascade) * vec4(worldPos, 1.0);
    vec3 projCoords = fragPosLS.xyz / max(fragPosLS.w, 1e-6);
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // Outside the cascade frustum or depth range => lit (border is opaque
    // white); unified no-leak special-case policy.
    if (projCoords.z < 0.0 || projCoords.z > 1.0 ||
        projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0)
        return 1.0;

    float currentDepth = projCoords.z;
    float nDotL = max(dot(normal, lightDir), 0.0);
    // Receiver bias scales with the real normalized-depth texel footprint
    // (see BIAS POLICY); resolution changes flow straight through.
    float bias = max(CSM_BIAS_SLOPE * (1.0 - nDotL), CSM_BIAS_BASE)
               * frame.cascadeBiasScales[cascade];

    // Radius in UV from the real shadow map resolution (no hardcoded size).
    vec2 shadowMapSize = vec2(textureSize(shadowMap, 0).xy);
    vec2 radius = (1.5 + float(cascade)) / shadowMapSize;

    // Rotation keyed on the (stabilized) shadow-map texel: world-stable.
    ivec2 texel = ivec2(floor(projCoords.xy * shadowMapSize));
    float angle = csmTexelRotationAngle(texel, cascade);
    float s = sin(angle);
    float c = cos(angle);
    mat2 rot = mat2(c, -s, s, c);

    // Hardware comparison PCF: texture() already returns the 2x2-filtered
    // lit fraction — accumulate directly (review P1: < 0.5 re-binarization
    // would discard it).
    float lit = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        vec2 uv = projCoords.xy + rot * (CSM_POISSON[i] * radius * 2.5);
        lit += texture(shadowMap, vec4(uv, float(cascade), currentDepth - bias));
    }
    return lit / 12.0;
}

// SHADOW amount (0.0 = fully lit, 1.0 = fully shadowed) — same semantics as
// the legacy terrain ShadowCalculation. Cascade select + 0.12-gap blend band
// at the top of cascades 0 and 1.
float sampleDirectionalShadow(vec3 worldPos, vec3 normal, vec3 lightDir, float viewDepth)
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

    float lit = csmSampleCascadeLit(worldPos, normal, lightDir, cascade);

    if (cascade < 2) {
        float gap = max(splitEnd - splitStart, 1.0);
        float band = gap * 0.12;
        float edge = splitEnd - band;
        if (viewDepth > edge) {
            float w = clamp((viewDepth - edge) / max(band, 1e-3), 0.0, 1.0);
            float litNext = csmSampleCascadeLit(worldPos, normal, lightDir, cascade + 1);
            lit = mix(lit, litNext, w);
        }
    }
    return 1.0 - lit;
}
