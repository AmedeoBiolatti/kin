// Cluster hierarchies for meshes (kin/render3d/mesh_clusters3d.hpp), without a GPU: the leaves cover the mesh once,
// errors and spheres only grow up the hierarchy, and every cut through it covers the surface once and leaves no
// crack (the open edges of what is selected, welded by position, are the mesh's own).

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kin/assets/bake_cache.hpp>
#include <kin/render3d/mesh_clusters3d.hpp>
#include <kin/render3d/mesh_lod3d.hpp>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace {

using namespace kin;

constexpr float pi = 3.14159265358979f;

// --- meshes ---

MeshVertex3D vertex(Vec3f p, Vec3f n = {0.0f, 1.0f, 0.0f}, Vec2f uv = {}) {
    MeshVertex3D v;
    v.position = p;
    v.normal = n;
    v.uv = uv;
    return v;
}

// A sphere of triangles from an octahedron, split `subdivisions` times; shared vertices and smooth normals, or
// (flat) every triangle with its own and the face's normal.
MeshData3D icosphere(int subdivisions, bool flat, float radius = 1.0f) {
    std::vector<Vec3f> p = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<std::array<u32, 3>> t = {{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0},
                                         {4, 3, 0}, {1, 3, 4}, {5, 3, 1}, {0, 3, 5}};
    for (int s = 0; s < subdivisions; ++s) {
        std::map<std::pair<u32, u32>, u32> mid;
        const auto middle = [&](u32 a, u32 b) {
            const auto key = std::minmax(a, b);
            const auto it = mid.find(key);
            if (it != mid.end()) {
                return it->second;
            }
            p.push_back(normalize((p[a] + p[b]) * 0.5f));
            return mid[key] = static_cast<u32>(p.size() - 1);
        };
        std::vector<std::array<u32, 3>> next;
        for (const auto& tri : t) {
            const u32 ab = middle(tri[0], tri[1]), bc = middle(tri[1], tri[2]), ca = middle(tri[2], tri[0]);
            next.push_back({tri[0], ab, ca});
            next.push_back({tri[1], bc, ab});
            next.push_back({tri[2], ca, bc});
            next.push_back({ab, bc, ca});
        }
        t = std::move(next);
    }
    MeshData3D m;
    if (flat) {
        for (const auto& tri : t) {
            const Vec3f a = p[tri[0]] * radius, b = p[tri[1]] * radius, c = p[tri[2]] * radius;
            const Vec3f n = normalize(cross(b - a, c - a));
            m.vertices.push_back(vertex(a, n));
            m.vertices.push_back(vertex(b, n));
            m.vertices.push_back(vertex(c, n));
        }
    } else {
        for (const Vec3f& q : p) {
            m.vertices.push_back(vertex(q * radius, q));
        }
        for (const auto& tri : t) {
            m.indices.insert(m.indices.end(), tri.begin(), tri.end());
        }
    }
    return m;
}

// A tube along Y with no caps, its ring closed (shared vertices): two open borders.
MeshData3D open_tube(int segments, int rings, float radius = 1.0f, float height = 4.0f) {
    MeshData3D m;
    for (int r = 0; r <= rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const float a = 2.0f * pi * static_cast<float>(s) / static_cast<float>(segments);
            const float wobble = 1.0f + 0.08f * std::sin(static_cast<float>(r) * 0.7f + 3.0f * a);
            m.vertices.push_back(
                vertex({radius * wobble * std::cos(a), height * static_cast<float>(r) / static_cast<float>(rings),
                        radius * wobble * std::sin(a)},
                       {std::cos(a), 0.0f, std::sin(a)}));
        }
    }
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const u32 a = static_cast<u32>(r * segments + s), b = static_cast<u32>(r * segments + (s + 1) % segments);
            const u32 c = a + static_cast<u32>(segments), d = b + static_cast<u32>(segments);
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    }
    return m;
}

// A bumpy square patch, open all round.
MeshData3D wavy_patch(int cells) {
    MeshData3D m;
    for (int z = 0; z <= cells; ++z) {
        for (int x = 0; x <= cells; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(cells),
                        fz = static_cast<float>(z) / static_cast<float>(cells);
            m.vertices.push_back(vertex({fx * 10.0f, 0.5f * std::sin(fx * 9.0f) * std::cos(fz * 7.0f), fz * 10.0f},
                                        {0.0f, 1.0f, 0.0f}, {fx, fz}));
        }
    }
    const int w = cells + 1;
    for (int z = 0; z < cells; ++z) {
        for (int x = 0; x < cells; ++x) {
            const u32 a = static_cast<u32>(z * w + x), b = a + 1, c = a + static_cast<u32>(w), d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    }
    return m;
}

// A patch flat on one side and rough on the other: groups at the seam between them mix inputs of very different errors,
// which is where a parent's error must be at least its largest child's, and not just what its own simplification added.
MeshData3D half_rough_patch(int cells) {
    MeshData3D m = wavy_patch(cells);
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (MeshVertex3D& v : m.vertices) {
        v.position.y = v.position.x < 5.0f ? 0.0f : (unit(rng) - 0.5f) * 0.6f;
    }
    return m;
}

// The same, as a soup of flat triangles with no normals of their own (as a model with none is read).
MeshData3D unindexed_flat(const MeshData3D& m) {
    MeshData3D out;
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const Vec3f a = m.vertices[m.indices[t]].position, b = m.vertices[m.indices[t + 1]].position,
                    c = m.vertices[m.indices[t + 2]].position;
        const Vec3f n = normalize(cross(b - a, c - a));
        out.vertices.push_back(vertex(a, n));
        out.vertices.push_back(vertex(b, n));
        out.vertices.push_back(vertex(c, n));
    }
    return out;
}

// --- measuring ---

using Edge = std::array<float, 6>;

Edge edge_of(Vec3f a, Vec3f b) {
    const std::array<float, 3> pa{a.x, a.y, a.z}, pb{b.x, b.y, b.z};
    return pa < pb ? Edge{a.x, a.y, a.z, b.x, b.y, b.z} : Edge{b.x, b.y, b.z, a.x, a.y, a.z};
}

