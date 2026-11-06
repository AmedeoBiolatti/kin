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

// A map of Material3D::graph_maps (the array, its layer `layer`): sRGB decoded when `srgb` (the array holds its texels
// as stored, whatever the pipeline).
vec4 kin_graph_sample(sampler2DArray maps, float layer, vec2 uv, bool srgb) {
    vec4 t = texture(maps, vec3(uv, layer));
    if (srgb) {
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

// The frame the UVs make across the pixel, as kin_graph_tangent_to_world's: `t` along +u, `b` along +v (in the
// direction v grows: down a picture), both on the viewer's side and square to `n`; false where the UVs do not change.
bool kin_graph_uv_frame(vec2 uv, vec3 n, out vec3 t, out vec3 b) {
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    const vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    const float det = duv1.x * duv2.y - duv2.x * duv1.y;
    t = vec3(0.0);
    b = vec3(0.0);
    if (abs(det) < 1e-20) {
        return false;
    }
    const float front = gl_FrontFacing ? 1.0 : -1.0;
    t = (dp1 * duv2.y - dp2 * duv1.y) * sign(det);
    b = (dp2 * duv1.x - dp1 * duv2.x) * sign(det);
    t = front * normalize(t - n * dot(n, t));
    b = front * normalize(b - n * dot(n, b));
    return true;
}

// A world vector in that frame (x along +u, y along +v, z along `n`: Unreal's tangent space), and back; unscaled (not
// normalized). Where the UVs do not change: z along `n` only.
vec3 kin_graph_world_to_uv(vec3 v, vec2 uv, vec3 n) {
    vec3 t, b;
    kin_graph_uv_frame(uv, n, t, b);
    return vec3(dot(v, t), dot(v, b), dot(v, n));
}
vec3 kin_graph_uv_to_world(vec3 v, vec2 uv, vec3 n) {
    vec3 t, b;
    kin_graph_uv_frame(uv, n, t, b);
    return t * v.x + b * v.y + n * v.z;
}

// A world vector in the camera's frame (x right, y up, z ahead: Unreal's view space), and back; the frame from the
// camera's forward and the world's up (a camera that does not roll; looking straight up or down: x along world x).
mat3 kin_graph_view_frame() {
    const vec3 f = normalize(frame.cascade_forward.xyz);
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

// The light the scene's surroundings send from direction `d` (world space): its environment's sharpest level, else
// the hemisphere's ambient (the sky's above, the ground's below).
vec3 kin_graph_environment(vec3 d) {
    d = normalize(d);
    if (frame.environment.w > 0.5) {
        return textureLod(uEnvSpecular, environment_uv(d), 0.0).rgb * frame.environment.x;
    }
    return mix(frame.ground_ambient.rgb, frame.ambient.rgb, d.y * 0.5 + 0.5);
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

// The same of a layer of the graph maps.
vec4 kin_graph_triplanar(sampler2DArray maps, float layer, vec3 p, vec3 n, float scale, float sharpness, bool srgb) {
    vec3 w = pow(abs(n), vec3(max(sharpness, 1.0)));
    w /= max(w.x + w.y + w.z, 1e-6);
    return kin_graph_sample(maps, layer, p.zy * scale, srgb) * w.x +
           kin_graph_sample(maps, layer, p.xz * scale, srgb) * w.y +
           kin_graph_sample(maps, layer, p.xy * scale, srgb) * w.z;
}

// HLSL's saturate (imported Custom nodes).
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

// The derivatives of an equirectangular uv (kin_graph_equirect_uv's) across the pixel: u's from its half-turned copy
// where that one changes less (the seam, where u wraps: else the smallest level).
void kin_graph_equirect_grads(vec3 uv, out vec2 dx, out vec2 dy) {
    dx = dFdx(uv.xy);
    dy = dFdy(uv.xy);
    const float dx2 = dFdx(uv.z), dy2 = dFdy(uv.z);
    dx.x = abs(dx2) < abs(dx.x) ? dx2 : dx.x;
    dy.x = abs(dy2) < abs(dy.x) ? dy2 : dy.x;
}

// An equirectangular map's texel in direction `d` (kin_graph_equirect_uv's layout), seamless.
vec4 kin_graph_sample_equirect(sampler2D map, vec3 d, bool srgb) {
    const vec3 uv = kin_graph_equirect_uv(d);
    vec2 dx, dy;
    kin_graph_equirect_grads(uv, dx, dy);
    vec4 t = textureGrad(map, uv.xy, dx, dy);
    if (srgb && frame.flags.y > 0.5) {
        t.rgb = decode(t.rgb);
    }
    return t;
}
vec4 kin_graph_sample_equirect(sampler2DArray maps, float layer, vec3 d, bool srgb) {
    const vec3 uv = kin_graph_equirect_uv(d);
    vec2 dx, dy;
    kin_graph_equirect_grads(uv, dx, dy);
    vec4 t = textureGrad(maps, vec3(uv.xy, layer), dx, dy);
    if (srgb) {
        t.rgb = decode(t.rgb);
    }
    return t;
}
