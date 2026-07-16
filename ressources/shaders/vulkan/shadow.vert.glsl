#version 450
// Depth-only shadow pass — cascade matrix via push constant.

layout(location = 0) in vec3 aPos;
layout(location = 1) in uint aPackedData;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in uint aPackedBiomeColor;

layout(push_constant) uniform PC {
    mat4 lightSpace;
} pc;

void main()
{
    gl_Position = pc.lightSpace * vec4(aPos, 1.0);
}
