// A surface shader (test_gpu_surface_shader): the base colour times kin_surface_param(0).rgb, the roughness
// kin_surface_param(0).w, and a glow of kin_surface_param(1).rgb where the surface faces +x.
void kin_surface(inout KinSurface s) {
    s.base_color *= kin_surface_param(0).rgb;
    s.roughness = clamp(kin_surface_param(0).w, 0.045, 1.0);
    s.emission += kin_surface_param(1).rgb * max(s.normal.x, 0.0);
}
