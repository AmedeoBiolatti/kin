#include <kin/render3d/mesh_clusters.hpp>

#include <meshoptimizer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace kin {

static_assert(offsetof(MeshVertex, position) == 0 && sizeof(Vec3f) == 3 * sizeof(f32), "meshoptimizer reads positions at the vertex's start");

namespace {

constexpr std::size_t max_cluster_vertices = 255; // a cluster's triangles index its vertices with a byte
constexpr u32 max_rounds = 64;
// A group must lose this share of its triangles to count as simplified; else it waits for another grouping.
constexpr f64 min_group_reduction = 0.15;

struct Sphere {
    Vec3f center{};
    f32 radius = 0.0f;
};

// A sphere that holds all of `spheres`: a good centre from meshoptimizer, the radius exactly large enough for the
// centre (and a hair more, so that float rounding cannot let a child poke out of its parent).
Sphere union_of(const std::vector<Sphere>& spheres) {
    if (spheres.size() == 1) {
        return spheres[0];
    }
    const meshopt_Bounds b = meshopt_computeSphereBounds(&spheres[0].center.x, spheres.size(), sizeof(Sphere), &spheres[0].radius, sizeof(Sphere));
    const Vec3f center{b.center[0], b.center[1], b.center[2]};
    f32 radius = 0.0f;
    for (const Sphere& s : spheres) {
        radius = std::max(radius, length(s.center - center) + s.radius);
    }
    return {center, radius * (1.0f + 1e-5f) + 1e-7f};
}

// Whether a mesh's every corner normal is its triangle's own (a model with no normals is read as that): mostly, so
// that a few degenerate or odd triangles do not decide it, and to within a quarter of a degree, so that a finely
// tessellated smooth surface (a sphere of 20,000 triangles has corners 1 degree off their face) is not taken for it.
bool is_flat(const MeshData& data) {
    const std::size_t count = data.triangle_count();
    std::size_t flat = 0, counted = 0;
    for (std::size_t t = 0; t < count; ++t) {
        const u32 i[3] = {data.indices.empty() ? static_cast<u32>(t * 3) : data.indices[t * 3],
                          data.indices.empty() ? static_cast<u32>(t * 3 + 1) : data.indices[t * 3 + 1],
                          data.indices.empty() ? static_cast<u32>(t * 3 + 2) : data.indices[t * 3 + 2]};
        const Vec3f a = data.vertices[i[0]].position, b = data.vertices[i[1]].position, c = data.vertices[i[2]].position;
        const Vec3f n = cross(b - a, c - a);
        const f32 n_length = length(n);
        if (n_length < 1e-20f) {
            continue;
        }
        ++counted;
        bool all = true;
        for (int k = 0; k < 3 && all; ++k) {
            all = dot(data.vertices[i[k]].normal, n) > 0.99999f * n_length;
        }
        flat += all ? 1 : 0;
    }
    return counted > 0 && flat * 100 >= counted * 99;
}

// Clusters built in a thread's own pools, appended to the mesh's in order.
struct BuiltCluster {
    u32 vertex_offset = 0, triangle_offset = 0;
    u16 vertex_count = 0, triangle_count = 0;
    Sphere cull{};
};
struct Batch {
    std::vector<u32> vertices;
    std::vector<u8> triangles;
    std::vector<BuiltCluster> clusters;
};

struct SplitSetup {
    std::size_t max_triangles = max_cluster_triangles;
    std::size_t min_triangles = 64;
};

// Splits `indices` (over `positions`, `vertex_count` of them) into clusters, spatially compact ones; `to_global`
// (when not null) turns the vertex numbers the indices use into the mesh's.
void split_into_clusters(const SplitSetup& setup, const u32* indices, std::size_t index_count, const f32* positions, std::size_t stride_bytes,
                         std::size_t vertex_count, const u32* to_global, Batch& batch) {
    const std::size_t stride = stride_bytes / sizeof(f32);
    if (index_count == 0) {
        return;
    }
    const std::size_t bound = meshopt_buildMeshletsBound(index_count, max_cluster_vertices, setup.min_triangles);
    std::vector<meshopt_Meshlet> meshlets(bound);
    std::vector<u32> meshlet_vertices(bound * max_cluster_vertices);
    std::vector<u8> meshlet_triangles(bound * setup.max_triangles * 3);
    const std::size_t count = meshopt_buildMeshletsSpatial(meshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(), indices, index_count,
                                                           positions, vertex_count, stride_bytes, max_cluster_vertices, setup.min_triangles,
                                                           setup.max_triangles, 0.5f);
    for (std::size_t m = 0; m < count; ++m) {
        const meshopt_Meshlet& meshlet = meshlets[m];
        const u32* v = meshlet_vertices.data() + meshlet.vertex_offset;
        const u8* t = meshlet_triangles.data() + meshlet.triangle_offset;
        // A sphere round the cluster's vertices: a good centre from meshoptimizer, the radius exactly large enough.
        f32 points[max_cluster_vertices * 3];
        for (u32 i = 0; i < meshlet.vertex_count; ++i) {
            const f32* p = positions + static_cast<std::size_t>(v[i]) * stride;
            points[i * 3] = p[0];
            points[i * 3 + 1] = p[1];
            points[i * 3 + 2] = p[2];
        }
        const meshopt_Bounds bounds = meshopt_computeSphereBounds(points, meshlet.vertex_count, 3 * sizeof(f32), nullptr, 0);
        f32 radius = 0.0f;
        for (u32 i = 0; i < meshlet.vertex_count; ++i) {
            const f32 d[3] = {points[i * 3] - bounds.center[0], points[i * 3 + 1] - bounds.center[1], points[i * 3 + 2] - bounds.center[2]};
            radius = std::max(radius, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        }
        BuiltCluster c;
        c.vertex_offset = static_cast<u32>(batch.vertices.size());
        c.triangle_offset = static_cast<u32>(batch.triangles.size());
        c.vertex_count = static_cast<u16>(meshlet.vertex_count);
        c.triangle_count = static_cast<u16>(meshlet.triangle_count);
        c.cull = {{bounds.center[0], bounds.center[1], bounds.center[2]}, radius * (1.0f + 1e-6f)};
        for (u32 i = 0; i < meshlet.vertex_count; ++i) {
            batch.vertices.push_back(to_global ? to_global[v[i]] : v[i]);
        }
        batch.triangles.insert(batch.triangles.end(), t, t + meshlet.triangle_count * 3u);
        batch.clusters.push_back(c);
    }
}

// Appends a thread's clusters to the mesh's, as leaves or as the outputs of `group`; returns the first new id.
u32 append_batch(MeshClusters& mesh, const Batch& batch, u32 depth, u32 group, f32 error, Sphere lod) {
    const u32 first = static_cast<u32>(mesh.clusters.size());
    const u32 vertex_base = static_cast<u32>(mesh.cluster_vertices.size());
    const u32 triangle_base = static_cast<u32>(mesh.triangles.size());
    mesh.cluster_vertices.insert(mesh.cluster_vertices.end(), batch.vertices.begin(), batch.vertices.end());
    mesh.triangles.insert(mesh.triangles.end(), batch.triangles.begin(), batch.triangles.end());
    for (const BuiltCluster& b : batch.clusters) {
        MeshCluster c;
        c.vertex_offset = vertex_base + b.vertex_offset;
        c.triangle_offset = triangle_base + b.triangle_offset;
        c.vertex_count = b.vertex_count;
        c.triangle_count = b.triangle_count;
        c.depth = depth;
        c.group = group;
        c.error = error;
        c.cull_center = b.cull.center;
        c.cull_radius = b.cull.radius;
        c.center = group == no_cluster ? b.cull.center : lod.center;
        c.radius = group == no_cluster ? b.cull.radius : lod.radius;
        mesh.total_triangles += c.triangle_count;
        mesh.clusters.push_back(c);
    }
    return first;
}

// Removes the flaps a simplification can leave: two triangles on the same three vertices, one the other turned over
// (a fold of no thickness), cancel; repeats of one triangle are kept once. Returns the new index count.
std::size_t drop_flaps(u32* indices, std::size_t count) {
    struct Key {
        u32 sorted[3];
        u32 at;
        int side; // +1 / -1: the winding, as the parity of the permutation that sorts the corners
    };
    std::vector<Key> keys(count / 3);
    for (std::size_t t = 0; t < keys.size(); ++t) {
        u32 a = indices[t * 3], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
        int side = 1;
        if (a > b) { std::swap(a, b); side = -side; }
        if (b > c) { std::swap(b, c); side = -side; }
        if (a > b) { std::swap(a, b); side = -side; }
        keys[t] = {{a, b, c}, static_cast<u32>(t), side};
    }
    std::sort(keys.begin(), keys.end(), [](const Key& x, const Key& y) {
        return std::lexicographical_compare(x.sorted, x.sorted + 3, y.sorted, y.sorted + 3) || (std::equal(x.sorted, x.sorted + 3, y.sorted) && x.at < y.at);
    });
    std::vector<u8> drop(keys.size(), 0);
    bool any = false;
    for (std::size_t i = 0; i < keys.size();) {
        std::size_t j = i;
        while (j < keys.size() && std::equal(keys[i].sorted, keys[i].sorted + 3, keys[j].sorted)) {
            ++j;
        }
        if (j - i > 1) {
            std::vector<u32> positive, negative;
            for (std::size_t k = i; k < j; ++k) {
                (keys[k].side > 0 ? positive : negative).push_back(keys[k].at);
            }
            const std::size_t pairs = std::min(positive.size(), negative.size());
            for (std::size_t k = 0; k < pairs; ++k) {
                drop[positive[k]] = drop[negative[k]] = 1;
            }
            for (std::size_t k = pairs + 1; k < positive.size(); ++k) {
                drop[positive[k]] = 1;
            }
            for (std::size_t k = pairs + 1; k < negative.size(); ++k) {
                drop[negative[k]] = 1;
            }
            any = true;
        }
        i = j;
    }
    if (!any) {
        return count;
    }
    std::size_t kept = 0;
    for (std::size_t t = 0; t < drop.size(); ++t) {
        if (!drop[t]) {
            indices[kept] = indices[t * 3];
            indices[kept + 1] = indices[t * 3 + 1];
            indices[kept + 2] = indices[t * 3 + 2];
            kept += 3;
        }
    }
    return kept;
}

struct GroupResult {
    bool ok = false;
    f32 error = 0.0f; // what the simplification added
    Batch batch;
};

} // namespace

std::vector<u32> MeshClusters::leaf_indices() const {
    std::vector<u32> out(static_cast<std::size_t>(leaf_triangles) * 3);
    std::size_t at = 0;
    for (u32 c = 0; c < leaf_count; ++c) {
        append_indices(c, out.data() + at);
        at += static_cast<std::size_t>(clusters[c].triangle_count) * 3;
    }
    return out;
}

MeshClusterInfo MeshClusters::info() const {
    MeshClusterInfo i;
    i.clusters = static_cast<u32>(clusters.size());
    i.levels = levels;
    i.roots = static_cast<u32>(roots.size());
    i.leaf_triangles = leaf_triangles;
    i.total_triangles = total_triangles;
    i.flat = flat;
    return i;
}

MeshClusters build_mesh_clusters(const MeshData& data, const MeshOptions& options, const ClusterParallelFor& parallel) {
    const auto started = std::chrono::steady_clock::now();
    MeshClusters mesh;
    const std::size_t source_vertices = data.vertices.size();
    const bool indexed = !data.indices.empty();
    const std::size_t source_indices = indexed ? data.indices.size() : source_vertices;
    mesh.flat = is_flat(data);
    if (source_vertices == 0) {
        return mesh;
    }

    // Welded. A flat-shaded mesh by position, UV and colour (its normals are made in the shader), the rest by every
    // attribute; an indexed one that is not flat is kept as it is.
    std::vector<u32> indices;
    if (mesh.flat || !indexed) {
        std::vector<u32> remap(source_vertices);
        std::size_t unique = 0;
        if (mesh.flat) {
            const auto* base = reinterpret_cast<const u8*>(data.vertices.data());
            const meshopt_Stream streams[3] = {{base + offsetof(MeshVertex, position), sizeof(Vec3f), sizeof(MeshVertex)},
                                               {base + offsetof(MeshVertex, uv), sizeof(Vec2f), sizeof(MeshVertex)},
                                               {base + offsetof(MeshVertex, color), sizeof(Color), sizeof(MeshVertex)}};
            unique = meshopt_generateVertexRemapMulti(remap.data(), indexed ? data.indices.data() : nullptr, source_indices, source_vertices, streams, 3);
        } else {
            unique = meshopt_generateVertexRemap(remap.data(), nullptr, source_indices, data.vertices.data(), source_vertices, sizeof(MeshVertex));
        }
        mesh.vertices.resize(unique);
        for (std::size_t i = 0; i < source_vertices; ++i) {
            mesh.vertices[remap[i]] = data.vertices[i];
        }
        if (mesh.flat) {
            for (MeshVertex& v : mesh.vertices) {
                v.normal = {};
            }
        }
        indices.resize(source_indices);
        if (indexed) {
            meshopt_remapIndexBuffer(indices.data(), data.indices.data(), source_indices, remap.data());
        } else {
            for (std::size_t i = 0; i < source_indices; ++i) {
                indices[i] = remap[i];
            }
        }
    } else {
        mesh.vertices = data.vertices;
        indices = data.indices;
    }
    // Triangles that lost a corner to the weld are not triangles any more.
    {
        std::size_t kept = 0;
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            if (indices[t] == indices[t + 1] || indices[t + 1] == indices[t + 2] || indices[t] == indices[t + 2]) {
                ++mesh.degenerate_removed;
                continue;
            }
            indices[kept] = indices[t];
            indices[kept + 1] = indices[t + 1];
            indices[kept + 2] = indices[t + 2];
            kept += 3;
        }
        indices.resize(kept);
    }
    const std::size_t vertex_count = mesh.vertices.size();
    const f32* positions = &mesh.vertices[0].position.x;

