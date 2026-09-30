#include <kin/tilemap/tilemap.hpp>

#include <algorithm>
#include <cassert>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

template <typename T, typename = void>
struct has_render_cache_valid : std::false_type {};

template <typename T>
struct has_render_cache_valid<T, std::void_t<decltype(std::declval<T>().render_cache_valid)>> : std::true_type {};

template <typename T, typename = void>
struct has_render_cache_chunks : std::false_type {};

template <typename T>
struct has_render_cache_chunks<T, std::void_t<decltype(std::declval<T>().render_cache_chunks)>> : std::true_type {};

static_assert(!has_render_cache_valid<kin::TileLayer>::value);
static_assert(!has_render_cache_chunks<kin::TileLayer>::value);

class FakeTextureBackend final : public kin::ITextureBackend {
public:
    explicit FakeTextureBackend(kin::Vec2i size)
        : _size(size) {
    }

    kin::Vec2i size() const override { return _size; }

private:
    kin::Vec2i _size{};
};

class FakeBackend final : public kin::IRenderer2DBackend {
public:
    std::string_view name() const override { return "fake"; }
    void clear(kin::Color) override {}
    void present() override {}
    void set_logical_size(kin::Vec2i) override {}
    void set_integer_logical_size(kin::Vec2i) override {}
    kin::Vec2i output_size() const override { return {320, 180}; }
    kin::Vec2f window_to_logical(kin::Vec2f value) const override { return value; }
    kin::Vec2f logical_to_window(kin::Vec2f value) const override { return value; }
    kin::Texture create_texture_from_rgba(const kin::u8*, kin::Vec2i size) override {
        return kin::Texture{std::make_shared<FakeTextureBackend>(size)};
    }
    void draw_texture(const kin::Texture&, kin::Rectf dest) override {
        rects.push_back(dest);
        commands.push_back("texture");
    }
    void draw_texture(const kin::Texture&, kin::Rectf, kin::Rectf dest) override {
        rects.push_back(dest);
        commands.push_back("sprite");
    }
    void fill_rect(kin::Rectf rect, kin::Color color) override {
        rects.push_back(rect);
        colors.push_back(color);
        commands.push_back("fill");
    }
    void draw_rect(kin::Rectf rect, kin::Color color) override {
        rects.push_back(rect);
        colors.push_back(color);
        commands.push_back("rect");
    }
    void draw_line(kin::Vec2f, kin::Vec2f, kin::Color color) override {
        colors.push_back(color);
        commands.push_back("line");
    }
    void set_viewport(kin::Rectf) override {}
    void reset_viewport() override {}
    void push_viewport(kin::Rectf rect) override {
        rects.push_back(rect);
        commands.push_back("push");
    }
    void pop_viewport() override { commands.push_back("pop"); }

    std::vector<std::string> commands;
    std::vector<kin::Rectf> rects;
    std::vector<kin::Color> colors;
};

kin::TileMap make_test_map() {
    kin::TileMap map;
    map.resize(4, 3);
    map.tile_w = 8;
    map.tile_h = 10;
    map.tileset.resize(3);
    map.tileset.tiles[1] = {
        .id = "floor",
        .solid = false,
        .color = kin::Color::rgb(20, 30, 40),
        .top_edge_color = kin::Color::rgb(25, 35, 45),
    };
    map.tileset.tiles[2] = {
        .id = "wall",
        .solid = true,
        .color = kin::Color::rgb(100, 110, 120),
        .top_edge_color = kin::Color::rgb(130, 140, 150),
    };
    map.set(1, 1, 1);
    map.set(2, 1, 2);
    return map;
}

bool render_key_eq(kin::RenderKey a, kin::RenderKey b) {
    return a.layer == b.layer &&
           a.order == b.order &&
           a.y == b.y &&
           a.use_y == b.use_y &&
           a.pass_mask == b.pass_mask;
}

bool render_command_eq(const kin::RenderCommand& a, const kin::RenderCommand& b) {
    return a.type == b.type &&
           render_key_eq(a.key, b.key) &&
           a.rect == b.rect &&
           a.color == b.color &&
           a.source == b.source &&
           a.texture.valid() == b.texture.valid();
}

std::vector<kin::RenderCommand> sorted_render_commands(std::span<const kin::RenderCommand> commands) {
    std::vector<kin::RenderCommand> sorted{commands.begin(), commands.end()};
    std::ranges::sort(sorted, [](const kin::RenderCommand& a, const kin::RenderCommand& b) {
        if (a.rect.y != b.rect.y) {
            return a.rect.y < b.rect.y;
        }
        if (a.rect.x != b.rect.x) {
            return a.rect.x < b.rect.x;
        }
        return static_cast<int>(a.type) < static_cast<int>(b.type);
    });
    return sorted;
}

kin::TileMap make_cache_test_map() {
    kin::TileMap map;
    map.resize(4, 4);
    map.tile_w = 8;
    map.tile_h = 10;
    map.tileset.resize(3);
    map.tileset.tiles[1] = {
        .id = "floor",
        .color = kin::Color::rgb(10, 20, 30),
        .top_edge_color = kin::Color::rgb(15, 25, 35),
    };
    map.tileset.tiles[2] = {
        .id = "wall",
        .solid = true,
        .color = kin::Color::rgb(110, 120, 130),
        .top_edge_color = kin::Color::rgb(140, 150, 160),
    };
    map.fill_rect({0, 0, 4, 4}, 1);
    map.clear_dirty();
    map.set_render_chunk_size(2, 2);
    return map;
}

void test_basic_grid_and_solidity() {
    kin::TileMap map = make_test_map();
    assert(map.cols == 4);
    assert(map.rows == 3);
    assert(map.get(1, 1) == 1);
    assert(map.get(2, 1) == 2);
    assert(map.get(-1, 0) == 0);
    assert(!map.solid(1, 1));
    assert(map.solid(2, 1));
    assert(map.world_col(17.0f) == 2);
    assert(map.world_row(21.0f) == 2);
    assert((map.world_size() == kin::Vec2f{32.0f, 30.0f}));
}

