#version 450 core
#include "kin_material.inc.glsl"

// One diagnostic shader, selected through param 0.x. Uses the same decoded
// material inputs as stock PBR, including the material's actual texture roles.
void main() {
    vec4 base = kin_base_color();
    if (material.params.w > 0.0 && base.a < material.params.w) discard;
    int mode = int(kin_param(0).x + 0.5);
    vec3 colour = base.rgb;
    if (mode == 2) colour = kin_normal() * 0.5 + 0.5;
    if (mode == 3) {
        vec2 uv = kin_uv();
        float checker = mod(floor(uv.x * 10.0) + floor(uv.y * 10.0), 2.0);
        colour = mix(vec3(0.04), vec3(fract(uv), 0.7), 0.3 + checker * 0.7);
    }
    if (mode == 4) colour = vec3(kin_metallic_roughness().x);
    if (mode == 5) colour = vec3(kin_metallic_roughness().y);
    if (mode == 6) colour = vec3(base.a);
    FragColor = kin_output(colour, mode == 1 ? base.a : 1.0);
}
