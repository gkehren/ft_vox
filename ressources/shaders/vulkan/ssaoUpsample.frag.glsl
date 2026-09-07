#version 450
// Joint bilateral upsample of the half-res SSAO target (raw AO + encoded
// normal) to the full-res AO buffer consumed by composite.frag.
//
// The four TRUE half-res texels straddling the output pixel are combined
// (texelFetch — no sampler filtering, so each tap is exactly one texel)
// with a joint bilateral weight: bilinear spatial weight x depth weight
// (10% relative view-depth sigma, view-space meters via invProj, sampled at
// each tap's texel CENTER so depth and AO describe the same surface patch).
// Sky taps get zero weight.
//
// Fallback when every depth weight underflows: the valid (non-sky) tap with
// the smallest |ΔviewZ| — not merely the spatially-nearest one — and only if
// it is genuinely depth-compatible (within 3 sigma); otherwise output 1.
// Background AO can therefore never land on thin foreground.
//
// Output (full-res R8 UNORM target): r = upsampled AO (only r is stored;
// the encoded half-res normal is re-read directly from the half-res RGBA
// target by composite.frag debug views).

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D ssaoHalfRes; // half-res RGBA8: r = AO, gba = encoded normal
layout(set = 0, binding = 1) uniform sampler2D depthBuffer; // full-res scene depth (nearest)

layout(push_constant) uniform PC {
    vec4 p0; // xy = inv full-res resolution, zw unused
    mat4 invProj;
} pc;

const float kSkyDepth = 0.9990;

vec3 viewPosFromDepth(vec2 uv, float depth)
{
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = pc.invProj * clip;
    return view.xyz / max(view.w, 1e-5);
}

void main()
{
    const vec2 halfSize = vec2(textureSize(ssaoHalfRes, 0));
    const ivec2 halfSizeI = ivec2(halfSize);

    float zc = texture(depthBuffer, vUV).r;
    if (zc >= kSkyDepth)
    {
        // Sky: unoccluded, neutral encoded (0,0,1) normal.
        outColor = vec4(1.0, 0.5, 0.5, 1.0);
        return;
    }
    float zvC = viewPosFromDepth(vUV, zc).z;
    const float depthSigma = max(0.1 * abs(zvC), 1e-3); // 10% relative view-depth sigma (meters)

    // Continuous half-res texel coordinate (textureSize — not
    // 2/resolution — so odd full-res sizes are handled exactly). The four
    // taps are the true texels surrounding the output pixel.
    vec2 pHalf = vUV * halfSize - 0.5;
    ivec2 base = ivec2(floor(pHalf));
    vec2 f = fract(pHalf);

    float aoAcc = 0.0;
    float wAcc = 0.0;
    float bestAo = 1.0;
    float bestDepthDiff = 3.4e38;
    bool haveBest = false;

    for (int dy = 0; dy < 2; ++dy)
    {
        for (int dx = 0; dx < 2; ++dx)
        {
            ivec2 tc = clamp(base + ivec2(dx, dy), ivec2(0), halfSizeI - 1);
            vec2 w2 = mix(vec2(1.0) - f, f, vec2(float(dx), float(dy)));
            float spatial = w2.x * w2.y;

            vec4 tap = texelFetch(ssaoHalfRes, tc, 0);

            // Representative full-res depth at this half-res texel's CENTER,
            // so the depth weight and the fetched AO describe the same patch.
            vec2 centerUV = (vec2(tc) + 0.5) / halfSize;
            float td = texture(depthBuffer, centerUV).r;

            float w = 0.0;
            if (td < kSkyDepth) // sky tap -> weight 0
            {
                float tapZ = viewPosFromDepth(centerUV, td).z;
                float depthDiff = abs(tapZ - zvC);
                w = spatial * exp(-depthDiff / depthSigma);
                if (!haveBest || depthDiff < bestDepthDiff)
                {
                    haveBest = true;
                    bestDepthDiff = depthDiff;
                    bestAo = tap.r;
                }
            }

            aoAcc += w * tap.r;
            wAcc += w;
        }
    }

    // Degenerate neighborhoods (all depth weights underflowed): use the
    // depth-nearest valid tap ONLY if it is genuinely depth-compatible with
    // the pixel (within 3 sigma); otherwise stay unoccluded. Falling back to
    // a mere "non-sky" tap would paint background AO onto thin foreground —
    // exactly the halo this pass exists to prevent.
    bool depthCompatible = haveBest && bestDepthDiff < 3.0 * depthSigma;
    float ao = wAcc > 1e-4 ? aoAcc / wAcc : (depthCompatible ? bestAo : 1.0);

    outColor = vec4(ao); // R8 target: only r is stored
}
