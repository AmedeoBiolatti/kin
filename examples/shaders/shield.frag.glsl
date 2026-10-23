#version 450 core
// Signal Breach's shield: a shell round an enemy that flashes when it is hit.
// A Fresnel rim (bright where the surface turns edge-on) and lines running up
// it, in the material's colour, added to the picture: it lights nothing and
// is lit by nothing, so it uses only kin's inputs, not its lighting.
//
// Parameters (Material3D::shader_params): x time (s), y strength (0 to 1,
// fading out), z lines a metre, w how fast they climb.
#include "kin_material.inc.glsl"

void main() {
    const vec4 params = kin_param(0);
    const vec4 base = kin_base_color();
    const float facing = clamp(abs(dot(kin_normal(), kin_view())), 0.0, 1.0);
    const float rim = pow(1.0 - facing, 2.5);
    const float line = smoothstep(0.55, 0.9, abs(fract(kin_position().y * params.z - params.x * params.w) - 0.5) * 2.0);
    const float glow = (0.12 + 0.88 * rim) * (0.55 + 0.45 * line) * params.y;
    FragColor = kin_output(base.rgb * glow * 3.0, glow * base.a);
}