struct EdgeCounts {
    std::map<Edge, int> uses;
    std::size_t open() const {
        std::size_t n = 0;
        for (const auto& [e, c] : uses) {
            n += c == 1 ? 1 : 0;
        }
        return n;
    }
    std::size_t overused() const { // more than two triangles on an edge: a surface covered twice
        std::size_t n = 0;
        for (const auto& [e, c] : uses) {
            n += c > 2 ? 1 : 0;
        }
        return n;
    }
    void add(Vec3f a, Vec3f b, Vec3f c) {
        ++uses[edge_of(a, b)];
        ++uses[edge_of(b, c)];
        ++uses[edge_of(c, a)];
    }
};

EdgeCounts edges_of(const MeshData3D& m) {
    EdgeCounts e;
    const std::size_t n = m.triangle_count();
    for (std::size_t t = 0; t < n; ++t) {
        const auto at = [&](std::size_t k) {
            return m.vertices[m.indices.empty() ? t * 3 + k : m.indices[t * 3 + k]].position;
        };
        e.add(at(0), at(1), at(2));
    }
    return e;
}

EdgeCounts edges_of(const MeshClusterBuild3D& mesh, const std::vector<SelectedCluster3D>& selection) {
    EdgeCounts e;
    std::vector<u32> indices;
    for (const SelectedCluster3D& s : selection) {
        indices.resize(static_cast<std::size_t>(mesh.clusters[s.cluster].triangle_count) * 3);
        mesh.append_indices(s.cluster, indices.data());
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            e.add(mesh.vertices[indices[t]].position, mesh.vertices[indices[t + 1]].position,
                  mesh.vertices[indices[t + 2]].position);
        }
    }
    return e;
}

using Tri = std::array<float, 9>;

Tri canonical(Vec3f a, Vec3f b, Vec3f c) {
    std::array<Tri, 3> rotations = {Tri{a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z},
                                    Tri{b.x, b.y, b.z, c.x, c.y, c.z, a.x, a.y, a.z},
                                    Tri{c.x, c.y, c.z, a.x, a.y, a.z, b.x, b.y, b.z}};
    return *std::min_element(rotations.begin(), rotations.end());
}

std::vector<Tri> triangles_of(const MeshData3D& m) {
    std::vector<Tri> out;
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        const auto at = [&](std::size_t k) {
            return m.vertices[m.indices.empty() ? t * 3 + k : m.indices[t * 3 + k]].position;
        };
        out.push_back(canonical(at(0), at(1), at(2)));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<SelectedCluster3D> all_leaves(const MeshClusters3D& mesh) {
    std::vector<SelectedCluster3D> out;
    for (u32 c = 0; c < mesh.leaf_count; ++c) {
        out.push_back({c, 1});
    }
    return out;
}

u64 triangles_in(const MeshClusters3D& mesh, const std::vector<SelectedCluster3D>& selection) {
    u64 n = 0;
    for (const SelectedCluster3D& s : selection) {
        n += mesh.clusters[s.cluster].triangle_count;
    }
    return n;
}

Vec3f center_of(const MeshData3D& m) {
    return m.bounds().center();
}

// A random copy and camera around a mesh: a scaled, moved, turned copy; an eye near it, far, or inside; a field of
// view and an allowed error.
ClusterCutView3D random_view(std::mt19937& rng, const MeshData3D& m) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float radius = m.bounds().radius();
    ClusterCutView3D v;
    const float scale = std::exp(std::log(0.4f) + unit(rng) * std::log(8.0f));
    v.transform = Mat4f::translation({unit(rng) * 6.0f - 3.0f, unit(rng) * 4.0f - 2.0f, unit(rng) * 6.0f - 3.0f}) *
                  Mat4f::rotation_y(unit(rng) * 360.0f) * Mat4f::scaling({scale, scale, scale});
    const Vec3f target = v.transform.apply(center_of(m));
    const float reach = radius * scale * std::exp(std::log(0.02f) + unit(rng) * std::log(300.0f));
    const Vec3f dir =
        normalize(Vec3f{unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f} + Vec3f{0.0f, 0.001f, 0.0f});
    v.eye = target + dir * reach;
    // Mostly looking at it, sometimes anywhere.
    const Vec3f aim =
        unit(rng) < 0.7f
            ? normalize(target - v.eye)
            : normalize(Vec3f{unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f} + Vec3f{0.0f, 0.001f, 0.0f});
    v.forward = aim;
    v.orthographic = unit(rng) < 0.1f;
    v.per_unit = v.orthographic ? 10.0f + unit(rng) * 400.0f : 100.0f + unit(rng) * 1500.0f;
    v.pixels = 0.25f + unit(rng) * 6.0f;
    v.nearest = 0.05f + unit(rng) * 0.2f;
    return v;
}

// --- tests ---

