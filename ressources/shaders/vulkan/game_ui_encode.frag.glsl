#version 450
#extension GL_GOOGLE_include_directive : require
#include "colorspace.inc.glsl"

layout(set = 0, binding = 0) uniform sampler2D displayLinear;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 pixel = texelFetch(displayLinear, ivec2(gl_FragCoord.xy), 0);
    outColor = vec4(linearToSrgb(clamp(pixel.rgb, 0.0, 1.0)), pixel.a);
}
