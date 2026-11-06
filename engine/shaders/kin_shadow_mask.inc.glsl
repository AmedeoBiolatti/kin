// The body of shadow_mask.frag.glsl (without #version), which a surface's cut-out variant shares.
// kin's 3D shadow pass for cut-out materials (Blend3D::Mask): depth only,
// where the base colour's alpha reaches the cutoff, as mesh.frag keeps it.
layout(location = 0) in vec2 vUV;
layout(location = 1) in float vAlpha;
#ifdef KIN_SURFACE_ALPHA
layout(location = 2) in vec3 vWorld;
#else
const vec3 vWorld = vec3(0.0);
#endif

#include "kin_cutout.inc.glsl"

void main() {
    if (kin_cutout_alpha(vUV, vWorld, vAlpha) < material.cutoff.x) {
        discard;
    }
}