void check_structure(const MeshData3D& data, const MeshClusterBuild3D& mesh, bool expect_flat) {
    assert(mesh.flat == expect_flat);
    assert(mesh.leaf_count > 0 && mesh.leaf_count <= mesh.clusters.size());
    // Leaves: each triangle of the (welded) mesh once, and nothing else.
    std::vector<Tri> leaves;
    u32 leaf_triangles = 0;
    for (u32 c = 0; c < mesh.leaf_count; ++c) {
        const MeshCluster3D& k = mesh.clusters[c];
        assert(k.depth == 0 && k.group == no_cluster && k.error == 0.0f);
        std::vector<u32> idx(k.triangle_count * 3u);
        mesh.append_indices(c, idx.data());
        for (std::size_t t = 0; t < idx.size(); t += 3) {
            leaves.push_back(canonical(mesh.vertices[idx[t]].position, mesh.vertices[idx[t + 1]].position,
                                       mesh.vertices[idx[t + 2]].position));
        }
        leaf_triangles += k.triangle_count;
        // The cull sphere holds the cluster's vertices.
        for (u32 i = 0; i < k.vertex_count; ++i) {
            const Vec3f p = mesh.vertices[mesh.cluster_vertices[k.vertex_offset + i]].position;
            assert(length(p - k.cull_center) <= k.cull_radius * 1.001f + 1e-4f);
        }
    }
    std::sort(leaves.begin(), leaves.end());
    const std::vector<Tri> original = triangles_of(data);
    assert(leaves == original); // each triangle exactly once (the multiset is equal)
    assert(mesh.leaf_triangles == leaf_triangles && leaf_triangles == original.size());

    // Every cluster: small, indices in range; each in at most one group as an input.
    std::vector<int> as_input(mesh.clusters.size(), 0), as_output(mesh.clusters.size(), 0);
    for (const MeshCluster3D& k : mesh.clusters) {
        assert(k.triangle_count >= 1 && k.triangle_count <= max_cluster_triangles);
        assert(k.vertex_count >= 3 && k.vertex_count <= 255);
        for (u32 i = 0; i < k.triangle_count * 3u; ++i) {
            assert(mesh.triangles[k.triangle_offset + i] < k.vertex_count);
        }
        for (u32 i = 0; i < k.vertex_count; ++i) {
            assert(mesh.cluster_vertices[k.vertex_offset + i] < mesh.vertices.size());
        }
    }
    for (u32 g = 0; g < mesh.groups.size(); ++g) {
        const MeshClusterGroup3D& group = mesh.groups[g];
        assert(group.input_count >= 1 && group.output_count >= 1);
        f32 inputs_error = 0.0f;
        for (u32 i = 0; i < group.input_count; ++i) {
            const u32 id = mesh.group_inputs[group.input_first + i];
            const MeshCluster3D& c = mesh.clusters[id];
            ++as_input[id];
            assert(c.parent_group == g);
            // Monotonic up the hierarchy: the parent's error is no smaller, its sphere holds the child's.
            assert(c.parent_error == group.error && c.parent_error >= c.error);
            assert(length(c.parent_center - c.center) + c.radius <= c.parent_radius * (1.0f + 1e-4f) + 1e-5f);
            assert(group.error > c.error || group.error == c.error);
            inputs_error = std::max(inputs_error, c.error);
        }
        assert(group.error >= inputs_error);
        for (u32 i = 0; i < group.output_count; ++i) {
            const MeshCluster3D& c = mesh.clusters[group.output_first + i];
            ++as_output[group.output_first + i];
            assert(c.group == g && c.error == group.error && c.center.x == group.center.x && c.radius == group.radius);
            assert(c.depth > 0);
        }
    }
    for (u32 c = 0; c < mesh.clusters.size(); ++c) {
        assert(as_input[c] <= 1 && as_output[c] <= 1);
        assert((c < mesh.leaf_count) == (as_output[c] == 0));
        const MeshCluster3D& k = mesh.clusters[c];
        assert((k.parent_group == no_cluster) == (as_input[c] == 0));
        if (k.parent_group == no_cluster) {
            assert(!std::isfinite(k.parent_error));
            assert(std::find(mesh.roots.begin(), mesh.roots.end(), c) != mesh.roots.end());
        } else {
            assert(k.parent_depth > k.depth);
        }
    }
    assert(mesh.roots.size() >= 1);
}

// The cut as a set, from the definition (every cluster tested), against the traversal.
std::set<u32> brute_cut(const MeshClusters3D& mesh, const ClusterCutView3D& view) {
    const ClusterCutTest3D test{view};
    std::set<u32> out;
    for (u32 c = 0; c < mesh.clusters.size(); ++c) {
        const MeshCluster3D& k = mesh.clusters[c];
        const bool forced = view.force_depth >= 0;
        const u32 depth = static_cast<u32>(std::max(view.force_depth, 0));
        const bool selected =
            forced ? k.depth <= depth && !(k.parent_depth != no_cluster && k.parent_depth <= depth) : test.selected(k);
        if (selected) {
            out.insert(c);
        }
    }
    return out;
}

// Along every chain of clusters from a leaf up to a root (through any output of each parent group) exactly one is
// selected: the surface is covered once.
void check_covers_once(const MeshClusters3D& mesh, const std::vector<SelectedCluster3D>& selection) {
    std::vector<u8> chosen(mesh.clusters.size(), 0);
    for (const SelectedCluster3D& s : selection) {
        assert(!chosen[s.cluster]);
        chosen[s.cluster] = 1;
    }
    std::vector<int> lo(mesh.clusters.size()), hi(mesh.clusters.size());
    for (u32 c = static_cast<u32>(mesh.clusters.size()); c-- > 0;) {
        const MeshCluster3D& k = mesh.clusters[c];
        int a = 0, b = 0;
        if (k.parent_group != no_cluster) {
            const MeshClusterGroup3D& g = mesh.groups[k.parent_group];
            a = 1 << 20;
            b = -1;
            for (u32 i = 0; i < g.output_count; ++i) {
                a = std::min(a, lo[g.output_first + i]);
                b = std::max(b, hi[g.output_first + i]);
            }
        }
        lo[c] = a + chosen[c];
        hi[c] = b + chosen[c];
    }
    for (u32 c = 0; c < mesh.leaf_count; ++c) {
        assert(lo[c] == 1 && hi[c] == 1);
    }
}

void check_cut(const MeshData3D& data, const MeshClusterBuild3D& mesh, const ClusterCutView3D& view,
               const EdgeCounts& original, const char* what) {
    const std::vector<SelectedCluster3D> cut = select_clusters(mesh, view, NoClusterCull3D{});
    const std::set<u32> brute = brute_cut(mesh, view);
    std::set<u32> traversed;
    for (const SelectedCluster3D& s : cut) {
        assert(s.views == 1);
        assert(traversed.insert(s.cluster).second); // none twice
    }
    if (traversed != brute) {
        std::cerr << what << ": traversal found " << traversed.size() << " clusters, the definition " << brute.size()
                  << "\n";
    }
    assert(traversed == brute);
    check_covers_once(mesh, cut);
    const EdgeCounts edges = edges_of(mesh, cut);
    if (edges.open() != original.open()) {
        std::cerr << what << ": open edges " << edges.open() << " (the mesh's own " << original.open() << "), overused "
                  << edges.overused() << "\n";
    }
    assert(edges.open() == original.open());
    // The simplifier now and then leaves a sliver on three vertices of a locked border, from each side of it (two
    // triangles of no area on one edge, a fold with nothing showing): a handful. A surface covered twice would put
    // nearly every edge here.
    assert(edges.overused() <= original.overused() + edges.uses.size() / 200 + 2);
    static_cast<void>(data);
}