    SplitSetup setup;
    setup.max_triangles = std::clamp<std::size_t>(options.cluster_triangles / 4 * 4, 4, max_cluster_triangles);
    setup.min_triangles = std::max<std::size_t>(4, setup.max_triangles / 2 / 4 * 4);

    // The leaves.
    {
        Batch leaves;
        split_into_clusters(setup, indices.data(), indices.size(), positions, sizeof(MeshVertex), vertex_count, nullptr, leaves);
        append_batch(mesh, leaves, 0, no_cluster, 0.0f, {});
    }
    mesh.leaf_count = static_cast<u32>(mesh.clusters.size());
    for (const MeshCluster& c : mesh.clusters) {
        mesh.leaf_triangles += c.triangle_count;
    }
    indices = {};
    mesh.levels = 1;

    // What the simplifier minds besides where the surface is, if the mesh has any variation of it.
    bool uv_varies = false, color_varies = false;
    for (const MeshVertex& v : mesh.vertices) {
        uv_varies = uv_varies || v.uv.x != mesh.vertices[0].uv.x || v.uv.y != mesh.vertices[0].uv.y;
        color_varies = color_varies || std::memcmp(&v.color, &mesh.vertices[0].color, sizeof(Color)) != 0;
    }
    std::vector<f32> weights;
    if (!mesh.flat) {
        weights.insert(weights.end(), 3, options.normal_weight);
    }
    if (uv_varies) {
        weights.insert(weights.end(), 2, options.uv_weight);
    }
    if (color_varies) {
        weights.insert(weights.end(), 4, options.color_weight);
    }
    const bool with_attributes = !weights.empty() && std::any_of(weights.begin(), weights.end(), [](f32 w) { return w > 0.0f; });
    const std::size_t attribute_count = weights.size();
    const auto attributes_of = [&](const MeshVertex& v, f32* a) {
        std::size_t k = 0;
        if (!mesh.flat) {
            a[k++] = v.normal.x;
            a[k++] = v.normal.y;
            a[k++] = v.normal.z;
        }
        if (uv_varies) {
            a[k++] = v.uv.x;
            a[k++] = v.uv.y;
        }
        if (color_varies) {
            a[k++] = static_cast<f32>(v.color.r) / 255.0f;
            a[k++] = static_cast<f32>(v.color.g) / 255.0f;
            a[k++] = static_cast<f32>(v.color.b) / 255.0f;
            a[k++] = static_cast<f32>(v.color.a) / 255.0f;
        }
    };

