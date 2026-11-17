#include "kin_particle_composition.inc.glsl"
layout(location=13) flat in uint vLook;
layout(set=2,binding=0) uniform sampler2DArray uAtlas;
layout(std430,set=2,binding=5) readonly buffer Looks { ParticleLook looks[]; };
struct MipRect { uvec4 rect, meta; };
layout(std430,set=2,binding=6) readonly buffer Mips { MipRect mips[]; };
struct CompositionFrag { vec4 tint, flags, disc, unproject; };
CompositionFrag frag;
ParticleLook composition_look;
bool composition_receives_shadow;
vec4 composition_mip(vec2 uv,uint level,bool nearest) {
    MipRect cell=mips[uint(composition_look.flags.z)+level];
    vec2 size=vec2(cell.rect.zw);
    if (nearest) uv=(floor(clamp(uv,vec2(0),vec2(1))*size)+0.5)/size;
    uv=clamp(uv,0.5/size,1.0-0.5/size);
    vec2 atlas_uv=(vec2(cell.rect.xy)+uv*size)/vec2(textureSize(uAtlas,0).xy);
    return textureLod(uAtlas,vec3(atlas_uv,float(cell.meta.x)),0.0);
}
vec4 composition_trilinear(vec2 uv,float lod,uint levels) {
    lod=clamp(lod,0.0,float(levels-1u)); uint level=uint(floor(lod));
    return mix(composition_mip(uv,level,false),composition_mip(uv,min(level+1u,levels-1u),false),fract(lod));
}
vec4 composition_picture(vec2 uv) {
    uint levels=uint(composition_look.flags.w);
    if (composition_look.stretch.w<0.5) return composition_mip(uv,0u,true);
    if (composition_look.stretch.w<1.5 || levels<=1u) return composition_mip(uv,0u,false);
    vec2 size=vec2(mips[uint(composition_look.flags.z)].rect.zw);
    vec2 atlas_size=vec2(textureSize(uAtlas,0).xy);
    float lod=textureQueryLod(uAtlas,uv*size/atlas_size).y;
    uint maximum=uint(frame_frag.disc.w);
    if (maximum<=1u) return composition_trilinear(uv,lod+frame_frag.flags.y,levels);
    // The atlas stores independent mip rectangles. Integrate a bounded major-
    // axis footprint explicitly so every tap clamps to this image, never its neighbour.
    vec2 dx=dFdx(uv)*size, dy=dFdy(uv)*size;
    float xx=dx.x*dx.x+dy.x*dy.x, xy=dx.x*dx.y+dy.x*dy.y, yy=dx.y*dx.y+dy.y*dy.y;
    float root=sqrt(max(0.0,(xx-yy)*(xx-yy)+4.0*xy*xy));
    float major=sqrt(max(0.0,0.5*(xx+yy+root)));
    float minor=sqrt(max(0.0,0.5*(xx+yy-root)));
    uint taps=clamp(uint(ceil(major/max(minor,1e-6))),1u,maximum);
    vec2 axis=abs(xy)>1e-8 ? normalize(vec2(xy,0.5*(xx+yy+root)-xx))
                                : xx>=yy ? vec2(1,0) : vec2(0,1);
    vec2 span=axis*major/size;
    float level=log2(max(max(major/float(taps),minor),1e-6))+frame_frag.flags.y;
    vec4 colour=vec4(0);
    for (uint i=0u;i<taps;++i)
        colour+=composition_trilinear(uv+span*((float(i)+0.5)/float(taps)-0.5),level,levels);
    return colour/float(taps);
}
void composition_fragment() {
    composition_look=looks[vLook];
    frag.tint=vec4(composition_look.tint.rgb,(composition_look.range.z&2u)!=0u ? frame_frag.tint.w : 0.0);
    frag.flags=vec4(frame_frag.flags.x,frame_frag.flags.z,composition_look.flags.xy);
    frag.disc=vec4(composition_look.disc.xyz,composition_look.tint.w);
    frag.disc.z*=frame_frag.flags.w;
    frag.unproject=frame_frag.unproject;
    composition_receives_shadow=composition_look.disc.w>0.5;
}
