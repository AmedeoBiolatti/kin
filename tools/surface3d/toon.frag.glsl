#version 450 core
#define KIN_DIRECT_LIGHT toon_light
#include "kin_material.inc.glsl"
// A lighting response can keep stock material sampling and shadow visibility
// while replacing the BRDF. This deliberately uses diffuse bands only.
vec3 toon_light(Surface surface, KinLight light) {
    float bands = max(kin_param(0).x, 1.0);
    float cosine = max(dot(surface.n, light.direction), 0.0);
    float stepped = floor(cosine * bands + 0.5) / bands;
    return surface.diffuse * light.irradiance * (stepped * light.visibility);
}
void main() {
    vec4 base = kin_base_color();
    vec2 mr = kin_metallic_roughness();
    FragColor = kin_output(kin_lit(base.rgb, kin_normal(), mr.x, mr.y, kin_occlusion()) + kin_emissive(base.rgb), base.a);
}
