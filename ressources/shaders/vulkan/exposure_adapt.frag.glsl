#version 450

// Exposure adaptation, final metering stage (issue #140). Averages the 4x4
// log-luminance texture with an exact 16-texel texelFetch (no filtering
// involved), derives the target exposure (middle grey + EV compensation,
// clamped to [minEv, maxEv] EV) and moves the shared adaptation history
// toward it with a frame-rate independent exponential
// (alpha = 1 - exp(-speed * dt)).
//
// Asymmetric speeds: the eye stops down fast when the scene brightens and
// dilates slower when it darkens. dt <= 0 leaves the exposure strictly
// unchanged (paused frames), and the exact-settle rule fires only when the
// adapted value is already within 1e-4 EV of the target - never on step
// size, so tiny steps can never snap the exposure to the target.
//
// When useSeed is set (mode change, startup) the history restarts from the
// current manual exposure instead of the stale value, so re-engaging auto
// mode has no hidden jump.
//
// Binding 1 is the ONE shared adaptation history (the temporal state is a
// single logical value; ordering across frames-in-flight is provided by a
// barrier plus same-queue in-order execution). Binding 2 is this frame
// slot's CPU-visible snapshot for the debug UI, read after the slot fence
// has been waited. Formulas are mirrored (and unit-tested) in
// Renderer/AutoExposure.hpp.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTexture; // 4x4 log-lum chain
layout(set = 0, binding = 1) buffer ExposureHistory
{
    float adaptedExposure; // linear exposure multiplier (composite input)
    float targetExposure;  // clamped target exposure this frame (debug)
    float meteredLogLum;   // log2 of clipped geometric-mean luminance (debug)
    uint clampState;       // 0 in range, 1 min clamp, 2 max clamp (debug)
} history;
layout(set = 0, binding = 2) buffer ExposureSnapshot
{
    float adaptedExposure;
    float targetExposure;
    float meteredLogLum;
    uint clampState;
} snapshot;

layout(push_constant) uniform PC
{
    vec4 p0; // x=dt (s, clamped CPU-side), y=speedUp, z=speedDown, w=useSeed
    vec4 p1; // x=seedExposure, y=target pre-tonemap luminance, z=compensationEv, w=minEv
    vec4 p2; // x=maxEv, yzw unused
} pc;

const float kMinLuminanceEps = 1e-4; // autoexposure::kMinLuminanceEps

void main()
{
    // Exact average of the 4x4 log-luminance texture (every texel exactly
    // once).
    float sum = 0.0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            sum += texelFetch(srcTexture, ivec2(x, y), 0).r;
    float meteredLogLum = sum * 0.0625;

    // The key is a pre-tonemap luminance, not display white. The shipped
    // 0.18 key and +1 EV ceiling arrive from PostProcessSettings; do not
    // hard-code a second gain here or in composite (manual mode stays exact).
    float targetLogEv = log2(max(pc.p1.y, kMinLuminanceEps)) - meteredLogLum + pc.p1.z;
    uint clampState = 0;
    if (targetLogEv <= pc.p1.w)
        clampState = 1;
    else if (targetLogEv >= pc.p2.x)
        clampState = 2;
    targetLogEv = clamp(targetLogEv, pc.p1.w, pc.p2.x);

    float curLogEv = pc.p0.w > 0.5
                         ? log2(max(pc.p1.x, kMinLuminanceEps)) // re-engage from manual exposure
                         : log2(max(history.adaptedExposure, kMinLuminanceEps));

    float newLogEv = curLogEv;
    if (pc.p0.x > 0.0) // dt <= 0: paused frame, leave the exposure unchanged
    {
        float speed = targetLogEv < curLogEv ? pc.p0.y : pc.p0.z;
        float alpha = 1.0 - exp(-speed * pc.p0.x);
        newLogEv = mix(curLogEv, targetLogEv, alpha);
        if (abs(targetLogEv - newLogEv) < 1e-4)
            newLogEv = targetLogEv; // arrived: settle exactly, no micro-drift
    }

    history.adaptedExposure = exp2(newLogEv);
    history.targetExposure = exp2(targetLogEv);
    history.meteredLogLum = meteredLogLum;
    history.clampState = clampState;
    snapshot.adaptedExposure = history.adaptedExposure;
    snapshot.targetExposure = history.targetExposure;
    snapshot.meteredLogLum = history.meteredLogLum;
    snapshot.clampState = history.clampState;

    outColor = vec4(history.adaptedExposure, history.targetExposure, meteredLogLum, 1.0);
}
