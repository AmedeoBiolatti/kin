#version 450 core
#include "kin_material.inc.glsl"
layout(set=2,binding=12) uniform sampler2D transport_field;
struct Sample {vec4 light;vec4 occluder;};
layout(std430,set=2,binding=13) readonly buffer History {Sample samples[];} history;
void main() {
    vec4 options=kin_param(0); // side, heatmap, sample count, half extent (metres)
    vec2 uv=clamp(kin_position().xz/(2.0*options.w)+0.5,0.0,0.999999);
    ivec2 dimensions=textureSize(transport_field,0);
    ivec2 p=ivec2(uv*vec2(dimensions.x/2,dimensions.y));
    p.x+=int(options.x)*(dimensions.x/2);
    vec4 signal=texelFetch(transport_field,p,0);
    vec2 cell=abs(fract(kin_position().xz+0.5)-0.5);
    float grid=1.0-smoothstep(0.015,0.04,min(cell.x,cell.y));
    vec3 colour=mix(vec3(0.055,0.075,0.10),vec3(0.13,0.17,0.22),grid);
    if(signal.w<0.5) {
        float stripe=step(0.5,fract((kin_position().x+kin_position().z)*2.0));
        colour=mix(vec3(0.16,0.035,0.13),vec3(0.30,0.07,0.22),stripe);
    } else if(options.y>0.5) {
        colour=mix(vec3(0.04,0.25,0.65),vec3(1.0,0.35,0.04),clamp(signal.y/2.0,0.0,1.0));
        colour*=signal.z>0.5?0.35:1.0;
    } else {
        colour+=vec3(1.0,0.61,0.22)*signal.x;
        colour=vec3(1.0)-exp(-colour);
    }
    // Diagram guides show CURRENT positions, not retarded object images.
    Sample now=history.samples[int(options.z)-1];
    float light_ring=1.0-smoothstep(0.035,0.09,abs(length(kin_position().xz-now.light.xz)-0.24));
    float sphere_ring=1.0-smoothstep(0.035,0.09,abs(length(kin_position().xz-now.occluder.xz)-1.0));
    colour=mix(colour,vec3(1.0,0.9,0.4),light_ring*0.85);
    colour=mix(colour,vec3(0.25,0.85,0.9),sphere_ring*0.8*step(0.001,now.occluder.w));
    FragColor=kin_output(colour,1.0);
}
