#version 450 core
layout(local_size_x=256) in;
#include "kin_particle_composition.inc.glsl"
layout(std430,set=0,binding=0) readonly buffer Particles { Particle particles[]; };
layout(std430,set=0,binding=1) readonly buffer Looks { ParticleLook looks[]; };
layout(std430,set=1,binding=0) writeonly buffer Order { uvec2 order[]; };
layout(std140,set=2,binding=0) uniform Params {
    vec4 eye, forward, planes[7];
    uvec4 counts; // admitted count, padded length, culling planes, sources
} u;
#include "kin_particle_source.inc.glsl"
#include "kin_particle_order.inc.glsl"
void main() {
    uint i=gl_GlobalInvocationID.x;
    uvec2 key=uvec2(0,0xffffffffu);
    if (i<u.counts.x) {
        Particle p=particles[i]; ParticleLook look=looks[particle_source(i,u.counts.w)];
        float radius=p.position_size.w*0.75;
        if (look.axis.w>0.5 && look.axis.w<1.5)
            radius+=max(length(p.axis_rotation.xyz)*abs(look.stretch.x),max(look.stretch.y,0.0));
        bool visible=p.position_size.w>0.0 && p.colour.a>0.0;
        for (uint plane=0u;plane<u.counts.z;++plane)
            visible=visible && dot(u.planes[plane],vec4(p.position_size.xyz,1))>=-radius;
        float depth=dot(p.position_size.xyz-u.eye.xyz,u.forward.xyz);
        uint bits=floatBitsToUint(depth);
        uint depth_key=(bits&0x80000000u)!=0u ? ~bits : bits^0x80000000u;
        key=uvec2(visible && !isnan(depth) && !isinf(depth) ? max(depth_key,1u) : 0u,i);
    }
    tile[gl_LocalInvocationID.x]=key; barrier();
    for (uint width=2u;width<=min(u.counts.y,256u);width*=2u)
        for (uint stride=width/2u;stride>0u;stride/=2u) particle_tile_merge(i,stride,width);
    if (i<u.counts.y) order[i]=tile[gl_LocalInvocationID.x];
}
