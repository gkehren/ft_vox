#version 450
#extension GL_GOOGLE_include_directive : require
#include "colorspace.inc.glsl"

layout(set = 0, binding = 0) uniform sampler2D font;
layout(set = 0, binding = 1) uniform sampler2DArray atlas;
layout(location = 0) in vec2 uv;
layout(location = 1) flat in vec4 tint;
layout(location = 2) flat in int textureLayer;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 art = vec4(1.0);
    if (textureLayer == -2)
        art.a = texture(font, uv).r;
    else if (textureLayer >= 0) {
        // The atlas view decodes sRGB RGB and leaves alpha linear.
        art = textureLayer < textureSize(atlas, 0).z ? texture(atlas, vec3(uv, textureLayer)) : vec4(0.0);
    }
    float alpha = art.a * tint.a;
    outColor = vec4(srgbToLinear(tint.rgb) * art.rgb * alpha, alpha);
}
