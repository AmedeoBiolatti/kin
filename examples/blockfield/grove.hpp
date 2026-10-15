#pragma once
// Shared scene recipe: one bounded disk file, streamed instance records, two tiny meshes/textures.
#include "world.hpp"
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <string_view>
#include <stdexcept>

namespace blockfield::grove {
constexpr u32 magic = 0x31444642, route_count = 1440, foliage_limit = 100000, transparent_limit = 20000;
constexpr u32 foliage_groups = 64, transparent_groups = 8;
struct Camera { Vec3f eye, target; };
inline void put(std::ostream& out, u32 v) { for (int i = 0; i < 4; ++i) out.put(char(v >> (8 * i))); }
inline u32 get(std::istream& in) {
    u32 v = 0; for (int i = 0; i < 4; ++i) { const int c = in.get(); if (c == EOF) throw std::runtime_error("truncated grove recipe"); v |= u32(c) << (8 * i); } return v;
}
inline void putf(std::ostream& out, f32 v) { put(out, std::bit_cast<u32>(v)); }
inline f32 getf(std::istream& in) {
    const f32 v = std::bit_cast<f32>(get(in));
    if (!std::isfinite(v) || std::abs(v) > 10000) throw std::runtime_error("invalid grove float");
    return v;
}
inline u32 hash(u32 v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; return v ^ (v >> 16); }
inline f32 random(u32 id, u32 lane) { return f32(hash(id * 17 + lane * 0x9e3779b9u) & 0xffffff) / 16777216.0f; }
inline MeshData card() {
    return {{{{-0.5f, 0, 0}, {0, 0, 1}, {0, 1}}, {{0.5f, 0, 0}, {0, 0, 1}, {1, 1}},
             {{0.5f, 1, 0}, {0, 0, 1}, {1, 0}}, {{-0.5f, 1, 0}, {0, 0, 1}, {0, 0}}}, {0, 1, 2, 0, 2, 3}};
}
inline void generate(const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write grove recipe");
    put(out, magic); put(out, 1); put(out, route_count); put(out, foliage_limit); put(out, transparent_limit);
    World world(1);
    // Bake clearance against the actual seeded terrain AND its existing voxel trees. Smooth height
    // changes conservatively from above so the tour never intersects the surface/crowns.
    std::array<Vec3f, route_count> route{};
    for (u32 i = 0; i < route_count; ++i) {
        const f32 a = f32(i) * 6.28318530718f / route_count;
        const f32 x = 24 * std::sin(a), z = 24 * std::cos(a);
        int top = 0;
        for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
            const int xx = int(std::floor(x)) + dx, zz = int(std::floor(z)) + dz;
            int h = world.height(xx, zz);
            for (int y = h; y < h + 8; ++y) if (world.generated({xx, y, zz}) != Block::Air) top = std::max(top, y + 1);
            top = std::max(top, h);
        }
        route[i] = {x, f32(top) + 2.4f, z};
    }
    auto raised = route;
    for (u32 i = 0; i < route_count; ++i) for (int d = -30; d <= 30; ++d) {
        const u32 j = u32((int(i) + d + int(route_count)) % int(route_count));
        raised[i].y = std::max(raised[i].y, route[j].y);
    }
    for (u32 i = 0; i < route_count; ++i) {
        f32 y = 0;
        for (int d = -15; d <= 15; ++d) y += raised[u32((int(i) + d + int(route_count)) % int(route_count))].y;
        route[i].y = y / 31;
    }
    for (u32 i = 0; i < route_count; ++i) {
        const auto eye = route[i]; const auto ahead = route[(i + 48) % route_count];
        for (const auto p : {eye, Vec3f{ahead.x, eye.y - 0.4f, ahead.z}}) for (f32 v : {p.x, p.y, p.z}) putf(out, v);
    }
    for (u32 kind = 0; kind < 2; ++kind) for (u32 i = 0; i < (kind ? transparent_limit : foliage_limit); ++i) {
        const u32 group = i % (kind ? transparent_groups : foliage_groups);
        f32 x, z;
        if (!kind) {
            x = (f32(group % 8) - 4) * 24 + random(i, 1) * 24;
            z = (f32(group / 8) - 4) * 24 + random(i, 2) * 24;
            // A narrow clear walking lane; keep the baked route unobstructed by new vegetation.
            const f32 r = std::sqrt(x * x + z * z);
            if (r > 21 && r < 27) { const f32 s = (r < 24 ? 20.5f : 27.5f) / r; x *= s; z *= s; }
        } else {
            const f32 a = f32(group) * 6.28318530718f / transparent_groups;
            x = 34 * std::sin(a) + (random(i, 1) - 0.5f) * 12;
            z = 34 * std::cos(a) + (random(i, 2) - 0.5f) * 12;
        }
        const f32 y = f32(world.height(int(std::floor(x)), int(std::floor(z)))) + (kind ? random(i, 3) * 5 + 0.5f : 0);
        const f32 h = kind ? 3 + random(i, 4) * 3 : 0.8f + random(i, 4) * 2;
        const f32 w = kind ? 3 + random(i, 5) * 3 : h * 0.7f;
        for (f32 v : {x, y, z, random(i, 6) * 360, w, h, kind ? 1.0f : w}) putf(out, v);
    }
    MeshData leaf;
    for (int i = 0; i < 3; ++i) leaf.append(card(), Mat4f::rotation_y(f32(i) * 60));
    for (u32 kind = 0; kind < 2; ++kind) {
        const MeshData mesh = kind ? card() : leaf;
        const u32 side = kind ? 64 : 128;
        put(out, u32(mesh.vertices.size())); put(out, u32(mesh.indices.size())); put(out, side);
        for (const auto& v : mesh.vertices) for (f32 n : {v.position.x, v.position.y, v.position.z}) putf(out, n);
        for (const auto& v : mesh.vertices) for (f32 n : {v.normal.x, v.normal.y, v.normal.z}) putf(out, n);
        for (const auto& v : mesh.vertices) for (f32 n : {v.uv.x, v.uv.y}) putf(out, n);
        for (u32 n : mesh.indices) put(out, n);
        for (u32 y = 0; y < side; ++y) for (u32 x = 0; x < side; ++x) {
            const f32 u = f32(x) / (side - 1) - 0.5f, h = 1 - f32(y) / (side - 1);
            u8 r, g, b, a;
            if (!kind) {
                const bool stem = std::abs(u) < 0.018f;
                const bool branch = std::abs(u) < 0.46f * (1 - h) && std::fmod(h * 13 + std::abs(u) * 2, 1.0f) < 0.48f;
                a = h < 0.97f && (stem || branch) ? 255 : 0;
                r = stem ? 103 : 45; g = stem ? 81 : u8(110 + h * 60); b = stem ? 40 : 43;
            } else {
                const f32 rr = (u * u + (h - 0.5f) * (h - 0.5f)) * 4;
                r = 174; g = 184; b = 192; a = u8(std::lround(std::pow(std::max(0.0f, 1 - rr), 2) * 32));
            }
            for (u8 c : {r, g, b, a}) out.put(char(c));
        }
    }
    if (!out) throw std::runtime_error("grove write failed");
}
struct Batch { std::vector<Mat4f> copies; Bounds3 bounds{}; };
class Dressing {
public:
    void load(const std::string& path, u32 foliage, u32 transparent, Renderer3D& renderer, Renderer2D& r2d) {
        std::ifstream in(path, std::ios::binary);
        if (!in || get(in) != magic || get(in) != 1 || get(in) != route_count || get(in) != foliage_limit || get(in) != transparent_limit)
            throw std::runtime_error("invalid grove recipe header");
        if (foliage > foliage_limit || transparent > transparent_limit) throw std::runtime_error("grove instance limit exceeded");
        for (auto& c : route) { c.eye = {getf(in), getf(in), getf(in)}; c.target = {getf(in), getf(in), getf(in)}; }
        for (u32 kind = 0; kind < 2; ++kind) {
            auto& batches = kind ? smoke : plants;
            const u32 count = kind ? transparent : foliage, groups = kind ? transparent_groups : foliage_groups;
            batches.resize(groups);
            for (u32 i = 0; i < count; ++i) {
                const Vec3f p{getf(in), getf(in), getf(in)}; const f32 yaw = getf(in);
                const Vec3f scale{getf(in), getf(in), getf(in)};
                if (scale.x <= 0 || scale.y <= 0 || scale.z <= 0) throw std::runtime_error("invalid grove scale");
                batches[i % groups].copies.push_back(Mat4f::trs(p, Quatf::axis_angle({0, 1, 0}, yaw), scale));
            }
            in.seekg(std::streamoff((kind ? transparent_limit : foliage_limit) - count) * 28, std::ios::cur);
        }
        for (u32 kind = 0; kind < 2; ++kind) {
            const u32 nv = get(in), ni = get(in), side = get(in);
            if (nv > 64 || ni > 128 || ni % 3 || (side != 64 && side != 128)) throw std::runtime_error("invalid grove mesh/texture");
            MeshData mesh; mesh.vertices.resize(nv); mesh.indices.resize(ni);
            for (auto& v : mesh.vertices) v.position = {getf(in), getf(in), getf(in)};
            for (auto& v : mesh.vertices) v.normal = {getf(in), getf(in), getf(in)};
            for (auto& v : mesh.vertices) v.uv = {getf(in), getf(in)};
            for (auto& i : mesh.indices) { i = get(in); if (i >= nv) throw std::runtime_error("invalid grove index"); }
            std::vector<u8> pixels(side * side * 4);
            in.read(reinterpret_cast<char*>(pixels.data()), std::streamsize(pixels.size()));
            if (!in) throw std::runtime_error("truncated grove texture");
            meshes[kind] = renderer.create_mesh(mesh);
            textures[kind] = r2d.create_texture_from_rgba(pixels.data(), {i32(side), i32(side)});
            if (!meshes[kind] || !textures[kind]) throw std::runtime_error("grove resource creation failed");
            r2d.set_scale_mode(textures[kind], ScaleMode::Linear);
        }
        if (in.peek() != EOF) throw std::runtime_error("trailing grove data");
        for (auto& b : plants) b.bounds = instance_bounds(meshes[0], b.copies);
        for (auto& b : smoke) b.bounds = instance_bounds(meshes[1], b.copies);
        const char* retain = std::getenv("KIN_GROVE_RETAINED");
        if (!retain || std::string_view(retain) != "0") {
            const char* cells = std::getenv("KIN_GROVE_CELL_BATCHES");
            const bool split = cells && std::string_view(cells) == "1";
            std::vector<MeshInstance> records;
            std::vector<InstanceGroup3D> groups;
            if (!split) records.reserve(foliage);
            for (auto& b : plants) if (!b.copies.empty()) {
                groups.push_back({static_cast<u32>(records.size()), static_cast<u32>(b.copies.size()), b.bounds});
                for (const auto& t : b.copies) records.push_back({t, colors::white, 0});
                std::vector<Mat4f>().swap(b.copies);
                if (split) {
                    foliage_batches.emplace_back(meshes[0], std::move(records), std::move(groups));
                    records.clear(); groups.clear();
                }
            }
            if (!split) foliage_batches.emplace_back(meshes[0], std::move(records), std::move(groups));
            std::vector<Batch>().swap(plants);
        }
        foliage_count = foliage; transparent_count = transparent;
    }
    void draw(Scene3D& scene, bool shadows, bool unlit) const {
        Material3D leaf{.unlit = unlit, .double_sided = true, .blend = Blend3D::Mask, .texture = textures[0]};
        leaf.alpha_cutoff = 0.45f; leaf.casts_shadow = leaf.receives_shadow = shadows;
        Material3D mist{.unlit = true, .double_sided = true, .blend = Blend3D::Alpha, .texture = textures[1]};
        mist.casts_shadow = mist.receives_shadow = false;
        if (!foliage_batches.empty()) for (const auto& batch : foliage_batches) scene.draw_instanced(batch, leaf);
        else for (const auto& b : plants) if (!b.copies.empty()) scene.draw_instanced_group(meshes[0], b.copies, b.bounds, leaf);
        for (const auto& b : smoke) if (!b.copies.empty()) scene.draw_instanced_group(meshes[1], b.copies, b.bounds, mist);
    }
    std::array<Camera, route_count> route{};
    std::vector<InstanceBatch3D> foliage_batches;
    std::vector<Batch> plants, smoke;
    std::array<Mesh, 2> meshes;
    std::array<Texture, 2> textures;
    u32 foliage_count = 0, transparent_count = 0;
};
} // namespace blockfield::grove
