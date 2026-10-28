#version 450 core
#include "kin_material.inc.glsl"
layout(set=2,binding=12) uniform sampler2DArrayShadow input_texture;
void main() { FragColor=kin_output(vec3(texture(input_texture,vec4(kin_uv(),kin_param(0).x,kin_param(0).y))),1.0); }
