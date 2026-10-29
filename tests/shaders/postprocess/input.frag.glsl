#version 450 core
#include "kin_material.inc.glsl"
layout(set=2,binding=12) uniform sampler2D input_image;
void main() {FragColor=kin_output(texture(input_image,kin_uv()).rgb,1.0);}
