// Hex grid demo: a small map of terrain with a unit, its movement range and an A*
// path to a goal, drawn in any of the four hex layouts.
//
//   Left click   set the goal          Tab  pointy-top / flat-top
//   Right click  move the unit         O    odd / even offset
//   R            reset                 Esc  quit

#include <kin/core/hex.hpp>
#include <kin/core/json.hpp>
#include <kin/pathfinding/pathfinding.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/ui2/text.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace demo {
namespace {

constexpr kin::Vec2f logical_size{960.0f, 600.0f};
constexpr kin::Rectf map_area{24.0f, 92.0f, 912.0f, 484.0f};
constexpr kin::i32 map_cols = 17;
constexpr kin::i32 map_rows = 11;
constexpr kin::i32 move_budget = 40;

enum class Terrain : kin::u8 { Plains, Forest, Hills, Water, Mountain };

struct TerrainInfo {
    std::string_view name;
    kin::i32 cost = 0; // 0 = impassable
    kin::Color color;
};

constexpr std::array<TerrainInfo, 5> terrain_info{{
    {"plains", 10, kin::Color::rgb(148, 176, 98)},
    {"forest", 20, kin::Color::rgb(66, 120, 76)},
    {"hills", 30, kin::Color::rgb(170, 144, 96)},
    {"water", 0, kin::Color::rgb(60, 112, 170)},
    {"mountain", 0, kin::Color::rgb(118, 116, 128)},
}};

const TerrainInfo& info(Terrain t) {
    return terrain_info[static_cast<std::size_t>(t)];
}

// Blobs of terrain placed on the map, by offset cell and radius in hexes.
struct Feature {
    kin::Vec2i cell;
    kin::i32 radius = 0;
    Terrain terrain = Terrain::Plains;
};

constexpr std::array<Feature, 9> features{{
    {{5, 2}, 2, Terrain::Water},
    {{11, 8}, 1, Terrain::Water},
    {{8, 5}, 1, Terrain::Mountain},
    {{9, 4}, 1, Terrain::Mountain},
    {{13, 2}, 2, Terrain::Forest},
    {{3, 8}, 2, Terrain::Forest},
    {{7, 9}, 1, Terrain::Hills},
    {{14, 8}, 1, Terrain::Hills},
    {{10, 1}, 1, Terrain::Hills},
}};

constexpr kin::Vec2i default_start{1, 5};
constexpr kin::Vec2i default_goal{15, 5};

kin::GameInfo make_game_info() {
    kin::InputMap input;
    input.bind("quit", kin::Key::Escape);
    input.bind("orientation", kin::Key::Tab);
    input.bind("offset", kin::Key::O);
    input.bind("reset", kin::Key::R);

    return {
        .id = "hex_demo",
        .title = "Kin Hex Demo",
        .version = "0.1",
        .description = "Hex coordinates, picking, movement range and A* paths in all four hex layouts.",
        .author = "Kin contributors",
        .window = {
            .width = static_cast<kin::i32>(logical_size.x),
            .height = static_cast<kin::i32>(logical_size.y),
            .logical_width = static_cast<kin::i32>(logical_size.x),
            .logical_height = static_cast<kin::i32>(logical_size.y),
            .resizable = true,
        },
        .tags = {"sample", "hex", "pathfinding"},
        .fields = {},
        .input_map = input,
    };
}

kin::Color scaled(kin::Color c, float k, kin::u8 alpha = 255) {
    const auto ch = [k](kin::u8 v) { return static_cast<kin::u8>(std::clamp(v * k, 0.0f, 255.0f)); };
    return kin::Color::rgba(ch(c.r), ch(c.g), ch(c.b), alpha);
}

class HexDemoScene final : public kin::Scene {
public:
    HexDemoScene() {
        generate_map();
        reset();
    }

    std::string_view name() const override { return "Hex Demo"; }

    void update(kin::SceneContext& ctx) override {
        if (ctx.window.close_requested() || ctx.input.pressed("quit")) {
            ctx.app.quit();
            return;
        }
        if (ctx.input.pressed("reset")) {
            reset();
        }
        if (ctx.input.pressed("orientation")) {
            _layout.orientation = _layout.orientation == kin::HexOrientation::PointyTop
                                      ? kin::HexOrientation::FlatTop
                                      : kin::HexOrientation::PointyTop;
            fit_layout();
        }
        if (ctx.input.pressed("offset")) {
            _layout.offset = _layout.offset == kin::HexOffset::Odd ? kin::HexOffset::Even : kin::HexOffset::Odd;
            fit_layout();
        }

        _hover = cell_at(ctx.renderer.window_to_logical(ctx.input.mouse_pos()));
        if (_hover && passable(*_hover)) {
            if (ctx.input.mouse_pressed(kin::MouseButton::Left)) {
                _goal = *_hover;
                replan();
            } else if (ctx.input.mouse_pressed(kin::MouseButton::Right)) {
                _start = *_hover;
                replan();
            }
        }
    }