struct Mesh1 {
    const char* name;
    MeshData3D data;
    bool flat;
    u32 min_clusters;
};

void test_meshes() {
    MeshOptions3D options;
    options.clusters = true;
    std::vector<Mesh1> meshes;
    meshes.push_back({"icosphere", icosphere(5, false), false, 100});
    meshes.push_back({"icosphere soup", icosphere(5, true), true, 100});
    meshes.push_back({"torus", meshes::torus(1.0f, 0.35f, 96, 48), false, 50});
    meshes.push_back({"open tube", open_tube(64, 80), false, 50});
    meshes.push_back({"wavy patch", wavy_patch(64), false, 50});
    meshes.push_back({"wavy patch soup", unindexed_flat(wavy_patch(48)), true, 30});
    meshes.push_back({"plane", meshes::plane({10.0f, 10.0f}, 60), true, 40});
    meshes.push_back({"half rough patch", half_rough_patch(64), false, 50});
    for (const Mesh1& m : meshes) {
        std::mt19937 rng(12345);
        const MeshClusterBuild3D mesh = build_mesh_clusters(m.data, options);
        std::cout << "  " << m.name << ": " << m.data.triangle_count() << " triangles, " << mesh.clusters.size()
                  << " clusters, " << mesh.levels << " levels, " << mesh.groups.size() << " groups, "
                  << mesh.roots.size() << " roots, " << mesh.stuck_groups << " stuck, " << mesh.total_triangles
                  << " triangles in all, " << mesh.build_ms << " ms\n";
        assert(mesh.clusters.size() >= m.min_clusters);
        check_structure(m.data, mesh, m.flat);
        const EdgeCounts original = edges_of(m.data);
        // Every leaf: the mesh itself.
        assert(edges_of(mesh, all_leaves(mesh)).uses == original.uses);
        // The cut by depth: 0 is the leaves, each is a crack-free cover, the last is the roots.
        for (i32 depth = 0; depth <= static_cast<i32>(mesh.levels) + 1; ++depth) {
            ClusterCutView3D view;
            view.force_depth = depth;
            check_cut(m.data, mesh, view, original, "forced depth");
            const std::vector<SelectedCluster3D> cut = select_clusters(mesh, view, NoClusterCull3D{});
            if (depth == 0) {
                assert(cut.size() == mesh.leaf_count && triangles_in(mesh, cut) == mesh.leaf_triangles);
            }
            if (depth >= static_cast<i32>(mesh.levels)) {
                assert(cut.size() == mesh.roots.size());
            }
        }
        // Many random cameras and errors.
        std::size_t smallest = ~std::size_t{0}, largest = 0;
        for (int i = 0; i < 150; ++i) {
            const ClusterCutView3D view = random_view(rng, m.data);
            check_cut(m.data, mesh, view, original, m.name);
            const std::size_t n = select_clusters(mesh, view, NoClusterCull3D{}).size();
            smallest = std::min(smallest, n);
            largest = std::max(largest, n);
        }
        // The cuts really did differ in size (not all the leaves, not all the root).
        assert(largest > smallest);
    }
}

void test_tiny_meshes() {
    MeshOptions3D options;
    options.clusters = true;
    for (const MeshData3D& m : {meshes::box({1.0f, 1.0f, 1.0f}), meshes::octahedron(1.0f),
                                meshes::star_prism(5, 1.0f, 0.5f, 0.3f), meshes::wedge({1.0f, 1.0f, 1.0f})}) {
        const MeshClusterBuild3D mesh = build_mesh_clusters(m, options);
        assert(mesh.clusters.size() == 1 && mesh.groups.empty() && mesh.roots.size() == 1 && mesh.levels == 1);
        assert(mesh.leaf_count == 1 && mesh.clusters[0].triangle_count == m.triangle_count() - mesh.degenerate_removed);
        const std::vector<SelectedCluster3D> cut = select_clusters(mesh, ClusterCutView3D{}, NoClusterCull3D{});
        assert(cut.size() == 1 && cut[0].cluster == 0);
        // A box is flat-shaded, and welded by position, UV and colour.
        check_structure(m, mesh, mesh.flat);
    }
    MeshData3D one;
    one.vertices = {vertex({0, 0, 0}), vertex({1, 0, 0}), vertex({0, 1, 0})};
    const MeshClusterBuild3D mesh = build_mesh_clusters(one, options);
    assert(mesh.clusters.size() == 1 && mesh.clusters[0].triangle_count == 1 && mesh.roots.size() == 1);
    // Degenerate triangles are dropped, not kept as clusters of nothing.
    MeshData3D degenerate = one;
    degenerate.vertices.push_back(vertex({0, 0, 0}));
    degenerate.vertices.push_back(vertex({0, 0, 0}));
    degenerate.vertices.push_back(vertex({1, 0, 0}));
    const MeshClusterBuild3D dropped = build_mesh_clusters(degenerate, options);
    assert(dropped.degenerate_removed == 1 && dropped.leaf_triangles == 1);
    // A smaller cluster size is honoured.
    options.cluster_triangles = 32;
    const MeshClusterBuild3D small = build_mesh_clusters(icosphere(3, false), options);
    for (const MeshCluster3D& c : small.clusters) {
        assert(c.triangle_count <= 32);
    }
}

