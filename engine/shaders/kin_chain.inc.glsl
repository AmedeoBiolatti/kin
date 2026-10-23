// What the compute shaders that read the linear depth chain share (gtao_chain.comp, contact_chain.comp).
// The chain (ao_depth_chain.comp) holds, in R32F mip levels, the distance in front of the camera (-z of the
// view space, 0 for the sky) of each pre-pass depth pixel, and at the coarser levels a weighted average of
// the pixels a texel covers, leaning to the farthest.

// A position in view space from a point in depth pixels (x right, y down, in full-size pixels) and the
// distance L in front of the camera. Needs the shader's Params block (declared before this include) to
// hold sizes (zw: the depth's size) and unproject_xy and unproject_zw. A perspective projection has no
// skew and its inverse's z is f / w, so x and y over w follow from the distance without a divide; an
// orthographic projection's do not depend on it at all.
vec3 chain_position(vec2 px, float L, bool ortho) {
    const vec2 ndc = vec2(px.x, params.sizes.w - px.y) * (2.0 / params.sizes.zw) - 1.0;
    const vec2 base = vec2(params.unproject_xy.x * ndc.x + params.unproject_xy.z, params.unproject_xy.y * ndc.y + params.unproject_xy.w);
    return vec3(base * (ortho ? 1.0 : -L / params.unproject_zw.y), -L);
}
