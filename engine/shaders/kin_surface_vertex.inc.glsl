// What a surface's vertex variants (KIN_SURFACE_VERTEX: a material graph's World Position Offset) have before its
// `vec3 kin_surface_offset()` and the stock vertex shader after it (kin_mesh_vertex.inc.glsl, kin_prepass_vertex.inc.glsl,
// kin_prepass_velocity_vertex.inc.glsl, kin_shadow_vertex.inc.glsl), which moves each vertex by kin_vertex_offset: the
// material's five maps and its graph maps as vertex samplers (its storage buffers bind after them), its parameters and
// the frame's clock and camera in a block of their own (pushed with each draw), and the graph's helpers as a vertex can
// have them (no derivatives: a texture's top level; no tangent frame).
#define KIN_VS_SHIFT 6
layout(set = 0, binding = 0) uniform sampler2D uBase;
layout(set = 0, binding = 1) uniform sampler2D uMetalRough;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uOcclusion;
layout(set = 0, binding = 4) uniform sampler2D uEmissive;
layout(set = 0, binding = 5) uniform sampler2DArray uGraphMaps;
layout(set = 1, binding = 1) uniform SurfaceVertex {
    vec4 surface[16]; // Material3D::shader_params
    vec4 time;        // x: Scene3D::time; y: the frame before's (the velocity's previous position)
    vec4 eye;         // xyz: the camera
    vec4 forward;     // xyz: the camera's forward
    vec4 flags;       // y: decode sRGB maps here (a gamma pipeline's undecoded textures)
    vec4 uv;          // the maps' scale and offset (Material3D::uv_scale, uv_offset)
} kin_vs;

#include "kin_srgb.inc.glsl"

// The vertex the offset is for (kin_vertex_offset sets them).
vec3 kin_v_world;
vec3 kin_v_normal;
vec2 kin_v_uv;
vec4 kin_v_color;
float kin_v_time;
mat4 kin_v_model; // the copy's placing (its model matrix: object to world)

vec2 kin_uv() {
    return kin_v_uv * kin_vs.uv.xy + kin_vs.uv.zw;
}
vec3 kin_position() {
    return kin_v_world;
}
vec3 kin_view() {
    return normalize(kin_vs.eye.xyz - kin_v_world);
}
vec4 kin_surface_param(int i) {
    return kin_vs.surface[i];
}
vec3 kin_graph_geometry_normal() {
    return kin_v_normal;
}

vec4 kin_graph_sample(sampler2D map, vec2 uv, bool srgb) {
    vec4 t = textureLod(map, uv, 0.0);
    if (srgb && kin_vs.flags.y > 0.5) {
        t.rgb = decode(t.rgb);
    }
    return t;
}
vec4 kin_graph_sample(sampler2DArray maps, float layer, vec2 uv, bool srgb) {
    vec4 t = textureLod(maps, vec3(uv, layer), 0.0);
    if (srgb) {
        t.rgb = decode(t.rgb);
    }
    return t;
}
vec3 kin_graph_unpack_normal(vec4 texel, float strength) {
    vec3 m = texel.xyz * 2.0 - 1.0;
    if (m.z < 0.0) {
        m.z = sqrt(max(1.0 - dot(m.xy, m.xy), 0.0));
    }
    m.xy *= strength;
    return normalize(m);
}
vec4 kin_graph_triplanar(sampler2D map, vec3 p, vec3 n, float scale, float sharpness, bool srgb) {
    vec3 w = pow(abs(n), vec3(max(sharpness, 1.0)));
    w /= max(w.x + w.y + w.z, 1e-6);
    return kin_graph_sample(map, p.zy * scale, srgb) * w.x + kin_graph_sample(map, p.xz * scale, srgb) * w.y +
           kin_graph_sample(map, p.xy * scale, srgb) * w.z;
}
vec4 kin_graph_triplanar(sampler2DArray maps, float layer, vec3 p, vec3 n, float scale, float sharpness, bool srgb) {
    vec3 w = pow(abs(n), vec3(max(sharpness, 1.0)));
    w /= max(w.x + w.y + w.z, 1e-6);
    return kin_graph_sample(maps, layer, p.zy * scale, srgb) * w.x +
           kin_graph_sample(maps, layer, p.xz * scale, srgb) * w.y +
           kin_graph_sample(maps, layer, p.xy * scale, srgb) * w.z;
}
// No tangent frame at a vertex (the fragment's comes from derivatives): tangent space is the normal's alone.
vec3 kin_graph_world_to_uv(vec3 v, vec2 uv, vec3 n) {
    return vec3(0.0, 0.0, dot(v, n));
}
vec3 kin_graph_uv_to_world(vec3 v, vec2 uv, vec3 n) {
    return n * v.z;
}
mat3 kin_graph_view_frame() {
    const vec3 f = normalize(kin_vs.forward.xyz);
    vec3 r = cross(f, vec3(0.0, 1.0, 0.0));
    r = dot(r, r) < 1e-8 ? vec3(1.0, 0.0, 0.0) : normalize(r);
    return mat3(r, cross(r, f), f);
}
vec3 kin_graph_view_to_world(vec3 v) {
    return kin_graph_view_frame() * v;
}
vec3 kin_graph_world_to_view(vec3 v) {
    return transpose(kin_graph_view_frame()) * v;
}
// (A cube map's environment is the fragment's: none at a vertex.)
vec3 kin_graph_environment(vec3 d) {
    return vec3(0.0);
}
float kin_graph_saturate(float x) {
    return clamp(x, 0.0, 1.0);
}
vec2 kin_graph_saturate(vec2 x) {
    return clamp(x, 0.0, 1.0);
}
vec3 kin_graph_saturate(vec3 x) {
    return clamp(x, 0.0, 1.0);
}
vec4 kin_graph_saturate(vec4 x) {
    return clamp(x, 0.0, 1.0);
}
#include "kin_graph_noise.inc.glsl"

// An equirectangular map's texel in direction `d` (kin_graph_equirect_uv's layout): its top level (no derivatives here).
vec4 kin_graph_sample_equirect(sampler2D map, vec3 d, bool srgb) {
    return kin_graph_sample(map, kin_graph_equirect_uv(d).xy, srgb);
}
vec4 kin_graph_sample_equirect(sampler2DArray maps, float layer, vec3 d, bool srgb) {
    return kin_graph_sample(maps, layer, kin_graph_equirect_uv(d).xy, srgb);
}

// The object's space (its copy's model matrix: what a vertex has, the fragments not): its origin in the world, and a
// point (w 1) or a direction (w 0) from it to the world and back.
vec3 kin_object_position() {
    return kin_v_model[3].xyz;
}
vec3 kin_graph_object_to_world(vec3 v, float w) {
    return (kin_v_model * vec4(v, w)).xyz;
}
vec3 kin_graph_world_to_object(vec3 v, float w) {
    return (inverse(kin_v_model) * vec4(v, w)).xyz;
}

// The graph's offset, in the world (kin's axes, metres): defined after this.
vec3 kin_surface_offset();

// `world` moved by the surface's offset, for a vertex of `normal` (world), `uv`, `color`, at `time`, of a copy placed
// by `model`.
vec3 kin_vertex_offset(vec3 world, vec3 normal, vec2 uv, vec4 color, float time, mat4 model) {
    kin_v_model = model;
    kin_v_world = world;
    kin_v_normal = dot(normal, normal) > 1e-20 ? normalize(normal) : vec3(0.0, 1.0, 0.0);
    kin_v_uv = uv;
    kin_v_color = color;
    kin_v_time = time;
    return world + kin_surface_offset();
}
