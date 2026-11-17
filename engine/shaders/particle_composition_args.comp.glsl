#version 450 core
layout(local_size_x=64) in;
layout(std430,set=0,binding=0) readonly buffer Order { uvec2 order[]; };
layout(std430,set=0,binding=1) readonly buffer Anchors { uvec2 anchors[]; };
layout(std430,set=1,binding=0) writeonly buffer Arguments { uvec4 arguments[]; };
layout(std140,set=2,binding=0) uniform Params { uvec4 counts; } u; // particles, anchors
uint boundary(uvec2 anchor) {
    uint lo=0u, hi=u.counts.x;
    while (lo<hi) {
        uint mid=(lo+hi)/2u; uvec2 key=order[mid];
        bool precedes=key.x>anchor.x || (key.x==anchor.x && key.y<anchor.y);
        if (precedes) lo=mid+1u; else hi=mid;
    }
    return lo;
}
void main() {
    uint i=gl_GlobalInvocationID.x;
    if (i>u.counts.y) return;
    uint first=i==0u ? 0u : boundary(anchors[i-1u]);
    uint end=i==u.counts.y ? boundary(uvec2(0,0)) : boundary(anchors[i]);
    arguments[i]=uvec4(6u,end-first,0u,first);
}
