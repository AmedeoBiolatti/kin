#version 450 core
#include "kin_material.inc.glsl"
layout(std430,set=2,binding=12) buffer Input { vec4 values[]; } data;
void main() {data.values[0]=vec4(1);FragColor=vec4(1);}
