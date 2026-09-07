#version 450

// HDR luminance metering downsample (issue #140). One stage of the
// HDR -> 64x64 -> 16x16 -> 4x4 reduction feeding exposure_adapt.frag.
// Each output texel averages 16 spread taps; every tap lands on a distinct
// texel row/column of the block it covers.
//
// Only stage 0 (source = HDR RGBA16F, always linear-filterable) converts
// luminance to log2; later stages average the already-converted .r channel
// with NEAREST taps (linear filtering of R32F is an optional format
// feature). Samples are clipped to [kMinMeteredLogLum, kMaxMeteredLogLum]
// so a handful of sun-disc or emissive pixels cannot dominate the mean
// (clipped log-average policy — mirrored in Renderer/AutoExposure.hpp).

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

#include "colorspace.inc.glsl"

layout(set = 0, binding = 0) uniform sampler2D srcTexture;

layout(push_constant) uniform PC
{
    vec4 stepUV;   // xy = 1.0 / DESTINATION size: tap spread in UV for this stage
    vec4 params;   // x = sourceIsLog: 1.0 when the source is already log2 luminance
} pc;

const float kMinMeteredLogLum = -8.0; // autoexposure::kMinMeteredLogLum
const float kMaxMeteredLogLum = 8.0;  // autoexposure::kMaxMeteredLogLum
const float kMinLuminanceEps = 1e-4;  // autoexposure::kMinLuminanceEps

float sampleLogLuminance(vec2 uv)
{
    vec3 c = texture(srcTexture, uv).rgb;
    if (pc.params.x > 0.5)
        return c.r; // source is already clipped log2 luminance
    float lum = dot(c, kRec709Luma);
    return clamp(log2(max(lum, kMinLuminanceEps)), kMinMeteredLogLum, kMaxMeteredLogLum);
}

void main()
{
    float sum = 0.0;
    for (int j = 0; j < 4; ++j)
    {
        for (int i = 0; i < 4; ++i)
        {
            vec2 offset = (vec2(float(i), float(j)) + 0.5) * 0.25 - 0.5;
            sum += sampleLogLuminance(vUV + offset * pc.stepUV.xy);
        }
    }
    outColor = vec4(sum * 0.0625, 0.0, 0.0, 0.0);
}
