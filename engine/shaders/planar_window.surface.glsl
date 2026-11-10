// kin's portal surface (docs/rendering_3d.md, Mirrors and portals): it shows a portal_view's picture, which the material
// carries as its emissive map (Material3D::emissive_texture, with `emissive` 0), read through the projector in its
// parameters (planar_view_params). For an unlit material (Material3D::unlit).
void kin_surface(inout KinSurface s) {
    kin_planar_window(s);
}
