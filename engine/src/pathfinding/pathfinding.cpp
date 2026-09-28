#include <kin/pathfinding/pathfinding.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <span>

namespace kin {
namespace {

struct Node {
    Vec2i tile{};
    i32 g = 0;
    i32 f = 0;
};

struct NodeGreater {
    bool operator()(const Node& a, const Node& b) const {
        if (a.f != b.f) {
            return a.f > b.f;
        }
        return a.g < b.g;
    }
};

struct DirEntry {
    Vec2i d{};
    i32 cost = 0;
};

constexpr i32 cost_cardinal = 10;
constexpr i32 cost_diagonal = 14;

i32 heuristic(Vec2i a, Vec2i b, GridPathOptions options) {
    const i32 dx = std::abs(a.x - b.x);
    const i32 dy = std::abs(a.y - b.y);
    if (!options.allow_diagonal) {
        return cost_cardinal * (dx + dy);
    }
    return cost_cardinal * std::max(dx, dy) + (cost_diagonal - cost_cardinal) * std::min(dx, dy);
}

std::size_t cell_count(const GridNav& nav) {
    if (nav.cols <= 0 || nav.rows <= 0) {
        return 0;
    }
    const auto cols = static_cast<std::size_t>(nav.cols);
    const auto rows = static_cast<std::size_t>(nav.rows);
    if (rows > std::numeric_limits<std::size_t>::max() / cols) {
        return 0;
    }
    return cols * rows;
}

std::size_t index_of(const GridNav& nav, Vec2i tile) {
    return static_cast<std::size_t>(tile.y) * static_cast<std::size_t>(nav.cols) +
           static_cast<std::size_t>(tile.x);
}

std::vector<Vec2i> reconstruct_path(const std::vector<Vec2i>& came_from,
                                    const GridNav& nav,
                                    Vec2i start,
                                    Vec2i goal) {
    std::vector<Vec2i> path;
    Vec2i current = goal;
    path.push_back(current);

    while (current != start) {
        const Vec2i previous = came_from[index_of(nav, current)];
        if (previous.x < 0 || previous.y < 0) {
            return {};
        }
        current = previous;
        path.push_back(current);
    }

    std::ranges::reverse(path);
    return path;
}

std::span<const DirEntry> directions(GridPathOptions options) {
    static constexpr std::array<DirEntry, 4> cardinal{{
        {{1, 0}, cost_cardinal},
        {{-1, 0}, cost_cardinal},
        {{0, 1}, cost_cardinal},
        {{0, -1}, cost_cardinal},
    }};
    static constexpr std::array<DirEntry, 8> diagonal{{
        {{1, 0}, cost_cardinal},
        {{-1, 0}, cost_cardinal},
        {{0, 1}, cost_cardinal},
        {{0, -1}, cost_cardinal},
        {{1, 1}, cost_diagonal},
        {{1, -1}, cost_diagonal},
        {{-1, 1}, cost_diagonal},
        {{-1, -1}, cost_diagonal},
    }};
    return options.allow_diagonal ? std::span<const DirEntry>{diagonal} : std::span<const DirEntry>{cardinal};
}

bool diagonal_blocked_by_corner(const GridNav& nav, Vec2i current, Vec2i delta, GridPathOptions options) {
    if (!options.prevent_corner_clipping || delta.x == 0 || delta.y == 0) {
        return false;
    }
    return nav.is_blocked({current.x + delta.x, current.y}) ||
           nav.is_blocked({current.x, current.y + delta.y});
}

} // namespace

bool GridNav::in_bounds(Vec2i tile) const {
    return tile.x >= 0 && tile.x < cols && tile.y >= 0 && tile.y < rows;
}

bool GridNav::is_blocked(Vec2i tile) const {
    return !in_bounds(tile) || (blocked && blocked(tile.x, tile.y));
}

i32 GridNav::movement_cost(Vec2i tile) const {
    return cost ? std::max(1, cost(tile.x, tile.y)) : cost_cardinal;
}

std::vector<Vec2i> find_path(const GridNav& nav, Vec2i start, Vec2i goal, GridPathOptions options) {
    const std::size_t count = cell_count(nav);
    if (count == 0 || count > static_cast<std::size_t>(std::numeric_limits<i32>::max())) {
        return {};
    }
    if (!nav.in_bounds(start) || !nav.in_bounds(goal) || nav.is_blocked(start) || nav.is_blocked(goal)) {
        return {};
    }
    if (start == goal) {
        return {start};
    }

    constexpr i32 inf = std::numeric_limits<i32>::max();
    std::vector<i32> best_cost(count, inf);
    std::vector<Vec2i> came_from(count, {-1, -1});
    std::priority_queue<Node, std::vector<Node>, NodeGreater> open;

    best_cost[index_of(nav, start)] = 0;
    open.push({
        .tile = start,
        .g = 0,
        .f = heuristic(start, goal, options),
    });

    while (!open.empty()) {
        const Node current = open.top();
        open.pop();

        if (current.tile == goal) {
            return reconstruct_path(came_from, nav, start, goal);
        }

        const std::size_t current_index = index_of(nav, current.tile);
        if (current.g != best_cost[current_index]) {
            continue;
        }

        for (const DirEntry& dir : directions(options)) {
            const Vec2i next{current.tile.x + dir.d.x, current.tile.y + dir.d.y};
            if (nav.is_blocked(next) || diagonal_blocked_by_corner(nav, current.tile, dir.d, options)) {
                continue;
            }

            const i32 move_cost = nav.movement_cost(next);
            if (move_cost > std::numeric_limits<i32>::max() / dir.cost) {
                continue;
            }
            const i32 step_cost = std::max(1, (dir.cost * move_cost) / cost_cardinal);
            if (current.g > std::numeric_limits<i32>::max() - step_cost) {
                continue;
            }
            const i32 next_g = current.g + step_cost;
            const std::size_t next_index = index_of(nav, next);
            if (next_g >= best_cost[next_index]) {
                continue;
            }

            best_cost[next_index] = next_g;
            came_from[next_index] = current.tile;
            open.push({
                .tile = next,
                .g = next_g,
                .f = next_g + heuristic(next, goal, options),
            });
        }
    }

    return {};
}

std::vector<Vec2i> find_path(const TileMap& map, Vec2i start, Vec2i goal, GridPathOptions options) {
    GridNav nav{
        .cols = map.cols,
        .rows = map.rows,
        .blocked = [&map](i32 col, i32 row) {
            return !map.walkable(col, row);
        },
        .cost = [&map](i32 col, i32 row) {
            return map.movement_cost(col, row);
        },
    };
    return find_path(nav, start, goal, options);
}

Vec2i world_to_tile(const TileMap& map, Vec2f world_pos) {
    return {map.world_col(world_pos.x), map.world_row(world_pos.y)};
}

Vec2f tile_center(const TileMap& map, Vec2i tile) {
    return {
        (static_cast<f32>(tile.x) + 0.5f) * static_cast<f32>(map.tile_w),
        (static_cast<f32>(tile.y) + 0.5f) * static_cast<f32>(map.tile_h),
    };
}

} // namespace kin
