// kin's mirror surface (docs/rendering_3d.md, Mirrors and portals): its reflection is a mirror_view's picture, which the
// material carries as its emissive map (Material3D::emissive_texture, with `emissive` 0), read through the projector in
// its parameters (planar_view_params). Lit as kin's own materials are: its colour, metallic and roughness say how much
// it reflects (a metal mirror all, water little head on and much at a glance).
void kin_surface(inout KinSurface s) {
    kin_planar_reflection(s);
}