    void render(kin::SceneContext& ctx) override {
        kin::Renderer2D& r = ctx.renderer;
        r.clear(kin::Color::rgb(20, 24, 30));
        ensure_hex_texture(r);

        std::vector<bool> in_range(static_cast<std::size_t>(map_cols * map_rows), false);
        for (const kin::HexReach& reach : _reach) {
            in_range[index(reach.cell)] = true;
        }
        for (kin::i32 row = 0; row < map_rows; ++row) {
            for (kin::i32 col = 0; col < map_cols; ++col) {
                const kin::Vec2i cell{col, row};
                const kin::Color base = info(terrain(cell)).color;
                // Cells outside the unit's range are dimmed.
                draw_hex(r, cell, in_range[index(cell)] ? base : scaled(base, 0.55f), 0.94f);
            }
        }
        for (kin::i32 row = 0; row < map_rows; ++row) {
            for (kin::i32 col = 0; col < map_cols; ++col) {
                outline_hex(r, {col, row}, kin::Color::rgba(12, 16, 20, 110));
            }
        }

        // The path: a line through the cell centres, with a dot on each step.
        const kin::Color path_color = kin::Color::rgb(255, 214, 102);
        for (std::size_t i = 1; i < _path.size(); ++i) {
            thick_line(r, center(_path[i - 1]), center(_path[i]), path_color);
        }
        for (std::size_t i = 1; i + 1 < _path.size(); ++i) {
            draw_hex(r, _path[i], path_color, 0.22f);
        }

        draw_hex(r, _goal, kin::Color::rgb(24, 26, 32), 0.52f);
        draw_hex(r, _goal, path_color, 0.4f);
        draw_hex(r, _start, kin::Color::rgb(24, 26, 32), 0.6f);
        draw_hex(r, _start, kin::Color::rgb(236, 92, 84), 0.48f);
        if (_hover) {
            outline_hex(r, *_hover, kin::Color::rgb(255, 255, 255));
        }
        draw_hud(r);
    }

    void collect_actions(kin::InputActionContext& actions) const override {
        actions.add("orientation", "Toggle pointy-top / flat-top");
        actions.add("offset", "Toggle odd / even offset");
        actions.add("reset", "Reset unit and goal");
        actions.add("quit", "Quit");
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("orientation", orientation_name());
        json.field("offset", _layout.offset == kin::HexOffset::Odd ? "odd" : "even");
        json.key("start").begin_array().value(static_cast<kin::i64>(_start.x)).value(static_cast<kin::i64>(_start.y)).end_array();
        json.key("goal").begin_array().value(static_cast<kin::i64>(_goal.x)).value(static_cast<kin::i64>(_goal.y)).end_array();
        json.field("path_cells", static_cast<kin::i64>(_path.size()));
        json.field("path_cost", static_cast<kin::i64>(path_cost()));
        json.field("reachable_cells", static_cast<kin::i64>(_reach.size()));
    }

private:
    static std::size_t index(kin::Vec2i cell) {
        return static_cast<std::size_t>(cell.y * map_cols + cell.x);
    }

    Terrain terrain(kin::Vec2i cell) const { return _terrain[index(cell)]; }
    bool passable(kin::Vec2i cell) const { return info(terrain(cell)).cost > 0; }

    void generate_map() {
        // Terrain belongs to offset cells, so it stays put when the layout changes.
        // Distances for the blobs use one fixed layout.
        const kin::HexLayout gen{};
        _terrain.assign(static_cast<std::size_t>(map_cols * map_rows), Terrain::Plains);
        for (kin::i32 row = 0; row < map_rows; ++row) {
            for (kin::i32 col = 0; col < map_cols; ++col) {
                const kin::Hex h = gen.from_offset({col, row});
                for (const Feature& f : features) {
                    if (kin::hex_distance(h, gen.from_offset(f.cell)) <= f.radius) {
                        _terrain[index({col, row})] = f.terrain;
                    }
                }
                // A scatter of single forest cells on open plains.
                const std::uint32_t hash = static_cast<std::uint32_t>(col * 73856093) ^
                                           static_cast<std::uint32_t>(row * 19349663);
                if (_terrain[index({col, row})] == Terrain::Plains && hash % 7 == 0) {
                    _terrain[index({col, row})] = Terrain::Forest;
                }
            }
        }
        _terrain[index(default_start)] = Terrain::Plains;
        _terrain[index(default_goal)] = Terrain::Plains;
    }

