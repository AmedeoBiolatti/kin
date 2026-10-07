#include "iso_draw.hpp"
#include "iso_math.hpp"
#include "iso_order.hpp"
#include "isometric_shader.hpp"

#include <kin/platform/input.hpp>
#include <kin/renderer/shader.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/windowed_app.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace isometric_demo {

constexpr kin::i32 MapCols = 16;
constexpr kin::i32 MapRows = 14;
constexpr kin::i32 ScreenW = 960;
constexpr kin::i32 ScreenH = 640;

constexpr kin::f32 HudH = 72.0f;
constexpr kin::f32 InitialCameraY = 116.0f;
constexpr kin::f32 RaisedTileStep = 13.0f;
constexpr kin::f32 MinZoom = 0.55f;
constexpr kin::f32 MaxZoom = 2.25f;
constexpr kin::f32 ZoomStep = 0.12f;
constexpr kin::f32 CameraSpeed = 360.0f;
constexpr kin::f32 TerrainBlendWidth = 0.18f;

enum class TileKind {
    Grass,
    Stone,
    Water,
    Block,
};

struct Tile {
    TileKind kind = TileKind::Grass;
    kin::i32 height = 0;
};

struct WorldObject {
    kin::Vec2i cell{};
    kin::i32 id = 0;
    kin::Color fill{};
    kin::Color edge{};
    kin::f32 lift = 0.0f;
    kin::f32 radius = 10.0f;
};

struct Building {
    kin::Vec2i origin{};
    kin::Vec2i footprint{1, 1};
    kin::i32 id = 0;
    kin::f32 height = 52.0f;
    kin::Color wall{};
    kin::Color roof{};
    kin::Color edge{};
};

struct RenderItem {
    enum class Kind {
        RaisedTile,
        Object,
        BuildingCell,
    };

    Kind kind = Kind::Object;
    kin::i32 index = 0;
    kin::Vec2i cell{};
    kin::i32 id = 0;
    kin::i32 depth = 0;
};

kin::Color tile_color(TileKind kind) {
    switch (kind) {
    case TileKind::Grass: return kin::Color::rgb(64, 128, 82);
    case TileKind::Stone: return kin::Color::rgb(98, 104, 112);
    case TileKind::Water: return kin::Color::rgb(38, 92, 126);
    case TileKind::Block: return kin::Color::rgb(118, 104, 88);
    }
    return kin::Color::rgb(64, 128, 82);
}

kin::Color with_alpha(kin::Color color, kin::u8 alpha) {
    return kin::Color::rgba(color.r, color.g, color.b, alpha);
}

