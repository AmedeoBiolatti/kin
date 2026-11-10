#version 450 core
layout(local_size_x=1,local_size_y=1) in;
layout(std430,set=1,binding=0) buffer Output { vec4 value[]; } output_data;
layout(std140,set=2,binding=0) uniform Params {vec4 value;vec4 a;vec4 b;vec4 c;} params;
void main() {output_data.value[0]=params.value;}
