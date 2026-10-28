#version 450 core
#include "kin_material.inc.glsl"
layout(set=2,binding=12) uniform sampler2DArray input_texture;
void main() { FragColor=kin_output(vec3(texture(input_texture,vec3(kin_uv(),kin_param(0).x)).r),1.0); }