void test_layers_and_roundtrip() {
    kin::TileMap map = make_test_map();
    kin::TileLayer& decor = map.add_layer("decor", 5);
    decor.visible = false;
    decor.y_sort = true;
    decor.participates_in_collision = false;
    decor.participates_in_navigation = false;
    map.set_cell("decor", {0, 0}, kin::TileCell{.tile_id = 1, .variant_id = 2, .flags = 7});
    map.tileset.tiles[1].sprite_id = "tiles.floor";
    map.tileset.tiles[1].walkable = true;
    map.tileset.tiles[1].cost = 15;
    map.tileset.tiles[1].blocks_sight = false;
    map.tileset.tiles[1].terrain = "floor";
    map.tileset.tiles[1].tags = {"safe", "indoor"};
    map.tileset.tiles[1].metadata["spawn"] = true;
    map.tileset.tiles[1].metadata["loot"] = std::string{"coin"};
    map.tileset.tiles[1].metadata["weight"] = 3;

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "kin-tilemap-roundtrip.kintilemap";
    assert(kin::save_tilemap(map, path));
    std::ifstream saved{path};
    std::string text{std::istreambuf_iterator<char>{saved}, std::istreambuf_iterator<char>{}};
    assert(text.find("\"format\": \"kin.tilemap\"") != std::string::npos);
    assert(text.find("\"version\": 1") != std::string::npos);

    const kin::TileMap loaded = kin::load_tilemap(path);

    assert(loaded.cols == 4);
    assert(loaded.rows == 3);
    assert(loaded.tile_w == 8);
    assert(loaded.tile_h == 10);
    assert(loaded.tileset.size() == 3);
    assert(loaded.tileset.tiles[1].sprite_id == "tiles.floor");
    assert(loaded.tileset.tiles[1].walkable == true);
    assert(loaded.tileset.tiles[1].cost == 15);
    assert(loaded.tileset.tiles[1].terrain == "floor");
    assert(loaded.tileset.tiles[1].tags.size() == 2);
    assert(loaded.tileset.tiles[1].tags[0] == "safe");
    assert(std::get<bool>(loaded.tileset.tiles[1].metadata.at("spawn")));
    assert(std::get<std::string>(loaded.tileset.tiles[1].metadata.at("loot")) == "coin");
    assert(std::get<kin::i32>(loaded.tileset.tiles[1].metadata.at("weight")) == 3);
    assert(loaded.solid(2, 1));
    assert(loaded.cell("decor", {0, 0}).tile_id == 1);
    assert(loaded.cell("decor", {0, 0}).variant_id == 2);
    assert(loaded.cell("decor", {0, 0}).flags == 7);
    const kin::TileLayer* loaded_decor = loaded.layer("decor");
    assert(loaded_decor != nullptr);
    assert(loaded_decor->order == 5);
    assert(!loaded_decor->visible);
    assert(loaded_decor->y_sort);
    assert(!loaded_decor->participates_in_collision);
    assert(!loaded_decor->participates_in_navigation);
}

void test_layer_identity_is_unique_by_id() {
    kin::TileMap map = make_test_map();
    const std::size_t initial_layers = map.layers.size();
    kin::TileLayer& decor = map.add_layer("decor", 5, 1);
    decor.visible = false;
    map.set("decor", 0, 0, 2);
    map.clear_dirty();

    kin::TileLayer& duplicate = map.add_layer("decor", 99, 2);
    assert(&duplicate == &decor);
    assert(map.layers.size() == initial_layers + 1);
    assert(duplicate.id == "decor");
    assert(duplicate.order == 5);
    assert(!duplicate.visible);
    assert(map.get("decor", 0, 0) == 2);
    assert(map.get("decor", 1, 0) == 1);
    assert(!map.layer_dirty("decor"));

    map.set("decor", 1, 0, 2);
    assert(map.get("decor", 1, 0) == 2);
    assert(map.dirty_regions("decor").size() == 1);
    assert((map.dirty_regions("decor").back() == kin::TileRect{1, 0, 1, 1}));

    decor.visible = true;
    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    map.submit(queue, {.draw_top_edge = false});
    assert(map.cached_render_chunk_count("decor") == 1);
}

void test_duplicate_layers_in_json_keep_first_layer() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "kin-tilemap-duplicate-layers.kintilemap";
    {
        std::ofstream out{path};
        out << "{\n";
        out << "  \"format\": \"kin.tilemap\",\n";
        out << "  \"version\": 1,\n";
        out << "  \"size\": {\"cols\": 2, \"rows\": 1},\n";
        out << "  \"tile_size\": {\"w\": 8, \"h\": 8},\n";
        out << "  \"tileset\": {\"tiles\": [\n";
        out << "    {\"tile_id\": 1, \"id\": \"floor\", \"solid\": false, \"cost\": 10, \"blocks_sight\": false, \"color\": [1,2,3], \"top_edge_color\": [4,5,6]},\n";
        out << "    {\"tile_id\": 2, \"id\": \"wall\", \"solid\": true, \"cost\": 10, \"blocks_sight\": false, \"color\": [7,8,9], \"top_edge_color\": [10,11,12]}\n";
        out << "  ]},\n";
        out << "  \"layers\": [\n";
        out << "    {\"id\": \"decor\", \"order\": 5, \"visible\": false, \"cells\": [[1, 0]]},\n";
        out << "    {\"id\": \"decor\", \"order\": 99, \"visible\": true, \"cells\": [[2, 2]]}\n";
        out << "  ]\n";
        out << "}\n";
    }

    const kin::TileMap loaded = kin::load_tilemap(path);
    assert(loaded.layers.size() == 1);
    const kin::TileLayer* decor = loaded.layer("decor");
    assert(decor != nullptr);
    assert(decor->order == 5);
    assert(!decor->visible);
    assert(loaded.get("decor", 0, 0) == 1);
    assert(loaded.get("decor", 1, 0) == 0);
}

