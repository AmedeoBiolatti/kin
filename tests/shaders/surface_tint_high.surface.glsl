// surface_tint.surface.glsl reading its parameters from the last two of Material3D::shader_params' 16 vec4s
// (test_gpu_surface_batching: the same pictures, merged and not, from floats 56 to 63).
void kin_surface(inout KinSurface s) {
    s.base_color *= kin_surface_param(14).rgb;
    s.roughness = clamp(kin_surface_param(14).w, 0.045, 1.0);
    s.emission += kin_surface_param(15).rgb * max(s.normal.x, 0.0);
}
