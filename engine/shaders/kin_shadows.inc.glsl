// Shared shadow sampling. The caller supplies frame, locals, vWorld,
// material.params.z and shadow_offset_normal(n).
// The share of the sun one cascade lets through: 3x3 comparisons against its
// layer of the shadow map, from a point pushed a little off the surface (no
// self-shadowing acne). Reverse Z: the map keeps the depth nearest the sun,
// the greatest. -1 where the point is outside the cascade's map.
float cascade_light(int k, vec3 n, vec3 world_dx, vec3 world_dy) {
    const bool receiver_plane = frame.shadow.y > 0.5;
    const vec3 offset_normal = shadow_offset_normal(n);
    const vec4 clip = frame.shadow_matrix[k] * vec4(vWorld + offset_normal * frame.cascade_bias[k].y, 1.0);
    const vec3 ndc = clip.xyz / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return -1.0;
    }
    vec2 gradient = vec2(0.0);
    if (receiver_plane) {
        // Directional maps are orthographic. Transform world derivatives with
        // the selected matrix rather than differentiating across cascade IDs.
        const vec3 dx = (frame.shadow_matrix[k] * vec4(world_dx, 0.0)).xyz;
        const vec3 dy = (frame.shadow_matrix[k] * vec4(world_dy, 0.0)).xyz;
        const vec2 ux = dx.xy * vec2(0.5, -0.5), uy = dy.xy * vec2(0.5, -0.5);
        const float determinant = ux.x * uy.y - ux.y * uy.x;
        if (abs(determinant) > 1e-12) {
            gradient = vec2(dx.z * uy.y - dy.z * ux.y, ux.x * dy.z - uy.x * dx.z) / determinant;
        }
    }
    // Bilinear comparisons span two texel centers on each axis. Their reach
    // depends on subtexel phase and can approach a full texel, not just half.
    // All integer-offset PCF taps share this phase. Bound the greatest plane
    // depth at their four centers without lifting the whole receiver.
    const vec2 phase = fract(uv / frame.shadow.z - 0.5);
    const float filter_bias = frame.shadow.z * (dot(max(gradient, vec2(0.0)), 1.0 - phase)
                                               + dot(max(-gradient, vec2(0.0)), phase));
    const float ref = ndc.z + frame.cascade_bias[k].x + filter_bias;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec2 tap = vec2(x, y) * frame.shadow.z;
            lit += texture(uShadow, vec4(uv + tap, float(k), ref + dot(gradient, tap)));
        }
    }
    return lit / 9.0;
}

// How much of the sun reaches here. With cascades, the one whose slice of
// the view holds this depth, faded into the next over the last tenth of its
// range (so the border isn't a visible seam); one map is just cascade 0.
float sunlight(vec3 n, vec3 world_dx, vec3 world_dy) {
    if (frame.flags.z < 0.5 || material.params.z < 0.5) {
        return 1.0;
    }
    const int count = int(frame.shadow.x);
    if (count <= 1) {
        const float one = cascade_light(0, n, world_dx, world_dy);
        return one < 0.0 ? 1.0 : one;
    }
    const float depth = dot(vWorld - frame.eye.xyz, frame.cascade_forward.xyz);
    int k = 0;
    for (int i = 0; i < count - 1; ++i) {
        k += depth > frame.cascade_end[i] ? 1 : 0;
    }
    float lit = cascade_light(k, n, world_dx, world_dy);
    if (k < count - 1 && depth > frame.cascade_blend[k]) {
        const float t = clamp((depth - frame.cascade_blend[k]) / max(frame.cascade_end[k] - frame.cascade_blend[k], 1e-4), 0.0, 1.0);
        const float next = cascade_light(k + 1, n, world_dx, world_dy);
        // Outside one's map (its fit stops at the area), the other's word stands.
        lit = lit < 0.0 ? next : next < 0.0 ? lit : mix(lit, next, t);
    }
    return lit < 0.0 ? 1.0 : lit;
}

// Custom surface shaders can sample sunlight directly. Call outside varying
// branches so receiver-plane derivatives are defined across the pixel quad.
float sunlight(vec3 n) {
    vec3 world_dx = vec3(0.0), world_dy = vec3(0.0);
    if (frame.shadow.y > 0.5) { world_dx = dFdx(vWorld); world_dy = dFdy(vWorld); }
    return sunlight(n, world_dx, world_dy);
}

