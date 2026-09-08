#version 450
// Dedicated spatial anti-aliasing pass (issue #143): FXAA 3.11, "quality" PC
// path, preset 12. Ported from the reference Fxaa3_11.h by Timothy Lottes
// (NVIDIA) — mirrored at
// github.com/GameTechDev/CMAA2/blob/master/Projects/CMAA2/FXAA/Fxaa3_11.h —
// replacing the abbreviated in-composite approximation that estimated only a
// local sub-pixel offset with no directional span search.
//
// Color-space contract (issue #143): the input is the composite pass's
// tone-mapped, graded LDR output, sRGB-ENCODED into the R8G8B8A8_UNORM
// `ldrColor` target. Edge detection, the span search and the final blend all
// run on those perceptual (display-encoded) values — the space FXAA 3.11 was
// designed for. Thresholds are therefore never evaluated on raw linear HDR
// (the old approximation's mistake). Output goes to the swapchain: for a
// hardware-sRGB attachment the result is decoded back to display-linear
// (pc.p0.z = 1) so the attachment's fixed-function encode round-trips the
// same 8-bit values; for a UNORM swapchain the encoded values pass through.
//
// Voxel-sharpness tuning (issue #143): subpix 0.75 (reference default) keeps
// the sub-pixel term moderate so magnified nearest-neighbour texels stay
// crisp; the directional span search carries the silhouette work.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D ldrBuffer;

layout(push_constant) uniform PC {
    vec4 p0; // xy = 1/frameSize, z = decodeSrgbOut (1.0 = hardware-sRGB swapchain), w unused
} pc;

#include "colorspace.inc.glsl"

// FXAA reference luma: Rec. 601 weights on perceptual (encoded) values —
// deliberately NOT the Rec. 709 display luma in kRec709Luma.
const vec3 kFxaaLuma = vec3(0.299, 0.587, 0.114);

float lumaAt(vec2 uv)
{
    return dot(texture(ldrBuffer, uv).rgb, kFxaaLuma);
}

// FXAA_QUALITY__PRESET 12 (medium dither): 5 unrolled search steps —
// P0 is the initial ±1 texel probe, then 1.5 / 2.0 / 4.0 / 12.0, i.e. the
// endpoint search can walk up to ±20.5 texels along an edge.
const float kStepP0 = 1.0;
const float kStepP1 = 1.5;
const float kStepP2 = 2.0;
const float kStepP3 = 4.0;
const float kStepP4 = 12.0;

// Reference tuning knobs (FxaaPixelShader inputs).
const float kSubpix = 0.75;           // 1.0 softer, 0.5 sharper
const float kEdgeThreshold = 0.166;   // minimum local contrast to apply AA
const float kEdgeThresholdMin = 0.0833; // trims the algorithm from darks

