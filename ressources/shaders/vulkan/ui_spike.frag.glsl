#version 450
#extension GL_GOOGLE_include_directive : require
#include "colorspace.inc.glsl"

layout(constant_id = 0) const bool srgbAttachment = true;
layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 tint;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 art = texture(atlas, uv);
    // The spike uses opaque swatches and cutout glyphs on either target format.
    if (art.a < 0.5)
        discard;
    vec3 encoded = art.rgb * tint.rgb;
    outColor = vec4(srgbAttachment ? srgbToLinear(encoded) : encoded, 1.0);
}
