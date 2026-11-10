#version 450 core
// Local coverage and reprojection for blended meshes after AO/contact shading.
// Custom blended shaders use their triangle footprint; opaque shaders keep opaque motion.
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vUV;
layout(set = 2, binding = 0) uniform sampler2D uBase;
layout(set = 3, binding = 0) uniform Reactive {
    vec4 flags; // x: custom coverage, y: discontinuous appearance
    vec4 uv;    // xy: the maps' scale, zw: their offset (Material3D::uv_scale, uv_offset)
}
reactive;
#include "kin_velocity.inc.glsl"
layout(location = 5) in vec3 vCurrent;
layout(location = 6) in vec3 vPrevious;
layout(location = 7) flat in float vFresh;
layout(location = 0) out vec4 FragReactive;
void main() {
    if (reactive.flags.x < 0.5 && vColor.a * texture(uBase, vUV * reactive.uv.xy + reactive.uv.zw).a <= 0.0)
        discard;
    const vec2 velocity = kin_velocity(vCurrent, vPrevious, vFresh);
    // R=2: matched coverage; R=3: newly incompatible history.
    FragReactive = vec4(vFresh > .5 || reactive.flags.y > .5 ? 3.0 : 2.0, 0.0, velocity);
}
