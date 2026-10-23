// kin's 3D mesh fragment shader, shared by mesh.frag and mesh_velocity.frag (which defines KIN_VELOCITY first).
// kin's 3D meshes (kin/render3d): a base colour lit in linear light by the
// scene, plus the material's own light; or unlit. The shading is in
// kin_material.inc.glsl, which custom material shaders share.
#define KIN_NO_CUSTOM_PARAMS
#include "kin_material.inc.glsl"

#ifdef KIN_VELOCITY
// Temporal anti-aliasing with no pre-pass: the velocity as a second output, in the last two of four floats as the
// pre-pass writes it (taa.comp reads them there). The pipeline masks the write for blended and wireframe draws.
layout(location = 5) in vec3 vCurrent;
layout(location = 6) in vec3 vPrevious;
layout(location = 7) flat in float vFresh;
layout(location = 8) in vec3 vOldInCurrent;
layout(location = 1) out vec4 FragVelocity;
#include "kin_velocity.inc.glsl"
#endif

void main() {
#ifdef KIN_VELOCITY
    const float motion_footprint = 0.25 * length(fwidth(vCurrent.xy / vCurrent.z));
#endif
    const vec4 color = kin_base_color();
    float alpha = color.a;
    if (alpha < material.params.w) {
        discard;
    }
    // A cut-out is opaque where it stays.
    if (material.params.w > 0.0) {
        alpha = 1.0;
    }
    vec3 c;
    if (material.params.y > 0.5) {
        c = color.rgb * (vec3(1.0) + vec3(material.params.x));
    } else {
        const vec2 mr = kin_metallic_roughness();
        c = kin_lit(color.rgb, kin_normal(), mr.x, mr.y, kin_occlusion()) + kin_emissive(color.rgb);
    }
    FragColor = kin_output(c, alpha);
#ifdef KIN_VELOCITY
    FragVelocity = vec4(0.0, kin_independent_motion(vCurrent, vOldInCurrent) > motion_footprint ? 4.0 : 0.0, kin_velocity(vCurrent, vPrevious, vFresh));
#endif
}
