#include "world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace blockfield {
namespace {
u64 mix(u64 x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
u64 noise(u64 seed, int x, int z) { return mix(seed ^ (u64(i64(x)) * 0x9e3779b97f4a7c15ull) ^ (u64(i64(z)) * 0x632be59bd9b4e019ull)); }
Cell from(const std::array<int, 3>& v) { return {v[0], v[1], v[2]}; }
Vec3f vec(const std::array<int, 3>& v) { return {f32(v[0]), f32(v[1]), f32(v[2])}; }
}
int floor_div(int v, int d) { return v / d - (v % d < 0 ? 1 : 0); }
ChunkKey chunk_of(Cell c) { return {floor_div(c.x, chunk_size), floor_div(c.z, chunk_size)}; }
const char* block_name(Block b) {
    static const char* names[] = {"Air", "Grass", "Dirt", "Stone", "Sand", "Wood", "Leaves", "Bedrock"};
    return names[u8(b)];
}
Color block_colour(Block b, int axis, int sign) {
    switch (b) {
    case Block::Grass: return axis == 1 && sign > 0 ? Color::rgb(96, 153, 60) : Color::rgb(128, 98, 62);
    case Block::Dirt: return Color::rgb(128, 98, 62);
    case Block::Stone: return Color::rgb(155, 159, 169);
    case Block::Sand: return Color::rgb(222, 200, 143);
    case Block::Wood: return axis == 1 ? Color::rgb(161, 128, 79) : Color::rgb(105, 78, 47);
    case Block::Leaves: return Color::rgb(70, 128, 48);
    case Block::Bedrock: return Color::rgb(65, 69, 77);
    default: return colors::white;
    }
}
int World::height(int x, int z) const {
    const double phase = double(_seed % 4096) * 0.17;
    return std::clamp(int(22 + 6 * std::sin(x * 0.035 + phase) + 5 * std::cos(z * 0.043 - phase) +
                          3 * std::sin((x + z) * 0.081) + int(noise(_seed, floor_div(x, 3), floor_div(z, 3)) % 3)), 8, 40);
}
Block World::generated(Cell c) const {
    if (c.y < 0 || c.y >= world_height) return Block::Air;
    if (c.y == 0) return Block::Bedrock;
    const int h = height(c.x, c.z);
    if (c.y < h) {
        // Winding underground passages, kept clear of the protected spawn and the surface skin.
        const double tunnel = h - 9 + 2.0 * std::sin(c.x * 0.12) + 2.0 * std::cos(c.z * 0.11);
        if (std::abs(c.x) + std::abs(c.z) > 8 && c.y > 3 && c.y < h - 3 &&
            std::abs(c.y - tunnel) < 2.0 && std::sin(c.x * 0.08 + c.z * 0.13) > 0.25) return Block::Air;
        return c.y == h - 1 ? (h < 17 ? Block::Sand : Block::Grass) : c.y >= h - 4 ? Block::Dirt : Block::Stone;
    }
    // A tree belongs to a seed cell, rather than to a loaded chunk. Crowns crossing a chunk boundary stay consistent.
    const int sx = floor_div(c.x, 8), sz = floor_div(c.z, 8);
    for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
        const u64 n = noise(_seed + 71, sx + dx, sz + dz);
        if (n % 5 > 1) continue;
        const int tx = (sx + dx) * 8 + 2 + int((n >> 8) % 4), tz = (sz + dz) * 8 + 2 + int((n >> 16) % 4);
        if (std::abs(tx) + std::abs(tz) < 8 || std::abs(c.x - tx) > 2 || std::abs(c.z - tz) > 2) continue;
        const int base = height(tx, tz), top = base + 4;
        if (c.x == tx && c.z == tz && c.y >= base && c.y <= top) return Block::Wood;
        const int ddx = c.x - tx, ddz = c.z - tz, ddy = c.y - top;
        if (ddx * ddx + ddz * ddz + ddy * ddy <= 6 && c.y >= base + 2) return Block::Leaves;
    }
    return Block::Air;
}
Block World::block(Cell c) const {
    const auto it = _edits.find(c);
    return it == _edits.end() ? generated(c) : it->second;
}
bool World::set(Cell c, Block b) {
    if (c.y <= 0 || c.y >= world_height || std::abs(i64(c.x)) > 1000000 || std::abs(i64(c.z)) > 1000000 ||
        u8(b) >= u8(Block::Bedrock) || block(c) == b) return false;
    const Block base = generated(c);
    if (b == base) _edits.erase(c);
    else {
        if (!_edits.contains(c) && _edits.size() >= max_edits) return false;
        _edits[c] = b;
    }
    ++_revision;
    return true;
}
std::vector<u8> World::save() const {
    std::vector<u8> bytes;
    const auto put = [&](u64 v, int n) { for (int i = 0; i < n; ++i) bytes.push_back(u8(v >> (i * 8))); };
    put(0x444c4642, 4); put(1, 4); put(_seed, 8); put(_edits.size(), 4);
    for (const auto& [c, b] : _edits) { put(u32(c.x), 4); put(u32(c.y), 4); put(u32(c.z), 4); put(u8(b), 1); }
    return bytes;
}
bool World::restore(std::span<const u8> bytes) {
    if (bytes.size() < 20 || bytes.size() > 20 + max_edits * 13) return false;
    std::size_t at = 0;
    const auto get = [&](int n) { u64 v = 0; for (int i = 0; i < n; ++i) v |= u64(bytes[at++]) << (i * 8); return v; };
    if (get(4) != 0x444c4642 || get(4) != 1 || get(8) != _seed) return false;
    const u64 count = get(4);
    if (count > max_edits || bytes.size() != 20 + count * 13) return false;
    std::map<Cell, Block> edits;
    for (u64 i = 0; i < count; ++i) {
        Cell c{int(i32(get(4))), int(i32(get(4))), int(i32(get(4)))};
        const Block b = Block(get(1));
        if (c.y <= 0 || c.y >= world_height || u8(b) >= u8(Block::Bedrock) ||
            std::abs(i64(c.x)) > 1000000 || std::abs(i64(c.z)) > 1000000 || edits.contains(c)) return false;
        edits[c] = b;
    }
    _edits = std::move(edits); ++_revision;
    return true;
}
u64 World::hash() const {
    u64 value = 14695981039346656037ull;
    for (u8 b : save()) { value ^= b; value *= 1099511628211ull; }
    return value;
}
std::optional<BlockHit> raycast(const World& world, Vec3f origin, Vec3f direction, f32 reach) {
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        !std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z) ||
        std::abs(origin.x) > 1000000 || std::abs(origin.y) > 1000000 || std::abs(origin.z) > 1000000 ||
        length(direction) < 1e-6f || reach <= 0 || reach > 100) return {};
    direction = normalize(direction);
    std::array<int, 3> c{int(std::floor(origin.x)), int(std::floor(origin.y)), int(std::floor(origin.z))}, previous = c;
    const std::array<f32, 3> o{origin.x, origin.y, origin.z}, d{direction.x, direction.y, direction.z};
    std::array<f32, 3> next{}, delta{};
    std::array<int, 3> sign{};
    for (int i = 0; i < 3; ++i) {
        sign[i] = d[i] > 0 ? 1 : -1;
        delta[i] = d[i] == 0 ? std::numeric_limits<f32>::infinity() : std::abs(1 / d[i]);
        next[i] = d[i] == 0 ? delta[i] : (f32(c[i] + (d[i] > 0 ? 1 : 0)) - o[i]) / d[i];
    }
    f32 distance = 0;
    for (int steps = 0; steps < 400 && distance <= reach; ++steps) {
        const Block b = world.block(from(c));
        if (b != Block::Air) return BlockHit{from(c), from(previous), b, distance};
        const int axis = next[0] <= next[1] && next[0] <= next[2] ? 0 : next[1] <= next[2] ? 1 : 2;
        previous = c; c[axis] += sign[axis]; distance = next[axis]; next[axis] += delta[axis];
    }
    return {};
}

