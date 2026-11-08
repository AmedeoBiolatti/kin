// The second half of kin's 3D mesh fragment shader (kin_mesh_fragment.inc.glsl): main().
//
// Where KIN_SURFACE names a function `void name(inout KinSurface s)` (a surface shader's, defined between the two
// halves), the surface is kin_stock_surface() as that function leaves it: alpha-tested, then lit, or unlit (its base
// colour as it is), plus its emission. Without it, kin's own material as ever.
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
    const vec3 c = material.params.y > 0.5
                       ? s.base_color + s.emission
                       : kin_lit_surface(s) + s.emission;
#else
    const vec4 color = kin_base_color();
    float alpha = color.a;
    if (alpha < material.params.w) {
        discard;
    }
    // A cut-out is opaque where it stays.
    if (material.params.w > 0.0) {
        alpha = 1.0;
    }
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
#endif
    FragColor = kin_output(c, alpha);
#ifdef KIN_VELOCITY
    FragVelocity = vec4(0.0, kin_independent_motion(vCurrent, vOldInCurrent) > motion_footprint ? 4.0 : 0.0, kin_velocity(vCurrent, vPrevious, vFresh));
#endif
}
