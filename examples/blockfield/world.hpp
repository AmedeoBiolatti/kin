#pragma once

#include <kin/render3d/mesh.hpp>

#include <compare>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace blockfield {
using namespace kin;
inline constexpr int chunk_size = 16, world_height = 64;
inline constexpr std::size_t max_edits = 16384;
enum class Block : u8 { Air, Grass, Dirt, Stone, Sand, Wood, Leaves, Bedrock };
struct Cell { int x = 0, y = 0, z = 0; auto operator<=>(const Cell&) const = default; };
struct ChunkKey { int x = 0, z = 0; auto operator<=>(const ChunkKey&) const = default; };
int floor_div(int value, int divisor);
ChunkKey chunk_of(Cell cell);
const char* block_name(Block block);
Color block_colour(Block block, int axis = 1, int sign = 1);

// Seeded terrain plus bounded sparse edits. Generated blocks need no entities, bodies, textures or heap allocations.
class World {
public:
    explicit World(u64 seed = 1) : _seed(seed) {}
    u64 seed() const { return _seed; }
    int height(int x, int z) const;
    Block generated(Cell cell) const;
    Block block(Cell cell) const;
    bool set(Cell cell, Block value);
    std::size_t edits() const { return _edits.size(); }
    u64 revision() const { return _revision; }
    std::vector<u8> save() const;
    bool restore(std::span<const u8> bytes);
    u64 hash() const;
private:
    u64 _seed;
    u64 _revision = 0;
    std::map<Cell, Block> _edits;
};

struct BlockHit { Cell cell, previous; Block block = Block::Air; f32 distance = 0; };
std::optional<BlockHit> raycast(const World& world, Vec3f origin, Vec3f direction, f32 reach = 6);

// Positive/negative face masks and greedy rectangles eliminate hidden faces and merge coplanar faces of the same block.
// The callback includes neighboring chunks: seams use exactly the same data as collision, mining and rendering.
MeshData greedy_mesh(const std::function<Block(Cell)>& read, Cell dimensions);
MeshData build_chunk(const World& world, ChunkKey key);
std::size_t mesh_bytes(const MeshData& mesh);
} // namespace blockfield
