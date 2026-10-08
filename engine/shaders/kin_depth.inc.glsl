// What the compute shaders that read the pre-pass share (gtao.comp, contact.comp).

// Interleaved gradient noise (Jimenez): smooth under a blur, cheap.
float noise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// The pre-pass's octahedral normal (see octahedral in kin_prepass.inc.glsl).
vec3 decode_normal(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

// A depth pixel's position in view space. Needs the shader's Params block
// (declared before this include) to hold sizes (zw: the depth's size) and
// unproject_xy and unproject_zw.
vec3 view_position(ivec2 p, float depth) {
    const vec2 ndc = vec2(p.x + 0.5, params.sizes.w - p.y - 0.5) * (2.0 / params.sizes.zw) - 1.0;
    const float w = params.unproject_zw.z * depth + params.unproject_zw.w;
    return vec3(params.unproject_xy.x * ndc.x + params.unproject_xy.z, params.unproject_xy.y * ndc.y + params.unproject_xy.w,
                params.unproject_zw.x * depth + params.unproject_zw.y) / w;
}
