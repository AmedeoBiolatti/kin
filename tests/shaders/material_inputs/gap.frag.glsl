#version 450 core
#include "kin_material.inc.glsl"
layout(set=2,binding=13) uniform sampler2D input_texture;
void main() {FragColor=texture(input_texture,kin_uv());}
