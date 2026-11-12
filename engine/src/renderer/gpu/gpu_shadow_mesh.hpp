#pragma once

#include <array>
#include <bit>
#include <kin/render3d/mesh_lod3d.hpp>
#include <unordered_map>
#include <vector>

namespace kin::gpu {

// Stock rigid, opaque shadows read positions only. Weld exact position bits,
// not distances: retain signed zero, every triangle, winding and index order.
// The levels keep their original first_index/index_count ranges. The caller
// excludes skins, morphs, cluster cuts and shaders that read other attributes.
// Inputs have already passed create_mesh's buffer-size limits.
struct ShadowMesh {
    std::vector<Vec3f> positions;
    std::vector<u32> indices;
};

inline ShadowMesh build_shadow_mesh(const MeshData3D& mesh, const MeshLodChain3D* chain = nullptr) {
    struct Hash {
        std::size_t operator()(const std::array<u32, 3>& key) const {
            u64 h = 14695981039346656037ull;
            for (const u32 word : key) {
                h ^= word;
                h *= 1099511628211ull;
            }
            return static_cast<std::size_t>(h);
        }
    };
    ShadowMesh result;
    const std::size_t count = mesh.vertices.size() + (chain ? chain->extra_vertices.size() : 0);
    std::unordered_map<std::array<u32, 3>, u32, Hash> unique;
    unique.reserve(count);
    std::vector<u32> remap(count);
    result.positions.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3f p = i < mesh.vertices.size() ? mesh.vertices[i].position
                                                 : chain->extra_vertices[i - mesh.vertices.size()].position;
        const std::array key{std::bit_cast<u32>(p.x), std::bit_cast<u32>(p.y), std::bit_cast<u32>(p.z)};
        const auto [entry, added] = unique.try_emplace(key, static_cast<u32>(result.positions.size()));
        if (added)
            result.positions.push_back(p);
        remap[i] = entry->second;
    }
    std::size_t indices = mesh.indices.empty() ? mesh.vertices.size() : mesh.indices.size();
    for (std::size_t l = 1; chain && l < chain->levels.size(); ++l)
        indices += chain->levels[l].indices.size();
    result.indices.reserve(indices);
    if (mesh.indices.empty()) {
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
            result.indices.push_back(remap[i]);
    } else {
        for (const u32 i : mesh.indices) {
            if (i >= mesh.vertices.size())
                return {};
            result.indices.push_back(remap[i]);
        }
    }
    for (std::size_t l = 1; chain && l < chain->levels.size(); ++l)
        for (const u32 i : chain->levels[l].indices) {
            if (i >= remap.size())
                return {};
            result.indices.push_back(remap[i]);
        }
    return result;
}

} // namespace kin::gpu