kin::u32 hash_cell(kin::Vec2i cell, kin::u32 salt = 0) {
    kin::u32 x = static_cast<kin::u32>(cell.x) * 0x8da6b343u;
    kin::u32 y = static_cast<kin::u32>(cell.y) * 0xd8163841u;
    kin::u32 value = x ^ y ^ (salt * 0xcb1ab31fu);
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

kin::ShaderHandle create_spirv_shader(kin::Renderer2D& renderer, const char* spv_name) {
    const std::span<const unsigned char> bytes = isometric_shader(spv_name);
    if (bytes.empty()) {
        return {}; // glslc was not found when the demo was built
    }

    kin::ShaderDesc desc;
    desc.num_samplers = 1;
    desc.num_uniform_buffers = 1;
    desc.spirv = {bytes.data(), static_cast<kin::u32>(bytes.size())};
    return renderer.create_shader(desc);
}

kin::ShaderParams water_params(kin::Color color_a, kin::Color color_b, kin::f32 time, kin::Vec2i cell) {
    const auto normalized = [](kin::u8 value) {
        return static_cast<kin::f32>(value) / 255.0f;
    };
    const kin::f32 salt = static_cast<kin::f32>(hash_cell(cell, 17) & 1023u) / 1023.0f;

    kin::ShaderParams params{};
    params.uniforms = {
        normalized(color_a.r), normalized(color_a.g), normalized(color_a.b), normalized(color_a.a),
        normalized(color_b.r), normalized(color_b.g), normalized(color_b.b), normalized(color_b.a),
        time, salt, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
    };
    return params;
}

bool in_bounds(kin::Vec2i cell) {
    return cell.x >= 0 && cell.x < MapCols && cell.y >= 0 && cell.y < MapRows;
}

class IsometricDemo {
public:
    IsometricDemo() {
        reset();
    }

    void reset() {
        _view = {
            .origin = {ScreenW * 0.5f, InitialCameraY},
            .camera = {},
            .zoom = 1.0f,
        };
        _player = {2, 2};
        _hover.reset();
        _dragging = false;
        _time = 0.0f;
        build_map();
    }

    void update(kin::FrameContext& ctx) {
        if (ctx.window.close_requested() || ctx.input.pressed("quit")) {
            ctx.app.quit();
            return;
        }
        if (ctx.input.pressed("restart")) {
            reset();
            return;
        }

        const kin::Vec2f mouse = ctx.renderer.window_to_logical(ctx.input.mouse_pos());
        update_hover(mouse);
        update_zoom(ctx, mouse);
        update_camera(ctx);
        update_player(ctx);

        if (ctx.input.mouse_pressed(kin::MouseButton::Left) && _hover && walkable(*_hover)) {
            _player = *_hover;
        }

        _time += ctx.dt;
    }

    void render(kin::FrameContext& ctx) const {
        ensure_water_shader(ctx.renderer);
        draw_world(ctx.renderer);
        draw_hud(ctx.renderer);
        ctx.renderer.present();
    }

private:
    static constexpr std::array<Building, 2> DefaultBuildings{{
        {
            .origin = {1, 7},
            .footprint = {3, 2},
            .id = 100,
            .height = 58.0f,
            .wall = kin::Color::rgb(92, 104, 128),
            .roof = kin::Color::rgb(168, 116, 82),
            .edge = kin::Color::rgb(226, 196, 148),
        },
        {
            .origin = {12, 9},
            .footprint = {2, 3},
            .id = 101,
            .height = 44.0f,
            .wall = kin::Color::rgb(72, 112, 104),
            .roof = kin::Color::rgb(104, 174, 148),
            .edge = kin::Color::rgb(196, 236, 212),
        },
    }};

    static constexpr std::array<WorldObject, 3> StaticObjects{{
        {.cell = {4, 2}, .id = 2, .fill = kin::Color::rgb(244, 184, 78), .edge = kin::Color::rgb(255, 236, 164), .lift = 8.0f, .radius = 7.0f},
        {.cell = {10, 10}, .id = 3, .fill = kin::Color::rgb(92, 220, 152), .edge = kin::Color::rgb(198, 248, 210), .lift = 8.0f, .radius = 7.0f},
        {.cell = {13, 6}, .id = 4, .fill = kin::Color::rgb(238, 96, 102), .edge = kin::Color::rgb(255, 190, 176), .lift = 8.0f, .radius = 7.0f},
    }};

    Tile& at(kin::Vec2i cell) {
        return _tiles[static_cast<std::size_t>(cell.y * MapCols + cell.x)];
    }

    const Tile& at(kin::Vec2i cell) const {
        return _tiles[static_cast<std::size_t>(cell.y * MapCols + cell.x)];
    }

    void set_block(kin::Vec2i cell, kin::i32 height) {
        Tile& tile = at(cell);
        tile.kind = TileKind::Block;
        tile.height = height;
    }

    void build_map() {
        _tiles.fill(Tile{});
        for (kin::i32 row = 0; row < MapRows; ++row) {
            for (kin::i32 col = 0; col < MapCols; ++col) {
                Tile& tile = at({col, row});
                tile.kind = ((col + row) % 7 == 0) ? TileKind::Stone : TileKind::Grass;
                tile.height = 0;
            }
        }
        for (kin::i32 col = 4; col < 10; ++col) {
            at({col, 5}).kind = TileKind::Water;
        }
        for (kin::i32 row = 7; row < 11; ++row) {
            at({11, row}).kind = TileKind::Water;
        }
        set_block({6, 3}, 2);
        set_block({7, 3}, 1);
        set_block({8, 8}, 3);
        set_block({9, 8}, 2);
        set_block({12, 4}, 2);
        set_block({3, 9}, 1);
    }

    bool building_contains(const Building& building, kin::Vec2i cell) const {
        return cell.x >= building.origin.x &&
               cell.y >= building.origin.y &&
               cell.x < building.origin.x + building.footprint.x &&
               cell.y < building.origin.y + building.footprint.y;
    }

    bool occupied_by_building(kin::Vec2i cell) const {
        for (const Building& building : _buildings) {
            if (building_contains(building, cell)) {
                return true;
            }
        }
        return false;
    }

    bool walkable(kin::Vec2i cell) const {
        if (!in_bounds(cell)) {
            return false;
        }
        const Tile& tile = at(cell);
        return tile.kind != TileKind::Water && tile.kind != TileKind::Block && !occupied_by_building(cell);
    }

    void update_hover(kin::Vec2f mouse) {
        const kin::Vec2f world = _view.screen_to_world(mouse);
        const kin::Vec2i cell{
            static_cast<kin::i32>(std::floor(world.x)),
            static_cast<kin::i32>(std::floor(world.y)),
        };
        _hover = in_bounds(cell) ? std::optional<kin::Vec2i>{cell} : std::nullopt;
    }

    void update_zoom(kin::FrameContext& ctx, kin::Vec2f mouse) {
        const kin::f32 wheel = ctx.input.mouse_wheel_y();
        if (wheel == 0.0f) {
            return;
        }
        const kin::Vec2f before = _view.screen_to_world(mouse);
        _view.zoom = std::clamp(_view.zoom * std::exp(wheel * ZoomStep), MinZoom, MaxZoom);
        const kin::Vec2f after = _view.screen_to_world(mouse);
        const kin::Vec2f before_iso = _view.world_to_iso(before);
        const kin::Vec2f after_iso = _view.world_to_iso(after);
        _view.camera.x += before_iso.x - after_iso.x;
        _view.camera.y += before_iso.y - after_iso.y;
    }

    void update_camera(kin::FrameContext& ctx) {
        const kin::Vec2f mouse = ctx.renderer.window_to_logical(ctx.input.mouse_pos());
        if (ctx.input.mouse_pressed(kin::MouseButton::Right) || ctx.input.mouse_pressed(kin::MouseButton::Middle)) {
            _dragging = true;
            _last_mouse = mouse;
        }
        if (!ctx.input.mouse_held(kin::MouseButton::Right) && !ctx.input.mouse_held(kin::MouseButton::Middle)) {
            _dragging = false;
        }
        if (_dragging) {
            const kin::Vec2f delta{mouse.x - _last_mouse.x, mouse.y - _last_mouse.y};
            _view.camera.x -= delta.x / _view.zoom;
            _view.camera.y -= delta.y / _view.zoom;
            _last_mouse = mouse;
        }

        kin::Vec2f pan{};
        if (ctx.input.held("camera_left"))  pan.x -= 1.0f;
        if (ctx.input.held("camera_right")) pan.x += 1.0f;
        if (ctx.input.held("camera_up"))    pan.y -= 1.0f;
        if (ctx.input.held("camera_down"))  pan.y += 1.0f;
        if (pan.x != 0.0f || pan.y != 0.0f) {
            const kin::f32 len = std::sqrt(pan.x * pan.x + pan.y * pan.y);
            const kin::f32 speed = CameraSpeed / std::max(0.4f, _view.zoom);
            _view.camera.x += (pan.x / len) * speed * ctx.dt;
            _view.camera.y += (pan.y / len) * speed * ctx.dt;
        }
    }

    void update_player(kin::FrameContext& ctx) {
        kin::Vec2i delta{};
        if (ctx.input.pressed("move_left")) {
            delta = {-1, 0};
        } else if (ctx.input.pressed("move_right")) {
            delta = {1, 0};
        } else if (ctx.input.pressed("move_up")) {
            delta = {0, -1};
        } else if (ctx.input.pressed("move_down")) {
            delta = {0, 1};
        }
        if (delta.x == 0 && delta.y == 0) {
            return;
        }
        const kin::Vec2i next{_player.x + delta.x, _player.y + delta.y};
        if (walkable(next)) {
            _player = next;
        }
    }

    bool tile_visible(kin::Vec2i cell) const {
        const kin::Vec2f p = _view.world_to_screen({static_cast<kin::f32>(cell.x) + 0.5f, static_cast<kin::f32>(cell.y) + 0.5f});
        const kin::f32 pad = _view.tile_w * _view.zoom;
        return p.x >= -pad && p.x <= ScreenW + pad &&
               p.y >= -pad && p.y <= ScreenH - HudH + pad;
    }

    void draw_ground(kin::Renderer2D& renderer) const {
        for (kin::i32 row = 0; row < MapRows; ++row) {
            for (kin::i32 col = 0; col < MapCols; ++col) {
                const kin::Vec2i cell{col, row};
                if (!tile_visible(cell)) {
                    continue;
                }
                const Tile& tile = at(cell);
                const kin::Vec2f center = _view.world_to_screen({static_cast<kin::f32>(col) + 0.5f, static_cast<kin::f32>(row) + 0.5f});
                const kin::Color base = tile_color(tile.kind);
                kin::Color fill = base;
                kin::Color edge = with_alpha(shade(base, 18), 34);
                if (tile.height > 0) {
                    fill = shade(base, -26);
                    edge = with_alpha(shade(base, -8), 84);
                }
                if (occupied_by_building(cell)) {
                    fill = shade(base, -20);
                    edge = with_alpha(shade(base, 10), 62);
                }
                if (_hover && *_hover == cell) {
                    fill = walkable(cell) ? shade(base, 42) : kin::Color::rgb(146, 82, 76);
                    edge = kin::Color::rgb(244, 230, 150);
                }
                draw_diamond(renderer, center, _view.tile_w * _view.zoom, _view.tile_h * _view.zoom, fill, edge);
                draw_terrain_transitions(renderer, cell);
                if (!draw_water_shader(renderer, cell, center)) {
                    draw_floor_texture(renderer, cell, center, base);
                }
            }
        }
    }

    void ensure_water_shader(kin::Renderer2D& renderer) const {
        if (_water_shader_attempted) {
            return;
        }
        _water_shader_attempted = true;
        if (renderer.capabilities().materials_2d && renderer.backend_name() == "SDL_GPU") {
            _water_shader = create_spirv_shader(renderer, "water.frag.spv");
        }
    }

    bool draw_water_shader(kin::Renderer2D& renderer, kin::Vec2i cell, kin::Vec2f center) const {
        if (at(cell).kind != TileKind::Water || !_water_shader) {
            return false;
        }

        const kin::f32 width = _view.tile_w * _view.zoom;
        const kin::f32 height = _view.tile_h * _view.zoom;
        const kin::Rectf rect{center.x - width * 0.5f, center.y - height * 0.5f, width, height};
        renderer.draw_shader_surface(
            rect,
            _water_shader,
            water_params(kin::Color::rgba(34, 92, 128, 224),
                         kin::Color::rgba(72, 176, 204, 224),
                         _time,
                         cell));
        return true;
    }

    kin::Color terrain_transition_color(TileKind current, TileKind neighbor) const {
        if (current == TileKind::Water && neighbor != TileKind::Water) {
            return kin::Color::rgba(80, 174, 188, 118);
        }
        if (current != TileKind::Water && neighbor == TileKind::Water) {
            return kin::Color::rgba(174, 184, 128, 116);
        }
        if (current == TileKind::Grass && neighbor == TileKind::Stone) {
            return kin::Color::rgba(82, 124, 96, 82);
        }
        if (current == TileKind::Stone && neighbor == TileKind::Grass) {
            return kin::Color::rgba(82, 112, 94, 76);
        }
        if (current == TileKind::Block || neighbor == TileKind::Block) {
            return kin::Color::rgba(84, 74, 62, 72);
        }
        return with_alpha(shade(tile_color(neighbor), -4), 58);
    }

    void draw_edge_blend(kin::Renderer2D& renderer,
                         kin::Vec2f a,
                         kin::Vec2f b,
                         kin::Vec2f center,
                         kin::Color color) const {
        const kin::f32 width = std::clamp(TerrainBlendWidth * _view.zoom, 0.10f, 0.30f);
        const auto inward = [center, width](kin::Vec2f p) {
            return kin::Vec2f{
                p.x + (center.x - p.x) * width,
                p.y + (center.y - p.y) * width,
            };
        };
        fill_convex_quad_body(renderer, {{a, b, inward(b), inward(a)}}, color);
    }

    void draw_terrain_transitions(kin::Renderer2D& renderer, kin::Vec2i cell) const {
        const Tile& tile = at(cell);
        if (tile.height > 0 || occupied_by_building(cell)) {
            return;
        }

        const kin::Vec2f nw = _view.world_to_screen({static_cast<kin::f32>(cell.x), static_cast<kin::f32>(cell.y)});
        const kin::Vec2f ne = _view.world_to_screen({static_cast<kin::f32>(cell.x + 1), static_cast<kin::f32>(cell.y)});
        const kin::Vec2f se = _view.world_to_screen({static_cast<kin::f32>(cell.x + 1), static_cast<kin::f32>(cell.y + 1)});
        const kin::Vec2f sw = _view.world_to_screen({static_cast<kin::f32>(cell.x), static_cast<kin::f32>(cell.y + 1)});
        const kin::Vec2f center{(nw.x + se.x) * 0.5f, (nw.y + se.y) * 0.5f};

        const auto edge = [&](kin::Vec2i neighbor, kin::Vec2f a, kin::Vec2f b) {
            if (!in_bounds(neighbor)) {
                return;
            }
            const Tile& other = at(neighbor);
            if (other.kind == tile.kind || other.height > 0) {
                return;
            }
            draw_edge_blend(renderer, a, b, center, terrain_transition_color(tile.kind, other.kind));
        };

        edge({cell.x, cell.y - 1}, nw, ne);
        edge({cell.x + 1, cell.y}, ne, se);
        edge({cell.x, cell.y + 1}, se, sw);
        edge({cell.x - 1, cell.y}, sw, nw);
    }

    void draw_floor_texture(kin::Renderer2D& renderer, kin::Vec2i cell, kin::Vec2f center, kin::Color base) const {
        const kin::f32 z = _view.zoom;
        const kin::f32 half_w = _view.tile_w * z * 0.5f;
        const kin::f32 half_h = _view.tile_h * z * 0.5f;
        const kin::Color grain = kin::Color::rgba(
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(base.r) + 28, 0, 255)),
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(base.g) + 28, 0, 255)),
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(base.b) + 28, 0, 255)),
            70);
        const kin::Color shadow = kin::Color::rgba(
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(base.r) - 26, 0, 255)),
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(base.g) - 26, 0, 255)),
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(base.b) - 26, 0, 255)),
            56);

        const kin::u32 seed = hash_cell(cell);
        const kin::i32 strokes = at(cell).kind == TileKind::Water ? 2 : 3;
        for (kin::i32 i = 0; i < strokes; ++i) {
            const kin::u32 bits = hash_cell(cell, static_cast<kin::u32>(i + 1));
            const kin::f32 lane = -0.52f + 0.34f * static_cast<kin::f32>(i) + static_cast<kin::f32>((bits >> 8) & 7u) * 0.012f;
            const kin::f32 y = center.y + lane * half_h;
            const kin::f32 span = half_w * (1.0f - std::abs(lane));
            const kin::f32 inset = static_cast<kin::f32>(bits & 15u) * 0.42f * z;
            const kin::Color color = ((seed >> i) & 1u) != 0u ? grain : shadow;
            renderer.draw_line({center.x - span * 0.70f + inset, y},
                               {center.x + span * 0.64f - inset, y},
                               color);
        }
    }

    void draw_world(kin::Renderer2D& renderer) const {
        renderer.clear(12, 17, 22);
        renderer.fill_rect({0.0f, 0.0f, ScreenW, ScreenH - HudH}, kin::Color::rgb(18, 25, 30));
        draw_ground(renderer);
        draw_sorted_world_content(renderer);
    }

    kin::i32 order_depth(kin::Vec2i cell, kin::i32 type_offset) const {
        return render_depth(cell, type_offset, MapCols);
    }

    kin::i32 raised_tile_count() const {
        kin::i32 count = 0;
        for (const Tile& tile : _tiles) {
            if (tile.height > 0) {
                ++count;
            }
        }
        return count;
    }

    kin::i32 building_cell_count() const {
        kin::i32 count = 0;
        for (const Building& building : _buildings) {
            count += building.footprint.x * building.footprint.y;
        }
        return count;
    }

    WorldObject player_object() const {
        return {
            .cell = _player,
            .id = 1,
            .fill = kin::Color::rgb(92, 184, 255),
            .edge = kin::Color::rgb(218, 238, 255),
            .lift = 11.0f,
            .radius = 10.0f,
        };
    }

    void append_render_items(std::vector<RenderItem>& items) const {
        for (kin::i32 row = 0; row < MapRows; ++row) {
            for (kin::i32 col = 0; col < MapCols; ++col) {
                const kin::Vec2i cell{col, row};
                if (at(cell).height <= 0) {
                    continue;
                }
                items.push_back({
                    .kind = RenderItem::Kind::RaisedTile,
                    .cell = cell,
                    .depth = order_depth(cell, BaseContentOffset),
                });
            }
        }

        for (kin::i32 i = 0; i < static_cast<kin::i32>(_static_objects.size()); ++i) {
            const WorldObject& object = _static_objects[static_cast<std::size_t>(i)];
            items.push_back({
                .kind = RenderItem::Kind::Object,
                .index = i,
                .cell = object.cell,
                .id = object.id,
                .depth = order_depth(object.cell, MovableObjectOffset),
            });
        }

        const WorldObject player = player_object();
        items.push_back({
            .kind = RenderItem::Kind::Object,
            .index = -1,
            .cell = player.cell,
            .id = player.id,
            .depth = order_depth(player.cell, MovableObjectOffset),
        });

        for (kin::i32 i = 0; i < static_cast<kin::i32>(_buildings.size()); ++i) {
            const Building& building = _buildings[static_cast<std::size_t>(i)];
            for (kin::i32 row = 0; row < building.footprint.y; ++row) {
                for (kin::i32 col = 0; col < building.footprint.x; ++col) {
                    const kin::Vec2i cell{building.origin.x + col, building.origin.y + row};
                    items.push_back({
                        .kind = RenderItem::Kind::BuildingCell,
                        .index = i,
                        .cell = cell,
                        .id = building.id,
                        .depth = order_depth(cell, BaseContentOffset),
                    });
                }
            }
        }
    }

    void draw_building_cell(kin::Renderer2D& renderer, const Building& building, kin::Vec2i cell) const {
        const kin::f32 z = _view.zoom;
        const kin::f32 top_lift = building.height * z;
        const kin::Vec2f nw = _view.world_to_screen({static_cast<kin::f32>(cell.x), static_cast<kin::f32>(cell.y)});
        const kin::Vec2f ne = _view.world_to_screen({static_cast<kin::f32>(cell.x + 1), static_cast<kin::f32>(cell.y)});
        const kin::Vec2f se = _view.world_to_screen({static_cast<kin::f32>(cell.x + 1), static_cast<kin::f32>(cell.y + 1)});
        const kin::Vec2f sw = _view.world_to_screen({static_cast<kin::f32>(cell.x), static_cast<kin::f32>(cell.y + 1)});
        const kin::Vec2f tn{nw.x, nw.y - top_lift};
        const kin::Vec2f te{ne.x, ne.y - top_lift};
        const kin::Vec2f ts{se.x, se.y - top_lift};
        const kin::Vec2f tw{sw.x, sw.y - top_lift};

        const bool edge_tile = cell.x == building.origin.x ||
                               cell.y == building.origin.y ||
                               cell.x == building.origin.x + building.footprint.x - 1 ||
                               cell.y == building.origin.y + building.footprint.y - 1;
        if (cell.y == building.origin.y + building.footprint.y - 1) {
            fill_convex_quad(renderer, {tw, ts, se, sw}, shade(building.wall, -38), shade(building.wall, -76));
            draw_wall_texture(renderer, tw, ts, sw, se, shade(building.wall, -64));
        }
        if (cell.x == building.origin.x + building.footprint.x - 1) {
            fill_convex_quad(renderer, {te, ts, se, ne}, shade(building.wall, -20), shade(building.wall, -62));
            draw_wall_texture(renderer, te, ts, ne, se, shade(building.wall, -46));
        }
        if (cell.x == building.origin.x + building.footprint.x / 2 &&
            cell.y == building.origin.y + building.footprint.y - 1) {
            draw_door_on_south_wall(renderer, tw, ts, sw, se, 10.0f * z, 22.0f * z, shade(building.wall, -72));
        }
        fill_convex_quad_body(renderer, {tn, te, ts, tw}, edge_tile ? building.roof : shade(building.roof, 12));
        draw_roof_texture(renderer, building, cell, tn, te, ts, tw);

        if (cell.y == building.origin.y) {
            renderer.draw_line(tn, te, shade(building.edge, 8));
        }
        if (cell.x == building.origin.x + building.footprint.x - 1) {
            renderer.draw_line(te, ts, building.edge);
        }
        if (cell.y == building.origin.y + building.footprint.y - 1) {
            renderer.draw_line(tw, ts, shade(building.edge, -24));
            renderer.draw_line(tw, sw, shade(building.edge, -55));
            renderer.draw_line(ts, se, shade(building.edge, -55));
        }
        if (cell.x == building.origin.x) {
            renderer.draw_line(tn, tw, shade(building.edge, 14));
        }
        if (cell.x == building.origin.x + building.footprint.x - 1) {
            renderer.draw_line(ts, se, shade(building.edge, -55));
            renderer.draw_line(te, ne, shade(building.edge, -45));
        }
    }

    void draw_wall_texture(kin::Renderer2D& renderer,
                           kin::Vec2f top_left,
                           kin::Vec2f top_right,
                           kin::Vec2f bottom_left,
                           kin::Vec2f bottom_right,
                           kin::Color color) const {
        for (kin::i32 i = 1; i <= 2; ++i) {
            const kin::f32 t = static_cast<kin::f32>(i) / 3.0f;
            const kin::Vec2f top{
                top_left.x + (top_right.x - top_left.x) * t,
                top_left.y + (top_right.y - top_left.y) * t,
            };
            const kin::Vec2f bottom{
                bottom_left.x + (bottom_right.x - bottom_left.x) * t,
                bottom_left.y + (bottom_right.y - bottom_left.y) * t,
            };
            renderer.draw_line(top, bottom, kin::Color::rgba(color.r, color.g, color.b, 82));
        }

        const kin::Vec2f mid_left{
            top_left.x + (bottom_left.x - top_left.x) * 0.58f,
            top_left.y + (bottom_left.y - top_left.y) * 0.58f,
        };
        const kin::Vec2f mid_right{
            top_right.x + (bottom_right.x - top_right.x) * 0.58f,
            top_right.y + (bottom_right.y - top_right.y) * 0.58f,
        };
        renderer.draw_line(mid_left, mid_right, kin::Color::rgba(color.r, color.g, color.b, 64));
    }

    void draw_roof_texture(kin::Renderer2D& renderer,
                           const Building& building,
                           kin::Vec2i cell,
                           kin::Vec2f n,
                           kin::Vec2f e,
                           kin::Vec2f s,
                           kin::Vec2f w) const {
        const kin::Color grain = kin::Color::rgba(
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(building.roof.r) + 24, 0, 255)),
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(building.roof.g) + 20, 0, 255)),
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(building.roof.b) + 14, 0, 255)),
            78);
        const kin::Color groove = kin::Color::rgba(
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(building.roof.r) - 34, 0, 255)),
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(building.roof.g) - 30, 0, 255)),
            static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(building.roof.b) - 24, 0, 255)),
            72);

        const kin::u32 bits = hash_cell(cell, static_cast<kin::u32>(building.id));
        const kin::f32 t0 = 0.30f + static_cast<kin::f32>(bits & 7u) * 0.012f;
        const kin::f32 t1 = 0.67f - static_cast<kin::f32>((bits >> 4) & 7u) * 0.012f;
        const auto lerp = [](kin::Vec2f a, kin::Vec2f b, kin::f32 t) {
            return kin::Vec2f{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
        };
        renderer.draw_line(lerp(n, w, t0), lerp(e, s, t0), grain);
        renderer.draw_line(lerp(n, e, t1), lerp(w, s, t1), groove);
    }

    void draw_raised_tile(kin::Renderer2D& renderer, kin::Vec2i cell) const {
        const Tile& tile = at(cell);
        if (tile.height <= 0) {
            return;
        }
        const kin::f32 lift = static_cast<kin::f32>(tile.height) * RaisedTileStep * _view.zoom;
        const kin::Vec2f center = _view.world_to_screen({
            static_cast<kin::f32>(cell.x) + 0.5f,
            static_cast<kin::f32>(cell.y) + 0.5f,
        });
        const kin::Vec2f lifted{center.x, center.y - lift};
        const kin::Color base = tile_color(tile.kind);
        draw_column_sides(renderer, lifted, _view.tile_w * _view.zoom, _view.tile_h * _view.zoom, lift, base);
        draw_diamond(renderer, lifted, _view.tile_w * _view.zoom, _view.tile_h * _view.zoom, base, shade(base, 34));
    }

    void draw_world_object(kin::Renderer2D& renderer, const WorldObject& object) const {
        if (!tile_visible(object.cell)) {
            return;
        }
        const Tile& tile = at(object.cell);
        const kin::f32 tile_lift = static_cast<kin::f32>(tile.height) * RaisedTileStep * _view.zoom;
        const kin::Vec2f base = _view.world_to_screen({
            static_cast<kin::f32>(object.cell.x) + 0.5f,
            static_cast<kin::f32>(object.cell.y) + 0.5f,
        });
        const kin::Vec2f foot{base.x, base.y - tile_lift};
        draw_diamond(renderer,
                     foot,
                     _view.tile_w * _view.zoom * 0.28f,
                     _view.tile_h * _view.zoom * 0.28f,
                     kin::Color::rgba(10, 16, 18, 112),
                     kin::Color::rgba(10, 16, 18, 148));
        draw_disk(renderer,
                  {foot.x, foot.y - object.lift * _view.zoom},
                  object.radius * _view.zoom,
                  object.fill,
                  object.edge);
    }

    void draw_sorted_world_content(kin::Renderer2D& renderer) const {
        std::vector<RenderItem> items;
        items.reserve(static_cast<std::size_t>(raised_tile_count() + building_cell_count() + _static_objects.size() + 1));
        append_render_items(items);

        std::ranges::stable_sort(items, [](const RenderItem& a, const RenderItem& b) {
            if (a.depth != b.depth) return a.depth < b.depth;
            if (a.cell.x != b.cell.x) return a.cell.x < b.cell.x;
            if (a.cell.y != b.cell.y) return a.cell.y < b.cell.y;
            return a.id < b.id;
        });

        for (const RenderItem& item : items) {
            switch (item.kind) {
            case RenderItem::Kind::RaisedTile:
                if (tile_visible(item.cell)) {
                    draw_raised_tile(renderer, item.cell);
                }
                break;
            case RenderItem::Kind::BuildingCell:
                if (tile_visible(item.cell)) {
                    draw_building_cell(renderer, _buildings[static_cast<std::size_t>(item.index)], item.cell);
                }
                break;
            case RenderItem::Kind::Object:
                draw_world_object(renderer, item.index < 0 ? player_object() : _static_objects[static_cast<std::size_t>(item.index)]);
                break;
            }
        }
    }

    void draw_digit(kin::Renderer2D& renderer, kin::Vec2f p, kin::i32 value, kin::Color color) const {
        static constexpr std::array<kin::u8, 10> masks{
            0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
            0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111,
        };
        const kin::u8 mask = masks[static_cast<std::size_t>(std::clamp(value, 0, 9))];
        const kin::f32 w = 10.0f;
        const kin::f32 h = 18.0f;
        const kin::f32 t = 2.0f;
        const auto seg = [&](kin::i32 bit, kin::Rectf r) {
            if ((mask & (1 << bit)) != 0) {
                renderer.fill_rect(r, color);
            }
        };
        seg(0, {p.x + t, p.y, w - t * 2.0f, t});
        seg(1, {p.x + w - t, p.y + t, t, h * 0.5f - t});
        seg(2, {p.x + w - t, p.y + h * 0.5f, t, h * 0.5f - t});
        seg(3, {p.x + t, p.y + h - t, w - t * 2.0f, t});
        seg(4, {p.x, p.y + h * 0.5f, t, h * 0.5f - t});
        seg(5, {p.x, p.y + t, t, h * 0.5f - t});
        seg(6, {p.x + t, p.y + h * 0.5f - t * 0.5f, w - t * 2.0f, t});
    }

    void draw_number(kin::Renderer2D& renderer, kin::Vec2f p, kin::i32 value, kin::Color color) const {
        value = std::clamp(value, 0, 99);
        draw_digit(renderer, p, value / 10, color);
        draw_digit(renderer, {p.x + 14.0f, p.y}, value % 10, color);
    }

    void draw_hud(kin::Renderer2D& renderer) const {
        const kin::f32 y = ScreenH - HudH;
        renderer.fill_rect({0.0f, y, ScreenW, HudH}, kin::Color::rgb(16, 20, 27));
        renderer.draw_line({0.0f, y}, {ScreenW, y}, kin::Color::rgb(82, 96, 110));

        const kin::Color label = kin::Color::rgb(94, 108, 122);
        const kin::Color value = kin::Color::rgb(218, 230, 238);
        for (kin::i32 i = 0; i < 4; ++i) {
            renderer.draw_rect({24.0f + i * 138.0f, y + 16.0f, 104.0f, 38.0f}, label);
        }
        draw_number(renderer, {38.0f, y + 26.0f}, _player.x, value);
        draw_number(renderer, {76.0f, y + 26.0f}, _player.y, value);
        if (_hover) {
            draw_number(renderer, {176.0f, y + 26.0f}, _hover->x, value);
            draw_number(renderer, {214.0f, y + 26.0f}, _hover->y, value);
        } else {
            renderer.draw_line({178.0f, y + 34.0f}, {236.0f, y + 34.0f}, label);
        }
        draw_number(renderer, {314.0f, y + 26.0f}, static_cast<kin::i32>(std::round(_view.zoom * 10.0f)), value);

        const kin::f32 controls_x = 600.0f;
        for (kin::i32 i = 0; i < 9; ++i) {
            const kin::Color c = i < 4 ? kin::Color::rgb(92, 184, 255) : (i < 7 ? kin::Color::rgb(244, 184, 78) : kin::Color::rgb(92, 220, 152));
            renderer.fill_rect({controls_x + i * 24.0f, y + 22.0f, 16.0f, 16.0f}, c);
            renderer.draw_rect({controls_x + i * 24.0f, y + 22.0f, 16.0f, 16.0f}, value);
        }
        renderer.draw_line({controls_x, y + 49.0f}, {controls_x + 88.0f, y + 49.0f}, kin::Color::rgb(92, 184, 255));
        renderer.draw_line({controls_x + 96.0f, y + 49.0f}, {controls_x + 160.0f, y + 49.0f}, kin::Color::rgb(244, 184, 78));
        renderer.draw_line({controls_x + 168.0f, y + 49.0f}, {controls_x + 208.0f, y + 49.0f}, kin::Color::rgb(92, 220, 152));
    }

    std::array<Tile, MapCols * MapRows> _tiles{};
    std::array<Building, 2> _buildings = DefaultBuildings;
    std::array<WorldObject, 3> _static_objects = StaticObjects;
    IsoView _view{};
    kin::Vec2i _player{2, 2};
    std::optional<kin::Vec2i> _hover;
    bool _dragging = false;
    kin::Vec2f _last_mouse{};
    kin::f32 _time = 0.0f;
    mutable bool _water_shader_attempted = false;
    mutable kin::ShaderHandle _water_shader{};
};

