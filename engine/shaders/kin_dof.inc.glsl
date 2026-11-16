// Depth of field (DepthOfField3D): the parameters its passes share, and each pixel's distance and blur.
layout(set = 2, binding = 0) uniform Params {
    vec4 size;         // the full picture's extent and reciprocals
    vec4 lens;         // x: the radius (full pixels) at infinity, y: the focus (world units), z: the widest radius,
                       // w: 1 when the focus is uFocus' (autofocus)
    vec4 depth;        // x: 1 when uDepth holds the depth, y: where the overlay's slice of it starts (2: none)
    vec4 unproject_zw; // e, f, g, h: a view-space z of a depth is (e depth + f) / (g depth + h)
    vec4 extra;        // the pass' own (dof_focus: the focus point, the easing, a reset)
}
params;

// The distance along the view of the depth at `uv` (60000 for the sky); 0 for the overlay, in front of everything.
float dof_distance(sampler2D depth_map, vec2 uv) {
    if (params.depth.x < 0.5) {
        return params.lens.y; // no depth: everything in focus
    }
    float d = textureLod(depth_map, uv, 0.0).r;
    if (params.depth.y <= 1.0) {
        if (d >= params.depth.y) {
            return 0.0;
        }
        d /= params.depth.y;
    }
    const float z = -(params.unproject_zw.x * d + params.unproject_zw.y) /
                    (params.unproject_zw.z * d + params.unproject_zw.w);
    return z > 0.0 && z < 60000.0 ? z : 60000.0;
}

// The signed blur radius in full pixels at `distance` with the focus at `focus` (kin::depth_of_field_radius), limited
// to the widest; the overlay (distance 0) is sharp.
float dof_radius(float distance, float focus) {
    if (distance <= 0.0) {
        return 0.0;
    }
    return clamp(params.lens.x * (1.0 - focus / distance), -params.lens.z, params.lens.z);
}
