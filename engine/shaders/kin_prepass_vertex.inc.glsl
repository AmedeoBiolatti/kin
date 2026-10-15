// kin's 3D ambient occlusion pre-pass: mesh.vert's placing, and the world
// normal (and, for cut-outs, the UV and alpha prepass_mask.frag tests). No
// colours or world position: only depth and a normal come out of it.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

#include "kin_instance.inc.glsl"

layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection;
    uvec4 first; // x: this draw's first instance
} draw;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vAlpha; // the instance's and the vertex's
layout(location = 3) out vec3 vWorld;

void main() {
    const Instance instance = instances[instance_at(draw.first.x + uint(gl_InstanceIndex))];
    const vec4 world = instance.model * vec4(aPos, 1.0);
    vWorld = world.xyz;
    gl_Position = draw.view_projection * world;
    vNormal = mat3(instance.normal0.xyz, instance.normal1.xyz, instance.normal2.xyz) * aNormal;
    vUV = aUV;
    vAlpha = instance.color.a * aColor.a;
}
