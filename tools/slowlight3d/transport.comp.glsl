#version 450 core
// Example-only direct transport for a static y=0 receiver. All times are relative
// to the latest 30 Hz sample, so a long-running demo keeps float precision.
layout(local_size_x=8, local_size_y=8) in;
struct Sample { vec4 light; vec4 occluder; };
layout(std430, set=0, binding=0) readonly buffer History { Sample samples[]; } history;
layout(rgba16f, set=1, binding=0) uniform writeonly image2D field;
layout(std140, set=2, binding=0) uniform Params { vec4 model; vec4 eye; vec4 b; vec4 c; } params;
int count;
float step_seconds, oldest;
int interval_at(float t) { return clamp(int(floor((t-oldest)/step_seconds)), 0, count-2); }
Sample sample_at(float t) {
    if (count==1) return history.samples[0];
    int i=interval_at(t);
    float f=clamp((t-oldest)/step_seconds-float(i), 0.0, 1.0);
    Sample v;
    v.light=vec4(mix(history.samples[i].light.xyz,history.samples[i+1].light.xyz,f),history.samples[i].light.w);
    v.occluder=vec4(mix(history.samples[i].occluder.xyz,history.samples[i+1].occluder.xyz,f),history.samples[i].occluder.w);
    if(t>=0.0) {
        v.light.w=history.samples[count-1].light.w;
        v.occluder.w=history.samples[count-1].occluder.w;
    }
    return v;
}
float minimum_distance_squared(vec3 relative,vec3 velocity,float duration) {
    float v2=dot(velocity,velocity);
    float t=v2>1e-20?clamp(-dot(relative,velocity)/v2,0.0,duration):0.0;
    vec3 nearest=relative+velocity*t;
    return dot(nearest,nearest);
}
void main() {
    ivec2 pixel=ivec2(gl_GlobalInvocationID.xy), size=imageSize(field);
    if(any(greaterThanEqual(pixel,size))) return;
    int side=size.x/2;
    bool instant=pixel.x>=side;
    vec2 uv=(vec2(pixel.x%side,pixel.y)+0.5)/vec2(side,size.y);
    vec3 receiver=vec3((uv.x*2.0-1.0)*params.model.w,0.0,(uv.y*2.0-1.0)*params.model.w);
    count=int(params.model.y);
    step_seconds=params.model.z;
    oldest=-float(count-1)*step_seconds;
    float speed=params.model.x;
    float surface_time=instant?0.0:-length(receiver-params.eye.xyz)/speed;
    float emission_time=0.0;
    Sample emitter=history.samples[count-1];
    if(!instant) {
        if(count<2 || surface_time<oldest ||
           oldest+length(history.samples[0].light.xyz-receiver)/speed>surface_time) {
            imageStore(field,pixel,vec4(0.0)); return;
        }
        float lo=oldest,hi=surface_time;
        for(int n=0;n<22;++n) {
            float t=(lo+hi)*0.5;
            if(t+length(sample_at(t).light.xyz-receiver)/speed<surface_time) lo=t;
            else hi=t;
        }
        emission_time=(lo+hi)*0.5;
        emitter=sample_at(emission_time);
    }
    vec3 source=emitter.light.xyz,ray=receiver-source;
    float distance=length(ray);
    bool blocked=false;
    if(instant) {
        vec4 sphere=history.samples[count-1].occluder;
        blocked=sphere.w>0.0 && minimum_distance_squared(source-sphere.xyz,ray,1.0)<sphere.w*sphere.w;
    } else if(surface_time-emission_time>1e-8) {
        vec3 photon_velocity=ray/(surface_time-emission_time);
        int first=interval_at(emission_time),last=interval_at(surface_time);
        for(int i=first;i<=last;++i) {
            float begin=max(emission_time,oldest+float(i)*step_seconds);
            float end=min(surface_time,oldest+float(i+1)*step_seconds);
            float radius=history.samples[i].occluder.w;
            if(end<=begin || radius<=0.0) continue;
            vec3 sphere_velocity=(history.samples[i+1].occluder.xyz-history.samples[i].occluder.xyz)/step_seconds;
            vec3 photon=source+photon_velocity*(begin-emission_time);
            vec3 sphere=sample_at(begin).occluder.xyz;
            if(minimum_distance_squared(photon-sphere,photon_velocity-sphere_velocity,end-begin)<radius*radius) {
                blocked=true; break;
            }
        }
    }
    float energy=blocked?0.0:emitter.light.w*max(0.0,-ray.y)/max(distance,0.001)/(1.0+distance*distance);
    // R irradiance, G total delay, B incoming occlusion, A history validity.
    imageStore(field,pixel,vec4(energy,-emission_time,float(blocked),1.0));
}
