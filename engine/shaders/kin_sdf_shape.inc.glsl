// Shared native/WebGL analytic shape distance, one-pixel coverage and compositing.
float kin_shape_box(vec2 p, vec2 half_size, float r, bool sharp) {
    vec2 q = abs(p) - half_size + r;
    if (sharp) return max(q.x, q.y);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float kin_shape_ellipse(vec2 p, vec2 ab) {
    float k0 = length(p / ab);
    float k1 = length(p / (ab * ab));
    return k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : -min(ab.x, ab.y);
}

vec4 kin_shape_color(vec2 p, vec4 size, float kind, vec4 fill, vec4 stroke) {
    float d = kind > 1.5 ? kin_shape_ellipse(p, size.xy) : kin_shape_box(p, size.xy, size.z, kind > 0.5);
    float pixel = max(length(vec2(dFdx(d), dFdy(d))), 1e-6);
    float fill_cover = clamp(1.0 - d / pixel, 0.0, 1.0);
    float stroke_cover = size.w > 0.0 ? clamp(1.0 - (abs(d) - size.w) / pixel, 0.0, 1.0) : 0.0;
    vec4 f = vec4(fill.rgb, 1.0) * fill.a * fill_cover;
    vec4 s = vec4(stroke.rgb, 1.0) * stroke.a * stroke_cover;
    vec4 c = s + f * (1.0 - s.a);
    return c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
}
