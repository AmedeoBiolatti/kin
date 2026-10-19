#pragma once

// What the bakes of levels of detail (mesh_lod.cpp) and of cluster hierarchies (mesh_clusters.cpp) share: a mesh's
// bytes for a key, and vertices as bytes. For the engine's sources only.

#include <cstddef>
#include <kin/assets/bake_cache.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <meshoptimizer.h>

namespace kin::mesh_bake {

// A vertex is 36 bytes with no padding (the key hashes them as they are, and they are written field by field).
static_assert(sizeof(Vec3f) == 12 && sizeof(Vec2f) == 8 && sizeof(Color) == 4);
static_assert(offsetof(MeshVertex3D, normal) == 12 && offsetof(MeshVertex3D, uv) == 24 &&
                  offsetof(MeshVertex3D, color) == 32 && sizeof(MeshVertex3D) == 36,
              "a field added to MeshVertex3D must be added to the bakes (and their versions raised)");
constexpr std::size_t vertex_bytes = 36;

// The mesh as the builds read it, and the simplifier's version (another version simplifies otherwise).
inline void add_mesh(BakeKey& key, const MeshData3D& data) {
    key.value(static_cast<u32>(MESHOPTIMIZER_VERSION));
    key.bytes(data.vertices.data(), data.vertices.size() * sizeof(MeshVertex3D));
    key.bytes(data.indices.data(), data.indices.size() * sizeof(u32));
}

inline void write_vertices(BakeWriter& w, const std::vector<MeshVertex3D>& vertices) {
    w.value<u64>(vertices.size());
    if constexpr (std::endian::native == std::endian::little) {
        w.raw(vertices.data(), vertices.size() * vertex_bytes);
    } else {
        for (const MeshVertex3D& v : vertices) {
            for (const f32 f :
                 {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y}) {
                w.value(f);
            }
            for (const u8 c : {v.color.r, v.color.g, v.color.b, v.color.a}) {
                w.value(c);
            }
        }
    }
}

inline bool read_vertices(BakeReader& r, std::vector<MeshVertex3D>& vertices) {
    u64 n = 0;
    if (!r.count(n, vertex_bytes)) {
        return false;
    }
    vertices.resize(static_cast<std::size_t>(n));
    if constexpr (std::endian::native == std::endian::little) {
        return r.raw(vertices.data(), vertices.size() * vertex_bytes);
    } else {
        for (MeshVertex3D& v : vertices) {
            for (f32* f : {&v.position.x, &v.position.y, &v.position.z, &v.normal.x, &v.normal.y, &v.normal.z, &v.uv.x,
                           &v.uv.y}) {
                r.value(*f);
            }
            for (u8* c : {&v.color.r, &v.color.g, &v.color.b, &v.color.a}) {
                r.value(*c);
            }
        }
        return r.ok();
    }
}

} // namespace kin::mesh_bake
