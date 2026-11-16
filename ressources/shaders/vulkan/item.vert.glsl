#version 450
// Dropped item mini-cube (issue #229, Epic #223 Phase F): one shared unit
// cube, one instance per visible entity. The model matrix carries the
// interpolated position + presentation spin/bob; atlasAndSky selects the
// block atlas layer (registry placeableBlock) and the derived local skylight.
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in mat4 model;
layout(location=7) in vec4 atlasAndSky;
layout(location=8) in vec4 blockLight;
layout(push_constant) uniform Push { mat4 viewProjection; } pc;
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 worldNormal;
layout(location=2) out vec2 texCoord;
layout(location=3) flat out float vLayer;
layout(location=4) out float vSkyLight;
layout(location=5) out vec3 vBlockLightRGB;
void main() {
    vec4 p = model * vec4(position, 1);
    worldPosition = p.xyz;
    worldNormal = normalize(mat3(model) * normal);
    texCoord = uv;
    vLayer = atlasAndSky.x;
    vSkyLight = atlasAndSky.y;
    vBlockLightRGB = blockLight.rgb;
    gl_Position = pc.viewProjection * p;
}
