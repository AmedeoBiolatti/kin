#version 300 es
precision highp float;
precision highp int;
#include "environment.inc.glsl"
uniform mat4 uInverseViewProjection;
uniform vec2 uTarget;
uniform vec4 uDisc;
uniform vec3 uMoonColor, uSunColor;
uniform bool uLinear; // an HDR view's float picture: linear light
uniform bool uVolumeOnly; // no sky: only the fog volume's light, added over the background (alpha 0)
out vec4 outColor;
vec3 encode(vec3 c) {
    c=clamp(c,0.0,1.0);
    return mix(c*12.92,1.055*pow(c,vec3(1.0/2.4))-0.055,step(0.0031308,c));
}
void main() {
    vec2 ndc=gl_FragCoord.xy/uTarget*2.0-1.0;
    vec4 far_point=uInverseViewProjection*vec4(ndc,0.0,1.0);
    vec3 d=uFog.view.x>0.5 ? normalize(uForward) : normalize(far_point.xyz-uEye*far_point.w);
    if (uVolumeOnly) {
        // The volume's light where nothing is drawn, as native fog_volume_apply.frag adds it (the sun's past the
        // volume from the height fog alone).
        vec4 v=fog_volume(d,true);
        vec3 air=v.rgb;
        if (uFogVolumeAt.y>0.5 && uFog.density.y>0.0) {
            float total=kin_fog_opacity(uFog,kin_fog_optical_depth(uFog,uEye,d,0.0,true));
            air+=kin_fog_sun_light(uFog,d,uSky.sun.xyz,uSunColor)*max(total-v.a,0.0);
        }
        outColor=vec4(uLinear ? air : encode(air),0.0);
        return;
    }
    vec3 scattered=kin_sky_scatter(uSky,d);
    vec3 color=kin_sky_over_ground(uSky,d,scattered);
    float pixel=max(max(length(dFdx(d)),length(dFdy(d))),1e-6);
    float above=clamp(d.y/pixel+0.5,0.0,1.0);
    float angle=atan(length(cross(d,uSky.sun.xyz)),dot(d,uSky.sun.xyz));
    float cover=clamp((uDisc.x-angle)/pixel+0.5,0.0,1.0);
    float limb=0.75+0.25*sqrt(max(1.0-angle*angle/(uDisc.x*uDisc.x),0.0));
    color+=uSky.sun.w*kin_sun_transmittance(uSky,uSky.sun.y)*uDisc.z*cover*above*limb;
    if (uDisc.y>0.0) {
        float moon_angle=atan(length(cross(d,uSky.moon.xyz)),dot(d,uSky.moon.xyz));
        color+=uMoonColor*clamp((uDisc.y-moon_angle)/pixel+0.5,0.0,1.0)*above;
    }
    bool volume=uFogEnabled && uFogVolumeAt.x>0.0, sun_in_volume=volume && uFogVolumeAt.y>0.5;
    float opacity=0.0;
    if (uFogEnabled && uFog.density.y>0.0) {
        opacity=kin_fog_opacity(uFog,kin_fog_optical_depth(uFog,uEye,d,0.0,true));
        vec3 tint=uFog.colour.w>0.5 ? scattered : uFog.colour.rgb;
        color=mix(color,sun_in_volume ? tint : kin_fog_sun(uFog,tint,d,uSky.sun.xyz,uSunColor),opacity);
    }
    if (volume) { // the volume's light (fog_volume_apply.frag's for the sky), the sun's past it from the height fog
        vec4 v=fog_volume(d,true);
        color+=v.rgb;
        if (sun_in_volume) color+=kin_fog_sun_light(uFog,d,uSky.sun.xyz,uSunColor)*max(opacity-v.a,0.0);
    }
    outColor=uLinear ? vec4(color,1.0) : vec4(encode(color),1.0);
}
