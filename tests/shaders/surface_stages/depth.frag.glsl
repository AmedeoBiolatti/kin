#version 450 core
layout(location=0) out vec4 fColor;
layout(location=0) in struct {vec4 Color;vec2 UV;} In;
layout(set=2,binding=0) uniform sampler2D depth_image;
void main(){float d=texture(depth_image,In.UV).r;fColor=vec4(vec3(d*4.0),1.0);}
