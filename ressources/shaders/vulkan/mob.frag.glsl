#version 450
#include "frame_ubo.inc.glsl"
#include "csm.inc.glsl"
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=2) in vec2 texCoord;
layout(set=1,binding=0) uniform sampler2D mobTexture;
layout(location=0) out vec4 outColor;
void main() {
    vec4 texel=texture(mobTexture,texCoord); if(texel.a<0.5) discard;
    vec3 n=normalize(worldNormal), light=normalize(frame.lightDirection.xyz);
    float viewDepth=-(frame.view*vec4(worldPosition,1)).z;
    float shadow=viewDepth<frame.cascadeSplits.z?sampleDirectionalShadow(worldPosition,n,light,viewDepth):0.0;
    float diffuse=max(dot(n,light),0)*(1-shadow)*frame.lightParams.y;
    vec3 lighting=vec3(frame.lightParams.x+diffuse)+frame.moonAmbient.rgb*frame.moonAmbient.w*frame.skyParams.w;
    vec3 color=texel.rgb*lighting;
    float distanceToCamera=length(worldPosition-frame.viewPos.xyz);
    float fog=smoothstep(frame.fogParams.x,max(frame.fogParams.x+1,frame.fogParams.y),distanceToCamera);
    outColor=vec4(mix(color,frame.fogColor.rgb,fog),1);
}
