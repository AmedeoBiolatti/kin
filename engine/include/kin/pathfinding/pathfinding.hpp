#pragma once

#include <kin/core/hex.hpp>
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

// A hex grid stored as a cols x rows rectangle of offset cells (col, row), laid
// out as `layout` says (only its orientation and offset are used). The callbacks
// work as in GridNav; `cost` is the cost of entering a cell, 10 when unset.
struct HexGridNav {
    i32 cols = 0;
    i32 rows = 0;
    HexLayout layout{};
    std::function<bool(i32 col, i32 row)> blocked;
    std::function<i32(i32 col, i32 row)> cost;

    bool in_bounds(Vec2i cell) const;
    bool is_blocked(Vec2i cell) const;
    i32 movement_cost(Vec2i cell) const;
    // The in-bounds neighbours of `cell`, in HexDirections order.
    std::vector<Vec2i> neighbors(Vec2i cell) const;
};

struct HexPathOptions {
    // The A* heuristic assumes no step costs less than this. Lower it when some
    // cells cost less than 10, or the path may not be the cheapest.
    i32 min_step_cost = 10;
};

// Cheapest path between two offset cells, both included. Empty when start or
// goal is out of bounds or blocked, or the goal is unreachable. Ties between
// equally cheap paths are broken the same way on every run.
std::vector<Vec2i> find_path(const HexGridNav& nav, Vec2i start, Vec2i goal, HexPathOptions options = {});

struct HexReach {
    Vec2i cell{};
    i32 cost = 0; // cheapest total cost to enter `cell` from the start
};

// Every cell reachable from `start` for at most `budget` in total cost (a unit's
// movement range), start included at cost 0. Ordered by cost, then row, then col.
std::vector<HexReach> reachable_cells(const HexGridNav& nav, Vec2i start, i32 budget);

} // namespace kin
