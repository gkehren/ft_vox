#version 450
layout(location=2) in vec2 texCoord;
layout(set=1,binding=0) uniform sampler2D mobTexture;
void main() { if(texture(mobTexture,texCoord).a<0.5) discard; }
