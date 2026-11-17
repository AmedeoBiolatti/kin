#version 450 core
layout(location=0) in vec2 vUV;
layout(set=2,binding=0) uniform sampler2D source;
layout(set=3,binding=0) uniform Params { vec4 flags; } u; // mip
layout(location=0) out vec4 colour;
void main() {
    ivec2 size=textureSize(source,int(u.flags.x));
    ivec2 pixel=clamp(ivec2(vUV*vec2(size)),ivec2(0),size-1);
    colour=texelFetch(source,pixel,int(u.flags.x));
}
