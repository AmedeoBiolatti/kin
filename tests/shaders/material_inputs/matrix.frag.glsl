#version 450 core
#include "kin_material.inc.glsl"
layout(std430,set=2,binding=12) readonly buffer Input {mat2 value;} data;
void main() {FragColor=kin_output(vec3(data.value[0],0.0),1.0);}
