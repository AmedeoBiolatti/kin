#pragma once

// Clusters for one huge mesh (docs/3d_plan.md, G3b): the CPU side, with no GPU, so that it can be tested alone.
// Renderer3D::create_mesh(data, {.clusters = true}) calls build_mesh_clusters, uploads the welded vertices and keeps
// the rest (MeshClusters, the hierarchy alone); the renderer calls select_clusters for each copy, each frame.
//
// The hierarchy, as Nanite's. The mesh is welded and cut into leaf clusters of at most 128 triangles. Then, round
// after round: the clusters of the round are grouped by adjacency (about 4 to a group), each group's triangles are
// merged and simplified to about half with the group's OUTER border locked (its vertices do not move, and the mesh's
// own open borders are such borders), and the result is split into new clusters, the group's outputs. A cluster
// belongs to one group as an input (its parent group) and to one as an output (the group that made it, none for a
// leaf). A group has an error (the largest error among its inputs plus what its simplification added: it grows up the
// hierarchy) and a bounding sphere that contains its inputs' spheres. All the outputs of a group take the group's
// (error, sphere) as their own: that is what a cluster is judged by, and it is the same for every sibling.
//
// The cut. For a camera, a cluster is "fine" when its own error, projected on the screen at the nearest point of its
// sphere, is at most the allowed pixels. A cluster is drawn when it is fine and its parent group (whose error and
// sphere are at least as large, so it projects larger) is not. Since siblings share one (error, sphere) and
// consumers of a group's outputs share another, every decision is the same for all the clusters of a group and needs
// no word from the neighbours; and since the borders between groups were locked in every simplification above them,
// two neighbouring clusters of different levels have the same edge: no crack. The projected error is monotonic up
// every chain of clusters (error and sphere only grow), so on each chain from a leaf to a root exactly one cluster is
// drawn: the surface is covered once. Everything is in the mesh's own (object) space; a copy's transform scales it.

#include <kin/core/math3d.hpp>
#include <kin/render3d/mesh.hpp>

#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace kin {

constexpr u32 max_cluster_triangles = 128;
constexpr u32 no_cluster = ~0u;

struct MeshCluster {
    // Its triangles: `triangle_count` of them, three u8 each from `triangle_offset` in MeshClusters::triangles,
    // indexing its `vertex_count` vertices from `vertex_offset` in MeshClusters::cluster_vertices, which hold
    // indices into the welded vertices (MeshClusterBuild::vertices).
    u32 vertex_offset = 0;
    u32 triangle_offset = 0;
    u16 vertex_count = 0;
    u16 triangle_count = 0;
    // The round that made it (0: a leaf, a piece of the mesh itself); the round that grouped it into a parent
    // (no_cluster for a root); and the groups it came from and went into (no_cluster for none).
    u32 depth = 0;
    u32 parent_depth = no_cluster;
    u32 group = no_cluster;        // made by
    u32 parent_group = no_cluster; // taken as an input by
    // How far its surface may lie from the finest one, in the mesh's units (0 for a leaf), and the sphere that
    // error is measured at (the group that made it: its sphere holds the surface of all that is under it).
    f32 error = 0.0f;
    Vec3f center{};
    f32 radius = 0.0f;
    // The same for the parent group (infinite error for a root: always too coarse).
    f32 parent_error = std::numeric_limits<f32>::infinity();
    Vec3f parent_center{};
    f32 parent_radius = 0.0f;
    // The tightest sphere round its own triangles, for culling.
    Vec3f cull_center{};
    f32 cull_radius = 0.0f;
};

struct MeshClusterGroup {
    u32 input_first = 0, input_count = 0;   // its inputs in MeshClusters::group_inputs
    u32 output_first = 0, output_count = 0; // its outputs: consecutive clusters
    f32 error = 0.0f;
    Vec3f center{};
    f32 radius = 0.0f;
};