void main()
{
    const vec2 rcpFrame = pc.p0.xy;

    vec4 rgbyM = texture(ldrBuffer, vUV);
    vec2 posM = vUV;
    float lumaM = dot(rgbyM.rgb, kFxaaLuma);
    // Reference FxaaTexOff probes: ±1 texel around the center (N/S/E/W).
    float lumaS = lumaAt(posM + vec2(0.0, rcpFrame.y));
    float lumaE = lumaAt(posM + vec2(rcpFrame.x, 0.0));
    float lumaN = lumaAt(posM + vec2(0.0, -rcpFrame.y));
    float lumaW = lumaAt(posM + vec2(-rcpFrame.x, 0.0));

    // Early exit: local contrast below threshold (covers flat interiors and
    // darks via kEdgeThresholdMin) — the crisp-texture guard.
    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    vec3 result = rgbyM.rgb;
    if (range >= max(kEdgeThresholdMin, rangeMax * kEdgeThreshold))
    {
        // Full FXAA path (reference FxaaPixelShader body after the early
        // exit). Everything below runs on perceptual luma of encoded values.

        float lumaNW = lumaAt(posM + vec2(-rcpFrame.x, -rcpFrame.y));
        float lumaSE = lumaAt(posM + vec2(rcpFrame.x, rcpFrame.y));
        float lumaNE = lumaAt(posM + vec2(rcpFrame.x, -rcpFrame.y));
        float lumaSW = lumaAt(posM + vec2(-rcpFrame.x, rcpFrame.y));

        // Edge direction: compare horizontal vs vertical luma second
        // derivatives across the 3x3 neighborhood (reference edgeHorz/Vert chain).
        float lumaNS = lumaN + lumaS;
        float lumaWE = lumaW + lumaE;
        float edgeHorz1 = (-2.0 * lumaM) + lumaNS;
        float edgeVert1 = (-2.0 * lumaM) + lumaWE;
        float lumaNESE = lumaNE + lumaSE;
        float lumaNWNE = lumaNW + lumaNE;
        float edgeHorz2 = (-2.0 * lumaE) + lumaNESE;
        float edgeVert2 = (-2.0 * lumaN) + lumaNWNE;
        float lumaNWSW = lumaNW + lumaSW;
        float lumaSWSE = lumaSW + lumaSE;
        float edgeHorz4 = (abs(edgeHorz1) * 2.0) + abs(edgeHorz2);
        float edgeVert4 = (abs(edgeVert1) * 2.0) + abs(edgeVert2);
        float edgeHorz3 = (-2.0 * lumaW) + lumaNWSW;
        float edgeVert3 = (-2.0 * lumaS) + lumaSWSE;
        float edgeHorz = abs(edgeHorz3) + edgeHorz4;
        float edgeVert = abs(edgeVert3) + edgeVert4;

        float subpixNSWE = lumaNS + lumaWE;
        bool horzSpan = edgeHorz >= edgeVert;
        float subpixNWSWNESE = lumaNWSW + lumaNESE;
        float lengthSign = rcpFrame.x;
        float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
        if (!horzSpan)
        {
            lumaN = lumaW;
            lumaS = lumaE;
        }
        else
        {
            lengthSign = rcpFrame.y;
        }
        float subpixB = (subpixA * (1.0 / 12.0)) - lumaM;

        // Pick the edge pair with the stronger gradient; the search starts
        // from the half-texel-adjacent sample pair across that edge.
        float gradientN = lumaN - lumaM;
        float gradientS = lumaS - lumaM;
        bool pairN = abs(gradientN) >= abs(gradientS);
        float gradient = max(abs(gradientN), abs(gradientS));
        float lumaNN = lumaN + lumaM;
        float lumaSS = lumaS + lumaM;
        if (!pairN)
            lumaNN = lumaSS;
        if (pairN)
            lengthSign = -lengthSign;
        float subpixC = clamp(abs(subpixB) / range, 0.0, 1.0);

        vec2 posB = posM;
        vec2 offNP = horzSpan ? vec2(rcpFrame.x, 0.0) : vec2(0.0, rcpFrame.y);
        if (!horzSpan)
            posB.x += lengthSign * 0.5;
        if (horzSpan)
            posB.y += lengthSign * 0.5;

        // Endpoint search: walk both directions along the edge until the luma
        // delta (scaled by 0.5 against the chosen pair) exceeds gradient/4.
        vec2 posN = posB - offNP * kStepP0;
        vec2 posP = posB + offNP * kStepP0;
        float lumaEndN = lumaAt(posN);
        float lumaEndP = lumaAt(posP);
        float gradientScaled = gradient * 0.25;
        float lumaMM = lumaM - lumaNN * 0.5;
        bool lumaMLTZero = lumaMM < 0.0;
        lumaEndN -= lumaNN * 0.5;
        lumaEndP -= lumaNN * 0.5;
        bool doneN = abs(lumaEndN) >= gradientScaled;
        bool doneP = abs(lumaEndP) >= gradientScaled;
        if (!doneN)
            posN -= offNP * kStepP1;
        if (!doneP)
            posP += offNP * kStepP1;

        // Steps P2..P4: sample the (possibly advanced) endpoints, stop when
        // both directions are resolved; the final P4 advance never re-samples,
        // so the last accepted endpoint values are the post-P3 ones
        // (reference behavior for preset 12).
        const float kAdvSteps[3] = float[3](kStepP2, kStepP3, kStepP4);
        for (int i = 0; i < 3; ++i)
        {
            if (doneN && doneP)
                break;
            if (!doneN)
            {
                lumaEndN = lumaAt(posN) - lumaNN * 0.5;
                doneN = abs(lumaEndN) >= gradientScaled;
            }
            if (!doneP)
            {
                lumaEndP = lumaAt(posP) - lumaNN * 0.5;
                doneP = abs(lumaEndP) >= gradientScaled;
            }
            if (!doneN)
                posN -= offNP * kAdvSteps[i];
            if (!doneP)
                posP += offNP * kAdvSteps[i];
        }

        // Blend: distance to the nearer endpoint along the span, flipped to
        // the side whose endpoint luma continues the edge, plus the sub-pixel
        // term (reference subpixC..H chain:
        // subpixD = 3 - 2c, subpixE = c², subpixF = D*E, subpixH = F²·subpix).
        float dstN = posM.x - posN.x;
        float dstP = posP.x - posM.x;
        if (!horzSpan)
            dstN = posM.y - posN.y;
        if (!horzSpan)
            dstP = posP.y - posM.y;

        bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
        float spanLength = dstP + dstN;
        bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
        float spanLengthRcp = 1.0 / spanLength;
        bool directionN = dstN < dstP;
        float dstMin = min(dstN, dstP);
        bool goodSpan = directionN ? goodSpanN : goodSpanP;
        float pixelOffset = (dstMin * (-spanLengthRcp)) + 0.5;
        float subpixD = (-2.0 * subpixC) + 3.0;
        float subpixE = subpixC * subpixC;
        float subpixF = subpixD * subpixE;
        float subpixH = subpixF * subpixF * kSubpix;

        float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
        float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
        if (!horzSpan)
            posM.x += pixelOffsetSubpix * lengthSign;
        if (horzSpan)
            posM.y += pixelOffsetSubpix * lengthSign;

        result = texture(ldrBuffer, posM).rgb;
    }

    // Output transfer: for a hardware-sRGB swapchain the blend result is
    // decoded back to display-linear so the attachment's fixed-function
    // encode round-trips the same 8-bit values. Applies to BOTH the blended
    // and the early-exit path — the early exit stores encoded values too
    // (skipping this decode double-encodes flat regions; the +20% underwater
    // luma drift that first shipped with this pass).
    if (pc.p0.z > 0.5)
        result = srgbToLinear(result);
    outColor = vec4(result, 1.0);
}
