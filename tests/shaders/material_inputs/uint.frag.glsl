#version 450 core
#include "kin_material.inc.glsl"
layout(set=2,binding=12) uniform usampler2D input_texture;
void main() { FragColor=kin_output(vec3(texture(input_texture,kin_uv()).r)/255.0,1.0); }