void test_tileset_string_ids_and_metadata_defaults() {
    kin::TileMap map;
    map.resize(3, 2);
    const kin::u16 floor = map.tileset.ensure_tile("floor");
    const kin::u16 wall = map.tileset.ensure_tile("wall");
    assert(floor != 0);
    assert(wall != 0);
    map.tileset.tiles[wall].solid = true;
    map.tileset.tiles[wall].blocks_sight = true;
    map.tileset.tiles[wall].terrain = "stone";
    map.tileset.tiles[wall].tags = {"wall", "blocking"};

    assert(map.tile_id("floor") == floor);
    assert(map.tile("wall") == &map.tileset.tiles[wall]);
    assert(map.tileset.tiles[floor].is_walkable());
    assert(!map.tileset.tiles[wall].is_walkable());

    assert(map.set_cell({1, 0}, "wall"));
    assert(map.get(1, 0) == wall);
    assert(map.cell({1, 0}).tile_id == wall);
    assert(map.solid(1, 0));
    assert(!map.walkable(1, 0));

    map.tileset.tiles[wall].walkable = true;
    assert(map.walkable(1, 0));
    assert(!map.set_cell({2, 0}, "missing"));
    assert(map.get(2, 0) == 0);
}

void test_tilemap_coordinate_helpers() {
    kin::TileMap map = make_test_map();
    assert((map.world_to_map({17.0f, 21.0f}) == kin::Vec2i{2, 2}));
    assert((map.map_to_world({2, 1}) == kin::Vec2f{16.0f, 10.0f}));
    assert((map.tile_center({2, 1}) == kin::Vec2f{20.0f, 15.0f}));
}

void test_used_cells_rect_and_neighbors() {
    kin::TileMap empty;
    empty.resize(2, 2);
    empty.add_layer("decor");
    assert((empty.used_rect("ground") == kin::TileRect{}));
    assert((empty.used_rect("decor") == kin::TileRect{}));
    assert((empty.used_rect("missing") == kin::TileRect{}));
    assert((empty.used_rect() == kin::TileRect{}));

    kin::TileMap map = make_test_map();
    map.add_layer("decor", 5);
    map.set("decor", 3, 2, 1);

    const std::vector<kin::Vec2i> ground = map.used_cells("ground");
    assert(ground.size() == 2);
    assert((map.used_rect("ground") == kin::TileRect{1, 1, 2, 1}));
    assert((map.used_rect("decor") == kin::TileRect{3, 2, 1, 1}));
    assert((map.used_rect() == kin::TileRect{1, 1, 3, 2}));

    const std::vector<kin::Vec2i> corner_neighbors = map.neighbor_cells({0, 0});
    assert(corner_neighbors.size() == 2);
    assert((corner_neighbors[0] == kin::Vec2i{1, 0}));
    assert((corner_neighbors[1] == kin::Vec2i{0, 1}));

    const std::vector<kin::Vec2i> middle_neighbors = map.neighbor_cells({1, 1});
    assert(middle_neighbors.size() == 4);
}

void test_loads_old_line_based_tilemaps() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "kin-tilemap-old-format.kintilemap";
    {
        std::ofstream out{path};
        out << "size 2 2\n";
        out << "tile_size 6 7\n";
        out << "tile 1 floor solid=0 color=1,2,3 edge=4,5,6\n";
        out << "tile 2 wall solid=1 walkable=0 cost=20 blocks_sight=1 terrain=stone tags=hard,opaque\n";
        out << "layer ground order=3\n";
        out << "row 1 0\n";
        out << "row 0 2\n";
    }

    const kin::TileMap loaded = kin::load_tilemap(path);
    assert(loaded.cols == 2);
    assert(loaded.rows == 2);
    assert(loaded.tile_w == 6);
    assert(loaded.tile_h == 7);
    assert(loaded.layer("ground")->order == 3);
    assert(loaded.get(0, 0) == 1);
    assert(loaded.get(1, 1) == 2);
    assert(loaded.solid(1, 1));
    assert(!loaded.walkable(1, 1));
    assert(loaded.tileset.tiles[2].cost == 20);
    assert(loaded.tileset.tiles[2].blocks_sight);
    assert(loaded.tileset.tiles[2].terrain == "stone");
    assert(loaded.tileset.tiles[2].tags.size() == 2);
}

void test_runtime_editing_and_dirty_regions() {
    kin::TileMap map = make_test_map();
    map.clear_dirty();

    assert(map.fill_rect({0, 0, 2, 2}, 1) == 4);
    assert(map.get(0, 0) == 1);
    assert(map.get(1, 1) == 1);
    assert(map.layer_dirty("ground"));
    assert(map.dirty_regions("ground").size() == 1);
    assert((map.dirty_regions("ground").back() == kin::TileRect{0, 0, 2, 2}));

    map.clear_dirty("ground");
    assert(!map.layer_dirty("ground"));
    assert(map.dirty_regions("ground").empty());

    const std::array<kin::Vec2i, 2> cells{{{2, 0}, {3, 0}}};
    assert(map.set_cells(cells, "wall") == 2);
    assert(map.get(2, 0) == 2);
    assert(map.get(3, 0) == 2);
    assert(map.layer_dirty("ground"));
    assert(map.dirty_regions("ground").size() == 1);
    assert((map.dirty_regions("ground").back() == kin::TileRect{2, 0, 2, 1}));

    map.clear_dirty();
    assert(map.replace_tile("ground", 2, 1) == 3);
    assert(map.get(2, 0) == 1);
    assert(map.get(3, 0) == 1);
    assert(map.get(2, 1) == 1);
    assert(map.layer_dirty("ground"));
    assert(map.dirty_regions("ground").size() == 1);
    assert((map.dirty_regions("ground").back() == kin::TileRect{2, 0, 2, 2}));

    map.clear_dirty();
    assert(map.set_cells(cells, 1) == 2);
    assert(!map.layer_dirty("ground"));
    assert(map.dirty_regions("ground").empty());

    assert(map.fill_rect({0, 0, 1, 1}, 1) == 1);
    assert(!map.layer_dirty("ground"));
    assert(map.replace_tile("ground", 1, 1) > 0);
    assert(!map.layer_dirty("ground"));

    map.clear_dirty();
    assert(map.clear_cell({3, 0}));
    assert(map.get(3, 0) == 0);
    assert(map.layer_dirty("ground"));
}

