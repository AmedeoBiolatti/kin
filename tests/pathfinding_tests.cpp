#include <kin/pathfinding/pathfinding.hpp>

#include <cassert>

namespace {

kin::TileMap make_map(kin::i32 cols, kin::i32 rows) {
    kin::TileMap map;
    map.resize(cols, rows, 0);
    map.tileset.resize(2);
    map.tileset.tiles[1] = {
        .id = "wall",
        .solid = true,
    };
    return map;
}

void test_straight_path() {
    kin::TileMap map = make_map(5, 1);
    const auto path = kin::find_path(map, {0, 0}, {4, 0});
    assert(path.size() == 5);
    assert((path.front() == kin::Vec2i{0, 0}));
    assert((path.back() == kin::Vec2i{4, 0}));
}

void test_blocked_and_out_of_bounds() {
    kin::TileMap map = make_map(3, 1);
    map.set(1, 0, 1);
    assert(kin::find_path(map, {0, 0}, {2, 0}).empty());
    assert(kin::find_path(map, {-1, 0}, {2, 0}).empty());
    assert(kin::find_path(map, {0, 0}, {99, 0}).empty());
    map.set(0, 0, 1);
    assert(kin::find_path(map, {0, 0}, {2, 0}).empty());
}

void test_start_equals_goal() {
    kin::TileMap map = make_map(3, 3);
    const auto path = kin::find_path(map, {1, 1}, {1, 1});
    assert(path.size() == 1);
    assert((path[0] == kin::Vec2i{1, 1}));
}

void test_four_way_path_around_obstacle() {
    kin::TileMap map = make_map(3, 3);
    map.set(1, 0, 1);
    const auto path = kin::find_path(map, {0, 0}, {2, 0}, {.allow_diagonal = false});
    assert(!path.empty());
    assert((path.front() == kin::Vec2i{0, 0}));
    assert((path.back() == kin::Vec2i{2, 0}));
    for (kin::Vec2i tile : path) {
        assert((tile != kin::Vec2i{1, 0}));
    }
}

void test_diagonal_path_is_shorter_when_allowed() {
    kin::TileMap map = make_map(4, 4);
    const auto diagonal = kin::find_path(map, {0, 0}, {3, 3});
    const auto cardinal = kin::find_path(map, {0, 0}, {3, 3}, {.allow_diagonal = false});
    assert(diagonal.size() == 4);
    assert(cardinal.size() == 7);
}

void test_corner_clipping_can_be_prevented() {
    kin::TileMap map = make_map(3, 3);
    map.set(1, 0, 1);
    map.set(0, 1, 1);

    const auto clipped = kin::find_path(map, {0, 0}, {1, 1}, {.prevent_corner_clipping = false});
    assert(clipped.size() == 2);

    const auto blocked = kin::find_path(map, {0, 0}, {1, 1});
    assert(blocked.empty());
}

void test_grid_nav_callback() {
    bool blocked_tiles[9] = {};
    blocked_tiles[1] = true;
    kin::GridNav nav{
        .cols = 3,
        .rows = 3,
        .blocked = [&blocked_tiles](kin::i32 col, kin::i32 row) {
            return blocked_tiles[row * 3 + col];
        },
    };

    const auto path = kin::find_path(nav, {0, 0}, {2, 0}, {.allow_diagonal = false});
    assert(!path.empty());
    assert((path.front() == kin::Vec2i{0, 0}));
    assert((path.back() == kin::Vec2i{2, 0}));
}

void test_tile_costs_affect_path_choice() {
    kin::TileMap map;
    map.resize(3, 2);
    map.tileset.resize(2);
    map.tileset.tiles[1] = {
        .id = "mud",
        .solid = false,
        .cost = 100,
    };
    map.set(1, 0, 1);

    const auto path = kin::find_path(map, {0, 0}, {2, 0}, {.allow_diagonal = false});
    assert(path.size() == 5);
    assert((path[0] == kin::Vec2i{0, 0}));
    assert((path[1] == kin::Vec2i{0, 1}));
    assert((path[2] == kin::Vec2i{1, 1}));
    assert((path[3] == kin::Vec2i{2, 1}));
    assert((path[4] == kin::Vec2i{2, 0}));
}

void test_world_helpers() {
    kin::TileMap map = make_map(4, 4);
    map.tile_w = 16;
    map.tile_h = 8;
    assert((kin::world_to_tile(map, {24.0f, 9.0f}) == kin::Vec2i{1, 1}));
    assert((kin::tile_center(map, {2, 1}) == kin::Vec2f{40.0f, 12.0f}));
}

} // namespace

int main() {
    test_straight_path();
    test_blocked_and_out_of_bounds();
    test_start_equals_goal();
    test_four_way_path_around_obstacle();
    test_diagonal_path_is_shorter_when_allowed();
    test_corner_clipping_can_be_prevented();
    test_grid_nav_callback();
    test_tile_costs_affect_path_choice();
    test_world_helpers();
    return 0;
}
