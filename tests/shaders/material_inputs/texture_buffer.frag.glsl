#version 450 core
#include "kin_material.inc.glsl"
layout(set=2,binding=12) uniform sampler2D input_texture;
layout(std430,set=2,binding=13) readonly buffer Input { vec4 values[]; } data;
void main() { FragColor=kin_output(texture(input_texture,kin_uv()).rgb*data.values[0].rgb,1.0); }