#ifdef KIN_LIGHTS
// The share of a local light's shadow map that lets this surface point's light through (a spot's layer, or a face of a
// point light's cube): nine comparisons in the layer's corner of the array, from a point pushed a little off the surface,
// the reference moved toward the light by a bias of texels that grows with the surface's slope to it. Reverse Z, a
// perspective depth: depth(d) = near (far / d - 1) / (far - near). -1 where the point is outside the map's view.
// `offset_normal` is shadow_offset_normal's: the push and the slope are the mesh's own, not a normal map's.
float local_light(int layer, vec3 offset_normal, vec3 l) {
    const vec4 a = locals.local_a[layer];
    const vec4 b = locals.local_b[layer];
    const float texel_world = (locals.local_matrix[layer] * vec4(vWorld, 1.0)).w * a.y;
    const vec4 clip = locals.local_matrix[layer] * vec4(vWorld + offset_normal * (b.y * texel_world), 1.0);
    if (clip.w <= a.z) {
        return -1.0;
    }
    const vec2 ndc = clip.xy / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || clip.w > a.w) {
        return -1.0;
    }
    const float n_l = clamp(dot(offset_normal, l), 0.0, 1.0);
    const float slope = min(sqrt(max(1.0 - n_l * n_l, 0.0)) / max(n_l, 0.05), 12.0);
    const float d = max(clip.w - texel_world * b.x * (1.0 + slope), a.z);
    const float ref = a.z * (a.w / d - 1.0) / (a.w - a.z);
    const float texel = locals.local.x;
    const vec2 base = uv * a.x;
    const vec2 lo = vec2(0.5 * texel), hi = vec2(a.x - 0.5 * texel);
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec2 q = clamp(base + vec2(x, y) * (b.z * texel), lo, hi);
            lit += texture(uLocalShadow, vec4(q, float(layer), ref));
        }
    }
    return lit / 9.0;
}

// Its share of the light, with the shadow faded out toward `b.w` from the light (1: no shadow, or not in the map's view).
float local_shadow(int layer, vec3 offset_normal, vec3 l, float distance) {
    float lit = local_light(layer, offset_normal, l);
    if (lit < 0.0) {
        return 1.0;
    }
    const float end = locals.local_b[layer].w;
    return mix(lit, 1.0, smoothstep(0.8 * end, end, distance));
}

// A point light's: the face its direction from the light falls in (the axis it is longest along), its layer of the
// cube whose first is `first` (-1: no shadow).
float point_light_shadow_from(float first, vec3 offset_normal, vec3 toward, float distance) {
    if (first < 0.0) {
        return 1.0;
    }
    const vec3 from_light = -toward;
    const vec3 m = abs(from_light);
    int face;
    if (m.x >= m.y && m.x >= m.z) {
        face = from_light.x < 0.0 ? 1 : 0;
    } else if (m.y >= m.z) {
        face = from_light.y < 0.0 ? 3 : 2;
    } else {
        face = from_light.z < 0.0 ? 5 : 4;
    }
    return local_shadow(int(first) + face, offset_normal, toward, distance);
}

// The point light `i` of frame.lights'.
float point_light_shadow(int i, vec3 offset_normal, vec3 toward, float distance) {
    return point_light_shadow_from(locals.point_shadow[i].x, offset_normal, toward, distance);
}

// One of the moon's cascades, as cascade_light is the sun's; -1 outside its map.
float moon_cascade_light(int k, vec3 offset_normal) {
    const vec4 clip = locals.moon_matrix[k] * vec4(vWorld + offset_normal * locals.moon_bias[k].y, 1.0);
    const vec3 ndc = clip.xyz / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return -1.0;
    }
    const float ref = ndc.z + locals.moon_bias[k].x;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            lit += texture(uMoonShadow, vec4(uv + vec2(x, y) * locals.moon.x, float(k), ref));
        }
    }
    return lit / 9.0;
}

// How much of the moon reaches here: its cascade for this depth, faded into the next as the sun's are, then by its strength.
float moonlight(vec3 offset_normal) {
    const int count = int(locals.counts.y);
    if (count < 1 || material.params.z < 0.5) {
        return 1.0;
    }
    const float depth = dot(vWorld - frame.eye.xyz, frame.cascade_forward.xyz);
    int k = 0;
    for (int i = 0; i < count - 1; ++i) {
        k += depth > locals.moon_end[i] ? 1 : 0;
    }
    float lit = moon_cascade_light(k, offset_normal);
    if (k < count - 1 && depth > locals.moon_blend[k]) {
        const float t = clamp((depth - locals.moon_blend[k]) / max(locals.moon_end[k] - locals.moon_blend[k], 1e-4), 0.0, 1.0);
        const float next = moon_cascade_light(k + 1, offset_normal);
        lit = lit < 0.0 ? next : next < 0.0 ? lit : mix(lit, next, t);
    }
    return lit < 0.0 ? 1.0 : mix(1.0, lit, locals.counts.z);
}
#endif
