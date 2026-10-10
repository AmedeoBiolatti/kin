#version 450 core
// shadow_mask.vert for a skinned copy: the vertex moved by its joints, then placed as shadow_mask.vert places it.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

#include "kin_instance.inc.glsl"

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

layout(location = 0) out vec2 vUV;
layout(location = 1) out float vAlpha; // the instance's and the vertex's

void main() {
    const uint slot = instance_index();
    const Instance instance = instances[slot];
    gl_Position = draw.view_projection * (instance.model * (skin_matrix(slot) * vec4(aPos, 1.0)));
    vUV = aUV;
    vAlpha = instance.color.a * aColor.a;
}