void test_flood_fill() {
    kin::TileMap map;
    map.resize(4, 3);
    map.tileset.resize(4);
    map.tileset.tiles[1] = {.id = "floor"};
    map.tileset.tiles[2] = {.id = "wall", .solid = true};
    map.tileset.tiles[3] = {.id = "water"};

    map.fill_rect({0, 0, 4, 3}, 1);
    map.set(1, 0, 2);
    map.set(1, 1, 2);
    map.set(1, 2, 2);
    map.clear_dirty();

    assert(map.flood_fill({0, 0}, 3) == 3);
    assert(map.get(0, 0) == 3);
    assert(map.get(0, 1) == 3);
    assert(map.get(0, 2) == 3);
    assert(map.get(2, 0) == 1);
    assert(map.layer_dirty("ground"));
    assert(map.dirty_regions("ground").size() == 1);
    assert((map.dirty_regions("ground").back() == kin::TileRect{0, 0, 1, 3}));
}

void test_pattern_extract_stamp_and_roundtrip() {
    kin::TileMap map = make_test_map();
    map.set_cell({0, 0}, kin::TileCell{.tile_id = 1, .variant_id = 4});
    map.set(1, 0, 2);
    map.clear_dirty();

    kin::TilePattern pattern = map.extract_pattern("ground", {0, 0, 2, 2});
    pattern.id = "corner";
    assert((pattern.bounds == kin::TileRect{0, 0, 2, 2}));
    assert(pattern.cells.size() == 4);
    assert(pattern.cells[0].cell.tile_id == 1);
    assert(pattern.cells[0].cell.variant_id == 4);

    assert(map.stamp_pattern("ground", {2, 0}, pattern, true) == 3);
    assert(map.cell({2, 0}).tile_id == 1);
    assert(map.cell({2, 0}).variant_id == 4);
    assert(map.get(3, 0) == 2);
    assert(map.get(2, 1) == 2);
    assert(map.get(3, 1) == 1);
    assert(map.layer_dirty("ground"));
    assert(map.dirty_regions("ground").size() == 1);
    assert((map.dirty_regions("ground").back() == kin::TileRect{2, 0, 2, 2}));

    map.patterns.push_back(pattern);
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "kin-tilemap-pattern-roundtrip.kintilemap";
    assert(kin::save_tilemap(map, path));
    const kin::TileMap loaded = kin::load_tilemap(path);
    assert(loaded.patterns.size() == 1);
    assert(loaded.patterns[0].id == "corner");
    assert((loaded.patterns[0].bounds == kin::TileRect{0, 0, 2, 2}));
    assert(loaded.patterns[0].cells.size() == 4);
    assert(loaded.patterns[0].cells[0].cell.variant_id == 4);
}

void test_collision_queries_and_extraction() {
    kin::TileMap map;
    map.resize(4, 3);
    map.tile_w = 8;
    map.tile_h = 10;
    map.tileset.resize(3);
    map.tileset.tiles[1] = {.id = "floor"};
    map.tileset.tiles[2] = {
        .id = "wall",
        .solid = true,
        .collision_polygon = {{1.0f, 2.0f}, {7.0f, 2.0f}, {7.0f, 9.0f}, {1.0f, 9.0f}},
    };
    map.fill_rect({1, 0, 2, 2}, 2);
    map.clear_dirty();

    assert(map.blocks_point({9.0f, 1.0f}));
    assert(!map.blocks_point({1.0f, 1.0f}));
    assert(map.blocks_rect({15.0f, 5.0f, 4.0f, 4.0f}));
    assert(!map.blocks_rect({0.0f, 20.0f, 4.0f, 4.0f}));

    const std::vector<kin::Rectf> rects = map.collision_rects("ground");
    assert(rects.size() == 1);
    assert((rects[0] == kin::Rectf{8.0f, 0.0f, 16.0f, 20.0f}));

    const std::vector<std::vector<kin::Vec2f>> polygons = map.collision_polygons("ground");
    assert(polygons.size() == 4);
    assert(polygons[0].size() == 4);
    assert((polygons[0][0] == kin::Vec2f{9.0f, 2.0f}));
    assert((polygons[0][2] == kin::Vec2f{15.0f, 9.0f}));

    kin::TileLayer& decor = map.add_layer("decor", 5);
    decor.participates_in_collision = false;
    map.set("decor", 0, 0, 2);
    assert(!map.blocks_point("decor", {1.0f, 1.0f}));
    assert(map.collision_rects("decor").empty());
}

