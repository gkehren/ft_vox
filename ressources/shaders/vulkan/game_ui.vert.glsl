#version 450

layout(push_constant) uniform View { vec2 scale; } view;
layout(location = 0) in vec4 rect;
layout(location = 1) in vec4 uvRect;
layout(location = 2) in vec4 color;
layout(location = 3) in int layer;
layout(location = 0) out vec2 uv;
layout(location = 1) flat out vec4 tint;
layout(location = 2) flat out int textureLayer;

void main() {
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1),
                                   vec2(0, 1), vec2(1, 0), vec2(1, 1));
    vec2 corner = corners[gl_VertexIndex];
    gl_Position = vec4(mix(rect.xy, rect.zw, corner) * view.scale - 1.0, 0.0, 1.0);
    uv = mix(uvRect.xy, uvRect.zw, corner);
    tint = color;
    textureLayer = layer;
}