    void reset() {
        _layout = {};
        _start = default_start;
        _goal = default_goal;
        fit_layout();
        replan();
    }

    kin::HexGridNav nav() const {
        return {
            .cols = map_cols,
            .rows = map_rows,
            .layout = _layout,
            .blocked = [this](kin::i32 col, kin::i32 row) { return !passable({col, row}); },
            .cost = [this](kin::i32 col, kin::i32 row) { return info(terrain({col, row})).cost; },
        };
    }

    // Neighbours change with the layout, so the path and range are recomputed too.
    void replan() {
        const kin::HexGridNav grid = nav();
        _path = kin::find_path(grid, _start, _goal);
        _reach = kin::reachable_cells(grid, _start, move_budget);
    }

    kin::i32 path_cost() const {
        kin::i32 total = 0;
        for (std::size_t i = 1; i < _path.size(); ++i) {
            total += info(terrain(_path[i])).cost;
        }
        return total;
    }

    // Scale and place the layout so the whole map fits `map_area`.
    void fit_layout() {
        kin::HexLayout unit = _layout;
        unit.size = {1.0f, 1.0f};
        unit.origin = {};
        float x0 = std::numeric_limits<float>::max(), y0 = x0;
        float x1 = std::numeric_limits<float>::lowest(), y1 = x1;
        const kin::Vec2f half{unit.hex_extent().x * 0.5f, unit.hex_extent().y * 0.5f};
        for (kin::i32 row = 0; row < map_rows; ++row) {
            for (kin::i32 col = 0; col < map_cols; ++col) {
                const kin::Vec2f p = unit.to_pixel(unit.from_offset({col, row}));
                x0 = std::min(x0, p.x - half.x);
                y0 = std::min(y0, p.y - half.y);
                x1 = std::max(x1, p.x + half.x);
                y1 = std::max(y1, p.y + half.y);
            }
        }
        const float s = std::min(map_area.w / (x1 - x0), map_area.h / (y1 - y0));
        _layout.size = {s, s};
        _layout.origin = {map_area.x + (map_area.w - (x1 - x0) * s) * 0.5f - x0 * s,
                          map_area.y + (map_area.h - (y1 - y0) * s) * 0.5f - y0 * s};
        replan();
    }

    std::optional<kin::Vec2i> cell_at(kin::Vec2f p) const {
        const kin::Vec2i cell = _layout.to_offset(_layout.from_pixel(p));
        if (cell.x < 0 || cell.x >= map_cols || cell.y < 0 || cell.y >= map_rows) {
            return std::nullopt;
        }
        return cell;
    }

    kin::Vec2f center(kin::Vec2i cell) const {
        return _layout.to_pixel(_layout.from_offset(cell));
    }

    // One white hexagon, antialiased, drawn tinted for every filled cell. Rebuilt
    // when the orientation or scale changes.
    void ensure_hex_texture(kin::Renderer2D& r) {
        const kin::Vec2f extent = _layout.hex_extent();
        const kin::i32 w = static_cast<kin::i32>(std::ceil(extent.x)) + 2;
        const kin::i32 h = static_cast<kin::i32>(std::ceil(extent.y)) + 2;
        if (_hex_texture && _texture_orientation == _layout.orientation && _texture_size == kin::Vec2i{w, h}) {
            return;
        }
        kin::HexLayout local = _layout;
        local.origin = {static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f};
        const std::array<kin::Vec2f, 6> corners = local.corners({0, 0});
        const auto inside = [&](float x, float y) {
            // Convex polygon with clockwise corners: inside when right of every edge.
            for (std::size_t i = 0; i < 6; ++i) {
                const kin::Vec2f a = corners[i];
                const kin::Vec2f b = corners[(i + 1) % 6];
                if ((b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x) < 0.0f) {
                    return false;
                }
            }
            return true;
        };
        std::vector<kin::u8> pixels(static_cast<std::size_t>(w * h * 4));
        constexpr int ss = 4; // 4 x 4 samples per pixel
        for (kin::i32 y = 0; y < h; ++y) {
            for (kin::i32 x = 0; x < w; ++x) {
                int hits = 0;
                for (int sy = 0; sy < ss; ++sy) {
                    for (int sx = 0; sx < ss; ++sx) {
                        hits += inside(x + (sx + 0.5f) / ss, y + (sy + 0.5f) / ss) ? 1 : 0;
                    }
                }
                kin::u8* px = &pixels[static_cast<std::size_t>((y * w + x) * 4)];
                px[0] = px[1] = px[2] = 255;
                px[3] = static_cast<kin::u8>(hits * 255 / (ss * ss));
            }
        }
        _hex_texture = r.create_texture_from_rgba(pixels.data(), {w, h});
        _texture_orientation = _layout.orientation;
        _texture_size = {w, h};
    }