// collision_rects merges each run into the first earlier rect with the same
// columns ending where the run starts. Check it against that rule written as a
// plain scan, on random maps with several layers (merging across layers when all
// are visited) and a layer that does not collide.
void test_collision_rects_match_reference_merge() {
    kin::u32 seed = 99;
    const auto next = [&](kin::u32 range) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % range;
    };
    for (int trial = 0; trial < 20; ++trial) {
        kin::TileMap map;
        map.resize(3 + static_cast<kin::i32>(next(30)), 2 + static_cast<kin::i32>(next(30)));
        map.tile_w = 8;
        map.tile_h = 6;
        map.tileset.resize(3);
        map.tileset.tiles[1] = {.id = "floor"};
        map.tileset.tiles[2] = {.id = "wall", .solid = true};
        map.add_layer("upper", 1);
        map.add_layer("ghost", 2).participates_in_collision = false;
        const kin::u32 density = 2 + next(4);
        for (const std::string_view layer : {"ground", "upper", "ghost"}) {
            for (kin::i32 row = 0; row < map.rows; ++row) {
                for (kin::i32 col = 0; col < map.cols; ++col) {
                    // Column bands make runs line up across rows and layers.
                    const bool wall = (col / 3) % 2 == 0 ? next(density) != 0 : next(density) == 0;
                    map.set(layer, col, row, wall ? 2 : 1);
                }
            }
        }

        for (const std::string_view layer_id : {"", "ground", "upper", "ghost"}) {
            std::vector<kin::Rectf> expected;
            for (const kin::TileLayer& layer : map.layers) {
                if ((!layer_id.empty() && layer.id != layer_id) || !layer.participates_in_collision) {
                    continue;
                }
                for (kin::i32 row = 0; row < map.rows; ++row) {
                    kin::i32 run_start = -1;
                    for (kin::i32 col = 0; col <= map.cols; ++col) {
                        const kin::TileDefinition* tile = col < map.cols
                            ? map.tile(layer.cells[static_cast<std::size_t>(row * map.cols + col)].tile_id)
                            : nullptr;
                        const bool blocked = tile && tile->solid;
                        if (blocked && run_start < 0) {
                            run_start = col;
                        } else if (!blocked && run_start >= 0) {
                            const kin::Rectf rect{static_cast<float>(run_start * map.tile_w),
                                                  static_cast<float>(row * map.tile_h),
                                                  static_cast<float>((col - run_start) * map.tile_w),
                                                  static_cast<float>(map.tile_h)};
                            const auto merge = std::ranges::find_if(expected, [&](const kin::Rectf& existing) {
                                return existing.x == rect.x && existing.w == rect.w && existing.y + existing.h == rect.y;
                            });
                            if (merge != expected.end()) {
                                merge->h += rect.h;
                            } else {
                                expected.push_back(rect);
                            }
                            run_start = -1;
                        }
                    }
                }
            }
            assert(map.collision_rects(layer_id) == expected);
        }
    }
}

void test_query_layer_participation_filters() {
    kin::TileMap map;
    map.resize(2, 1);
    map.tile_w = 8;
    map.tile_h = 8;
    map.tileset.resize(4);
    map.tileset.tiles[1] = {.id = "floor", .cost = 2};
    map.tileset.tiles[2] = {.id = "mud", .cost = 30};
    map.tileset.tiles[3] = {.id = "wall", .solid = true, .walkable = false, .cost = 99};
    map.fill_rect({0, 0, 2, 1}, 1);

    kin::TileLayer& ignored = map.add_layer("ignored", 1);
    ignored.participates_in_collision = false;
    ignored.participates_in_navigation = false;
    map.set("ignored", 0, 0, 3);
    assert(!map.solid(0, 0));
    assert(!map.blocks_point({1.0f, 1.0f}));
    assert(!map.blocks_rect({0.0f, 0.0f, 8.0f, 8.0f}));
    assert(map.collision_rects("ignored").empty());
    assert(map.collision_polygons("ignored").empty());
    assert(map.walkable(0, 0));
    assert(map.movement_cost(0, 0) == 10);

    kin::TileLayer& mud = map.add_layer("mud", 2);
    mud.participates_in_collision = false;
    mud.participates_in_navigation = true;
    map.set("mud", 1, 0, 2);
    assert(map.walkable(1, 0));
    assert(map.movement_cost(1, 0) == 30);

    kin::TileLayer& blocker = map.add_layer("blocker", 3);
    blocker.participates_in_collision = false;
    blocker.participates_in_navigation = true;
    map.set("blocker", 1, 0, 3);
    assert(!map.walkable(1, 0));
    assert(map.movement_cost(1, 0) == std::numeric_limits<kin::i32>::max());
    assert(map.walkable("mud", 1, 0));
    assert(map.movement_cost("mud", 1, 0) == 30);
    assert(!map.walkable("blocker", 1, 0));
    assert(map.movement_cost("blocker", 1, 0) == 99);
}

void test_collision_rect_order_and_named_layer_blocking() {
    kin::TileMap map;
    map.resize(4, 3);
    map.tile_w = 8;
    map.tile_h = 8;
    map.tileset.resize(2);
    map.tileset.tiles[1] = {.id = "wall", .solid = true};

    kin::TileLayer& decor = map.add_layer("decor", 4);
    decor.participates_in_collision = true;
    map.set("decor", 0, 0, 1);
    map.set("decor", 1, 0, 1);
    map.set("decor", 3, 0, 1);
    map.set("decor", 0, 1, 1);
    map.set("decor", 1, 1, 1);

    const std::vector<kin::Rectf> rects = map.collision_rects("decor");
    assert(rects.size() == 2);
    assert((rects[0] == kin::Rectf{0.0f, 0.0f, 16.0f, 16.0f}));
    assert((rects[1] == kin::Rectf{24.0f, 0.0f, 8.0f, 8.0f}));
    assert(map.blocks_rect("decor", {25.0f, 1.0f, 2.0f, 2.0f}));

    decor.participates_in_collision = false;
    assert(!map.blocks_rect("decor", {25.0f, 1.0f, 2.0f, 2.0f}));
    assert(map.collision_rects("decor").empty());
}

void test_collision_polygon_roundtrip() {
    kin::TileMap map;
    map.resize(1, 1);
    map.tileset.resize(2);
    map.tileset.tiles[1] = {
        .id = "slope",
        .solid = true,
        .collision_polygon = {{0.0f, 8.0f}, {8.0f, 0.0f}, {8.0f, 8.0f}},
    };
    map.set(0, 0, 1);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "kin-tilemap-collision-roundtrip.kintilemap";
    assert(kin::save_tilemap(map, path));
    const kin::TileMap loaded = kin::load_tilemap(path);
    assert(loaded.tileset.tiles[1].collision_polygon.size() == 3);
    assert((loaded.tileset.tiles[1].collision_polygon[1] == kin::Vec2f{8.0f, 0.0f}));
}

