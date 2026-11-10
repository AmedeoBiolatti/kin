#version 450 core
layout(location=0) out vec4 fColor;
layout(set=2,binding=0) uniform sampler2D source;
layout(location=0) in struct {vec4 Color;vec2 UV;} In;
void main() {vec4 c=texture(source,In.UV);fColor=vec4(c.rgb/8.0,1.0);}
