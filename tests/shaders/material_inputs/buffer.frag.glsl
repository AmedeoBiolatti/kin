#version 450 core
#include "kin_material.inc.glsl"
layout(std430,set=2,binding=12) readonly buffer Input { vec4 values[]; } data;
void main() { FragColor=kin_output(data.values[0].rgb*kin_param(0).rgb,1.0); }
