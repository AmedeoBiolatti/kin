#include "shadow_maps.inc.glsl"
uniform bool uShadowPass, uReceivesShadow;
// A decal shades the surface under it, whose normal faces the viewer (its box's back faces are what is drawn).
#ifdef KIN_DECAL
float front_facing() { return 1.0; }
#else
float front_facing() { return gl_FrontFacing ? 1.0 : -1.0; }
#endif
float sunlight() {
    if (!uReceivesShadow) return 1.0;
    vec3 n=dot(vNormal,vNormal)>1e-20 ? normalize(vNormal)*front_facing() : normalize(cross(dFdx(vWorld),dFdy(vWorld)));
    return sunlight_at(vWorld,n);
}
