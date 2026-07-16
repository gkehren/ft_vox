#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D hdrBuffer;
layout(push_constant) uniform PC { float bloomThreshold; } pc;

void main()
{
    vec3 color = texture(hdrBuffer, vUV).rgb;
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float peak = max(color.r, max(color.g, color.b));

    // Sharper soft-knee threshold — only bright sun / emissive peaks contribute
    float thr = max(pc.bloomThreshold, 0.5);
    float soft = thr * 0.35; // narrower knee than typical → less soft wash bloom
    float contrib = 0.0;
    if (lum > thr)
        contrib = lum - thr + soft;
    else if (lum > thr - soft)
    {
        float x = lum - (thr - soft);
        contrib = (x * x) / max(4.0 * soft, 1e-4);
    }

    // Prefer warm / sun / lava-like (high R or R+G vs B) and pure bright peaks
    float warm = max(0.0, color.r - color.b) + max(0.0, 0.5 * (color.r + color.g) - color.b);
    float warmBoost = 1.0 + clamp(warm * 1.8, 0.0, 2.5);
    // Peak channel boost for sun disk / specular spikes
    float peakBoost = 1.0 + clamp((peak - thr) * 0.8, 0.0, 1.5);

    // Suppress soft green/blue midtones (sky, foliage) so bloom is sun/lava-biased
    float greenness = color.g - max(color.r, color.b);
    float coolSky = max(0.0, color.b - color.r) * max(0.0, 1.0 - peak * 0.5);
    float suppress = 1.0 - clamp(greenness * 2.2, 0.0, 0.75) - clamp(coolSky * 1.5, 0.0, 0.55);
    suppress = clamp(suppress, 0.15, 1.0);

    float scale = contrib * warmBoost * peakBoost * suppress / max(lum, 1e-4);
    vec3 bloom = max(color * scale, vec3(0.0));
    outColor = vec4(bloom, 1.0);
}
