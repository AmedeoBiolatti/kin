#version 450 core
// kin's 3D meshes (kin/render3d): a base colour lit in linear light by the
// scene, plus the material's own light; or unlit. The shading is in
// kin_material.inc.glsl, which custom material shaders share.
#define KIN_NO_CUSTOM_PARAMS
#include "kin_material.inc.glsl"

void main() {
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
}