kin::TileMap make_terrain_map() {
    kin::TileMap map;
    map.resize(4, 3);
    map.tileset.resize(8);
    map.tileset.tiles[1] = {.id = "road-0", .terrain = "road"};
    map.tileset.tiles[2] = {.id = "road-ew", .terrain = "road"};
    map.tileset.tiles[3] = {.id = "road-ns", .terrain = "road"};
    map.tileset.tiles[4] = {.id = "road-e", .terrain = "road"};
    map.tileset.tiles[5] = {.id = "road-w", .terrain = "road"};
    map.tileset.tiles[6] = {.id = "road-n", .terrain = "road"};
    map.tileset.tiles[7] = {.id = "road-s", .terrain = "road"};
    map.tileset.add_terrain_variant("road", 0, 1, true);
    map.tileset.add_terrain_variant("road", kin::TileTerrainEast | kin::TileTerrainWest, 2);
    map.tileset.add_terrain_variant("road", kin::TileTerrainNorth | kin::TileTerrainSouth, 3);
    map.tileset.add_terrain_variant("road", kin::TileTerrainEast, 4);
    map.tileset.add_terrain_variant("road", kin::TileTerrainWest, 5);
    map.tileset.add_terrain_variant("road", kin::TileTerrainNorth, 6);
    map.tileset.add_terrain_variant("road", kin::TileTerrainSouth, 7);
    return map;
}

void test_terrain_connect_updates_cells_and_neighbors() {
    kin::TileMap map = make_terrain_map();
    const std::array<kin::Vec2i, 3> road{{{1, 1}, {2, 1}, {3, 1}}};
    assert(map.set_terrain_connect("ground", road, "road") >= 3);
    assert(map.terrain_diagnostics.empty());
    assert(map.get(1, 1) == 4);
    assert(map.get(2, 1) == 2);
    assert(map.get(3, 1) == 5);
    assert(map.layer_dirty("ground"));
    assert(map.dirty_regions("ground").size() == 1);
    assert((map.dirty_regions("ground").back() == kin::TileRect{1, 1, 3, 1}));

    const std::array<kin::Vec2i, 1> branch{{{2, 0}}};
    map.clear_dirty();
    assert(map.set_terrain_connect("ground", branch, "road") >= 1);
    assert(map.get(2, 0) == 7);
    assert(map.get(2, 1) == 1);
    assert(!map.terrain_diagnostics.empty());
    assert(map.layer_dirty("ground"));
    assert(map.dirty_regions("ground").size() == 1);
    assert((map.dirty_regions("ground").back() == kin::TileRect{2, 0, 1, 2}));
}

void test_terrain_missing_fallback_and_roundtrip() {
    kin::TileMap missing;
    missing.resize(2, 2);
    const std::array<kin::Vec2i, 1> cells{{{0, 0}}};
    assert(missing.set_terrain_connect("ground", cells, "water") == 0);
    assert(!missing.terrain_diagnostics.empty());

    kin::TileMap map = make_terrain_map();
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "kin-tilemap-terrain-roundtrip.kintilemap";
    assert(kin::save_tilemap(map, path));
    const kin::TileMap loaded = kin::load_tilemap(path);
    assert(loaded.tileset.terrain_variants.size() == map.tileset.terrain_variants.size());
    assert(loaded.tileset.terrain_tile("road", kin::TileTerrainEast | kin::TileTerrainWest) == 2);
    assert(loaded.tileset.terrain_fallback_tile("road") == 1);
}

void test_asset_metadata_and_loader() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-tilemap-assets";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "level.kintilemap";
    assert(kin::save_tilemap(make_test_map(), path));

    kin::AssetManager assets{dir};
    assets.discover();
    const kin::AssetMetadata* metadata = assets.metadata("level.kintilemap");
    assert(metadata != nullptr);
    assert(metadata->type == kin::AssetType::TileMap);

    const std::shared_ptr<const kin::TileMap> loaded = assets.load<kin::TileMap>("level.kintilemap");
    assert(loaded != nullptr);
    assert(loaded->get(2, 1) == 2);
    metadata = assets.metadata("level.kintilemap");
    assert(metadata != nullptr);
    assert(metadata->status == kin::AssetStatus::Loaded);
}

void test_render_queue_submission_and_culling() {
    kin::TileMap map = make_test_map();
    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    kin::RenderView view{
        .cull_rect = {8.0f, 10.0f, 16.0f, 10.0f},
        .culling_enabled = true,
    };
    map.submit(queue, view, {.draw_top_edge = false});

    assert(queue.size() == 2);
    assert(queue.commands()[0].type == kin::RenderCommandType::FillRect);
    assert((queue.commands()[0].rect == kin::Rectf{8.0f, 10.0f, 8.0f, 10.0f}));
    assert((queue.commands()[1].rect == kin::Rectf{16.0f, 10.0f, 8.0f, 10.0f}));
}

void test_render_cache_matches_uncached_submission() {
    kin::TileMap map = make_cache_test_map();
    kin::RenderView view{
        .cull_rect = {0.0f, 0.0f, 64.0f, 64.0f},
        .culling_enabled = true,
    };
    kin::RenderQueue cached{kin::RenderSortMode::Submission};
    kin::RenderQueue uncached{kin::RenderSortMode::Submission};
    map.submit(cached, view, {.draw_top_edge = false});
    map.submit(uncached, view, {.draw_top_edge = false, .use_cache = false});

    assert(map.cached_render_chunk_count("ground") == 4);
    assert(cached.size() == uncached.size());
    const std::vector<kin::RenderCommand> cached_sorted = sorted_render_commands(cached.commands());
    const std::vector<kin::RenderCommand> uncached_sorted = sorted_render_commands(uncached.commands());
    for (std::size_t i = 0; i < cached_sorted.size(); ++i) {
        assert(render_command_eq(cached_sorted[i], uncached_sorted[i]));
    }
}

