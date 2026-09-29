#include <kin/pathfinding/pathfinding.hpp>

#include <algorithm>
#include <limits>
#include <queue>

namespace kin {
namespace {

constexpr i32 default_step_cost = 10;
constexpr i32 inf = std::numeric_limits<i32>::max();

// Open-list entry. The comparison is a total order (priority, then deeper g,
// then cell index), so the search visits cells in the same order with every
// standard library, not just on every run.
struct Node {
    i32 index = 0;
    i32 g = 0;
    i32 priority = 0;
};

struct NodeGreater {
    bool operator()(const Node& a, const Node& b) const {
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        if (a.g != b.g) {
            return a.g < b.g;
        }
        return a.index > b.index;
    }
};

std::size_t cell_count(const HexGridNav& nav) {
    if (nav.cols <= 0 || nav.rows <= 0) {
        return 0;
    }
    const auto cols = static_cast<std::size_t>(nav.cols);
    const auto rows = static_cast<std::size_t>(nav.rows);
    if (rows > static_cast<std::size_t>(inf) / cols) {
        return 0;
    }
    return cols * rows;
}

i32 index_of(const HexGridNav& nav, Vec2i cell) {
    return cell.y * nav.cols + cell.x;
}

Vec2i cell_of(const HexGridNav& nav, i32 index) {
    return {index % nav.cols, index / nav.cols};
}

// Best cost to every cell, and each cell's predecessor, from `start`. Stops once
// `goal` (if in bounds) is settled or every cell within `budget` is.
struct Search {
    std::vector<i32> cost;
    std::vector<i32> came_from;
};

Search search(const HexGridNav& nav, Vec2i start, Vec2i goal, i32 budget, i32 heuristic_step) {
    const std::size_t count = cell_count(nav);
    Search s{std::vector<i32>(count, inf), std::vector<i32>(count, -1)};
    const bool has_goal = nav.in_bounds(goal);
    const Hex goal_hex = nav.layout.from_offset(goal);
    const auto heuristic = [&](Vec2i cell) {
        if (!has_goal || heuristic_step <= 0) {
            return 0;
        }
        const i64 h = static_cast<i64>(hex_distance(nav.layout.from_offset(cell), goal_hex)) * heuristic_step;
        return static_cast<i32>(std::min<i64>(h, inf / 2));
    };

    std::priority_queue<Node, std::vector<Node>, NodeGreater> open;
    const i32 start_index = index_of(nav, start);
    s.cost[static_cast<std::size_t>(start_index)] = 0;
    open.push({start_index, 0, heuristic(start)});
    const i32 goal_index = has_goal ? index_of(nav, goal) : -1;

    while (!open.empty()) {
        const Node current = open.top();
        open.pop();
        if (current.g != s.cost[static_cast<std::size_t>(current.index)]) {
            continue; // a cheaper entry for this cell was already expanded
        }
        if (current.index == goal_index) {
            break;
        }
        const Vec2i cell = cell_of(nav, current.index);
        for (const Vec2i next : nav.neighbors(cell)) {
            if (nav.is_blocked(next)) {
                continue;
            }
            const i32 step = nav.movement_cost(next);
            if (current.g > inf - step) {
                continue;
            }
            const i32 next_g = current.g + step;
            if (next_g > budget) {
                continue;
            }
            const i32 next_index = index_of(nav, next);
            if (next_g >= s.cost[static_cast<std::size_t>(next_index)]) {
                continue;
            }
            s.cost[static_cast<std::size_t>(next_index)] = next_g;
            s.came_from[static_cast<std::size_t>(next_index)] = current.index;
            const i32 h = heuristic(next);
            open.push({next_index, next_g, next_g > inf - h ? inf : next_g + h});
        }
    }
    return s;
}

bool valid_endpoint(const HexGridNav& nav, Vec2i cell) {
    return nav.in_bounds(cell) && !nav.is_blocked(cell);
}

} // namespace

bool HexGridNav::in_bounds(Vec2i cell) const {
    return cell.x >= 0 && cell.x < cols && cell.y >= 0 && cell.y < rows;
}

bool HexGridNav::is_blocked(Vec2i cell) const {
    return !in_bounds(cell) || (blocked && blocked(cell.x, cell.y));
}

i32 HexGridNav::movement_cost(Vec2i cell) const {
    return cost ? std::max(1, cost(cell.x, cell.y)) : default_step_cost;
}

std::vector<Vec2i> HexGridNav::neighbors(Vec2i cell) const {
    std::vector<Vec2i> out;
    out.reserve(6);
    const Hex h = layout.from_offset(cell);
    for (const Hex& dir : HexDirections) {
        const Vec2i next = layout.to_offset(h + dir);
        if (in_bounds(next)) {
            out.push_back(next);
        }
    }
    return out;
}

std::vector<Vec2i> find_path(const HexGridNav& nav, Vec2i start, Vec2i goal, HexPathOptions options) {
    if (cell_count(nav) == 0 || !valid_endpoint(nav, start) || !valid_endpoint(nav, goal)) {
        return {};
    }
    if (start == goal) {
        return {start};
    }
    const Search s = search(nav, start, goal, inf, options.min_step_cost);
    i32 index = index_of(nav, goal);
    if (s.cost[static_cast<std::size_t>(index)] == inf) {
        return {};
    }
    std::vector<Vec2i> path;
    const i32 start_index = index_of(nav, start);
    while (index != start_index) {
        path.push_back(cell_of(nav, index));
        index = s.came_from[static_cast<std::size_t>(index)];
    }
    path.push_back(start);
    std::ranges::reverse(path);
    return path;
}

std::vector<HexReach> reachable_cells(const HexGridNav& nav, Vec2i start, i32 budget) {
    if (cell_count(nav) == 0 || budget < 0 || !valid_endpoint(nav, start)) {
        return {};
    }
    // No goal: a plain uniform-cost search that stops at the budget.
    const Search s = search(nav, start, {-1, -1}, budget, 0);
    std::vector<HexReach> out;
    for (std::size_t i = 0; i < s.cost.size(); ++i) {
        if (s.cost[i] != inf) {
            out.push_back({cell_of(nav, static_cast<i32>(i)), s.cost[i]});
        }
    }
    std::ranges::sort(out, [](const HexReach& a, const HexReach& b) {
        if (a.cost != b.cost) {
            return a.cost < b.cost;
        }
        return a.cell.y != b.cell.y ? a.cell.y < b.cell.y : a.cell.x < b.cell.x;
    });
    return out;
}

} // namespace kin
