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
layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection;
    uvec4 first;
} draw;
void main() {
    const uint at = instance_at(draw.first.x + uint(gl_InstanceIndex));
    const vec4 world = instances[at].model * vec4(aPos, 1.0);
    gl_Position = draw.view_projection * world;
    vColor = instances[at].color * vec4(decode(aColor.rgb), aColor.a);
#ifdef KIN_UNLIT_TEXTURED
    vUV = aUV;
#endif
}