void test_render_cache_top_edges_chunk_override_and_option_rebuilds() {
    kin::TileMap map = make_cache_test_map();
    map.set_render_chunk_size(4, 4);
    kin::RenderView view{
        .cull_rect = {0.0f, 0.0f, 64.0f, 64.0f},
        .culling_enabled = true,
    };

    kin::RenderQueue cached{kin::RenderSortMode::Submission};
    kin::RenderQueue uncached{kin::RenderSortMode::Submission};
    map.submit(cached, view, {.draw_top_edge = true});
    map.submit(uncached, view, {.draw_top_edge = true, .use_cache = false});
    assert(cached.size() == uncached.size());
    const std::vector<kin::RenderCommand> cached_sorted = sorted_render_commands(cached.commands());
    const std::vector<kin::RenderCommand> uncached_sorted = sorted_render_commands(uncached.commands());
    for (std::size_t i = 0; i < cached_sorted.size(); ++i) {
        assert(render_command_eq(cached_sorted[i], uncached_sorted[i]));
    }
    assert(map.cached_render_chunk_count("ground") == 1);

    kin::RenderQueue override_chunks{kin::RenderSortMode::Submission};
    map.submit(override_chunks, view, {.draw_top_edge = false, .chunk_cols = 2, .chunk_rows = 2});
    assert(map.cached_render_chunk_count("ground") == 4);

    kin::RenderQueue custom_key{kin::RenderSortMode::Submission};
    map.submit(custom_key, view, {
        .layer = kin::layer_value(kin::RenderLayer::Effects),
        .order = 7,
        .draw_top_edge = false,
        .pass_mask = kin::render_pass_mask::effects,
    });
    assert(map.cached_render_chunk_count("ground") == 1);
    assert(!custom_key.empty());
    assert(custom_key.commands()[0].key.layer == kin::layer_value(kin::RenderLayer::Effects));
    assert(custom_key.commands()[0].key.order == 7);
    assert(custom_key.commands()[0].key.pass_mask == kin::render_pass_mask::effects);
}

void test_named_layer_top_edges_match_cached_and_uncached() {
    kin::TileMap map;
    map.resize(3, 3);
    map.tile_w = 8;
    map.tile_h = 8;
    map.tileset.resize(2);
    map.tileset.tiles[1] = {
        .id = "wall",
        .solid = true,
        .color = kin::Color::rgb(80, 90, 100),
        .top_edge_color = kin::Color::rgb(120, 130, 140),
    };
    map.default_layer().visible = false;
    map.add_layer("decor", 3);
    map.set("decor", 1, 1, 1);
    map.set("decor", 2, 1, 1);
    map.set("decor", 2, 0, 1);
    map.set_render_chunk_size(2, 2);

    kin::RenderView view{
        .cull_rect = {0.0f, 0.0f, 24.0f, 24.0f},
        .culling_enabled = true,
    };
    kin::RenderQueue cached{kin::RenderSortMode::Submission};
    map.submit(cached, view, {.draw_top_edge = true});
    assert(map.cached_render_chunk_count("decor") == 4);

    kin::RenderQueue uncached{kin::RenderSortMode::Submission};
    map.submit(uncached, view, {.draw_top_edge = true, .static_renderable = false});
    assert(cached.size() == uncached.size());
    const std::vector<kin::RenderCommand> cached_sorted = sorted_render_commands(cached.commands());
    const std::vector<kin::RenderCommand> uncached_sorted = sorted_render_commands(uncached.commands());
    for (std::size_t i = 0; i < cached_sorted.size(); ++i) {
        assert(render_command_eq(cached_sorted[i], uncached_sorted[i]));
    }

    const auto top_edges = std::ranges::count_if(cached.commands(), [](const kin::RenderCommand& command) {
        return command.rect.h == 2.0f && command.color == kin::Color::rgb(120, 130, 140);
    });
    assert(top_edges == 2);
}

void test_render_cache_sprites_and_visible_chunks() {
    kin::TileMap map = make_cache_test_map();
    map.tileset.tiles[1].sprite_id = "floor";
    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{8, 10})};
    kin::SpriteCatalog catalog;
    catalog.set_texture("atlas", texture);
    catalog.add({
        .id = "floor",
        .texture_id = "atlas",
        .source = {0.0f, 0.0f, 8.0f, 10.0f},
        .size = {8.0f, 10.0f},
    });

    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    kin::RenderView view{
        .cull_rect = {0.0f, 0.0f, 8.0f, 10.0f},
        .culling_enabled = true,
    };
    map.submit(queue, view, {.draw_top_edge = false, .sprites = &catalog});

    assert(map.cached_render_chunk_count("ground") == 4);
    assert(queue.size() == 4);
    for (const kin::RenderCommand& command : queue.commands()) {
        assert(command.type == kin::RenderCommandType::Sprite);
    }
}

void test_render_cache_invalidation_and_explicit_clear() {
    kin::TileMap map = make_cache_test_map();
    kin::RenderView view{
        .cull_rect = {0.0f, 0.0f, 8.0f, 10.0f},
        .culling_enabled = true,
    };
    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    map.submit(queue, view, {.draw_top_edge = false});
    assert(map.cached_render_chunk_count("ground") == 4);

    assert(map.fill_rect({0, 0, 2, 2}, 2) == 4);
    assert(map.dirty_regions("ground").size() == 1);
    assert((map.dirty_regions("ground").back() == kin::TileRect{0, 0, 2, 2}));
    map.clear_dirty();
    kin::RenderQueue changed{kin::RenderSortMode::Submission};
    map.submit(changed, view, {.draw_top_edge = false});
    assert(changed.size() == 4);
    assert(changed.commands()[0].color == kin::Color::rgb(110, 120, 130));

    map.invalidate_render_cache("ground");
    assert(map.cached_render_chunk_count("ground") == 0);
    kin::RenderQueue rebuilt{kin::RenderSortMode::Submission};
    map.submit(rebuilt, view, {.draw_top_edge = false});
    assert(map.cached_render_chunk_count("ground") == 4);

    kin::TileLayer& decor = map.add_layer("decor", 5);
    decor.cells.assign(static_cast<std::size_t>(map.cols * map.rows), kin::TileCell{.tile_id = 1});
    kin::RenderQueue layered{kin::RenderSortMode::Submission};
    map.submit(layered, view, {.draw_top_edge = false});
    assert(map.cached_render_chunk_count("ground") == 4);
    assert(map.cached_render_chunk_count("decor") == 4);

    map.invalidate_render_cache("ground");
    assert(map.cached_render_chunk_count("ground") == 0);
    assert(map.cached_render_chunk_count("decor") == 4);
}

