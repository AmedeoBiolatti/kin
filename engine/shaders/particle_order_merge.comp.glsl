#version 450 core
layout(local_size_x=256) in;
layout(std430,set=1,binding=0) buffer Order { uvec2 order[]; };
layout(std140,set=2,binding=0) uniform Params { uvec4 counts; } u; // length, width, stride (0: local tail)
#include "kin_particle_order.inc.glsl"
void main() {
    uint i=gl_GlobalInvocationID.x;
    if (u.counts.z!=0u) {
        uint other=i ^ u.counts.z;
        if (i>=u.counts.x || other<=i) return;
        uvec2 a=order[i], b=order[other];
        bool descending=(i & u.counts.y)==0u;
        bool first=particle_precedes(a,b);
        order[i]=descending==first ? a : b;
        order[other]=descending==first ? b : a;
    } else {
        tile[gl_LocalInvocationID.x]=order[i];
        barrier();
        for (uint stride=128u;stride>0u;stride/=2u)
            particle_tile_merge(i,stride,u.counts.y);
        order[i]=tile[gl_LocalInvocationID.x];
    }
}
