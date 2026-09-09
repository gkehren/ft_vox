#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in mat4 model;
layout(location=7) in vec4 uvScale;
layout(location=8) in vec4 localLight;
layout(push_constant) uniform Push { mat4 viewProjection; } pc;
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 worldNormal;
layout(location=2) out vec2 texCoord;
layout(location=3) out float vSkyLight;
layout(location=4) out vec3 vBlockLightRGB;
void main() {
    vec4 p=model*vec4(position,1);
    worldPosition=p.xyz; worldNormal=normalize(mat3(model)*normal);
    texCoord=uv*uvScale.xy;
    vSkyLight=localLight.x;
    vBlockLightRGB=localLight.yzw;
    gl_Position=pc.viewProjection*p;
}

