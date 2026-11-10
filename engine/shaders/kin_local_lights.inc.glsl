// Shared LocalLightUniforms ABI; the caller chooses KIN_LIGHTS_BINDING (and KIN_LIGHTS_SET: a compute shader's
// uniforms are set 2, a fragment shader's 3).
#ifndef KIN_LIGHTS_SET
#define KIN_LIGHTS_SET 3
#endif
const int MAX_SPOTS = 16;
// The local shadow maps (local_lights3d.hpp): a spot's map is its slot (0 to 7), a point light's cube the six from
// MAX_SPOT_MAPS + 6 x its slot. Each map is a square of a layer of the array (local_c); a spot's has its matrix, a cube's
// faces are the light's 90-degree views (kin_local_clip); a light's numbers are shared by its faces (local_a, local_b:
// a spot's at its slot, a point light's at MAX_SPOT_MAPS + its slot). (SDL_GPU binds at most 4 KiB of a uniform block.)
const int MAX_SPOT_MAPS = 8;
const int MAX_POINT_MAPS = 8;
const int MAX_LOCAL = MAX_SPOT_MAPS + 6 * MAX_POINT_MAPS;
layout(set = KIN_LIGHTS_SET, binding = KIN_LIGHTS_BINDING) uniform LocalLights {
    vec4 counts;      // x: spot lights, y: the moon's cascades (0: no shadow), z: the moon's shadow strength, w: the sun's
    vec4 local;       // x: 1 / the local array's side (a texel in uv)
    vec4 moon;        // x: a texel of the moon's map (uv)
    mat4 moon_matrix[4];
    vec4 moon_bias[4]; // per cascade: x: depth bias, y: normal offset (world units)
    vec4 moon_end;     // the view depth where each cascade ends
    vec4 moon_blend;   // and where it starts to fade into the next
    vec4 spots[MAX_SPOTS * 4]; // per spot: position.xyz radius; colour.rgb size^2; direction.xyz cos(outer); x: 1 / (cos inner - cos outer), y: its map (-1: no shadow)
    vec4 point_shadow[MAX_LIGHTS]; // per point light (as frame.lights): x: its cube's first map (-1: no shadow)
    mat4 local_matrix[MAX_SPOT_MAPS];               // a spot's map: world to its clip space
    vec4 local_a[MAX_SPOT_MAPS + MAX_POINT_MAPS];   // per light: x: its maps' share of the array's side, y: a texel's world size per unit of depth, z: near, w: far
    vec4 local_b[MAX_SPOT_MAPS + MAX_POINT_MAPS];   // per light: x: bias (texels), y: normal offset (texels), z: filter spacing (texels), w: where the shadow has faded to nothing (distance)
    vec4 local_c[MAX_LOCAL];                        // per map: x, y: its corner in its layer (uv), z: its layer of the array
    vec4 local_p[MAX_POINT_MAPS];                   // per cube: the light's position, w: its faces' half angle's tangent
                                                    // (w < 0: its faces' matrices are local_matrix from x on)
} locals;

// The light whose numbers (local_a, local_b) map `m` takes.
int kin_local_light(int m) {
    return m < MAX_SPOT_MAPS ? m : MAX_SPOT_MAPS + (m - MAX_SPOT_MAPS) / 6;
}

// `p` in map `m`'s clip space (w: the distance along its axis): a spot's matrix, or the cube face's view (the light's
// position, the face's axis, Mat4f::look_at's up for it, a perspective of the face's half angle; as cube_face_view).
vec4 kin_local_clip(int m, vec3 p) {
    if (m < MAX_SPOT_MAPS) {
        return locals.local_matrix[m] * vec4(p, 1.0);
    }
    const int cube = (m - MAX_SPOT_MAPS) / 6;
    const int face = (m - MAX_SPOT_MAPS) - 6 * cube;
    const vec4 light = locals.local_p[cube];
    if (light.w < 0.0) {
        return locals.local_matrix[int(light.x) + face] * vec4(p, 1.0);
    }
    const float sign = (face & 1) == 0 ? 1.0 : -1.0;
    const vec3 f = face < 2 ? vec3(sign, 0.0, 0.0) : face < 4 ? vec3(0.0, sign, 0.0) : vec3(0.0, 0.0, sign);
    const vec3 up = abs(f.y) > 0.99 ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);
    const vec3 s = normalize(cross(f, up));
    const vec3 u = cross(s, f);
    const vec3 d = p - light.xyz;
    return vec4(dot(s, d) / light.w, dot(u, d) / light.w, 0.0, dot(f, d));
}
