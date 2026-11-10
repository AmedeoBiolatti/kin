#pragma once

// A scene as it was rendered, written out for an offline path tracer to render the same (tools/cycles: Blender's
// Cycles, the truth kin's picture is compared with). For tools and tests, not games.

#include <filesystem>
#include <kin/render3d/renderer3d.hpp>

namespace kin {

struct SnapshotOptions3D {
    // kin's planar surfaces (PlanarSurfaces3D, kin/scene/planar_views3d.hpp): a material with the reflection surface is
    // written as the material under it (a mirror is then a real mirror, water a real refracting surface where its
    // refraction parameter is above 0); one with the window surface (a portal) is not compared (a hole in the truth's
    // picture, which the comparison leaves out).
    SurfaceShader3D reflection, window;
    // The view the picture was rendered with: its exposure, tonemap and background go with the scene.
    ViewOptions3D view{};
    // The bakes' density: texels a world unit along a triangle, each triangle's cell 8 to `max_cell` texels a side, in
    // pages of `page_size`.
    f32 texels_per_unit = 128.0f;
    u32 max_cell = 2048;
    u32 page_size = 4096;
    // The sky's panorama's face side (Scene3D::sky enabled), 16 to 1024.
    u32 environment_side = 256;
};

struct SnapshotStats3D {
    u32 objects = 0;                // mesh copies written
    u32 baked = 0;                  // of them, with their material baked into the pages
    u32 pages = 0;                  // texture pages
    u32 meshes = 0;                 // mesh files
    u64 triangles = 0;              // over all copies
    std::vector<std::string> notes; // what the truth leaves out or approximates (fog, particles, ...)
};

// Writes `scene` (as rendered into a view of `size` with options.view) into `folder`: scene.json (the camera, the
// lights, the sky or ambient, each object's mesh, placing and material), meshes/*.bin (object-space triangles, three
// vertices each of 13 floats: position, normal, UV (a baked copy's: in its page, v down), linear colour, page (-1:
// none); skinned copies posed; copies of one mesh that are not baked share a file), pages/*: each baked material's
// channels (a material with maps or a surface: its base colour (sRGB PNG), alpha (PNG), world normal (PNG, n * 0.5 +
// 0.5, kin's axes), occlusion, roughness and metallic (PNG, linear) and emission (.hdr, only for a page with any),
// drawn per copy over its triangles' own cells of the pages), and sky.hdr (the sky's light as a panorama, without the
// sun's disc: the sun is a lamp). Waits for the GPU. Fails with the reason when something cannot be read or written.
Result3D<SnapshotStats3D> write_scene_snapshot(Renderer3D& renderer, const Scene3D& scene, Vec2i size,
                                               const std::filesystem::path& folder,
                                               const SnapshotOptions3D& options = {});

} // namespace kin
