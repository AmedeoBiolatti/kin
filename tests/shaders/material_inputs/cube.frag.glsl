#version 450 core
#include "kin_material.inc.glsl"
layout(set=2,binding=12) uniform samplerCube input_texture;
void main() {FragColor=texture(input_texture,vec3(1,0,0));}
