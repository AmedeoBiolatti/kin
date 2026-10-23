#version 450 core
// kin's 3D shadow pass for cut-out materials (Blend3D::Mask): shadow.vert's
// placing, and the UV and alpha shadow_mask.frag tests.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

// The model matrix and colour are used here.
#include "kin_instance.inc.glsl"

// With cascades, a cascade's copies are a list of indices into the instances
// (the caster test is per cascade); without, they are one run from first.x.
layout(std430, set = 0, binding = 1) readonly buffer Casters {
    uint casters[];
};

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
    const Instance instance = instances[instance_index()];
    gl_Position = draw.view_projection * (instance.model * vec4(aPos, 1.0));
    vUV = aUV;
    vAlpha = instance.color.a * aColor.a;
}
