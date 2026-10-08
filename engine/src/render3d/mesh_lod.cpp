#include <kin/render3d/mesh_lod.hpp>

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace kin {

static_assert(offsetof(MeshVertex, position) == 0 && sizeof(Vec3f) == 3 * sizeof(f32), "meshoptimizer reads positions at the vertex's start");

namespace {

// The largest extent of the positions that `indices` use (what meshopt_simplifyScale gives for them): the simplifier's
// errors, with Sparse, are relative to it. No copy of the positions is made.
template <typename Position>
f32 extent_of(const std::vector<u32>& indices, const Position& position_of) {
    Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const u32 i : indices) {
        const Vec3f p = position_of(i);
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    return indices.empty() ? 0.0f : std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
}

// Whether a mesh with no indices is flat-shaded: every corner's normal is its triangle's own (what a model with no
// normals is read as). Its triangles then share no vertex, and a level is better made over welded positions, with its
// own vertices (flat normals again).
bool is_flat_soup(const MeshData& data) {
    std::size_t flat = 0, counted = 0;
    for (std::size_t t = 0; t + 2 < data.vertices.size(); t += 3) {
        const Vec3f a = data.vertices[t].position, b = data.vertices[t + 1].position, c = data.vertices[t + 2].position;
        const Vec3f n = cross(b - a, c - a);
        const f32 length_n = length(n);
        if (length_n < 1e-20f) {
            continue;
        }
        ++counted;
        bool all = true;
        for (std::size_t k = 0; k < 3 && all; ++k) {
            all = dot(data.vertices[t + k].normal, n) > 0.999f * length_n;
        }
        flat += all ? 1 : 0;
    }
    return counted > 0 && flat * 100 >= counted * 99;
}

} // namespace

MeshLodChain build_mesh_levels(const MeshData& data, const MeshOptions& options) {
    MeshLodChain chain;
    const std::size_t vertex_count = data.vertices.size();
    const std::size_t triangles = data.triangle_count();
    chain.levels.push_back({static_cast<u32>(triangles), 0.0f, {}});
    const u32 wanted = std::min(options.levels, max_mesh_levels);
    if (wanted < 2 || triangles < 2 * std::max<std::size_t>(options.min_triangles, 1)) {
        return chain;
    }
    const f32 ratio = std::clamp(options.target_ratio, 0.1f, 0.9f);
    const f32 max_error = std::clamp(options.max_error, 1e-4f, 1.0f);
    // Sparse: a level's triangles use a share of the vertices (the others are the finer levels'). Without it the
    // simplifier mistakes the unused vertices that sit where a used one does (a seam's other side) for neighbours.
    // Its errors are then relative to the extent of the vertices in use.
    const unsigned flags = meshopt_SimplifySparse | (options.lock_border ? meshopt_SimplifyLockBorder : 0u);
    const auto target_of = [&](std::size_t current_triangles) {
        return std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<f64>(current_triangles) * ratio)) * 3;
    };
    // Too little gained, or too little left: this is as far as the chain goes.
    const auto stops = [&](std::size_t kept_triangles, std::size_t current_triangles) {
        return kept_triangles < options.min_triangles ||
               static_cast<f64>(kept_triangles) > static_cast<f64>(current_triangles) * (1.0 - static_cast<f64>(options.min_reduction));
    };

    // A flat-shaded mesh with no indices (a model with no normals; the kiln's 15 M vertices): welded by position, simplified
    // on positions alone (cheap: the 9 attributes of 15 M vertices took gigabytes), and each level gets vertices of its own
    // with the flat normal of its triangle and the UV and colour of the vertex it replaces. The mesh's vertices are not touched.
    if (data.indices.empty() && is_flat_soup(data)) {
        std::vector<u32> current(vertex_count);
        const meshopt_Stream stream{&data.vertices[0].position, 3 * sizeof(f32), sizeof(MeshVertex)};
        const std::size_t unique = meshopt_generateVertexRemapMulti(current.data(), nullptr, vertex_count, vertex_count, &stream, 1);
        std::vector<u32> from(unique, ~0u); // a corner of the mesh for each welded position
        for (std::size_t i = vertex_count; i-- > 0;) {
            from[current[i]] = static_cast<u32>(i);
        }
        const auto position_of = [&](u32 w) { return data.vertices[from[w]].position; };
        std::vector<f32> positions(unique * 3); // meshoptimizer reads a position array: the welded ones
        for (std::size_t w = 0; w < unique; ++w) {
            const Vec3f p = data.vertices[from[w]].position;
            positions[w * 3] = p.x;
            positions[w * 3 + 1] = p.y;
            positions[w * 3 + 2] = p.z;
        }
        std::vector<u32> simplified, ordered;
        f32 error = 0.0f;
        for (u32 level = 1; level < wanted; ++level) {
            const std::size_t current_triangles = current.size() / 3;
            const f32 scale = extent_of(current, position_of);
            simplified.resize(current.size());
            f32 relative = 0.0f;
            const std::size_t kept = meshopt_simplify(simplified.data(), current.data(), current.size(), positions.data(), unique, 3 * sizeof(f32),
                                                      target_of(current_triangles), max_error, flags, &relative);
            if (stops(kept / 3, current_triangles)) {
                break;
            }
            simplified.resize(kept);
            ordered.resize(kept);
            meshopt_optimizeVertexCache(ordered.data(), simplified.data(), kept, unique);
            error += relative * scale;
            MeshLodLevel out{static_cast<u32>(kept / 3), error, {}};
            out.indices.resize(kept);
            for (std::size_t t = 0; t < kept / 3; ++t) {
                const MeshVertex corner[3] = {data.vertices[from[ordered[t * 3]]], data.vertices[from[ordered[t * 3 + 1]]], data.vertices[from[ordered[t * 3 + 2]]]};
                const Vec3f n = cross(corner[1].position - corner[0].position, corner[2].position - corner[0].position);
                const f32 n_length = length(n);
                const Vec3f normal = n_length > 1e-20f ? n * (1.0f / n_length) : corner[0].normal;
                for (std::size_t k = 0; k < 3; ++k) {
                    MeshVertex v = corner[k];
                    v.normal = normal;
                    out.indices[t * 3 + k] = static_cast<u32>(vertex_count + chain.extra_vertices.size());
                    chain.extra_vertices.push_back(v);
                }
            }
            chain.levels.push_back(std::move(out));
            current.swap(ordered);
        }
        return chain;
    }

    // The mesh's indices. Where it has none (its vertices are the triangles, three by three) the simplifier has no
    // neighbours to work with, so identical vertices are taken as one: each is replaced by the first of its kind, which
    // is a vertex of the mesh, so the levels still index the mesh's own vertices.
    std::vector<u32> current;
    if (data.indices.empty()) {
        std::vector<u32> remap(vertex_count);
        const std::size_t unique = meshopt_generateVertexRemap(remap.data(), nullptr, vertex_count, data.vertices.data(), vertex_count,
                                                               sizeof(MeshVertex));
        std::vector<u32> first(unique, ~0u);
        for (std::size_t i = 0; i < vertex_count; ++i) {
            first[remap[i]] = std::min(first[remap[i]], static_cast<u32>(i));
        }
        current.resize(vertex_count);
        for (std::size_t i = 0; i < vertex_count; ++i) {
            current[i] = first[remap[i]];
        }
    } else {
        current = data.indices;
    }

    // What the simplifier minds besides where the surface is: normals, UVs, vertex colours.
    const float weights[9] = {options.normal_weight, options.normal_weight, options.normal_weight,
                              options.uv_weight,     options.uv_weight,
                              options.color_weight,  options.color_weight, options.color_weight, options.color_weight};
    std::size_t attribute_count = 0;
    for (const float w : weights) {
        attribute_count += w > 0.0f ? 1 : 0;
    }
    std::vector<f32> attributes;
    if (attribute_count > 0) {
        attributes.resize(vertex_count * 9);
        for (std::size_t i = 0; i < vertex_count; ++i) {
            const MeshVertex& v = data.vertices[i];
            f32* a = &attributes[i * 9];
            a[0] = v.normal.x; a[1] = v.normal.y; a[2] = v.normal.z;
            a[3] = v.uv.x;     a[4] = v.uv.y;
            a[5] = static_cast<f32>(v.color.r) / 255.0f; a[6] = static_cast<f32>(v.color.g) / 255.0f;
            a[7] = static_cast<f32>(v.color.b) / 255.0f; a[8] = static_cast<f32>(v.color.a) / 255.0f;
        }
    }

    const f32* positions = &data.vertices[0].position.x;
    const auto position_of = [&](u32 i) { return data.vertices[i].position; };
    std::vector<u32> simplified, ordered;
    f32 error = 0.0f;
    for (u32 level = 1; level < wanted; ++level) {
        const std::size_t current_triangles = current.size() / 3;
        simplified.resize(current.size());
        const f32 scale = extent_of(current, position_of);
        f32 relative = 0.0f;
        const std::size_t kept = attribute_count > 0
            ? meshopt_simplifyWithAttributes(simplified.data(), current.data(), current.size(), positions, vertex_count, sizeof(MeshVertex),
                                             attributes.data(), 9 * sizeof(f32), weights, 9, nullptr, target_of(current_triangles), max_error, flags, &relative)
            : meshopt_simplify(simplified.data(), current.data(), current.size(), positions, vertex_count, sizeof(MeshVertex),
                               target_of(current_triangles), max_error, flags, &relative);
        if (stops(kept / 3, current_triangles)) {
            break;
        }
        simplified.resize(kept);
        ordered.resize(kept);
        meshopt_optimizeVertexCache(ordered.data(), simplified.data(), kept, vertex_count);
        error += relative * scale; // each step is measured against the one before: they add up to a bound
        chain.levels.push_back({static_cast<u32>(kept / 3), error, ordered});
        current.swap(ordered);
    }
    return chain;
}

} // namespace kin
