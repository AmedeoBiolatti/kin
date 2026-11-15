// The native atmosphere/fog math, with a texture-backed fog lookup table.
#include "kin_fog.inc.glsl"
uniform KinSky uSky;
uniform KinFog uFog;
uniform bool uFogEnabled;
uniform vec3 uEye, uForward;
uniform highp sampler2D uFogTable;
vec3 fog_sky(vec3 dir) {
    vec3 v=normalize(vec3(dir.x,max(dir.y,0.0),dir.z)+vec3(1e-6,0,0));
    float row=kin_fog_row(v.y);
    int i=min(int(row),KIN_FOG_ROWS-2);
    float w=row-float(i);
    vec3 r=mix(texelFetch(uFogTable,ivec2(0,i),0).rgb,texelFetch(uFogTable,ivec2(0,i+1),0).rgb,w);
    vec3 m=mix(texelFetch(uFogTable,ivec2(1,i),0).rgb,texelFetch(uFogTable,ivec2(1,i+1),0).rgb,w);
    float mu=dot(v,uSky.sun.xyz);
    vec3 light=kin_rayleigh_phase(mu)*r+kin_mie_phase(mu,uSky.shape.y)*m;
    if (uSky.shape.w>0.0) {
        r=mix(texelFetch(uFogTable,ivec2(2,i),0).rgb,texelFetch(uFogTable,ivec2(2,i+1),0).rgb,w);
        m=mix(texelFetch(uFogTable,ivec2(3,i),0).rgb,texelFetch(uFogTable,ivec2(3,i+1),0).rgb,w);
        mu=dot(v,uSky.moon.xyz);
        light+=kin_rayleigh_phase(mu)*r+kin_mie_phase(mu,uSky.shape.y)*m;
    }
    return light+uSky.night.rgb;
}
// Fog3D::volumetric's light (fog_volume.frag's sums), in front of what is `to` away (`sky`: the whole volume's), as native
// kin_fog_apply and fog_volume_apply.frag read it: rgb its light, a the share of the ray's fog inside the volume.
uniform highp sampler3D uFogVolume;
uniform vec4 uFogVolumeAt; // x: the volume's depth (0: none), y: 1 when it holds the sun's in-scattering, zw: 1 / size
vec4 fog_volume(vec3 to,bool sky) {
    float slices=float(textureSize(uFogVolume,0).z);
    float w=sky ? 1.0 : min(sqrt(max(dot(to,uForward),0.0)/uFogVolumeAt.x),1.0);
    float near=clamp(w*slices,0.0,1.0);
    vec4 sum=textureLod(uFogVolume,vec3(gl_FragCoord.xy*uFogVolumeAt.zw,max(w-0.5/slices,0.5/slices)),0.0);
    return vec4(sum.rgb*near,1.0-mix(1.0,sum.a,near));
}
vec3 fogged(vec3 color,vec3 world,vec3 sun_direction,vec3 sun_color,bool additive) {
    if (!uFogEnabled) return color;
    vec3 to=world-uEye;
    float dist=length(to);
    vec3 dir=dist>1e-6 ? to/dist : vec3(0,0,-1);
    if (uFog.view.x>0.5) { dir=uForward; dist=dot(to,dir); }
    float opacity=kin_fog_opacity(uFog,kin_fog_optical_depth(uFog,uEye,dir,dist,false));
    if (additive) return color*(1.0-opacity);
    vec3 tint=uFog.colour.w>0.5 ? fog_sky(dir) : uFog.colour.rgb;
    vec3 sun=kin_fog_sun_light(uFog,dir,sun_direction,sun_color);
    // With the sun in the volume, its light in the air comes from there, and past the volume unshadowed.
    bool sun_in_volume=uFogVolumeAt.x>0.0 && uFogVolumeAt.y>0.5;
    vec3 result=mix(color,tint+(sun_in_volume ? vec3(0.0) : sun),opacity);
    if (uFogVolumeAt.x>0.0) {
        vec4 v=fog_volume(to,false);
        result+=v.rgb;
        if (sun_in_volume) result+=sun*max(opacity-v.a,0.0);
    }
    return result;
}
