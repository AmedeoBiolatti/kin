// A surface shader (test_gpu_gi_proxy): a base colour of its own, kin_surface_param(0).rgb, whatever its base map holds
// (as a material graph's may hold a mask or a normal map), and an emission of its own, kin_surface_param(1).rgb.
void kin_surface(inout KinSurface s) {
    s.base_color = kin_surface_param(0).rgb;
    s.emission = kin_surface_param(1).rgb;
}