// The cut against the formula, written out here: a cluster is chosen when its own error spans no more pixels than
// allowed at the nearest point of its sphere (in front of the camera) and its parent's does span more; with the
// copy's scale.
void test_cut_formula() {
    MeshOptions3D options;
    options.clusters = true;
    const MeshData3D data = icosphere(5, false);
    const MeshClusterBuild3D mesh = build_mesh_clusters(data, options);
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::size_t scales_differ = 0;
    for (int i = 0; i < 60; ++i) {
        ClusterCutView3D view;
        const float scale = 0.3f + unit(rng) * 2.2f;
        view.transform = Mat4f::translation({1.0f, 2.0f, -3.0f}) * Mat4f::scaling({scale, scale, scale});
        view.eye = {0.0f, 1.0f, 20.0f * unit(rng) + 8.0f};
        view.forward = {0.0f, 0.0f, -1.0f}; // looking toward the copy
        view.per_unit = 400.0f + unit(rng) * 800.0f;
        view.pixels = 0.5f + unit(rng) * 3.0f;
        view.nearest = 0.1f;
        const std::vector<SelectedCluster3D> cut = select_clusters(mesh, view, NoClusterCull3D{});
        // The formula, for a sphere wholly in front of the camera.
        const auto projected = [&](f32 error, Vec3f c, f32 r) {
            if (!std::isfinite(error)) {
                return 1e30f;
            }
            const Vec3f world = view.transform.apply(c);
            const float depth = dot(world - view.eye, view.forward) - r * scale;
            assert(depth > 0.0f); // the test keeps the camera outside every sphere
            return error * scale * view.per_unit / depth;
        };
        std::set<u32> expected;
        for (u32 c = 0; c < mesh.clusters.size(); ++c) {
            const MeshCluster3D& k = mesh.clusters[c];
            if (projected(k.error, k.center, k.radius) <= view.pixels &&
                projected(k.parent_error, k.parent_center, k.parent_radius) > view.pixels) {
                expected.insert(c);
            }
        }
        std::set<u32> got;
        for (const SelectedCluster3D& s : cut) {
            got.insert(s.cluster);
        }
        assert(got == expected);
        // A copy twice as large with the same camera is judged finer.
        ClusterCutView3D bigger = view;
        bigger.transform =
            Mat4f::translation({1.0f, 2.0f, -3.0f}) * Mat4f::scaling({scale * 2.0f, scale * 2.0f, scale * 2.0f});
        if (triangles_in(mesh, select_clusters(mesh, bigger, NoClusterCull3D{})) != triangles_in(mesh, cut)) {
            ++scales_differ;
        }
    }
    assert(scales_differ > 0);
}

// With a frustum-like cull: the cut is the uncut one without the clusters that no view reaches, in the same order,
// and a cluster is marked with the views it is in.
struct HalfSpaceCull {
    Vec3f normal;
    f32 offset;
    // Two views: in front of the plane, and in front of the plane turned over and moved.
    u32 views(Vec3f c, f32 r) const {
        u32 v = 0;
        v |= dot(c, normal) + offset >= -r ? 1u : 0u;
        v |= dot(c, normal) - offset * 0.5f - 0.7f <= r ? 2u : 0u;
        return v;
    }
    bool reaches(Vec3f c, f32 r) const { return views(c, r) != 0; }
};

void test_culled_cut() {
    MeshOptions3D options;
    options.clusters = true;
    const MeshData3D data = icosphere(5, false);
    const MeshClusterBuild3D mesh = build_mesh_clusters(data, options);
    std::mt19937 rng(5);
    for (int i = 0; i < 80; ++i) {
        const ClusterCutView3D view = random_view(rng, data);
        HalfSpaceCull cull{
            normalize(Vec3f{std::sin(static_cast<float>(i)), std::cos(static_cast<float>(i) * 1.7f), 0.6f}),
            0.3f * static_cast<float>(i % 5) - 0.5f};
        const std::vector<SelectedCluster3D> all = select_clusters(mesh, view, NoClusterCull3D{});
        const std::vector<SelectedCluster3D> some = select_clusters(mesh, view, cull);
        std::vector<SelectedCluster3D> expected;
        const f32 scale = cluster_scale_of(view.transform);
        for (const SelectedCluster3D& s : all) {
            const MeshCluster3D& c = mesh.clusters[s.cluster];
            const u32 v = cull.views(view.transform.apply(c.cull_center), c.cull_radius * scale);
            if (v != 0) {
                expected.push_back({s.cluster, v});
            }
        }
        assert(some.size() == expected.size());
        for (std::size_t k = 0; k < some.size(); ++k) {
            assert(some[k].cluster == expected[k].cluster && some[k].views == expected[k].views);
        }
        // Deterministic.
        const std::vector<SelectedCluster3D> again = select_clusters(mesh, view, cull);
        assert(again.size() == some.size());
        for (std::size_t k = 0; k < some.size(); ++k) {
            assert(again[k].cluster == some[k].cluster && again[k].views == some[k].views);
        }
    }
}

// The builder with threads gives the hierarchy the serial one does.
void test_parallel_build() {
    MeshOptions3D options;
    options.clusters = true;
    const MeshData3D data = icosphere(5, false);
    const MeshClusterBuild3D serial = build_mesh_clusters(data, options);
    const ClusterParallelFor3D reversed = [](u32 count, const std::function<void(u32)>& body) {
        for (u32 i = count; i-- > 0;) {
            body(i); // any order: the results are placed by group
        }
    };
    const MeshClusterBuild3D other = build_mesh_clusters(data, options, reversed);
    assert(serial.clusters.size() == other.clusters.size() && serial.groups.size() == other.groups.size());
    assert(serial.cluster_vertices == other.cluster_vertices && serial.triangles == other.triangles &&
           serial.roots == other.roots);
    for (std::size_t c = 0; c < serial.clusters.size(); ++c) {
        assert(serial.clusters[c].error == other.clusters[c].error &&
               serial.clusters[c].triangle_count == other.clusters[c].triangle_count);
    }
}

// Flat meshes: the welded vertices carry no normal (the shader makes it), and weld by position, UV and colour.
void test_flat_weld() {
    MeshOptions3D options;
    options.clusters = true;
    const MeshData3D soup = icosphere(4, true);
    const MeshClusterBuild3D mesh = build_mesh_clusters(soup, options);
    const MeshData3D shared = icosphere(4, false);
    assert(mesh.flat && mesh.vertices.size() == shared.vertices.size());
    for (const MeshVertex3D& v : mesh.vertices) {
        assert(v.normal.x == 0.0f && v.normal.y == 0.0f && v.normal.z == 0.0f);
    }
    // A smooth mesh keeps its normals.
    const MeshClusterBuild3D smooth = build_mesh_clusters(shared, options);
    assert(!smooth.flat && smooth.vertices.size() == shared.vertices.size());
    assert(std::abs(length(smooth.vertices[7].normal) - 1.0f) < 1e-5f);
    // A box has a normal a face yet its corners are shared by three faces: still flat.
    assert(build_mesh_clusters(meshes::box({1.0f, 2.0f, 3.0f}), options).flat);
}

