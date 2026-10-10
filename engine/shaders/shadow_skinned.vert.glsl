#version 450 core
// shadow.vert for a skinned copy: the vertex moved by its joints, then placed as shadow.vert places it.
layout(location = 0) in vec3 aPos;

#include "kin_instance.inc.glsl"

// With cascades, a cascade's copies are a list of indices into the instances
// (the caster test is per cascade); without, they are one run from first.x.
layout(std430, set = 0, binding = 1) readonly buffer Casters {
    uint casters[];
};

#define KIN_SKIN_BINDING 2
#include "kin_skin.inc.glsl"

layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection;
    uvec4 first; // x: this draw's first instance; y: where its list starts, plus one (0: no list)
} draw;

uint instance_index() {
    return draw.first.y != 0u ? casters[draw.first.y - 1u + uint(gl_InstanceIndex)] : draw.first.x + uint(gl_InstanceIndex);
}

void main() {
    const uint slot = instance_index();
    const mat4 model = instances[slot].model;
    gl_Position = draw.view_projection * (model * (skin_matrix(slot) * vec4(aPos, 1.0)));
}
