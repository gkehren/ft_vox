// Shared color-space transfer functions (issue #135).
// Must stay in sync with src/Renderer/ColorSpace.hpp.
// Inputs are expected clamped to [0, 1].

// Standard IEC 61966-2-1 sRGB decode: non-linear sRGB -> linear light.
vec3 srgbToLinear(vec3 c)
{
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 higher = pow((c + vec3(0.055)) / vec3(1.055), vec3(2.4));
    vec3 lower = c / vec3(12.92);
    return mix(higher, lower, cutoff);
}

// Standard IEC 61966-2-1 sRGB encode: linear light -> non-linear sRGB.
vec3 linearToSrgb(vec3 c)
{
    bvec3 cutoff = lessThanEqual(c, vec3(0.0031308));
    vec3 higher = vec3(1.055) * pow(c, vec3(1.0 / 2.4)) - vec3(0.055);
    vec3 lower = c * vec3(12.92);
    return mix(higher, lower, cutoff);
}

// Rec. 709 linear-light luminance weights — valid on linear-light RGB only.
// (Perceptual/FXAA luminance keeps the Rec. 601-on-sqrt convention.)
const vec3 kRec709Luma = vec3(0.2126, 0.7152, 0.0722);

// Contrast in luminance, pivoted at linear middle gray. Unlike an affine
// contrast around 0.5 this cannot clip positive shadow detail to black.
vec3 gradeContrast(vec3 color, float contrast)
{
    color = max(color, vec3(0.0));
    float y = dot(color, kRec709Luma);
    return color * pow(max(y, 1e-6) / 0.18, clamp(contrast, 0.5, 1.5) - 1.0);
}

// Limit saturation to the nonnegative RGB gamut instead of clipping individual
// channels (which changes hue and produces neon greens in deep shadows).
vec3 gradeSaturation(vec3 color, float saturation)
{
    color = max(color, vec3(0.0));
    float y = dot(color, kRec709Luma);
    float lo = min(color.r, min(color.g, color.b));
    float limit = y / max(y - lo, 1e-6);
    return mix(vec3(y), color, min(max(saturation, 0.0), limit));
}
