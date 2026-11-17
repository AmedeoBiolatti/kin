#version 450 core
layout(local_size_x=64) in;
layout(std430,set=0,binding=0) readonly buffer Current { uvec4 current[]; };
layout(std430,set=0,binding=1) readonly buffer Prior { uvec4 prior[]; };
layout(std430,set=0,binding=2) readonly buffer Order { uvec2 order[]; };
layout(std430,set=1,binding=0) writeonly buffer CurrentOut { uvec4 output_current[]; };
layout(std430,set=1,binding=1) writeonly buffer PriorOut { uvec4 output_prior[]; };
layout(std140,set=2,binding=0) uniform Params {
    uvec4 counts; // source first, destination first, count, use sorted source index
    uvec4 flags; // capture prior, compatible history, GPU source
} u;
void main() {
    uint n=gl_GlobalInvocationID.x;
    if (n>=u.counts.z) return;
    uint at=u.counts.x+n;
    bool visible=true;
    if (u.counts.w!=0u) { uvec2 key=order[n]; at=key.y; visible=key.x!=0u; }
    uint dst=(u.counts.y+n)*4u;
    for (uint j=0u;j<4u;++j) output_current[dst+j]=current[at*4u+j];
    if (!visible) { output_current[dst].w=0u; output_current[dst+1u]=uvec4(0); }
    if (u.flags.x!=0u) {
        for (uint j=0u;j<4u;++j) output_prior[dst+j]=u.flags.y!=0u ? prior[at*4u+j] : uvec4(0,0,0,0x3f800000u);
        if (u.flags.z!=0u && u.flags.y==0u) output_prior[dst+1u]=uvec4(0);
    }
}