// What a mesh keeps (MeshClusters3D) holds no vertex: the welded ones are only in the build, which the renderer uploads
// and lets go; the hierarchy moved out of a build is whole, and cuts as the build does.
template <typename T>
concept holds_vertices = requires(const T& t) { t.vertices; };
static_assert(holds_vertices<MeshClusterBuild3D> && !holds_vertices<MeshClusters3D>,
              "a kept hierarchy must not hold the welded vertices");

void test_hierarchy_alone() {
    MeshOptions3D options;
    options.clusters = true;
    MeshClusterBuild3D built = build_mesh_clusters(icosphere(5, true), options);
    const MeshClusterBuild3D copy = built;
    const std::vector<u32> leaves = built.leaf_indices();
    const MeshClusters3D kept = std::move(static_cast<MeshClusters3D&>(built));
    assert(kept.levels > 1 && kept.clusters.size() == copy.clusters.size() && kept.roots == copy.roots &&
           kept.flat == copy.flat);
    assert(kept.leaf_indices() == leaves && kept.info().total_triangles == copy.info().total_triangles);
    ClusterCutView3D view;
    view.eye = {0.0f, 0.0f, 6.0f};
    view.per_unit = 500.0f;
    view.pixels = 2.0f;
    const std::vector<SelectedCluster3D> a = select_clusters(kept, view, NoClusterCull3D{}),
                                         b = select_clusters(copy, view, NoClusterCull3D{});
    assert(!a.empty() && a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        assert(a[i].cluster == b[i].cluster);
    }
}

// --- bakes (kin/assets/bake_cache.hpp) ---

// Byte for byte, the vectors' elements included (MeshCluster3D, MeshClusterGroup3D and MeshVertex3D have no padding:
// mesh_clusters.cpp and mesh_bake.hpp assert their sizes).
template <typename V> bool same_bytes(const V& a, const V& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
}

bool same_build(const MeshClusterBuild3D& a, const MeshClusterBuild3D& b) {
    return a.flat == b.flat && same_bytes(a.clusters, b.clusters) && same_bytes(a.groups, b.groups) &&
           same_bytes(a.group_inputs, b.group_inputs) && same_bytes(a.cluster_vertices, b.cluster_vertices) &&
           same_bytes(a.triangles, b.triangles) && same_bytes(a.roots, b.roots) && a.leaf_count == b.leaf_count &&
           a.levels == b.levels && a.leaf_triangles == b.leaf_triangles && a.total_triangles == b.total_triangles &&
           a.degenerate_removed == b.degenerate_removed && std::memcmp(&a.build_ms, &b.build_ms, sizeof(f64)) == 0 &&
           a.stuck_groups == b.stuck_groups && same_bytes(a.vertices, b.vertices);
}

bool same_chain(const MeshLodChain3D& a, const MeshLodChain3D& b) {
    if (a.levels.size() != b.levels.size() || !same_bytes(a.extra_vertices, b.extra_vertices)) {
        return false;
    }
    for (std::size_t l = 0; l < a.levels.size(); ++l) {
        if (a.levels[l].triangles != b.levels[l].triangles ||
            std::memcmp(&a.levels[l].error, &b.levels[l].error, sizeof(f32)) != 0 ||
            a.levels[l].indices != b.levels[l].indices) {
            return false;
        }
    }
    return true;
}

// A build saved and restored is the build, every field to the bit, and the same as a fresh build of the same mesh (but
// for the time it took); bytes that are not a whole build are refused, never read out of range.
void test_cluster_bake() {
    MeshOptions3D options;
    options.clusters = true;
    for (const MeshData3D& data : {icosphere(5, false), icosphere(5, true), open_tube(96, 48), half_rough_patch(60)}) {
        const MeshClusterBuild3D built = build_mesh_clusters(data, options);
        assert(built.levels > 1 && !built.vertices.empty());
        const std::vector<u8> bytes = save_mesh_clusters(built);
        MeshClusterBuild3D restored;
        assert(restore_mesh_clusters(bytes, restored));
        assert(same_build(built, restored) && restored.leaf_indices() == built.leaf_indices());
        MeshClusterBuild3D again = build_mesh_clusters(data, options);
        again.build_ms = built.build_ms;
        assert(same_build(again, restored));
        assert(save_mesh_clusters(restored) == bytes);

        // Short, long, or out of range: refused, and `out` left empty.
        for (const std::size_t size :
             {std::size_t{0}, std::size_t{1}, std::size_t{36}, std::size_t{45}, bytes.size() / 2, bytes.size() - 1}) {
            MeshClusterBuild3D out = built;
            assert(!restore_mesh_clusters(std::span<const u8>{bytes.data(), size}, out) && out.clusters.empty() &&
                   out.vertices.empty());
        }
        std::vector<u8> longer = bytes;
        longer.push_back(0);
        assert(!restore_mesh_clusters(longer, restored) && restored.clusters.empty());
        // The layout: flat (1), leaf_count, levels, leaf_triangles (4 each), total_triangles (8), degenerate_removed
        // (4), build_ms (8), stuck_groups (4), the cluster count (8), then the first cluster's vertex_offset.
        std::vector<u8> bad = bytes;
        bad[45] = bad[46] = bad[47] = bad[48] = 0xff;
        assert(!restore_mesh_clusters(bad, restored));
        bad = bytes;
        ++bad[9]; // leaf_triangles: no longer the leaves' own (leaf_indices() would write past its end)
        assert(!restore_mesh_clusters(bad, restored));
        bad = bytes;
        bad[bytes.size() - built.vertices.size() * 36 - 8] ^=
            0x40; // the vertex count's low byte: more or fewer than there are
        assert(!restore_mesh_clusters(bad, restored));
    }
}

