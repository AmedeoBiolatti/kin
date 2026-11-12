// The second half of kin's 3D mesh fragment shader (kin_mesh_fragment.inc.glsl): main().
//
// Where KIN_SURFACE names a function `void name(inout KinSurface s)` (a surface shader's, defined between the two
// halves), the surface is kin_stock_surface() as that function leaves it: alpha-tested, then lit, or unlit (its base
// colour as it is), plus its emission. Without it, kin's own material as ever.
// A bake's channel (RenderDebugOptions3D::surface_channel) in place of the light: 1 the base colour, 2 the normal
// (world, as it is), 3 occlusion, roughness and metalness, 4 the emission (an unlit surface's: its base colour too),
// 5 the alpha. Every pixel drawn is opaque (alpha 1: what the bake covered).
vec3 kin_surface_channel(int which, vec3 base, vec3 normal, float occlusion, float roughness, float metallic,
                         vec3 emission, bool unlit, float alpha) {
    if (which == 5)
        return vec3(alpha);
    if (which == 1)
        return base;
    if (which == 2)
        return normal;
    if (which == 3)
        return vec3(occlusion, roughness, metallic);
    return unlit ? base + emission : emission;
}

void main() {
#ifdef KIN_VELOCITY
    const float motion_footprint = 0.25 * length(fwidth(vCurrent.xy / vCurrent.z));
#endif
#ifdef KIN_SURFACE
    KinSurface s = kin_stock_surface();
    KIN_SURFACE(s);
    float alpha = s.alpha;
    if (alpha < material.params.w) {
        discard;
    }
    if (material.params.w > 0.0) {
        alpha = 1.0;
    }
    vec3 c = material.params.y > 0.5
                 ? s.base_color + s.emission
                 : kin_lit_surface(s) + s.emission;
    if (frame.time.z > 0.5) {
        c = kin_surface_channel(int(frame.time.z + 0.5), s.base_color, s.normal, s.occlusion, s.roughness, s.metallic,
                                s.emission, material.params.y > 0.5, alpha);
        alpha = 1.0;
    }
#else
    const vec4 color = kin_base_color();
    float alpha = color.a;
#ifndef KIN_OPAQUE_DEPTH
    if (alpha < material.params.w) {
        discard;
    }
    // A cut-out is opaque where it stays.
    if (material.params.w > 0.0) {
        alpha = 1.0;
    }
#endif
    vec3 c;
    if (material.params.y > 0.5) {
        // Keep the legacy no-map operation order identical to the minimal unlit shader.
        c = material.emission.w < 0.5 && material.maps.y < 0.5
                ? color.rgb * (vec3(1.0) + vec3(material.params.x))
                : color.rgb + kin_emissive(color.rgb);
    } else {
        const vec2 mr = kin_metallic_roughness();
        c = kin_lit(color.rgb, kin_normal(), mr.x, mr.y, kin_occlusion()) + kin_emissive(color.rgb);
    }
    if (frame.time.z > 0.5) {
        const vec2 mr = kin_metallic_roughness();
        c = kin_surface_channel(int(frame.time.z + 0.5), color.rgb, kin_normal(), kin_occlusion(), mr.y, mr.x,
                                kin_emissive(color.rgb), material.params.y > 0.5, alpha);
        alpha = 1.0;
    }
#endif
    FragColor = kin_output(c, alpha);
#ifdef KIN_VELOCITY
    FragVelocity = vec4(0.0, kin_independent_motion(vCurrent, vOldInCurrent) > motion_footprint ? 4.0 : 0.0, kin_velocity(vCurrent, vPrevious, vFresh));
#endif
}