    std::vector<u32> frontier(mesh.leaf_count);
    for (u32 i = 0; i < mesh.leaf_count; ++i) {
        frontier[i] = i;
    }
    for (u32 round = 1; frontier.size() > 1 && round <= max_rounds; ++round) {
        // Groups of about 4 clusters that touch, by the vertices they share (and, where none, by nearness).
        std::vector<u32> counts(frontier.size()), shared;
        std::size_t total = 0;
        for (std::size_t i = 0; i < frontier.size(); ++i) {
            counts[i] = mesh.clusters[frontier[i]].vertex_count;
            total += counts[i];
        }
        shared.reserve(total);
        for (const u32 id : frontier) {
            const MeshCluster& c = mesh.clusters[id];
            shared.insert(shared.end(), mesh.cluster_vertices.begin() + c.vertex_offset, mesh.cluster_vertices.begin() + c.vertex_offset + c.vertex_count);
        }
        std::vector<u32> partition(frontier.size());
        std::size_t partitions = meshopt_partitionClusters(partition.data(), shared.data(), shared.size(), counts.data(), frontier.size(), positions,
                                                          vertex_count, sizeof(MeshVertex), 4);
        shared = {};
        if (partitions >= frontier.size()) {
            // Nothing to group by (loose pieces): neighbours in the frontier's order, which is spatial.
            partitions = (frontier.size() + 3) / 4;
            for (std::size_t i = 0; i < frontier.size(); ++i) {
                partition[i] = static_cast<u32>(i / 4);
            }
        }
        std::vector<std::vector<u32>> members(partitions);
        for (std::size_t i = 0; i < frontier.size(); ++i) {
            members[partition[i]].push_back(frontier[i]);
        }
        members.erase(std::remove_if(members.begin(), members.end(), [](const std::vector<u32>& m) { return m.empty(); }), members.end());

        std::vector<GroupResult> results(members.size());
        const auto process = [&](u32 g) {
            const std::vector<u32>& in = members[g];
            GroupResult& result = results[g];
            std::size_t triangles = 0;
            std::vector<u32> local_to_global;
            for (const u32 id : in) {
                const MeshCluster& c = mesh.clusters[id];
                triangles += c.triangle_count;
                local_to_global.insert(local_to_global.end(), mesh.cluster_vertices.begin() + c.vertex_offset,
                                       mesh.cluster_vertices.begin() + c.vertex_offset + c.vertex_count);
            }
            std::sort(local_to_global.begin(), local_to_global.end());
            local_to_global.erase(std::unique(local_to_global.begin(), local_to_global.end()), local_to_global.end());
            const std::size_t m = local_to_global.size();
            std::vector<u32> merged;
            merged.reserve(triangles * 3);
            for (const u32 id : in) {
                const MeshCluster& c = mesh.clusters[id];
                const u32* v = mesh.cluster_vertices.data() + c.vertex_offset;
                const u8* t = mesh.triangles.data() + c.triangle_offset;
                for (u32 i = 0; i < c.triangle_count * 3u; ++i) {
                    merged.push_back(static_cast<u32>(std::lower_bound(local_to_global.begin(), local_to_global.end(), v[t[i]]) - local_to_global.begin()));
                }
            }
            std::vector<f32> local_positions(m * 3);
            for (std::size_t i = 0; i < m; ++i) {
                const Vec3f p = mesh.vertices[local_to_global[i]].position;
                local_positions[i * 3] = p.x;
                local_positions[i * 3 + 1] = p.y;
                local_positions[i * 3 + 2] = p.z;
            }
            std::vector<f32> local_attributes;
            if (with_attributes) {
                local_attributes.resize(m * attribute_count);
                for (std::size_t i = 0; i < m; ++i) {
                    attributes_of(mesh.vertices[local_to_global[i]], &local_attributes[i * attribute_count]);
                }
            }
            // About half; the group's outer border (and the mesh's open ones) stays where it is.
            const std::size_t target = std::max<std::size_t>(1, triangles / 2) * 3;
            std::vector<u32> simplified(merged.size());
            f32 error = 0.0f;
            const unsigned flags = meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute;
            const std::size_t kept = with_attributes
                ? meshopt_simplifyWithAttributes(simplified.data(), merged.data(), merged.size(), local_positions.data(), m, 3 * sizeof(f32),
                                                 local_attributes.data(), attribute_count * sizeof(f32), weights.data(), attribute_count, nullptr, target,
                                                 std::numeric_limits<f32>::max(), flags, &error)
                : meshopt_simplify(simplified.data(), merged.data(), merged.size(), local_positions.data(), m, 3 * sizeof(f32), target,
                                   std::numeric_limits<f32>::max(), flags, &error);
            if (kept == 0 || static_cast<f64>(kept / 3) > static_cast<f64>(triangles) * (1.0 - min_group_reduction)) {
                return; // stuck: not ok
            }
            simplified.resize(drop_flaps(simplified.data(), kept));
            if (simplified.empty()) {
                return;
            }
            split_into_clusters(setup, simplified.data(), simplified.size(), local_positions.data(), 3 * sizeof(f32), m, local_to_global.data(), result.batch);
            result.error = error;
            result.ok = true;
        };
        if (parallel && members.size() > 1) {
            parallel(static_cast<u32>(members.size()), process);
        } else {
            for (u32 g = 0; g < members.size(); ++g) {
                process(g);
            }
        }

        // In order: the groups made, their outputs, and the clusters of groups that could not be simplified, which wait.
        bool progress = false;
        std::vector<u32> next;
        std::vector<Sphere> spheres;
        for (u32 g = 0; g < members.size(); ++g) {
            const std::vector<u32>& in = members[g];
            if (!results[g].ok) {
                ++mesh.stuck_groups;
                next.insert(next.end(), in.begin(), in.end());
                continue;
            }
            progress = true;
            spheres.clear();
            f32 inputs_error = 0.0f;
            for (const u32 id : in) {
                spheres.push_back({mesh.clusters[id].center, mesh.clusters[id].radius});
                inputs_error = std::max(inputs_error, mesh.clusters[id].error);
            }
            const Sphere lod = union_of(spheres);
            const f32 error = inputs_error + std::max(results[g].error, 0.0f);
            const u32 group_id = static_cast<u32>(mesh.groups.size());
            MeshClusterGroup group;
            group.input_first = static_cast<u32>(mesh.group_inputs.size());
            group.input_count = static_cast<u32>(in.size());
            group.error = error;
            group.center = lod.center;
            group.radius = lod.radius;
            for (const u32 id : in) {
                mesh.group_inputs.push_back(id);
                MeshCluster& c = mesh.clusters[id];
                c.parent_error = error;
                c.parent_center = lod.center;
                c.parent_radius = lod.radius;
                c.parent_depth = round;
                c.parent_group = group_id;
            }
            group.output_first = append_batch(mesh, results[g].batch, round, group_id, error, lod);
            group.output_count = static_cast<u32>(results[g].batch.clusters.size());
            for (u32 i = 0; i < group.output_count; ++i) {
                next.push_back(group.output_first + i);
            }
            mesh.groups.push_back(group);
            results[g] = {};
        }
        if (!progress) {
            break;
        }
        frontier.swap(next);
        mesh.levels = round + 1;
    }
    mesh.roots = frontier;
    mesh.build_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    return mesh;
}

