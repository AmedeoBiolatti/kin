#version 450 core
#include "kin_fog.inc.glsl"
#include "kin_particle_composition.inc.glsl"
layout(std430,set=0,binding=0) readonly buffer Current { Particle particles[]; };
layout(std430,set=0,binding=1) readonly buffer Prior { vec4 prior[]; };
layout(std430,set=0,binding=2) readonly buffer Order { uvec2 order[]; };
layout(std430,set=0,binding=3) readonly buffer Looks { ParticleLook looks[]; };
#include "kin_particle_source.inc.glsl"
#include "kin_particle_draw.inc.glsl"
#include "kin_particle_quad.inc.glsl"
layout(set=1,binding=2) uniform Temporal { mat4 current, previous; } temporal;
layout(location=0) out vec2 vUV0;
layout(location=1) out vec2 vUV1;
layout(location=2) out vec4 vColour;
layout(location=3) out vec2 vLocal;
layout(location=4) out float vDepth;
layout(location=5) out vec4 vFog;
layout(location=6) out float vMix;
layout(location=8) out vec3 vWorld;
layout(location=9) flat out vec3 vCentre;
layout(location=10) out vec3 vCurrent;
layout(location=11) out vec3 vPrevious;
layout(location=12) flat out float vFresh;
layout(location=13) flat out uint vLook;
const vec2 CORNERS[4]=vec2[4](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(-1,1));
const int CORNER_OF[6]=int[6](0,1,2,0,2,3);
vec2 frame_uv(float frame,vec2 uv,vec4 atlas) {
    return (vec2(mod(frame,atlas.x),floor(frame/atlas.x))+uv)/atlas.xy;
}
void main() {
    uvec2 key=order[uint(gl_InstanceIndex)]; uint slot=key.y;
    vLook=particle_source(slot,draw.first.x);
    ParticleLook look=looks[vLook]; Particle p=particles[slot];
    if (key.x==0u) { p.colour=vec4(0); p.position_size.w=0.0; }
    int corner=CORNER_OF[gl_VertexIndex]; vec2 c=CORNERS[corner];
    vec3 world=kin_particle_corner(p,c,draw.right,draw.up,draw.eye,draw.forward,look.axis,look.stretch);
    gl_Position=draw.view_projection*vec4(world,1);
    gl_ClipDistance[0]=dot(draw.clip.xyz,world)+draw.clip.w;
    vWorld=world; vCentre=p.position_size.xyz; vColour=p.colour; vLocal=c;
    vDepth=dot(world-draw.eye.xyz,draw.forward.xyz);
    vec2 uv=vec2(c.x*0.5+0.5,0.5-c.y*0.5);
    float f=clamp(p.frame.x,0.0,look.atlas.z-1.0);
    float f0=look.atlas.w>0.5 ? floor(f) : floor(f+0.5);
    vUV0=frame_uv(f0,uv,look.atlas);
    vUV1=frame_uv(min(f0+1.0,look.atlas.z-1.0),uv,look.atlas);
    vMix=look.atlas.w>0.5 ? f-f0 : 0.0;
    vec4 before=vec4(world,1);
    if (draw.first.y!=0u) {
        if (look.range.w==0u) before=prior[slot*4u+uint(corner)];
        else if ((look.range.z&1u)!=0u) {
            Particle old=Particle(prior[slot*4u],prior[slot*4u+1u],prior[slot*4u+2u],prior[slot*4u+3u]);
            bool known=all(equal(floatBitsToUint(old.frame.yzw),floatBitsToUint(p.frame.yzw))) &&
                old.colour.a>0.0 && abs(old.colour.a-p.colour.a)<0.15 &&
                all(lessThan(abs(old.colour.rgb-p.colour.rgb),vec3(0.15))) &&
                abs(old.frame.x-p.frame.x)<1.0 &&
                (look.atlas.w>0.5 || floor(old.frame.x+0.5)==floor(p.frame.x+0.5)) &&
                dot(old.position_size.xyz-p.position_size.xyz,old.position_size.xyz-p.position_size.xyz)<
                    max(1.0,p.position_size.w*p.position_size.w*16.0);
            if (known) before=vec4(kin_particle_corner(old,c,look.right,look.up,look.eye,look.forward,
                                                     look.axis,look.stretch),0);
        }
    }
    vCurrent=(temporal.current*vec4(world,1)).xyw;
    vPrevious=(temporal.previous*vec4(before.xyz,1)).xyw; vFresh=before.w;
    vFog=vec4(0);
    if (draw.first.z!=0u && look.stretch.z>0.5) {
        vec3 to=world-draw.eye.xyz; float dist=length(to);
        vec3 dir=dist>1e-6 ? to/dist : vec3(0,0,-1);
        if (draw.eye.w>0.5) { dir=draw.forward.xyz; dist=dot(to,dir); }
        float tau=kin_fog_optical_depth(draw.fog,draw.eye.xyz,dir,dist,false);
        float opacity=kin_fog_opacity(draw.fog,tau);
        if (opacity>0.0) {
            vec3 colour=draw.fog.colour.w>0.5 ? particle_fog_sky(dir) : draw.fog.colour.rgb;
            vFog=vec4(draw.fog.view.z>0.5 ? colour : kin_fog_sun(draw.fog,colour,dir,
                           draw.sun_direction.xyz,draw.sun_light.rgb),opacity);
        }
    }
}
