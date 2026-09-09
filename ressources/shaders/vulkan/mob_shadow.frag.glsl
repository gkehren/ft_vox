#version 450
#include "cutout.inc.glsl"
layout(location=2) in vec2 texCoord;
layout(location=3) in float vSkyLight;
layout(location=4) in vec3 vBlockLightRGB;
layout(set=1,binding=0) uniform sampler2D mobTexture;
void main() { if(texture(mobTexture,texCoord).a < kAlphaCutoutThreshold) discard; }
