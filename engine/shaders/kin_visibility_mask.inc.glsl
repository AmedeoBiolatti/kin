// The body of visibility_mask.frag.glsl (without #version), which a surface's cut-out variant shares.
// The visibility pass for cut-out materials (Blend3D::Mask).
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vUV;
#ifdef KIN_SURFACE_ALPHA
layout(location = 3) in vec3 vWorld;
#else
const vec3 vWorld = vec3(0.0);
#endif
layout(location = 4) flat in uint vId;
layout(location = 0) out vec4 FragColor;
#include "kin_cutout.inc.glsl"
layout(std140, set = 3, binding = 1) uniform Visibility { uvec4 ids; } query;
void main() {
    if (query.ids.z != 0u && vId == query.ids.y + 1u) discard;
    if (kin_cutout_alpha(vUV, vWorld, vColor.a) < material.cutoff.x) discard;
    const float visible = vId == query.ids.x + 1u ? 1.0 : 0.0;
    FragColor = vec4(vec3(visible), 1.0);
}
