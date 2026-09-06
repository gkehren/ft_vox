#version 450
#include "frame_ubo.inc.glsl"
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=2) in vec2 texCoord;
layout(set=1,binding=0) uniform sampler2D mobTexture;
layout(set=1,binding=1) uniform sampler2DArray shadowMap;
layout(location=0) out vec4 outColor;
void main() {
    vec4 texel=texture(mobTexture,texCoord); if(texel.a<0.5) discard;
    vec3 n=normalize(worldNormal), light=normalize(frame.lightDirection.xyz);
    float viewDepth=-(frame.view*vec4(worldPosition,1)).z;
    int c=viewDepth<frame.cascadeSplits.x?0:(viewDepth<frame.cascadeSplits.y?1:2);
    mat4 matrix=c==0?frame.cascadeMatrix0:(c==1?frame.cascadeMatrix1:frame.cascadeMatrix2);
    vec4 ls=matrix*vec4(worldPosition,1);vec3 p=ls.xyz/ls.w;p.xy=p.xy*0.5+0.5;
    float shadow=0;
    if(viewDepth<frame.cascadeSplits.z && all(greaterThanEqual(p,vec3(0))) && all(lessThanEqual(p,vec3(1)))) {
        vec2 texelSize=1.0/vec2(textureSize(shadowMap,0).xy);
        float bias=max(0.001*(1-dot(n,light)),0.0003);
        for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
            shadow+=p.z-bias>texture(shadowMap,vec3(p.xy+vec2(x,y)*texelSize,c)).r?1:0;
        shadow/=9;
    }
    float diffuse=max(dot(n,light),0)*(1-shadow)*frame.lightParams.y;
    vec3 lighting=vec3(frame.lightParams.x+diffuse)+frame.moonAmbient.rgb*frame.moonAmbient.w*frame.skyParams.w;
    vec3 color=texel.rgb*lighting;
    float distanceToCamera=length(worldPosition-frame.viewPos.xyz);
    float fog=smoothstep(frame.fogParams.x,max(frame.fogParams.x+1,frame.fogParams.y),distanceToCamera);
    outColor=vec4(mix(color,frame.fogColor.rgb,fog),1);
}