kin::GameInfo make_game_info() {
    kin::InputMap input;
    input.bind("quit", kin::Key::Escape);
    input.bind("restart", kin::Key::Enter);
    input.bind("move_up", {kin::Key::Up, kin::Key::W});
    input.bind("move_down", {kin::Key::Down, kin::Key::S});
    input.bind("move_left", {kin::Key::Left, kin::Key::A});
    input.bind("move_right", {kin::Key::Right, kin::Key::D});
    input.bind("camera_up", kin::Key::I);
    input.bind("camera_down", kin::Key::K);
    input.bind("camera_left", kin::Key::J);
    input.bind("camera_right", kin::Key::L);

    return {
        .id = "isometric_demo",
        .title = "Kin Isometric Demo",
        .version = "0.1",
        .description = "A small 2:1 isometric projection and picking prototype.",
        .window = {
            .width = ScreenW,
            .height = ScreenH,
            .logical_width = ScreenW,
            .logical_height = ScreenH,
            .resizable = true,
        },
        .tags = {"sample", "isometric", "prototype"},
        .input_map = input,
    };
}

} // namespace isometric_demo

int main(int argc, char** argv) {
    const kin::GameInfo game = isometric_demo::make_game_info();
    kin::WindowedAppConfig config = kin::window_config(game);
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--smoke") {
            config.hidden = true;
            config.max_frames = 2;
        }
    }
    isometric_demo::IsometricDemo demo;
    kin::run_windowed_app(config,
                          [&](kin::FrameContext& ctx) {
                              demo.update(ctx);
                          },
                          [&](kin::FrameContext& ctx) {
                              demo.render(ctx);
                          });
    return 0;
}
