// What material graphs' nodes call (docs/material_graph_plan.md; kin/render3d/material_graph3d.hpp writes the calls):
// included by a graph's surface shader, after kin_mesh_surface.inc.glsl (the kin_* functions and the samplers).

// A map read as colour: sRGB decoded where kin reads its textures undecoded (as kin_base_color does), or as data.
vec4 kin_graph_sample(sampler2D map, vec2 uv, bool srgb) {
    vec4 t = texture(map, uv);
    if (srgb && frame.flags.y > 0.5) {
        t.rgb = decode(t.rgb);
    }
    return t;
}

// The surface's normal before any map: the mesh's, on the viewer's side (a face's own where the mesh has none).
vec3 kin_graph_geometry_normal() {
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    const float front = gl_FrontFacing ? 1.0 : -1.0;
    return dot(vNormal, vNormal) < 1e-20 ? kin_screen_normal(dp1, dp2) : normalize(vNormal) * front;
}

// A tangent-space normal (x right, y up as glTF's maps have it, z out) in world space, by the frame the UVs make across
// the pixel (as mapped_normal's), about `n`.
vec3 kin_graph_tangent_to_world(vec3 m, vec2 uv, vec3 n) {
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    const vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    const float det = duv1.x * duv2.y - duv2.x * duv1.y;
    if (abs(det) < 1e-20) {
        return n;
    }
    const float front = gl_FrontFacing ? 1.0 : -1.0;
    vec3 t = (dp1 * duv2.y - dp2 * duv1.y) * sign(det);
    vec3 b = (dp2 * duv1.x - dp1 * duv2.x) * sign(det);
    t = normalize(t - n * dot(n, t));
    b = normalize(b - n * dot(n, b));
    return normalize(front * (t * m.x - b * m.y) + n * m.z);
}

// A normal map's texel as a tangent-space normal, its slope times `strength` (two-channel maps: z rebuilt).
vec3 kin_graph_unpack_normal(vec4 texel, float strength) {
    vec3 m = texel.xyz * 2.0 - 1.0;
    if (m.z < 0.0) {
        m.z = sqrt(max(1.0 - dot(m.xy, m.xy), 0.0));
    }
    m.xy *= strength;
    return normalize(m);
}

// A map projected along the three world axes, blended by how the surface faces each (`sharpness`: the blend's power).
vec4 kin_graph_triplanar(sampler2D map, vec3 p, vec3 n, float scale, float sharpness, bool srgb) {
    vec3 w = pow(abs(n), vec3(max(sharpness, 1.0)));
    w /= max(w.x + w.y + w.z, 1e-6);
    return kin_graph_sample(map, p.zy * scale, srgb) * w.x + kin_graph_sample(map, p.xz * scale, srgb) * w.y +
           kin_graph_sample(map, p.xy * scale, srgb) * w.z;
}

#include "kin_graph_noise.inc.glsl"