void test_levels_bake() {
    MeshOptions3D options;
    options.levels = 5;
    const MeshData3D smooth = icosphere(5, false), soup = unindexed_flat(wavy_patch(60));
    for (const MeshData3D* data : {&smooth, &soup}) {
        const MeshLodChain3D chain = build_mesh_levels(*data, options);
        assert(chain.count() > 2);
        const std::vector<u8> bytes = save_mesh_levels(chain);
        MeshLodChain3D restored;
        assert(restore_mesh_levels(bytes, *data, restored) && same_chain(chain, restored));
        assert(same_chain(build_mesh_levels(*data, options), restored));
        for (const std::size_t size : {std::size_t{0}, std::size_t{8}, bytes.size() / 2, bytes.size() - 1}) {
            assert(!restore_mesh_levels(std::span<const u8>{bytes.data(), size}, *data, restored) &&
                   restored.count() == 0);
        }
        // Another mesh's: its level 0 is not this mesh, or the indices pass its vertices.
        assert(!restore_mesh_levels(bytes, icosphere(3, false), restored));
    }
    assert(!build_mesh_levels(soup, options).extra_vertices.empty());
}

// A flat soup has two UV islands and colour/alpha discontinuities at their shared positions. A position-only remap
// used to replace one island's corners with the other one's. Keep level zero and preserve the seam at every level.
void test_flat_lod_seams() {
    MeshData3D soup;
    constexpr int side = 16;
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side * 2; ++x) {
            const bool right = x >= side;
            const auto vertex = [&](int dx, int dy) {
                const Vec3f p{f32(x + dx) / side - 1, f32(y + dy) / side, 0};
                return MeshVertex3D{p,
                                    {0, 0, 1},
                                    {p.x + (right ? 4 : 0), p.y},
                                    right ? Color::rgba(0, 255, 0, 80) : Color::rgb(255, 0, 0)};
            };
            for (const auto v : {vertex(0, 0), vertex(1, 0), vertex(0, 1), vertex(1, 0), vertex(1, 1), vertex(0, 1)})
                soup.vertices.push_back(v);
        }
    const auto chain = build_mesh_levels(soup, {.levels = 4});
    assert(chain.count() > 1 && chain.levels[0].indices.empty() && chain.levels[0].triangles == soup.triangle_count());
    const auto vertex = [&](u32 index) -> const MeshVertex3D& {
        return index < soup.vertices.size() ? soup.vertices[index] : chain.extra_vertices[index - soup.vertices.size()];
    };
    for (std::size_t level = 1; level < chain.count(); ++level)
        for (std::size_t i = 0; i < chain.levels[level].indices.size(); i += 3) {
            const auto& a = vertex(chain.levels[level].indices[i]);
            const auto& b = vertex(chain.levels[level].indices[i + 1]);
            const auto& c = vertex(chain.levels[level].indices[i + 2]);
            assert(a.color == b.color && a.color == c.color);
            const bool right = a.color.g != 0;
            for (const auto* v : {&a, &b, &c}) {
                assert(v->uv.x == v->position.x + (right ? 4 : 0));
                assert(right ? v->position.x >= 0 : v->position.x <= 0);
            }
        }
}

// A key changes with what the build reads, and only with that.
void test_bake_keys() {
    const MeshData3D mesh = icosphere(4, false);
    MeshOptions3D c;
    c.clusters = true;
    const u64 key = mesh_clusters_bake_key(mesh, c);
    assert(key == mesh_clusters_bake_key(mesh, c));
    MeshOptions3D o = c;
    o.levels = 5;
    o.target_ratio = 0.3f;
    o.lock_border = false;
    assert(mesh_clusters_bake_key(mesh, o) == key); // not read by build_mesh_clusters
    for (const auto change : std::initializer_list<void (*)(MeshOptions3D&)>{
             [](MeshOptions3D& m) { m.cluster_triangles = 64; }, [](MeshOptions3D& m) { m.normal_weight = 0.25f; },
             [](MeshOptions3D& m) { m.uv_weight = 0.0f; }, [](MeshOptions3D& m) { m.color_weight = 1.0f; }}) {
        o = c;
        change(o);
        assert(mesh_clusters_bake_key(mesh, o) != key);
    }
    MeshData3D other = mesh;
    other.vertices[3].uv.x += 0.5f;
    assert(mesh_clusters_bake_key(other, c) != key);
    other = mesh;
    other.indices.resize(other.indices.size() - 3);
    assert(mesh_clusters_bake_key(other, c) != key);

    MeshOptions3D l;
    l.levels = 4;
    const u64 levels = mesh_levels_bake_key(mesh, l);
    assert(levels != key && levels == mesh_levels_bake_key(mesh, l));
    o = l;
    o.cluster_triangles = 64;
    assert(mesh_levels_bake_key(mesh, o) == levels);
    for (const auto change : std::initializer_list<void (*)(MeshOptions3D&)>{
             [](MeshOptions3D& m) { m.levels = 5; }, [](MeshOptions3D& m) { m.target_ratio = 0.4f; },
             [](MeshOptions3D& m) { m.min_reduction = 0.2f; }, [](MeshOptions3D& m) { m.min_triangles = 32; },
             [](MeshOptions3D& m) { m.max_error = 0.05f; }, [](MeshOptions3D& m) { m.normal_weight = 0.0f; },
             [](MeshOptions3D& m) { m.uv_weight = 1.0f; }, [](MeshOptions3D& m) { m.color_weight = 0.0f; },
             [](MeshOptions3D& m) { m.lock_border = false; }}) {
        o = l;
        change(o);
        assert(mesh_levels_bake_key(mesh, o) != levels);
    }
}

