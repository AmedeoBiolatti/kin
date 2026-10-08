#pragma once

// Levels of detail for meshes (docs/3d_plan.md, G3a): the CPU side, with no
// GPU, so that it can be tested alone. Renderer3D::create_mesh(data, options)
// calls it and uploads the result.

#include <kin/render3d/mesh.hpp>

#include <vector>

namespace kin {

struct MeshLodLevel {
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

struct MeshLodChain {
    // [0] is the mesh as given; each next one is simpler, with a larger error.
    std::vector<MeshLodLevel> levels;
    // Vertices the levels have of their own, to be kept after the mesh's: made for a flat-shaded mesh with no
    // indices (its triangles share no vertex, so a level is made of new ones). Empty otherwise.
    std::vector<MeshVertex> extra_vertices;
    std::size_t count() const { return levels.size(); }
};

// The chain for `data` (which must be valid: MeshData::validate). One level
// (just level 0) where options.levels is 1, the mesh is small, or it cannot be
// simplified by `options.min_reduction`. Takes time in proportion to the
// triangles: about a second for a million.
MeshLodChain build_mesh_levels(const MeshData& data, const MeshOptions& options);

} // namespace kin
