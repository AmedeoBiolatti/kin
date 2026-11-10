#version 450 core
#include "kin_material.inc.glsl"
layout(set=2,binding=12) uniform sampler2D t0;
layout(set=2,binding=13) uniform sampler2D t1;
layout(set=2,binding=14) uniform sampler2D t2;
layout(set=2,binding=15) uniform sampler2D t3;
layout(std430,set=2,binding=16) readonly buffer B0 {vec4 value[];} b0;
layout(std430,set=2,binding=17) readonly buffer B1 {vec4 value[];} b1;
layout(std430,set=2,binding=18) readonly buffer B2 {vec4 value[];} b2;
layout(std430,set=2,binding=19) readonly buffer B3 {vec4 value[];} b3;
layout(std430,set=2,binding=20) readonly buffer B4 {vec4 value[];} b4;
layout(std430,set=2,binding=21) readonly buffer B5 {vec4 value[];} b5;
layout(std430,set=2,binding=22) readonly buffer B6 {vec4 value[];} b6;
layout(std430,set=2,binding=23) readonly buffer B7 {vec4 value[];} b7;
void main() {vec3 texture_sum=texture(t0,kin_uv()).rgb+texture(t1,kin_uv()).rgb+texture(t2,kin_uv()).rgb+texture(t3,kin_uv()).rgb;vec3 buffer_sum=b0.value[0].rgb+b1.value[0].rgb+b2.value[0].rgb+b3.value[0].rgb+b4.value[0].rgb+b5.value[0].rgb+b6.value[0].rgb+b7.value[0].rgb;FragColor=kin_output(texture_sum*buffer_sum/32.0,1.0);}
