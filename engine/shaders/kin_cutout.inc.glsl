// What the cut-out variants of the shadow, pick and pre-passes bind
// (Blend3D::Mask): the base colour, whose alpha is tested, and the cutoff.
layout(set = 2, binding = 0) uniform sampler2D uBase;
layout(set = 3, binding = 0) uniform Cutoff {
    vec4 cutoff; // x: the alpha below which nothing is drawn
    vec4 uv;     // xy: the maps' scale, zw: their offset (Material3D::uv_scale, uv_offset)
} material;
