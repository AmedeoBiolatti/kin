#version 450 core
layout(local_size_x=8,local_size_y=8) in;
layout(rgba16f,set=1,binding=0) uniform writeonly image2D target_image;
layout(std140,set=2,binding=0) uniform Params {vec4 colour;vec4 mode;vec4 c;vec4 d;} params;
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy),size=imageSize(target_image);
    if(any(greaterThanEqual(p,size)))return;
    bool bright=all(lessThan(abs(p-size/2),ivec2(4)));
    imageStore(target_image,p,params.colour*(params.mode.x>0.5?float(bright):1.0));
}
