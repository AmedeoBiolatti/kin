#version 450 core
// Persistent visual simulation; one invocation owns one ring slot. Output is
// the same four-vec4 sprite record consumed by kin_particle_vertex.inc.glsl.
layout(local_size_x = 64) in;
struct State { vec4 life; vec4 random; }; // age, lifetime, base size, spin
struct Sprite { vec4 position_size; vec4 colour; vec4 axis_rotation; vec4 frame; };
layout(std430, set=1, binding=0) buffer States { State states[]; };
layout(std430, set=1, binding=1) buffer Sprites { Sprite sprites[]; };
layout(std140, set=2, binding=0) uniform Simulation {
    mat4 transform;
    uvec4 counts; // capacity, first, births, sequence
    uvec4 flags; // seed, clear, shape, surface
    vec4 time; // dt, drag, wind influence, spread
    vec4 extent, velocity_min, velocity_max, gravity, wind;
    vec4 lifetime_speed, size_rotation, spin, sizes;
    vec4 colour_start, colour_end, curves, ground, atlas;
} u;
uint hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    x *= 0x846ca68bu; return x ^ (x >> 16);
}
float random(uint birth, uint purpose) {
    return float(hash(birth ^ hash(u.flags.x) ^ hash(purpose)) >> 8) * (1.0 / 16777216.0);
}
vec3 direction(float z, float angle) {
    float r = sqrt(max(0.0,1.0-z*z));
    return vec3(r*cos(angle),z,r*sin(angle));
}
void main() {
    uint i=gl_GlobalInvocationID.x;
    if (i>=u.counts.x) return;
    State s;
    Sprite p;
    if (u.flags.y!=0u) {
        s=State(vec4(0),vec4(0));
        p=Sprite(vec4(0),vec4(0),vec4(0),vec4(0));
    } else {
        s=states[i]; p=sprites[i];
    }
    uint offset=(i+u.counts.x-u.counts.y)%u.counts.x;
    bool spawn=offset<u.counts.z;
    float dt=u.time.x;
    if (spawn) {
        uint birth=u.counts.w+offset;
        // Identity survives updates and GPU copies; a recycled slot or clear
        // cannot inherit the previous view's particle motion.
        p.frame.y=uintBitsToFloat(birth);
        p.frame.z=uintBitsToFloat(floatBitsToUint(u.spin.z)+uint(birth<u.counts.w));
        p.frame.w=u.spin.w;
        float a=random(birth,1u), b=random(birth,2u), c=random(birth,3u);
        vec3 at=vec3(0);
        vec3 dir=direction(mix(1.0,cos(u.time.w),a),b*6.28318530718);
        if (u.flags.z==1u) {
            at=(vec3(a,b,c)*2.0-1.0)*u.extent.xyz;
            if (u.flags.w!=0u) {
                uint face=min(uint(random(birth,4u)*6.0),5u);
                at[face/2u]=u.extent[face/2u]*((face%2u)==0u ? -1.0 : 1.0);
            }
        } else if (u.flags.z==2u) {
            dir=direction(a*2.0-1.0,b*6.28318530718);
            at=dir*u.extent.x*(u.flags.w!=0u ? 1.0 : pow(c,1.0/3.0));
        } else if (u.flags.z==3u || u.flags.z==4u) {
            float r=sqrt(c)*u.extent.x;
            float angle=random(birth,4u)*6.28318530718;
            at=vec3(cos(angle)*r,0,sin(angle)*r);
        }
        vec3 extra=mix(u.velocity_min.xyz,u.velocity_max.xyz,
                       vec3(random(birth,5u),random(birth,6u),random(birth,7u)));
        vec3 velocity=dir*mix(u.lifetime_speed.z,u.lifetime_speed.w,random(birth,8u))+extra;
        p.position_size=vec4((u.transform*vec4(at,1)).xyz,0);
        p.axis_rotation=vec4((u.transform*vec4(velocity,0)).xyz,
                            mix(u.size_rotation.z,u.size_rotation.w,random(birth,9u)));
        s.life=vec4(0,mix(u.lifetime_speed.x,u.lifetime_speed.y,random(birth,10u)),
                    mix(u.size_rotation.x,u.size_rotation.y,random(birth,11u)),
                    mix(u.spin.x,u.spin.y,random(birth,12u)));
        s.random=vec4(random(birth,13u),random(birth,14u),0,0);
        // Rate births have sub-frame ages; explicit bursts happen at the end.
        dt=float(offset)<u.atlas.y ? max(0.0,u.atlas.z-float(offset)*u.atlas.w) : 0.0;
    }
    if (s.life.y>0.0 && s.life.x<s.life.y) {
        s.life.x+=dt;
        vec3 v=p.axis_rotation.xyz;
        vec3 air=u.wind.xyz*u.time.z;
        float k=u.time.y, x=k*dt;
        float factor=x<0.01 ? dt*(1.0-x*0.5+x*x/6.0) : (1.0-exp(-x))/k;
        float integral=x<0.01 ? dt*dt*(0.5-x/6.0+x*x/24.0) : (dt-factor)/k;
        vec3 acceleration=u.gravity.xyz+k*(air-v);
        p.position_size.xyz+=v*dt+acceleration*integral;
        p.axis_rotation.xyz=v+acceleration*factor;
        if (u.ground.x>0.5 && p.position_size.y<u.ground.y) {
            if (u.ground.z<=0.0) s.life.x=s.life.y;
            else {
                p.position_size.y=u.ground.y;
                p.axis_rotation.y=abs(p.axis_rotation.y)*clamp(u.ground.z,0.0,1.0);
            }
        }
        p.axis_rotation.w+=s.life.w*dt;
        float t=clamp(s.life.x/s.life.y,0.0,1.0);
        float scale=t<0.5 ? mix(u.sizes.x,u.sizes.y,t*2.0) : mix(u.sizes.y,u.sizes.z,t*2.0-1.0);
        p.position_size.w=max(0.0,s.life.z*scale);
        p.colour=mix(u.colour_start,u.colour_end,t);
        p.colour.rgb*=1.0-clamp(u.curves.y,0.0,1.0)*s.random.x;
        p.colour.a*=u.curves.x>0.0 ? clamp(t/u.curves.x,0.0,1.0) : 1.0;
        p.frame.x=mod(t*max(u.curves.z,0.0)*u.atlas.x+(u.curves.w>0.5 ? s.random.y*u.atlas.x : 0.0),max(u.atlas.x,1.0));
    }
    if (s.life.y<=0.0 || s.life.x>=s.life.y) {
        p.position_size.w=0.0;
        p.colour=vec4(0);
    }
    states[i]=s;
    sprites[i]=p;
}
