#include <kin/core/hex.hpp>
#include <kin/pathfinding/pathfinding.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace {

using kin::Hex;
using kin::HexLayout;
using kin::HexOffset;
using kin::HexOrientation;
using kin::Vec2f;
using kin::Vec2i;

constexpr std::array<HexLayout, 4> all_layouts() {
    return {{
        {.orientation = HexOrientation::PointyTop, .offset = HexOffset::Odd},
        {.orientation = HexOrientation::PointyTop, .offset = HexOffset::Even},
        {.orientation = HexOrientation::FlatTop, .offset = HexOffset::Odd},
        {.orientation = HexOrientation::FlatTop, .offset = HexOffset::Even},
    }};
}

bool near(Vec2f a, Vec2f b, float eps = 1e-3f) {
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps;
}

std::uint64_t splitmix(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// The axial basics are constexpr; check them at compile time too.
static_assert(kin::hex_distance({0, 0}, {3, -1}) == 3);
static_assert(kin::hex_neighbor({0, 0}, 3) == Hex{-1, 0});
static_assert(kin::hex_neighbor({0, 0}, -1) == Hex{0, 1});
static_assert(kin::hex_rotate_cw(Hex{1, 0}) == Hex{0, 1});

void test_axial_basics() {
    const Hex h{2, -3};
    assert(h.s() == 1);
    for (int d = 0; d < 6; ++d) {
        const Hex n = kin::hex_neighbor(h, d);
        assert(kin::hex_distance(h, n) == 1);
        // The opposite direction steps straight back.
        assert(kin::hex_neighbor(n, d + 3) == h);
        // Rotating a direction clockwise gives the previous index.
        assert(kin::hex_rotate_cw(kin::HexDirections[static_cast<std::size_t>(d)]) ==
               kin::HexDirections[static_cast<std::size_t>(kin::hex_wrap_direction(d - 1))]);
    }
    const std::array<Hex, 6> ns = kin::hex_neighbors(h);
    std::set<std::pair<int, int>> distinct;
    for (const Hex& n : ns) {
        distinct.insert({n.q, n.r});
    }
    assert(distinct.size() == 6);

    Hex spun = h;
    for (int i = 0; i < 6; ++i) {
        spun = kin::hex_rotate_ccw(spun);
        assert(kin::hex_distance({0, 0}, spun) == kin::hex_distance({0, 0}, h));
    }
    assert(spun == h);
    assert(kin::hex_rotate_ccw(kin::hex_rotate_cw(h)) == h);
}

void test_rings_ranges_lines() {
    const Hex c{-4, 7};
    assert(kin::hex_ring(c, 0) == std::vector<Hex>{c});
    assert(kin::hex_ring(c, -1).empty());
    for (int radius = 1; radius <= 5; ++radius) {
        const std::vector<Hex> ring = kin::hex_ring(c, radius);
        assert(static_cast<int>(ring.size()) == 6 * radius);
        for (std::size_t i = 0; i < ring.size(); ++i) {
            assert(kin::hex_distance(c, ring[i]) == radius);
            // Consecutive ring cells are neighbours, and the ring closes.
            assert(kin::hex_distance(ring[i], ring[(i + 1) % ring.size()]) == 1);
        }
        const std::vector<Hex> range = kin::hex_range(c, radius);
        assert(static_cast<int>(range.size()) == 3 * radius * (radius + 1) + 1);
        for (const Hex& h : range) {
            assert(kin::hex_distance(c, h) <= radius);
        }
    }

    const Hex a{0, 0};
    const Hex b{5, -2};
    const std::vector<Hex> line = kin::hex_line(a, b);
    assert(line.size() == 6);
    assert(line.front() == a && line.back() == b);
    for (std::size_t i = 1; i < line.size(); ++i) {
        assert(kin::hex_distance(line[i - 1], line[i]) == 1);
    }
    assert(kin::hex_line(b, b) == std::vector<Hex>{b});
}

void test_rounding() {
    assert((kin::hex_round(0.1f, -0.2f) == Hex{0, 0}));
    assert((kin::hex_round(2.9f, -1.1f) == Hex{3, -1}));
    assert((kin::hex_round(-1.6f, 0.7f) == Hex{-2, 1}));
    // Whatever point is rounded, the result is a valid hex nearby.
    for (float q = -3.0f; q <= 3.0f; q += 0.37f) {
        for (float r = -3.0f; r <= 3.0f; r += 0.41f) {
            const Hex h = kin::hex_round(q, r);
            assert(std::abs(static_cast<float>(h.q) - q) <= 1.0f);
            assert(std::abs(static_cast<float>(h.r) - r) <= 1.0f);
        }
    }
}

void test_layout_pixels() {
    for (HexLayout layout : all_layouts()) {
        layout.size = {20.0f, 14.0f}; // squashed, to catch mixed-up axes
        layout.origin = {300.0f, -40.0f};
        assert(near(layout.to_pixel({0, 0}), layout.origin));
        for (const Hex& h : kin::hex_range({1, -2}, 4)) {
            const Vec2f center = layout.to_pixel(h);
            assert(layout.from_pixel(center) == h);
            // Points well inside the hex, towards each corner, stay in it.
            for (const Vec2f& corner : layout.corners(h)) {
                const Vec2f inside{center.x + (corner.x - center.x) * 0.8f,
                                   center.y + (corner.y - center.y) * 0.8f};
                assert(layout.from_pixel(inside) == h);
            }
            // Neighbours' centres are one hex apart and share two corners.
            for (int d = 0; d < 6; ++d) {
                const Hex n = kin::hex_neighbor(h, d);
                int shared = 0;
                for (const Vec2f& a : layout.corners(h)) {
                    for (const Vec2f& b : layout.corners(n)) {
                        shared += near(a, b, 1e-2f) ? 1 : 0;
                    }
                }
                assert(shared == 2);
            }
        }
    }

    const HexLayout pointy{.orientation = HexOrientation::PointyTop, .size = {10.0f, 10.0f}};
    // Pointy-top: direction 0 is east, corner 0 is 30 degrees below +x.
    assert(near(pointy.to_pixel(kin::HexDirections[0]), {10.0f * std::sqrt(3.0f), 0.0f}));
    assert(near(pointy.corner_offset(0), {10.0f * std::sqrt(3.0f) / 2.0f, 5.0f}));
    assert(near(pointy.hex_extent(), {10.0f * std::sqrt(3.0f), 20.0f}));
    const HexLayout flat{.orientation = HexOrientation::FlatTop, .size = {10.0f, 10.0f}};
    // Flat-top: direction 2 is straight up, corner 0 is on +x.
    assert(near(flat.to_pixel(kin::HexDirections[2]), {0.0f, -10.0f * std::sqrt(3.0f)}));
    assert(near(flat.corner_offset(0), {10.0f, 0.0f}));
    assert(near(flat.hex_extent(), {20.0f, 10.0f * std::sqrt(3.0f)}));
}

void test_offset_coordinates() {
    for (const HexLayout& layout : all_layouts()) {
        for (int row = -5; row <= 5; ++row) {
            for (int col = -5; col <= 5; ++col) {
                const Vec2i cell{col, row};
                assert(layout.to_offset(layout.from_offset(cell)) == cell);
            }
        }
    }
    // Odd rows (pointy) / columns (flat) sit half a hex right / down with Odd,
    // so their neighbour "up-left" / "up-right" keeps the same column / row.
    const HexLayout odd_r{.orientation = HexOrientation::PointyTop, .offset = HexOffset::Odd};
    const HexLayout even_r{.orientation = HexOrientation::PointyTop, .offset = HexOffset::Even};
    assert((odd_r.to_offset(kin::hex_neighbor(odd_r.from_offset({3, 1}), 2)) == Vec2i{3, 0}));  // NW
    assert((even_r.to_offset(kin::hex_neighbor(even_r.from_offset({3, 1}), 1)) == Vec2i{3, 0})); // NE
    const HexLayout odd_q{.orientation = HexOrientation::FlatTop, .offset = HexOffset::Odd};
    const HexLayout even_q{.orientation = HexOrientation::FlatTop, .offset = HexOffset::Even};
    assert((odd_q.to_offset(kin::hex_neighbor(odd_q.from_offset({1, 3}), 3)) == Vec2i{0, 3}));  // NW
    assert((even_q.to_offset(kin::hex_neighbor(even_q.from_offset({1, 3}), 4)) == Vec2i{0, 3})); // SW
    // A rectangle of offset cells is laid out as a rectangle on screen.
    for (const HexLayout& layout : all_layouts()) {
        const Vec2f top_left = layout.to_pixel(layout.from_offset({0, 0}));
        const Vec2f next_row = layout.to_pixel(layout.from_offset({0, 2}));
        const Vec2f next_col = layout.to_pixel(layout.from_offset({2, 0}));
        assert(std::abs(next_row.x - top_left.x) < 1e-3f);
        assert(std::abs(next_col.y - top_left.y) < 1e-3f);
    }
}

kin::HexGridNav open_nav(HexLayout layout, int cols, int rows) {
    return {.cols = cols, .rows = rows, .layout = layout};
}

void test_neighbors_on_grid() {
    for (const HexLayout& layout : all_layouts()) {
        const kin::HexGridNav nav = open_nav(layout, 6, 5);
        assert(nav.neighbors({2, 2}).size() == 6);
        assert(nav.neighbors({0, 0}).size() <= 3);
        for (int row = 0; row < nav.rows; ++row) {
            for (int col = 0; col < nav.cols; ++col) {
                for (const Vec2i n : nav.neighbors({col, row})) {
                    assert(nav.in_bounds(n));
                    assert(kin::hex_distance(layout.from_offset({col, row}), layout.from_offset(n)) == 1);
                }
            }
        }
    }
}

void test_paths() {
    const HexLayout layout{};
    kin::HexGridNav nav = open_nav(layout, 9, 7);
    const std::vector<Vec2i> straight = kin::find_path(nav, {0, 3}, {8, 3});
    assert(straight.size() == 9);
    assert(straight.front() == (Vec2i{0, 3}) && straight.back() == (Vec2i{8, 3}));
    assert((kin::find_path(nav, {4, 4}, {4, 4}) == std::vector<Vec2i>{{4, 4}}));

    // A wall down column 4 with a gap at the bottom: the path goes through it.
    nav.blocked = [](int col, int row) { return col == 4 && row < 6; };
    const std::vector<Vec2i> around = kin::find_path(nav, {0, 0}, {8, 0});
    assert(!around.empty());
    assert((std::ranges::find(around, Vec2i{4, 6}) != around.end()));
    for (std::size_t i = 1; i < around.size(); ++i) {
        assert(!nav.is_blocked(around[i]));
        assert(kin::hex_distance(layout.from_offset(around[i - 1]), layout.from_offset(around[i])) == 1);
    }

    // Closing the gap leaves no path; blocked or out-of-bounds ends give none either.
    nav.blocked = [](int col, int) { return col == 4; };
    assert(kin::find_path(nav, {0, 0}, {8, 0}).empty());
    assert(kin::find_path(nav, {4, 2}, {0, 0}).empty());
    assert(kin::find_path(nav, {0, 0}, {9, 0}).empty());
    assert(kin::find_path(kin::HexGridNav{}, {0, 0}, {0, 0}).empty());

    // Expensive cells are avoided when a cheaper detour exists.
    nav.blocked = {};
    nav.cost = [](int col, int row) { return row == 3 && col > 0 && col < 8 ? 100 : 10; };
    const std::vector<Vec2i> detour = kin::find_path(nav, {0, 3}, {8, 3});
    for (std::size_t i = 1; i + 1 < detour.size(); ++i) {
        assert(detour[i].y != 3);
    }
}

void test_reachable_cells() {
    const HexLayout layout{.orientation = HexOrientation::FlatTop};
    kin::HexGridNav nav = open_nav(layout, 15, 15);
    const Vec2i start{7, 7};
    const std::vector<kin::HexReach> reach = kin::reachable_cells(nav, start, 30);
    // On open ground with cost 10 per step, a budget of 30 is a radius-3 hex.
    assert(reach.size() == 37);
    assert(reach.front().cell == start && reach.front().cost == 0);
    for (std::size_t i = 0; i < reach.size(); ++i) {
        const int steps = kin::hex_distance(layout.from_offset(start), layout.from_offset(reach[i].cell));
        assert(reach[i].cost == steps * 10);
        if (i > 0) {
            assert(reach[i - 1].cost <= reach[i].cost);
        }
    }
    // Rough ground shrinks the range; a blocked start or negative budget gives nothing.
    nav.cost = [](int, int) { return 15; };
    assert(kin::reachable_cells(nav, start, 30).size() == 19);
    nav.blocked = [start](int col, int row) { return col == start.x && row == start.y; };
    assert(kin::reachable_cells(nav, start, 30).empty());
    nav.blocked = {};
    assert(kin::reachable_cells(nav, start, -1).empty());
    assert(kin::reachable_cells(nav, start, 0).size() == 1);
}

// A* must find a valid path whose cost is exactly the optimum a plain
// uniform-cost search (reachable_cells) finds, on many random maps.
void test_paths_match_uniform_cost_search() {
    std::uint64_t seed = 12345;
    for (int trial = 0; trial < 200; ++trial) {
        const HexLayout layout = all_layouts()[static_cast<std::size_t>(trial % 4)];
        const int cols = 4 + static_cast<int>(splitmix(seed) % 14);
        const int rows = 4 + static_cast<int>(splitmix(seed) % 14);
        std::vector<int> cost(static_cast<std::size_t>(cols * rows));
        for (int& c : cost) {
            const std::uint64_t roll = splitmix(seed) % 10;
            c = roll < 2 ? 0 : 10 + static_cast<int>(roll) * 5; // 0 = blocked
        }
        kin::HexGridNav nav{
            .cols = cols,
            .rows = rows,
            .layout = layout,
            .blocked = [&](int col, int row) { return cost[static_cast<std::size_t>(row * cols + col)] == 0; },
            .cost = [&](int col, int row) { return cost[static_cast<std::size_t>(row * cols + col)]; },
        };
        const Vec2i start{static_cast<int>(splitmix(seed) % static_cast<std::uint64_t>(cols)),
                          static_cast<int>(splitmix(seed) % static_cast<std::uint64_t>(rows))};
        const Vec2i goal{static_cast<int>(splitmix(seed) % static_cast<std::uint64_t>(cols)),
                         static_cast<int>(splitmix(seed) % static_cast<std::uint64_t>(rows))};
        const std::vector<Vec2i> path = kin::find_path(nav, start, goal);
        assert(path == kin::find_path(nav, start, goal)); // same answer every time

        if (nav.is_blocked(start) || nav.is_blocked(goal)) {
            assert(path.empty());
            continue;
        }
        const std::vector<kin::HexReach> all = kin::reachable_cells(nav, start, 1'000'000);
        const auto it = std::ranges::find_if(all, [&](const kin::HexReach& r) { return r.cell == goal; });
        if (it == all.end()) {
            assert(path.empty());
            continue;
        }
        assert(!path.empty() && path.front() == start && path.back() == goal);
        int total = 0;
        for (std::size_t i = 1; i < path.size(); ++i) {
            assert(!nav.is_blocked(path[i]));
            assert(kin::hex_distance(layout.from_offset(path[i - 1]), layout.from_offset(path[i])) == 1);
            total += nav.movement_cost(path[i]);
        }
        assert(total == it->cost);
    }
}

} // namespace

int main() {
    test_axial_basics();
    test_rings_ranges_lines();
    test_rounding();
    test_layout_pixels();
    test_offset_coordinates();
    test_neighbors_on_grid();
    test_paths();
    test_reachable_cells();
    test_paths_match_uniform_cost_search();
    return 0;
}