// The hierarchy: everything the cut reads, and all that a mesh keeps of it on the CPU. Its clusters index the welded
// vertices that were built with it (MeshClusterBuild), which are on the GPU once the mesh is made and not kept here:
// the cut never reads a vertex, and the kiln's 2.5 M welded vertices would hold 90 MB for nothing.
struct MeshClusters {
    bool flat = false; // the welded vertices have no normal (zero), for the shader to make from derivatives
    std::vector<MeshCluster> clusters; // the leaves first, then each round's
    std::vector<MeshClusterGroup> groups;
    std::vector<u32> group_inputs;
    std::vector<u32> cluster_vertices;
    std::vector<u8> triangles;
    std::vector<u32> roots;
    u32 leaf_count = 0;       // clusters [0, leaf_count) are leaves
    u32 levels = 0;           // rounds, leaves included
    u32 leaf_triangles = 0;
    u64 total_triangles = 0;
    u32 degenerate_removed = 0; // triangles with a repeated vertex, dropped at the weld
    f64 build_ms = 0.0;       // what the build took (restored from a bake: the build's that was baked)
    u32 stuck_groups = 0;     // groups that could not be simplified (their clusters wait for a regrouping)

    // Appends cluster `c`'s triangles as indices into the welded vertices (triangle_count * 3 of them) at `out`.
    void append_indices(u32 c, u32* out) const {
        const MeshCluster& k = clusters[c];
        const u32* v = cluster_vertices.data() + k.vertex_offset;
        const u8* t = triangles.data() + k.triangle_offset;
        for (u32 i = 0, n = k.triangle_count * 3u; i < n; ++i) {
            out[i] = v[t[i]];
        }
    }
    // The leaves' triangles as indices, in cluster order: the mesh at full detail.
    std::vector<u32> leaf_indices() const;
    MeshClusterInfo info() const;
};

// What build_mesh_clusters makes: the hierarchy and the mesh welded, by position, UV and colour (a flat-shaded mesh:
// its normals are then zero, for the shader to make from derivatives), else by every attribute; indexed by the
// clusters. Renderer3D::create_mesh uploads `vertices` with the leaves' triangles, then keeps the hierarchy alone (it
// moves the MeshClusters part out) and lets the vertices go: they live only while the mesh is being made.
struct MeshClusterBuild : MeshClusters {
    std::vector<MeshVertex> vertices;
};

// Runs `body(i)` for i in [0, count) on any threads; nothing else is assumed. Empty: serial.
using ClusterParallelFor = std::function<void(u32 count, const std::function<void(u32)>& body)>;

// The hierarchy for `data` (valid: MeshData::validate). Takes about as long as the simplifier takes on the mesh
// (seconds for millions of triangles; `parallel` spreads the groups over threads). One cluster, no groups, for a
// mesh of at most options.cluster_triangles triangles.
MeshClusterBuild build_mesh_clusters(const MeshData& data, const MeshOptions& options, const ClusterParallelFor& parallel = {});

// For a bake cache (kin/assets/bake_cache.hpp; Renderer3D::set_bake_cache uses these). The key of what
// build_mesh_clusters(data, options) makes: the mesh's vertex and index bytes, the options the build reads
// (cluster_triangles and the three attribute weights), max_cluster_triangles, this format's version and
// meshoptimizer's. The build as bytes: every field of the hierarchy and the welded vertices, little-endian, fixed
// width (the leaves' indices are not kept: leaf_indices() makes them again, the same). And back: false, with `out`
// emptied, for bytes that are not a whole build (short, too long, or an offset, count or index out of range), so
// that no bytes can make a hierarchy the cut or the upload would read out of bounds.
u64 mesh_clusters_bake_key(const MeshData& data, const MeshOptions& options);
std::vector<u8> save_mesh_clusters(const MeshClusterBuild& build);
bool restore_mesh_clusters(std::span<const u8> bytes, MeshClusterBuild& out);

