#pragma once

#include <kin/core/types.hpp>
#include <kin/tilemap/tilemap.hpp>

#include <functional>
#include <vector>

namespace kin {

struct GridNav {
    i32 cols = 0;
    i32 rows = 0;
    std::function<bool(i32 col, i32 row)> blocked;
    std::function<i32(i32 col, i32 row)> cost;

    bool in_bounds(Vec2i tile) const;
    bool is_blocked(Vec2i tile) const;
    i32 movement_cost(Vec2i tile) const;
};

struct GridPathOptions {
    bool allow_diagonal = true;
    bool prevent_corner_clipping = true;
};

// Returns tile coords from start to goal, inclusive.
// Returns empty when start/goal are out of bounds, blocked, or unreachable.
std::vector<Vec2i> find_path(const GridNav& nav, Vec2i start, Vec2i goal, GridPathOptions options = {});

// TileMap convenience wrapper. Treats any solid tile in any layer as blocked.
std::vector<Vec2i> find_path(const TileMap& map, Vec2i start, Vec2i goal, GridPathOptions options = {});

Vec2i world_to_tile(const TileMap& map, Vec2f world_pos);
Vec2f tile_center(const TileMap& map, Vec2i tile);

} // namespace kin
