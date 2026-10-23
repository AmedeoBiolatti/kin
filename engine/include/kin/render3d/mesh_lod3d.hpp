#pragma once

// Levels of detail for meshes (docs/3d_plan.md, G3a): the CPU side, with no
// GPU, so that it can be tested alone. Renderer3D::create_mesh(data, options)
// calls it and uploads the result.

#include <kin/render3d/mesh3d.hpp>
#include <span>
#include <vector>

namespace kin {

struct MeshLodLevel3D {
    u32 triangles = 0;
    // An upper bound on the distance from this level's surface to the mesh's,
    // in the mesh's units: the errors the simplifier reported for the steps
    // down to here, added up. 0 for level 0.
    f32 error = 0.0f;
    // The triangles (three indices each) over the mesh's own vertices, and past them the chain's extra vertices
    // (index vertex_count + i is extra_vertices[i]). Empty for level 0, which is the mesh's own indices (or its
    // vertices, three by three).
    std::vector<u32> indices;
};

struct MeshLodChain3D {
    // [0] is the mesh as given; each next one is simpler, with a larger error.
    std::vector<MeshLodLevel3D> levels;
    // Vertices the levels have of their own, to be kept after the mesh's: made for a flat-shaded mesh with no
    // indices (its triangles share no vertex, so a level is made of new ones). Each is a welded position some level
    // uses, once, shared by all the levels, with a zero normal: the shader then makes the face's own (as for flat
    // clusters; a normal map has no effect on those levels). Empty otherwise.
    std::vector<MeshVertex3D> extra_vertices;
    std::size_t count() const { return levels.size(); }
};

// The chain for `data` (which must be valid: MeshData3D::validate). One level
// (just level 0) where options.levels is 1, the mesh is small, or it cannot be
// simplified by `options.min_reduction`. Takes time in proportion to the
// triangles: about a second for a million.
MeshLodChain3D build_mesh_levels(const MeshData3D& data, const MeshOptions3D& options);

// For a bake cache (kin/assets/bake_cache.hpp; Renderer3D::set_bake_cache uses these). The key of what
// build_mesh_levels(data, options) makes: the mesh's vertex and index bytes, every option the chain reads (levels,
// target_ratio, min_reduction, min_triangles, max_error, the three weights, lock_border), this format's version and
// meshoptimizer's. The chain as bytes (every level's triangles, error and indices, and the extra vertices;
// little-endian, fixed width), and back for the same `data`: false, with `out` emptied, for bytes that are not a whole
// chain of that mesh (short, too long, a level 0 that is not the mesh, an index past its vertices and the extra ones).
u64 mesh_levels_bake_key(const MeshData3D& data, const MeshOptions3D& options);
std::vector<u8> save_mesh_levels(const MeshLodChain3D& chain);
bool restore_mesh_levels(std::span<const u8> bytes, const MeshData3D& data, MeshLodChain3D& out);

} // namespace kin