MeshData greedy_mesh(const std::function<Block(Cell)>& read, Cell dimensions) {
    MeshData mesh;
    const std::array<int, 3> size{dimensions.x, dimensions.y, dimensions.z};
    for (int axis = 0; axis < 3; ++axis) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        std::vector<int> mask(std::size_t(size[u] * size[v]));
        std::array<int, 3> p{};
        for (p[axis] = -1; p[axis] < size[axis]; ++p[axis]) {
            int n = 0;
            for (p[v] = 0; p[v] < size[v]; ++p[v]) for (p[u] = 0; p[u] < size[u]; ++p[u]) {
                auto q = p; ++q[axis];
                const Block a = read(from(p)), b = read(from(q));
                mask[n++] = a != Block::Air && b == Block::Air && p[axis] >= 0 ? int(a) :
                            a == Block::Air && b != Block::Air && q[axis] < size[axis] ? -int(b) : 0;
            }
            for (int j = 0; j < size[v]; ++j) for (int i = 0; i < size[u];) {
                const int start = j * size[u] + i, face = mask[start];
                if (!face) { ++i; continue; }
                int w = 1, h = 1;
                while (i + w < size[u] && mask[start + w] == face) ++w;
                bool grow = true;
                while (j + h < size[v] && grow) {
                    for (int k = 0; k < w; ++k) if (mask[start + h * size[u] + k] != face) { grow = false; break; }
                    if (grow) ++h;
                }
                std::array<int, 3> base{}, du{}, dv{};
                base[axis] = p[axis] + 1; base[u] = i; base[v] = j; du[u] = w; dv[v] = h;
                const Vec3f a = vec(base), b = a + vec(du), c = b + vec(dv), e = a + vec(dv);
                std::array<int, 3> normal{}; normal[axis] = face > 0 ? 1 : -1;
                const Color colour = block_colour(Block(std::abs(face)), axis, face > 0 ? 1 : -1);
                const u32 first = u32(mesh.vertices.size());
                for (const auto& [point, uv] : std::array<std::pair<Vec3f, Vec2f>, 4>{{{a, {0, 0}}, {b, {f32(w), 0}},
                       {c, {f32(w), f32(h)}}, {e, {0, f32(h)}}}})
                    mesh.vertices.push_back({point, vec(normal), uv, colour});
                if (face > 0) mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
                else mesh.indices.insert(mesh.indices.end(), {first, first + 2, first + 1, first, first + 3, first + 2});
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) mask[start + y * size[u] + x] = 0;
                i += w;
            }
        }
    }
    return mesh;
}
MeshData build_chunk(const World& world, ChunkKey key) {
    // One small halo snapshot makes meshing independent of loading order and avoids resampling terrain for every face.
    constexpr int side = chunk_size + 2, vertical = world_height + 2;
    std::vector<Block> voxels(side * side * vertical);
    const auto index = [](Cell c) { return (c.y + 1) * side * side + (c.z + 1) * side + c.x + 1; };
    for (int y = -1; y <= world_height; ++y) for (int z = -1; z <= chunk_size; ++z) for (int x = -1; x <= chunk_size; ++x)
        voxels[index({x, y, z})] = world.block({key.x * chunk_size + x, y, key.z * chunk_size + z});
    return greedy_mesh([&](Cell c) { return voxels[index(c)]; }, {chunk_size, world_height, chunk_size});
}
std::size_t mesh_bytes(const MeshData& mesh) { return mesh.vertices.size() * sizeof(MeshVertex) + mesh.indices.size() * sizeof(u32); }
} // namespace blockfield
