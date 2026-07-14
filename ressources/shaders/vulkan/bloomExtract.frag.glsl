#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D hdrBuffer;
layout(push_constant) uniform PC { float bloomThreshold; } pc;
void main() {
    vec3 color = texture(hdrBuffer, vUV).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    outColor = brightness > pc.bloomThreshold ? vec4(color, 1.0) : vec4(0.0);
}
