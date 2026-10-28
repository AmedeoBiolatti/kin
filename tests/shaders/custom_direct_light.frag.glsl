#version 450 core
#define KIN_DIRECT_LIGHT test_light
#include "kin_material.inc.glsl"
vec3 test_light(Surface surface, KinLight light) {
    // 0: stock response, 1: ignore visibility, 2: show its lighting kind,
    // 3: wrapped diffuse (exercises directionals on the back-facing side).
    if (kin_param(0).x > 3.5)
        return vec3(light.distance * 0.05, length(light.position - kin_position()) * 0.05, float(light.kind) / 3.0);
    if (kin_param(0).x > 2.5) {
        float wrapped = clamp((dot(surface.n, light.direction) + 0.5) / 1.5, 0.0, 1.0);
        return surface.diffuse * light.irradiance * wrapped;
    }
    if (kin_param(0).x > 1.5) {
        if (dot(light.irradiance, vec3(1.0)) <= 0.0) return vec3(0.0);
        return light.kind == KIN_LIGHT_SPOT ? vec3(0.0, 0.5, 0.0) : light.kind == KIN_LIGHT_POINT ? vec3(0.5, 0.0, 0.0) : vec3(0.0, 0.0, 0.5);
    }
    return shade(surface, light.direction, light.irradiance * (kin_param(0).x > 0.5 ? 1.0 : light.visibility));
}
void main() {
    vec4 base = kin_base_color();
    vec2 mr = kin_metallic_roughness();
    FragColor = kin_output(kin_lit(base.rgb, kin_normal(), mr.x, mr.y, kin_occlusion()) + kin_emissive(base.rgb), base.a);
}
