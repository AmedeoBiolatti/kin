#version 450 core
#include "kin_material.inc.glsl"
layout(std430,set=2,binding=13) readonly buffer Input { vec4 values[]; } data;
void main() {FragColor=data.values[0];}