// The folder: misses, a write then a hit of the same bytes, corrupt, truncated and misplaced files refused (and
// removed), the oldest-used entries evicted past the bound, nothing at all when off.
void test_bake_cache() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
                         ("kin-bake-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<u8> bytes(1000);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<u8>(i * 7 + 3);
    }
    {
        BakeCache off;
        assert(!off.enabled() && !off.read(1));
        off.write(1, bytes);
        assert(off.stats().misses == 0 && off.stats().writes == 0);
    }
    BakeCache cache{dir, 4200};
    assert(!cache.read(7) && cache.stats().misses == 1 && cache.stats().refused == 0); // no folder yet
    cache.write(7, bytes);
    assert(fs::is_regular_file(cache.path_of(7)) && cache.stats().writes == 1 && cache.stats().bytes_written == 1000);
    std::optional<std::vector<u8>> back = cache.read(7);
    assert(back && *back == bytes && cache.stats().hits == 1 && cache.stats().bytes_read == 1000);
    cache.write(8, {});
    back = cache.read(8);
    assert(back && back->empty());

    const auto refused = [&](u64 key) {
        const BakeCacheStats before = cache.stats();
        const bool missed = !cache.read(key);
        return missed && cache.stats().refused == before.refused + 1 && cache.stats().misses == before.misses + 1 &&
               !fs::exists(cache.path_of(key));
    };
    {
        cache.write(7, bytes);
        std::fstream file(cache.path_of(7), std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(32 + 500);
        file.put(static_cast<char>(bytes[500] ^ 0x10));
    }
    assert(refused(7)); // the checksum
    cache.write(7, bytes);
    fs::resize_file(cache.path_of(7), 20);
    assert(refused(7)); // no whole header
    cache.write(7, bytes);
    fs::resize_file(cache.path_of(7), 32 + 999);
    assert(refused(7)); // a payload cut short
    cache.write(7, bytes);
    fs::rename(cache.path_of(7), cache.path_of(9));
    assert(refused(9)); // another key's
    { std::ofstream(cache.path_of(9), std::ios::binary) << "not a bake at all, but long enough for a header"; }
    assert(refused(9));

    // Bounded: 1032 bytes an entry, four fit; the fifth evicts the one used longest ago. A read counts as a use.
    fs::remove(cache.path_of(8));
    const auto at = [&](u64 key, int minutes_ago) {
        fs::last_write_time(cache.path_of(key), fs::file_time_type::clock::now() - std::chrono::minutes{minutes_ago});
    };
    for (u64 key = 1; key <= 3; ++key) {
        cache.write(key, bytes);
        at(key, 40 - static_cast<int>(key)); // 1 the oldest
    }
    assert(cache.read(1)); // now the newest
    cache.write(4, bytes);
    assert(cache.stats().evictions == 0);
    cache.write(5, bytes);
    assert(cache.stats().evictions == 1 && !fs::exists(cache.path_of(2)));
    for (const u64 key : {1, 3, 4, 5}) {
        assert(fs::exists(cache.path_of(key)));
    }
    // Too big to keep at all: not written. A temporary file a crash left an hour ago goes at the next write.
    const BakeCacheStats before = cache.stats();
    cache.write(6, std::vector<u8>(4200));
    assert(cache.stats().write_failures == before.write_failures + 1 && !fs::exists(cache.path_of(6)));
    const fs::path stale = dir / "0000000000000001.0000000000000002.tmp";
    std::ofstream(stale) << "half a bake";
    fs::last_write_time(stale, fs::file_time_type::clock::now() - std::chrono::hours{2});
    cache.write(6, bytes);
    assert(!fs::exists(stale) && fs::exists(cache.path_of(6)));
    std::error_code error;
    fs::remove_all(dir, error);
}

// The hash tells apart a change of any byte, in any lane or the tail, of the length and of the seed; and it is fast
// enough for a key of the kiln's 540 MB of vertices.
void test_bake_hash() {
    std::vector<u8> bytes(100);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<u8>(i * 13 + 1);
    }
    const u64 h0 = bake_hash(bytes.data(), bytes.size());
    assert(h0 == bake_hash(bytes.data(), bytes.size()) && h0 != bake_hash(bytes.data(), bytes.size(), 1) &&
           h0 != bake_hash(bytes.data(), 99));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        std::vector<u8> changed = bytes;
        changed[i] ^= 1;
        assert(bake_hash(changed.data(), changed.size()) != h0);
    }
    // Swapping two lanes' bytes is a change too (the lanes are seeded apart).
    std::vector<u8> swapped = bytes;
    std::swap_ranges(swapped.begin(), swapped.begin() + 8, swapped.begin() + 8);
    std::vector<u8> zeros(64, 0);
    assert(bake_hash(swapped.data(), swapped.size()) != h0 &&
           bake_hash(zeros.data(), 32) != bake_hash(zeros.data(), 64));
    const BakeKey k1 = BakeKey{"kind", 1}.value(u32{5}), k2 = BakeKey{"kind", 2}.value(u32{5}),
                  k3 = BakeKey{"kinds", 1}.value(u32{5});
    const u64 again = BakeKey("kind", 1).value(u32{5}).hash();
    assert(k1.hash() != k2.hash() && k1.hash() != k3.hash() && k1.hash() == again);
    const u64 ab_c = BakeKey("k", 1).bytes("ab", 2).bytes("c", 1).hash(),
              a_bc = BakeKey("k", 1).bytes("a", 1).bytes("bc", 2).hash();
    assert(ab_c != a_bc);
    std::vector<u8> big(64u << 20, 1);
    const auto started = std::chrono::steady_clock::now();
    const u64 h = bake_hash(big.data(), big.size());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::cout << "  bake_hash: 64 MiB in " << ms << " ms (" << h % 10 << ")\n";
}

} // namespace

int main() {
    std::cout << "meshes\n";
    test_meshes();
    std::cout << "tiny\n";
    test_tiny_meshes();
    std::cout << "formula\n";
    test_cut_formula();
    std::cout << "culled\n";
    test_culled_cut();
    std::cout << "parallel build\n";
    test_parallel_build();
    std::cout << "flat\n";
    test_flat_weld();
    std::cout << "hierarchy alone\n";
    test_hierarchy_alone();
    std::cout << "cluster bake\n";
    test_cluster_bake();
    std::cout << "levels bake\n";
    test_levels_bake();
    test_flat_lod_seams();
    std::cout << "bake keys\n";
    test_bake_keys();
    std::cout << "bake cache\n";
    test_bake_cache();
    std::cout << "bake hash\n";
    test_bake_hash();
    std::cout << "mesh cluster tests passed\n";
    return 0;
}
