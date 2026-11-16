// Unlit main-pass vertices: the instance ABI stays shared with all other passes.
// No normal transform, world-position or picking output; UVs only with a base texture.
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec4 aColor;
layout(location = 1) out vec4 vColor;
#ifdef KIN_UNLIT_TEXTURED
layout(location = 2) in vec2 aUV;
layout(location = 2) out vec2 vUV;
#endif
#include "kin_instance.inc.glsl"
#include "kin_srgb.inc.glsl"
// The same position as the pre-pass's to the bit (its depth can be the main pass's start: RenderDebugOptions3D::
// prepass_depth), as the lit vertex shaders declare it: without, an unlit mesh speckled with the background there.
invariant gl_Position;
layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection;
    uvec4 first;
    vec4 clip; // Camera3D::clip_plane: drawn where dot(xyz, world) + w >= 0 (0, 0, 0, 1: everywhere)
} draw;
void main() {
    const uint at = instance_at(draw.first.x + uint(gl_InstanceIndex));
    const vec4 world = instances[at].model * vec4(aPos, 1.0);
    gl_Position = draw.view_projection * world;
    gl_ClipDistance[0] = kin_clip_distance(draw.clip, world.xyz, instances[at].normal1.w, draw.first.z, instances[at].normal0.w);
    vColor = instances[at].color * vec4(decode(aColor.rgb), aColor.a);
#ifdef KIN_UNLIT_TEXTURED
    vUV = aUV;
#endif
}