f32 cluster_scale_of(const Mat4f& t) {
    const f32 sx = t.m[0] * t.m[0] + t.m[1] * t.m[1] + t.m[2] * t.m[2];
    const f32 sy = t.m[4] * t.m[4] + t.m[5] * t.m[5] + t.m[6] * t.m[6];
    const f32 sz = t.m[8] * t.m[8] + t.m[9] * t.m[9] + t.m[10] * t.m[10];
    return std::sqrt(std::max({sx, sy, sz}));
}

ClusterCutTest::ClusterCutTest(const ClusterCutView& v) : view(v), scale(cluster_scale_of(v.transform)) {}

f32 ClusterCutTest::distance(Vec3f c, f32 r) const {
    if (view.orthographic) {
        return 1.0f;
    }
    const Vec3f to = c - view.eye;
    const f32 depth = dot(to, view.forward);
    // The nearest point of the sphere: in front of the camera's plane, along the view; astride it, as near as can be;
    // behind it, the sphere's surface.
    const f32 d = depth > r ? depth - r : depth < -r ? length(to) - r : 0.0f;
    return std::max(d, view.nearest);
}

bool ClusterCutTest::fine(f32 error, Vec3f center, f32 radius) const {
    if (error <= 0.0f) {
        return true;
    }
    if (!std::isfinite(error)) {
        return false;
    }
    // pixels = error * scale * per_unit / distance, at most `pixels`.
    return error * scale * view.per_unit <= view.pixels * distance(view.transform.apply(center), radius * scale);
}

bool ClusterCutTest::selected(const MeshCluster& c) const {
    return fine(c.error, c.center, c.radius) && !fine(c.parent_error, c.parent_center, c.parent_radius);
}

} // namespace kin
