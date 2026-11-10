#version 450 core
layout(local_size_x=8,local_size_y=8) in;
layout(rgba16f,set=1,binding=0) uniform writeonly image2D field;
layout(std430,set=1,binding=1) buffer Palette { vec4 values[]; } palette;
layout(std140,set=2,binding=0) uniform Params {vec4 value;vec4 a;vec4 b;vec4 c;} params;
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy), size=imageSize(field);
    if(any(greaterThanEqual(p,size))) return;
    vec2 uv=(vec2(p)+0.5)/vec2(size);
    float cloud=0.7+0.3*sin(uv.x*12.0+params.value.x)*cos(uv.y*9.0-params.value.x);
    imageStore(field,p,vec4(vec3(cloud),1.0));
    if(all(equal(p,ivec2(0)))) palette.values[0]=vec4(1.0,0.9,0.75,1.0);
}