    void draw_hex(kin::Renderer2D& r, kin::Vec2i cell, kin::Color color, float scale) const {
        const kin::Vec2f c = center(cell);
        const float w = static_cast<float>(_texture_size.x) * scale;
        const float h = static_cast<float>(_texture_size.y) * scale;
        r.draw_texture(_hex_texture, {0.0f, 0.0f, static_cast<float>(_texture_size.x), static_cast<float>(_texture_size.y)},
                       {c.x - w * 0.5f, c.y - h * 0.5f, w, h}, color);
    }

    void outline_hex(kin::Renderer2D& r, kin::Vec2i cell, kin::Color color) const {
        const std::array<kin::Vec2f, 6> corners = _layout.corners(_layout.from_offset(cell));
        for (std::size_t i = 0; i < 6; ++i) {
            r.draw_line(corners[i], corners[(i + 1) % 6], color);
        }
    }

    static void thick_line(kin::Renderer2D& r, kin::Vec2f a, kin::Vec2f b, kin::Color color) {
        for (float d = -1.0f; d <= 1.0f; d += 1.0f) {
            r.draw_line({a.x + d, a.y}, {b.x + d, b.y}, color);
            r.draw_line({a.x, a.y + d}, {b.x, b.y + d}, color);
        }
    }

    std::string_view orientation_name() const {
        return _layout.orientation == kin::HexOrientation::PointyTop ? "pointy-top" : "flat-top";
    }

    void draw_hud(kin::Renderer2D& r) const {
        const kin::ui2::Font font = kin::ui2::system_ui_font(15);
        const auto text = [&](std::string_view value, float x, float y, float points, kin::Color color) {
            kin::ui2::draw_text(r, font, value, {x, y}, points / 15.0f, color);
        };
        const kin::Color ink = kin::Color::rgb(226, 232, 240);
        const kin::Color muted = kin::Color::rgb(150, 162, 178);

        std::string layout = "HEX GRID  ";
        layout += orientation_name();
        layout += _layout.offset == kin::HexOffset::Odd ? ", odd " : ", even ";
        layout += _layout.orientation == kin::HexOrientation::PointyTop ? "rows" : "columns";
        layout += " offset";
        text(layout, 24.0f, 14.0f, 20.0f, ink);
        text("Left click: goal    Right click: move unit    Tab: flat / pointy    O: odd / even    R: reset",
             24.0f, 44.0f, 13.0f, muted);

        std::string status = _path.empty() ? std::string{"No path to the goal"}
                                           : "Path: " + std::to_string(_path.size() - 1) + " steps, cost " +
                                                 std::to_string(path_cost());
        status += "    In range (" + std::to_string(move_budget) + "): " + std::to_string(_reach.size()) + " cells";
        if (_hover) {
            const kin::Hex h = _layout.from_offset(*_hover);
            status += "    Hover: cell " + std::to_string(_hover->x) + "," + std::to_string(_hover->y) + "  hex " +
                      std::to_string(h.q) + "," + std::to_string(h.r) + "  " + std::string{info(terrain(*_hover)).name};
        }
        text(status, 24.0f, 66.0f, 13.0f, kin::Color::rgb(255, 214, 102));

        float x = 24.0f;
        for (const TerrainInfo& t : terrain_info) {
            r.fill_rect({x, 582.0f, 10.0f, 10.0f}, t.color);
            std::string label{t.name};
            label += t.cost > 0 ? " " + std::to_string(t.cost) : " blocked";
            text(label, x + 16.0f, 578.0f, 12.0f, muted);
            x += 150.0f;
        }
    }

    kin::HexLayout _layout{};
    std::vector<Terrain> _terrain;
    kin::Vec2i _start = default_start;
    kin::Vec2i _goal = default_goal;
    std::optional<kin::Vec2i> _hover;
    std::vector<kin::Vec2i> _path;
    std::vector<kin::HexReach> _reach;
    kin::Texture _hex_texture;
    kin::HexOrientation _texture_orientation = kin::HexOrientation::PointyTop;
    kin::Vec2i _texture_size{};
};

} // namespace
} // namespace demo

int main(int argc, char** argv) {
    kin::GameInfo game = demo::make_game_info();
    kin::SceneManager scenes;

    const auto build_scenes = [](kin::SceneManager& target) {
        target.push(std::make_unique<demo::HexDemoScene>());
    };
    build_scenes(scenes);

    return kin::run_scene_app({
        .window = kin::window_config(game),
        .headless = kin::parse_headless_options(argc, argv),
        .game = &game,
        .reset_scenes = build_scenes,
    }, scenes);
}
