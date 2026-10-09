// Cluster hierarchies for meshes (kin/render3d/mesh_clusters.hpp), without a GPU: the leaves cover the mesh once,
// errors and spheres only grow up the hierarchy, and every cut through it covers the surface once and leaves no
// crack (the open edges of what is selected, welded by position, are the mesh's own).

#include <kin/render3d/mesh_clusters.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace {

using namespace kin;

constexpr float pi = 3.14159265358979f;

// --- meshes ---

MeshVertex vertex(Vec3f p, Vec3f n = {0.0f, 1.0f, 0.0f}, Vec2f uv = {}) {
    MeshVertex v;
    v.position = p;
    v.normal = n;
    v.uv = uv;
    return v;
}

// A sphere of triangles from an octahedron, split `subdivisions` times; shared vertices and smooth normals, or
// (flat) every triangle with its own and the face's normal.
MeshData icosphere(int subdivisions, bool flat, float radius = 1.0f) {
    std::vector<Vec3f> p = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<std::array<u32, 3>> t = {{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {4, 3, 0}, {1, 3, 4}, {5, 3, 1}, {0, 3, 5}};
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
    MeshData m;
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
MeshData open_tube(int segments, int rings, float radius = 1.0f, float height = 4.0f) {
    MeshData m;
    for (int r = 0; r <= rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const float a = 2.0f * pi * static_cast<float>(s) / static_cast<float>(segments);
            const float wobble = 1.0f + 0.08f * std::sin(static_cast<float>(r) * 0.7f + 3.0f * a);
            m.vertices.push_back(vertex({radius * wobble * std::cos(a), height * static_cast<float>(r) / static_cast<float>(rings), radius * wobble * std::sin(a)},
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
MeshData wavy_patch(int cells) {
    MeshData m;
    for (int z = 0; z <= cells; ++z) {
        for (int x = 0; x <= cells; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(cells), fz = static_cast<float>(z) / static_cast<float>(cells);
            m.vertices.push_back(vertex({fx * 10.0f, 0.5f * std::sin(fx * 9.0f) * std::cos(fz * 7.0f), fz * 10.0f}, {0.0f, 1.0f, 0.0f}, {fx, fz}));
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

// A patch flat on one side and rough on the other: groups at the seam between them mix inputs of very different errors, which is
// where a parent's error must be at least its largest child's, and not just what its own simplification added.
MeshData half_rough_patch(int cells) {
    MeshData m = wavy_patch(cells);
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (MeshVertex& v : m.vertices) {
        v.position.y = v.position.x < 5.0f ? 0.0f : (unit(rng) - 0.5f) * 0.6f;
    }
    return m;
}

// The same, as a soup of flat triangles with no normals of their own (as a model with none is read).
MeshData unindexed_flat(const MeshData& m) {
    MeshData out;
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const Vec3f a = m.vertices[m.indices[t]].position, b = m.vertices[m.indices[t + 1]].position, c = m.vertices[m.indices[t + 2]].position;
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

EdgeCounts edges_of(const MeshData& m) {
    EdgeCounts e;
    const std::size_t n = m.triangle_count();
    for (std::size_t t = 0; t < n; ++t) {
        const auto at = [&](std::size_t k) { return m.vertices[m.indices.empty() ? t * 3 + k : m.indices[t * 3 + k]].position; };
        e.add(at(0), at(1), at(2));
    }
    return e;
}

EdgeCounts edges_of(const MeshClusters& mesh, const std::vector<SelectedCluster>& selection) {
    EdgeCounts e;
    std::vector<u32> indices;
    for (const SelectedCluster& s : selection) {
        indices.resize(static_cast<std::size_t>(mesh.clusters[s.cluster].triangle_count) * 3);
        mesh.append_indices(s.cluster, indices.data());
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            e.add(mesh.vertices[indices[t]].position, mesh.vertices[indices[t + 1]].position, mesh.vertices[indices[t + 2]].position);
        }
    }
    return e;
}

using Tri = std::array<float, 9>;

Tri canonical(Vec3f a, Vec3f b, Vec3f c) {
    std::array<Tri, 3> rotations = {Tri{a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z}, Tri{b.x, b.y, b.z, c.x, c.y, c.z, a.x, a.y, a.z},
                                    Tri{c.x, c.y, c.z, a.x, a.y, a.z, b.x, b.y, b.z}};
    return *std::min_element(rotations.begin(), rotations.end());
}

std::vector<Tri> triangles_of(const MeshData& m) {
    std::vector<Tri> out;
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        const auto at = [&](std::size_t k) { return m.vertices[m.indices.empty() ? t * 3 + k : m.indices[t * 3 + k]].position; };
        out.push_back(canonical(at(0), at(1), at(2)));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<SelectedCluster> all_leaves(const MeshClusters& mesh) {
    std::vector<SelectedCluster> out;
    for (u32 c = 0; c < mesh.leaf_count; ++c) {
        out.push_back({c, 1});
    }
    return out;
}

u64 triangles_in(const MeshClusters& mesh, const std::vector<SelectedCluster>& selection) {
    u64 n = 0;
    for (const SelectedCluster& s : selection) {
        n += mesh.clusters[s.cluster].triangle_count;
    }
    return n;
}

Vec3f center_of(const MeshData& m) { return m.bounds().center(); }

// A random copy and camera around a mesh: a scaled, moved, turned copy; an eye near it, far, or inside; a field of
// view and an allowed error.
ClusterCutView random_view(std::mt19937& rng, const MeshData& m) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float radius = m.bounds().radius();
    ClusterCutView v;
    const float scale = std::exp(std::log(0.4f) + unit(rng) * std::log(8.0f));
    v.transform = Mat4f::translation({unit(rng) * 6.0f - 3.0f, unit(rng) * 4.0f - 2.0f, unit(rng) * 6.0f - 3.0f}) * Mat4f::rotation_y(unit(rng) * 360.0f) *
                  Mat4f::scaling({scale, scale, scale});
    const Vec3f target = v.transform.apply(center_of(m));
    const float reach = radius * scale * std::exp(std::log(0.02f) + unit(rng) * std::log(300.0f));
    const Vec3f dir = normalize(Vec3f{unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f} + Vec3f{0.0f, 0.001f, 0.0f});
    v.eye = target + dir * reach;
    // Mostly looking at it, sometimes anywhere.
    const Vec3f aim = unit(rng) < 0.7f ? normalize(target - v.eye) : normalize(Vec3f{unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f} + Vec3f{0.0f, 0.001f, 0.0f});
    v.forward = aim;
    v.orthographic = unit(rng) < 0.1f;
    v.per_unit = v.orthographic ? 10.0f + unit(rng) * 400.0f : 100.0f + unit(rng) * 1500.0f;
    v.pixels = 0.25f + unit(rng) * 6.0f;
    v.nearest = 0.05f + unit(rng) * 0.2f;
    return v;
}

// --- tests ---

void check_structure(const MeshData& data, const MeshClusters& mesh, bool expect_flat) {
    assert(mesh.flat == expect_flat);
    assert(mesh.leaf_count > 0 && mesh.leaf_count <= mesh.clusters.size());
    // Leaves: each triangle of the (welded) mesh once, and nothing else.
    std::vector<Tri> leaves;
    u32 leaf_triangles = 0;
    for (u32 c = 0; c < mesh.leaf_count; ++c) {
        const MeshCluster& k = mesh.clusters[c];
        assert(k.depth == 0 && k.group == no_cluster && k.error == 0.0f);
        std::vector<u32> idx(k.triangle_count * 3u);
        mesh.append_indices(c, idx.data());
        for (std::size_t t = 0; t < idx.size(); t += 3) {
            leaves.push_back(canonical(mesh.vertices[idx[t]].position, mesh.vertices[idx[t + 1]].position, mesh.vertices[idx[t + 2]].position));
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
    for (const MeshCluster& k : mesh.clusters) {
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
        const MeshClusterGroup& group = mesh.groups[g];
        assert(group.input_count >= 1 && group.output_count >= 1);
        f32 inputs_error = 0.0f;
        for (u32 i = 0; i < group.input_count; ++i) {
            const u32 id = mesh.group_inputs[group.input_first + i];
            const MeshCluster& c = mesh.clusters[id];
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
            const MeshCluster& c = mesh.clusters[group.output_first + i];
            ++as_output[group.output_first + i];
            assert(c.group == g && c.error == group.error && c.center.x == group.center.x && c.radius == group.radius);
            assert(c.depth > 0);
        }
    }
    for (u32 c = 0; c < mesh.clusters.size(); ++c) {
        assert(as_input[c] <= 1 && as_output[c] <= 1);
        assert((c < mesh.leaf_count) == (as_output[c] == 0));
        const MeshCluster& k = mesh.clusters[c];
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
std::set<u32> brute_cut(const MeshClusters& mesh, const ClusterCutView& view) {
    const ClusterCutTest test{view};
    std::set<u32> out;
    for (u32 c = 0; c < mesh.clusters.size(); ++c) {
        const MeshCluster& k = mesh.clusters[c];
        const bool forced = view.force_depth >= 0;
        const u32 depth = static_cast<u32>(std::max(view.force_depth, 0));
        const bool selected = forced ? k.depth <= depth && !(k.parent_depth != no_cluster && k.parent_depth <= depth) : test.selected(k);
        if (selected) {
            out.insert(c);
        }
    }
    return out;
}

// Along every chain of clusters from a leaf up to a root (through any output of each parent group) exactly one is
// selected: the surface is covered once.
void check_covers_once(const MeshClusters& mesh, const std::vector<SelectedCluster>& selection) {
    std::vector<u8> chosen(mesh.clusters.size(), 0);
    for (const SelectedCluster& s : selection) {
        assert(!chosen[s.cluster]);
        chosen[s.cluster] = 1;
    }
    std::vector<int> lo(mesh.clusters.size()), hi(mesh.clusters.size());
    for (u32 c = static_cast<u32>(mesh.clusters.size()); c-- > 0;) {
        const MeshCluster& k = mesh.clusters[c];
        int a = 0, b = 0;
        if (k.parent_group != no_cluster) {
            const MeshClusterGroup& g = mesh.groups[k.parent_group];
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

void check_cut(const MeshData& data, const MeshClusters& mesh, const ClusterCutView& view, const EdgeCounts& original, const char* what) {
    const std::vector<SelectedCluster> cut = select_clusters(mesh, view, NoClusterCull{});
    const std::set<u32> brute = brute_cut(mesh, view);
    std::set<u32> traversed;
    for (const SelectedCluster& s : cut) {
        assert(s.views == 1);
        assert(traversed.insert(s.cluster).second); // none twice
    }
    if (traversed != brute) {
        std::cerr << what << ": traversal found " << traversed.size() << " clusters, the definition " << brute.size() << "\n";
    }
    assert(traversed == brute);
    check_covers_once(mesh, cut);
    const EdgeCounts edges = edges_of(mesh, cut);
    if (edges.open() != original.open()) {
        std::cerr << what << ": open edges " << edges.open() << " (the mesh's own " << original.open() << "), overused " << edges.overused() << "\n";
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
    MeshData data;
    bool flat;
    u32 min_clusters;
};

void test_meshes() {
    MeshOptions options;
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
        const MeshClusters mesh = build_mesh_clusters(m.data, options);
        std::cout << "  " << m.name << ": " << m.data.triangle_count() << " triangles, " << mesh.clusters.size() << " clusters, " << mesh.levels
                  << " levels, " << mesh.groups.size() << " groups, " << mesh.roots.size() << " roots, " << mesh.stuck_groups << " stuck, "
                  << mesh.total_triangles << " triangles in all, " << mesh.build_ms << " ms\n";
        assert(mesh.clusters.size() >= m.min_clusters);
        check_structure(m.data, mesh, m.flat);
        const EdgeCounts original = edges_of(m.data);
        // Every leaf: the mesh itself.
        assert(edges_of(mesh, all_leaves(mesh)).uses == original.uses);
        // The cut by depth: 0 is the leaves, each is a crack-free cover, the last is the roots.
        for (i32 depth = 0; depth <= static_cast<i32>(mesh.levels) + 1; ++depth) {
            ClusterCutView view;
            view.force_depth = depth;
            check_cut(m.data, mesh, view, original, "forced depth");
            const std::vector<SelectedCluster> cut = select_clusters(mesh, view, NoClusterCull{});
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
            const ClusterCutView view = random_view(rng, m.data);
            check_cut(m.data, mesh, view, original, m.name);
            const std::size_t n = select_clusters(mesh, view, NoClusterCull{}).size();
            smallest = std::min(smallest, n);
            largest = std::max(largest, n);
        }
        // The cuts really did differ in size (not all the leaves, not all the root).
        assert(largest > smallest);
    }
}

void test_tiny_meshes() {
    MeshOptions options;
    options.clusters = true;
    for (const MeshData& m : {meshes::box({1.0f, 1.0f, 1.0f}), meshes::octahedron(1.0f), meshes::star_prism(5, 1.0f, 0.5f, 0.3f), meshes::wedge({1.0f, 1.0f, 1.0f})}) {
        const MeshClusters mesh = build_mesh_clusters(m, options);
        assert(mesh.clusters.size() == 1 && mesh.groups.empty() && mesh.roots.size() == 1 && mesh.levels == 1);
        assert(mesh.leaf_count == 1 && mesh.clusters[0].triangle_count == m.triangle_count() - mesh.degenerate_removed);
        const std::vector<SelectedCluster> cut = select_clusters(mesh, ClusterCutView{}, NoClusterCull{});
        assert(cut.size() == 1 && cut[0].cluster == 0);
        // A box is flat-shaded, and welded by position, UV and colour.
        check_structure(m, mesh, mesh.flat);
    }
    MeshData one;
    one.vertices = {vertex({0, 0, 0}), vertex({1, 0, 0}), vertex({0, 1, 0})};
    const MeshClusters mesh = build_mesh_clusters(one, options);
    assert(mesh.clusters.size() == 1 && mesh.clusters[0].triangle_count == 1 && mesh.roots.size() == 1);
    // Degenerate triangles are dropped, not kept as clusters of nothing.
    MeshData degenerate = one;
    degenerate.vertices.push_back(vertex({0, 0, 0}));
    degenerate.vertices.push_back(vertex({0, 0, 0}));
    degenerate.vertices.push_back(vertex({1, 0, 0}));
    const MeshClusters dropped = build_mesh_clusters(degenerate, options);
    assert(dropped.degenerate_removed == 1 && dropped.leaf_triangles == 1);
    // A smaller cluster size is honoured.
    options.cluster_triangles = 32;
    const MeshClusters small = build_mesh_clusters(icosphere(3, false), options);
    for (const MeshCluster& c : small.clusters) {
        assert(c.triangle_count <= 32);
    }
}

// The cut against the formula, written out here: a cluster is chosen when its own error spans no more pixels than
// allowed at the nearest point of its sphere (in front of the camera) and its parent's does span more; with the
// copy's scale.
void test_cut_formula() {
    MeshOptions options;
    options.clusters = true;
    const MeshData data = icosphere(5, false);
    const MeshClusters mesh = build_mesh_clusters(data, options);
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::size_t scales_differ = 0;
    for (int i = 0; i < 60; ++i) {
        ClusterCutView view;
        const float scale = 0.3f + unit(rng) * 2.2f;
        view.transform = Mat4f::translation({1.0f, 2.0f, -3.0f}) * Mat4f::scaling({scale, scale, scale});
        view.eye = {0.0f, 1.0f, 20.0f * unit(rng) + 8.0f};
        view.forward = {0.0f, 0.0f, -1.0f}; // looking toward the copy
        view.per_unit = 400.0f + unit(rng) * 800.0f;
        view.pixels = 0.5f + unit(rng) * 3.0f;
        view.nearest = 0.1f;
        const std::vector<SelectedCluster> cut = select_clusters(mesh, view, NoClusterCull{});
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
            const MeshCluster& k = mesh.clusters[c];
            if (projected(k.error, k.center, k.radius) <= view.pixels && projected(k.parent_error, k.parent_center, k.parent_radius) > view.pixels) {
                expected.insert(c);
            }
        }
        std::set<u32> got;
        for (const SelectedCluster& s : cut) {
            got.insert(s.cluster);
        }
        assert(got == expected);
        // A copy twice as large with the same camera is judged finer.
        ClusterCutView bigger = view;
        bigger.transform = Mat4f::translation({1.0f, 2.0f, -3.0f}) * Mat4f::scaling({scale * 2.0f, scale * 2.0f, scale * 2.0f});
        if (triangles_in(mesh, select_clusters(mesh, bigger, NoClusterCull{})) != triangles_in(mesh, cut)) {
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
    MeshOptions options;
    options.clusters = true;
    const MeshData data = icosphere(5, false);
    const MeshClusters mesh = build_mesh_clusters(data, options);
    std::mt19937 rng(5);
    for (int i = 0; i < 80; ++i) {
        const ClusterCutView view = random_view(rng, data);
        HalfSpaceCull cull{normalize(Vec3f{std::sin(static_cast<float>(i)), std::cos(static_cast<float>(i) * 1.7f), 0.6f}), 0.3f * static_cast<float>(i % 5) - 0.5f};
        const std::vector<SelectedCluster> all = select_clusters(mesh, view, NoClusterCull{});
        const std::vector<SelectedCluster> some = select_clusters(mesh, view, cull);
        std::vector<SelectedCluster> expected;
        const f32 scale = cluster_scale_of(view.transform);
        for (const SelectedCluster& s : all) {
            const MeshCluster& c = mesh.clusters[s.cluster];
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
        const std::vector<SelectedCluster> again = select_clusters(mesh, view, cull);
        assert(again.size() == some.size());
        for (std::size_t k = 0; k < some.size(); ++k) {
            assert(again[k].cluster == some[k].cluster && again[k].views == some[k].views);
        }
    }
}

// The builder with threads gives the hierarchy the serial one does.
void test_parallel_build() {
    MeshOptions options;
    options.clusters = true;
    const MeshData data = icosphere(5, false);
    const MeshClusters serial = build_mesh_clusters(data, options);
    const ClusterParallelFor reversed = [](u32 count, const std::function<void(u32)>& body) {
        for (u32 i = count; i-- > 0;) {
            body(i); // any order: the results are placed by group
        }
    };
    const MeshClusters other = build_mesh_clusters(data, options, reversed);
    assert(serial.clusters.size() == other.clusters.size() && serial.groups.size() == other.groups.size());
    assert(serial.cluster_vertices == other.cluster_vertices && serial.triangles == other.triangles && serial.roots == other.roots);
    for (std::size_t c = 0; c < serial.clusters.size(); ++c) {
        assert(serial.clusters[c].error == other.clusters[c].error && serial.clusters[c].triangle_count == other.clusters[c].triangle_count);
    }
}

// Flat meshes: the welded vertices carry no normal (the shader makes it), and weld by position, UV and colour.
void test_flat_weld() {
    MeshOptions options;
    options.clusters = true;
    const MeshData soup = icosphere(4, true);
    const MeshClusters mesh = build_mesh_clusters(soup, options);
    const MeshData shared = icosphere(4, false);
    assert(mesh.flat && mesh.vertices.size() == shared.vertices.size());
    for (const MeshVertex& v : mesh.vertices) {
        assert(v.normal.x == 0.0f && v.normal.y == 0.0f && v.normal.z == 0.0f);
    }
    // A smooth mesh keeps its normals.
    const MeshClusters smooth = build_mesh_clusters(shared, options);
    assert(!smooth.flat && smooth.vertices.size() == shared.vertices.size());
    assert(std::abs(length(smooth.vertices[7].normal) - 1.0f) < 1e-5f);
    // A box has a normal a face yet its corners are shared by three faces: still flat.
    assert(build_mesh_clusters(meshes::box({1.0f, 2.0f, 3.0f}), options).flat);
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
    std::cout << "mesh cluster tests passed\n";
    return 0;
}
