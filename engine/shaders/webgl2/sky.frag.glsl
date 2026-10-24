#version 300 es
precision highp float;
precision highp int;
#include "environment.inc.glsl"
uniform mat4 uInverseViewProjection;
uniform vec2 uTarget;
uniform vec4 uDisc;
uniform vec3 uMoonColor, uSunColor;
out vec4 outColor;
vec3 encode(vec3 c) {
    c=clamp(c,0.0,1.0);
    return mix(c*12.92,1.055*pow(c,vec3(1.0/2.4))-0.055,step(0.0031308,c));
}
void main() {
    vec2 ndc=gl_FragCoord.xy/uTarget*2.0-1.0;
    vec4 far_point=uInverseViewProjection*vec4(ndc,0.0,1.0);
    vec3 d=uFog.view.x>0.5 ? normalize(uForward) : normalize(far_point.xyz-uEye*far_point.w);
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
    if (uFogEnabled && uFog.density.y>0.0) {
        float opacity=kin_fog_opacity(uFog,kin_fog_optical_depth(uFog,uEye,d,0.0,true));
        vec3 tint=uFog.colour.w>0.5 ? scattered : uFog.colour.rgb;
        color=mix(color,kin_fog_sun(uFog,tint,d,uSky.sun.xyz,uSunColor),opacity);
    }
    outColor=vec4(encode(color),1.0);
}
