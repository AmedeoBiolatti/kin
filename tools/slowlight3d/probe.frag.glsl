#version 450 core
layout(location=0) out vec4 fColor;
layout(set=2,binding=0) uniform sampler2D field;
layout(location=0) in struct {vec4 Color;vec2 UV;} In;
void main() {
    ivec2 size=textureSize(field,0);
    vec4 value=texelFetch(field,clamp(ivec2(In.UV*vec2(size)),ivec2(0),size-1),0);
    fColor=vec4(clamp(value.x/8.0,0.0,1.0),clamp(value.y/4.0,0.0,1.0),0.5*(value.z+value.w),1.0);
}
