#version 450

layout(push_constant) uniform View {
    vec2 scale;
} view;

layout(location = 0) in vec4 rect;
layout(location = 1) in vec4 uvRect;
layout(location = 2) in vec4 color;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 tint;

void main() {
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1),
                                   vec2(0, 1), vec2(1, 0), vec2(1, 1));
    vec2 corner = corners[gl_VertexIndex];
    vec2 logical = mix(rect.xy, rect.zw, corner);
    gl_Position = vec4(logical * view.scale - 1.0, 0.0, 1.0);
    uv = mix(uvRect.xy, uvRect.zw, corner);
    tint = color;
}