// What a cut depends on besides the clusters, for one copy: its place in the world, and the camera.
struct ClusterCutView {
    Mat4f transform = Mat4f::identity(); // object to world
    Vec3f eye{};
    Vec3f forward{0.0f, 0.0f, -1.0f};
    bool orthographic = false;
    f32 per_unit = 1.0f; // pixels a world unit spans at distance 1 (perspective), or anywhere (orthographic)
    f32 pixels = 1.0f;   // the error allowed, in pixels
    f32 nearest = 0.1f;  // distances are not taken under this (inside a sphere, or behind the camera)
    // -1: by error. N >= 0: the cut at hierarchy depth N (0: the leaves; past the last depth: the roots).
    i32 force_depth = -1;
};

struct SelectedCluster {
    u32 cluster = 0;
    u32 views = 0; // the bits of the views (Cull::views) whose frustum the cluster's own sphere touches
};

// A cut without any culling: one view, everything in it.
struct NoClusterCull {
    bool reaches(Vec3f, f32) const { return true; }
    u32 views(Vec3f, f32) const { return 1u; }
};

// The scale a transform gives lengths (its largest axis').
f32 cluster_scale_of(const Mat4f& transform);

// Whether the cluster's own error is within the allowed pixels, and whether its parent's is (a root's never is).
// The functions the cut is made of: select_clusters is these over a traversal.
struct ClusterCutTest {
    explicit ClusterCutTest(const ClusterCutView& view);
    // The distance from the camera to the nearest point of the world sphere (or 1, orthographic): along its
    // forward where the sphere is in front, to the sphere's surface behind. Never under `nearest`.
    f32 distance(Vec3f world_center, f32 world_radius) const;
    bool fine(f32 error, Vec3f center, f32 radius) const;
    bool selected(const MeshCluster& c) const;

    const ClusterCutView& view;
    f32 scale = 1.0f;
};

// Every cluster of the cut for `view`, in a fixed order, found top-down through the hierarchy. `cull` (reaches,
// views; see NoClusterCull) takes the world spheres: a subtree whose sphere no view reaches is skipped, and a chosen
// cluster is kept when its own sphere is in some view (and says which). `seen` and `stack` are scratch; `tested`
// (optional) gets the number of clusters looked at.
template <typename Cull>
void select_clusters(const MeshClusters& mesh, const ClusterCutView& view, const Cull& cull, std::vector<SelectedCluster>& out,
                     std::vector<u8>& seen, std::vector<u32>& stack, u32* tested = nullptr) {
    out.clear();
    if (tested) {
        *tested = 0;
    }
    const ClusterCutTest test{view};
    seen.assign(mesh.groups.size(), 0);
    stack.assign(mesh.roots.rbegin(), mesh.roots.rend());
    const bool forced = view.force_depth >= 0;
    const u32 depth = forced ? static_cast<u32>(view.force_depth) : 0u;
    while (!stack.empty()) {
        const u32 id = stack.back();
        stack.pop_back();
        const MeshCluster& c = mesh.clusters[id];
        if (tested) {
            ++*tested;
        }
        const Vec3f lod_center = view.transform.apply(c.center);
        if (!cull.reaches(lod_center, c.radius * test.scale)) {
            continue;
        }
        const bool fine = forced ? c.depth <= depth : test.fine(c.error, c.center, c.radius);
        if (fine) {
            const bool parent_fine = forced ? c.parent_depth != no_cluster && c.parent_depth <= depth
                                            : test.fine(c.parent_error, c.parent_center, c.parent_radius);
            if (!parent_fine) {
                const u32 views = cull.views(view.transform.apply(c.cull_center), c.cull_radius * test.scale);
                if (views != 0) {
                    out.push_back({id, views});
                }
            }
        } else if (c.group != no_cluster && !seen[c.group]) {
            seen[c.group] = 1;
            const MeshClusterGroup& g = mesh.groups[c.group];
            for (u32 i = g.input_count; i-- > 0;) {
                stack.push_back(mesh.group_inputs[g.input_first + i]);
            }
        }
    }
}

template <typename Cull>
std::vector<SelectedCluster> select_clusters(const MeshClusters& mesh, const ClusterCutView& view, const Cull& cull) {
    std::vector<SelectedCluster> out;
    std::vector<u8> seen;
    std::vector<u32> stack;
    select_clusters(mesh, view, cull, out, seen, stack);
    return out;
}

} // namespace kin