void test_render_cache_mutation_apis_and_bypasses() {
    kin::TileMap map = make_cache_test_map();
    kin::RenderView view{
        .cull_rect = {0.0f, 0.0f, 64.0f, 64.0f},
        .culling_enabled = true,
    };
    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    map.submit(queue, view, {.draw_top_edge = false});
    assert(map.cached_render_chunk_count("ground") == 4);

    map.fill_rect({0, 0, 2, 2}, 2);
    map.replace_tile("ground", 2, 1);
    map.set(0, 0, 2);
    map.flood_fill("ground", {1, 0}, 2);
    kin::TilePattern pattern = map.extract_pattern("ground", {0, 0, 1, 1});
    map.stamp_pattern("ground", {3, 3}, pattern);
    kin::RenderQueue changed{kin::RenderSortMode::Submission};
    map.submit(changed, view, {.draw_top_edge = false});
    assert(map.cached_render_chunk_count("ground") == 4);
    assert(!changed.empty());

    const std::array<kin::Vec2i, 1> terrain_cells{{{2, 2}}};
    map.tileset.tiles[2].terrain = "road";
    map.tileset.add_terrain_variant("road", 0, 2, true);
    map.set_terrain_connect("ground", terrain_cells, "road");
    kin::RenderQueue terrain{kin::RenderSortMode::Submission};
    map.submit(terrain, view, {.draw_top_edge = false});
    assert(!terrain.empty());

    kin::TileLayer& decor = map.add_layer("decor", 5);
    decor.visible = false;
    map.set("decor", 0, 0, 2);
    map.invalidate_render_cache("decor");
    kin::RenderQueue invisible{kin::RenderSortMode::Submission};
    map.submit(invisible, view, {.draw_top_edge = false});
    assert(map.cached_render_chunk_count("decor") == 0);

    map.default_layer().y_sort = true;
    map.invalidate_render_cache("ground");
    kin::RenderQueue ysort{kin::RenderSortMode::Submission};
    map.submit(ysort, view, {.draw_top_edge = false});
    assert(map.cached_render_chunk_count("ground") == 0);
    map.default_layer().y_sort = false;

    kin::RenderQueue dynamic{kin::RenderSortMode::Submission};
    map.submit(dynamic, view, {.draw_top_edge = false, .static_renderable = false});
    assert(map.cached_render_chunk_count("ground") == 0);
}

void test_render_cache_clear_and_resize_safety() {
    kin::TileMap map = make_cache_test_map();
    kin::RenderView view{
        .cull_rect = {0.0f, 0.0f, 64.0f, 64.0f},
        .culling_enabled = true,
    };
    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    map.submit(queue, view, {.draw_top_edge = false});
    assert(map.cached_render_chunk_count("ground") == 4);

    map.clear();
    assert(map.cached_render_chunk_count("ground") == 0);
    kin::RenderQueue after_clear{kin::RenderSortMode::Submission};
    map.submit(after_clear, view, {.draw_top_edge = false});
    assert(after_clear.empty());

    map = make_cache_test_map();
    kin::RenderQueue before_resize{kin::RenderSortMode::Submission};
    map.submit(before_resize, view, {.draw_top_edge = false});
    assert(map.cached_render_chunk_count("ground") == 4);
    map.resize(2, 2, 1);
    assert(map.cached_render_chunk_count("ground") == 0);

    kin::RenderQueue after_resize{kin::RenderSortMode::Submission};
    map.submit(after_resize, view, {.draw_top_edge = false});
    assert(map.cached_render_chunk_count("ground") == 1);
    assert(!after_resize.empty());
}

void test_sprite_backed_tiles_submit_sprite_commands() {
    kin::TileMap map = make_test_map();
    map.tileset.tiles[1].sprite_id = "floor";

    kin::Texture texture{std::make_shared<FakeTextureBackend>(kin::Vec2i{8, 10})};
    kin::SpriteCatalog catalog;
    catalog.set_texture("atlas", texture);
    catalog.add({
        .id = "floor",
        .texture_id = "atlas",
        .source = {0.0f, 0.0f, 8.0f, 10.0f},
        .size = {8.0f, 10.0f},
    });

    kin::RenderQueue queue{kin::RenderSortMode::Submission};
    map.submit(queue, {.draw_top_edge = false, .sprites = &catalog});

    bool saw_sprite = false;
    for (const kin::RenderCommand& command : queue.commands()) {
        saw_sprite = saw_sprite || command.type == kin::RenderCommandType::Sprite;
    }
    assert(saw_sprite);
}

} // namespace

int main() {
    test_basic_grid_and_solidity();
    test_layers_and_roundtrip();
    test_layer_identity_is_unique_by_id();
    test_duplicate_layers_in_json_keep_first_layer();
    test_tileset_string_ids_and_metadata_defaults();
    test_tilemap_coordinate_helpers();
    test_used_cells_rect_and_neighbors();
    test_loads_old_line_based_tilemaps();
    test_runtime_editing_and_dirty_regions();
    test_flood_fill();
    test_pattern_extract_stamp_and_roundtrip();
    test_collision_queries_and_extraction();
    test_collision_rects_match_reference_merge();
    test_query_layer_participation_filters();
    test_collision_rect_order_and_named_layer_blocking();
    test_collision_polygon_roundtrip();
    test_terrain_connect_updates_cells_and_neighbors();
    test_terrain_missing_fallback_and_roundtrip();
    test_asset_metadata_and_loader();
    test_render_queue_submission_and_culling();
    test_render_cache_matches_uncached_submission();
    test_render_cache_top_edges_chunk_override_and_option_rebuilds();
    test_named_layer_top_edges_match_cached_and_uncached();
    test_render_cache_sprites_and_visible_chunks();
    test_render_cache_invalidation_and_explicit_clear();
    test_render_cache_mutation_apis_and_bypasses();
    test_render_cache_clear_and_resize_safety();
    test_sprite_backed_tiles_submit_sprite_commands();
    return 0;
}
