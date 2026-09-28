#include <kin/tilemap/tilemap.hpp>

#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace kin {
namespace {

std::vector<std::string_view> split_ws(std::string_view line) {
    std::vector<std::string_view> result;
    std::size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        const std::size_t begin = pos;
        while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        if (begin < pos) {
            result.push_back(line.substr(begin, pos - begin));
        }
    }
    return result;
}

bool parse_i32(std::string_view text, i32& out) {
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

bool parse_u16(std::string_view text, u16& out) {
    u32 value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value > std::numeric_limits<u16>::max()) {
        return false;
    }
    out = static_cast<u16>(value);
    return true;
}

bool parse_f32(std::string_view text, f32& out) {
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

Color parse_color(std::string_view text, Color fallback) {
    const std::size_t first = text.find(',');
    const std::size_t second = first == std::string_view::npos ? std::string_view::npos : text.find(',', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) {
        return fallback;
    }
    i32 r = 0;
    i32 g = 0;
    i32 b = 0;
    if (!parse_i32(text.substr(0, first), r) ||
        !parse_i32(text.substr(first + 1, second - first - 1), g) ||
        !parse_i32(text.substr(second + 1), b)) {
        return fallback;
    }
    return Color::rgb(static_cast<u8>(std::clamp(r, 0, 255)),
                      static_cast<u8>(std::clamp(g, 0, 255)),
                      static_cast<u8>(std::clamp(b, 0, 255)));
}

Vec2f parse_vec2f_text(std::string_view text, Vec2f fallback) {
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) {
        return fallback;
    }
    f32 x = 0.0f;
    f32 y = 0.0f;
    if (!parse_f32(text.substr(0, comma), x) || !parse_f32(text.substr(comma + 1), y)) {
        return fallback;
    }
    return {x, y};
}

std::string color_text(Color color) {
    return std::to_string(color.r) + "," + std::to_string(color.g) + "," + std::to_string(color.b);
}

Color color_from_json(const JsonValue* value, Color fallback) {
    if (!value || !value->is_array() || value->items().size() < 3) {
        return fallback;
    }
    const auto& items = value->items();
    return Color::rgb(static_cast<u8>(std::clamp<i64>(items[0].as_int(fallback.r), 0, 255)),
                      static_cast<u8>(std::clamp<i64>(items[1].as_int(fallback.g), 0, 255)),
                      static_cast<u8>(std::clamp<i64>(items[2].as_int(fallback.b), 0, 255)));
}

Vec2f vec2_from_json(const JsonValue& value) {
    if (value.is_array() && value.items().size() >= 2) {
        return {
            static_cast<f32>(value.items()[0].as_number()),
            static_cast<f32>(value.items()[1].as_number()),
        };
    }
    if (value.is_object()) {
        return {
            static_cast<f32>(value.number_at("x")),
            static_cast<f32>(value.number_at("y")),
        };
    }
    return {};
}

bool truthy(std::string_view value) {
    return value == "1" || value == "true" || value == "yes" || value == "solid";
}

std::vector<std::string> split_csv(std::string_view text) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(',', begin);
        const std::string_view item = text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
        if (!item.empty()) {
            result.emplace_back(item);
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

u64 pass_mask_for_layer(i32 layer) {
    if (layer >= layer_value(RenderLayer::Debug)) {
        return render_pass_mask::debug;
    }
    if (layer >= layer_value(RenderLayer::UI)) {
        return render_pass_mask::ui;
    }
    if (layer >= layer_value(RenderLayer::Effects)) {
        return render_pass_mask::effects;
    }
    return render_pass_mask::world;
}

Rectf visible_world_rect(const TileMap& map, const RenderView& view) {
    if (!view.culling_enabled) {
        return {0.0f, 0.0f, map.world_size().x, map.world_size().y};
    }
    if (view.camera) {
        return view.camera->visible_rect(view.cull_padding);
    }
    if (view.cull_rect.w > 0.0f && view.cull_rect.h > 0.0f) {
        return {
            view.cull_rect.x - view.cull_padding,
            view.cull_rect.y - view.cull_padding,
            view.cull_rect.w + view.cull_padding * 2.0f,
            view.cull_rect.h + view.cull_padding * 2.0f,
        };
    }
    return {0.0f, 0.0f, map.world_size().x, map.world_size().y};
}

TileCell cell_in_layer(const TileLayer& layer, i32 cols, i32 rows, i32 col, i32 row) {
    if (col < 0 || row < 0 || col >= cols || row >= rows) {
        return {};
    }
    const std::size_t idx = static_cast<std::size_t>(row * cols + col);
    return idx < layer.cells.size() ? layer.cells[idx] : TileCell{};
}

const TileDefinition* tile_definition_in_layer(const TileMap& map, const TileLayer& layer, i32 col, i32 row) {
    return map.tile(cell_in_layer(layer, map.cols, map.rows, col, row).tile_id);
}

bool solid_in_layer(const TileMap& map, const TileLayer& layer, i32 col, i32 row) {
    const TileDefinition* definition = tile_definition_in_layer(map, layer, col, row);
    return definition ? definition->solid : false;
}

bool walkable_in_layer(const TileMap& map, const TileLayer& layer, i32 col, i32 row) {
    const TileDefinition* definition = tile_definition_in_layer(map, layer, col, row);
    return definition ? definition->is_walkable() : true;
}

i32 movement_cost_in_layer(const TileMap& map, const TileLayer& layer, i32 col, i32 row) {
    const TileDefinition* definition = tile_definition_in_layer(map, layer, col, row);
    return definition ? std::max(1, definition->cost) : 10;
}

void append_tile_render_commands(const TileMap& map,
                                 const TileLayer& tile_layer,
                                 i32 col,
                                 i32 row,
                                 i32 render_layer,
                                 i32 order,
                                 bool draw_top_edge,
                                 u64 pass_mask,
                                 const SpriteCatalog* sprites,
                                 std::vector<RenderCommand>& out) {
    const TileDefinition* definition = tile_definition_in_layer(map, tile_layer, col, row);
    if (!definition) {
        return;
    }

    const Rectf rect = map.tile_rect(col, row);
    const RenderKey key{
        .layer = render_layer,
        .order = order,
        .y = rect.y + rect.h,
        .use_y = tile_layer.y_sort,
        .pass_mask = pass_mask,
    };

    ResolvedSprite resolved;
    if (sprites &&
        !definition->sprite_id.empty() &&
        sprites->resolve(definition->sprite_id, resolved)) {
        Rectf sprite_rect = rect;
        sprite_rect.x += definition->render_offset_px.x;
        sprite_rect.y += definition->render_offset_px.y;
        if (definition->render_size_px.x > 0.0f) {
            sprite_rect.w = definition->render_size_px.x;
        }
        if (definition->render_size_px.y > 0.0f) {
            sprite_rect.h = definition->render_size_px.y;
        }
        out.push_back({
            .type = RenderCommandType::Sprite,
            .key = key,
            .rect = sprite_rect,
            .color = colors::white,
            .sprite = resolved.sprite,
        });
        return;
    }

    out.push_back({
        .type = RenderCommandType::FillRect,
        .key = key,
        .rect = rect,
        .color = definition->color,
    });
    if (draw_top_edge && !solid_in_layer(map, tile_layer, col, row - 1)) {
        out.push_back({
            .type = RenderCommandType::FillRect,
            .key = key,
            .rect = {rect.x, rect.y, rect.w, std::min(2.0f, rect.h)},
            .color = definition->top_edge_color,
        });
    }
}

bool looks_like_json(std::string_view text) {
    const std::size_t pos = text.find_first_not_of(" \t\r\n");
    return pos != std::string_view::npos && text[pos] == '{';
}

TileRect cell_rect(Vec2i cell) {
    return {cell.x, cell.y, 1, 1};
}

TileRect clipped_rect(TileRect rect, i32 cols, i32 rows) {
    const i32 x0 = std::clamp(rect.x, 0, cols);
    const i32 y0 = std::clamp(rect.y, 0, rows);
    const i32 x1 = std::clamp(rect.x + rect.w, 0, cols);
    const i32 y1 = std::clamp(rect.y + rect.h, 0, rows);
    if (x1 <= x0 || y1 <= y0) {
        return {};
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

bool rect_empty(TileRect rect) {
    return rect.w <= 0 || rect.h <= 0;
}

struct DirtyAccumulator {
    TileRect rect{};
    bool dirty = false;

    void include(Vec2i cell) {
        if (!dirty) {
            rect = cell_rect(cell);
            dirty = true;
            return;
        }
        const i32 x0 = std::min(rect.x, cell.x);
        const i32 y0 = std::min(rect.y, cell.y);
        const i32 x1 = std::max(rect.x + rect.w, cell.x + 1);
        const i32 y1 = std::max(rect.y + rect.h, cell.y + 1);
        rect = {x0, y0, x1 - x0, y1 - y0};
    }
};

struct CellWriteResult {
    bool accepted = false;
    bool changed = false;
};

bool cell_in_bounds(Vec2i cell, i32 cols, i32 rows) {
    return cell.x >= 0 && cell.x < cols && cell.y >= 0 && cell.y < rows;
}

std::size_t cell_index(Vec2i cell, i32 cols) {
    return static_cast<std::size_t>(cell.y * cols + cell.x);
}

CellWriteResult write_cell(TileLayer& layer, i32 cols, i32 rows, Vec2i cell, TileCell tile, DirtyAccumulator* dirty) {
    if (!cell_in_bounds(cell, cols, rows)) {
        return {};
    }
    const std::size_t i = cell_index(cell, cols);
    if (i >= layer.cells.size()) {
        return {};
    }
    TileCell& existing = layer.cells[i];
    if (existing == tile) {
        return {.accepted = true};
    }
    existing = tile;
    if (dirty) {
        dirty->include(cell);
    }
    return {.accepted = true, .changed = true};
}

struct RectAccumulator {
    TileRect rect{};
    bool any = false;

    void include(Vec2i cell) {
        if (!any) {
            rect = cell_rect(cell);
            any = true;
            return;
        }
        const i32 x0 = std::min(rect.x, cell.x);
        const i32 y0 = std::min(rect.y, cell.y);
        const i32 x1 = std::max(rect.x + rect.w, cell.x + 1);
        const i32 y1 = std::max(rect.y + rect.h, cell.y + 1);
        rect = {x0, y0, x1 - x0, y1 - y0};
    }
};

template <typename Fn>
void visit_layers(const TileMap& map, std::string_view layer_id, Fn&& fn) {
    if (!layer_id.empty()) {
        if (const TileLayer* tile_layer = map.layer(layer_id)) {
            fn(*tile_layer);
        }
        return;
    }
    for (const TileLayer& tile_layer : map.layers) {
        fn(tile_layer);
    }
}

template <typename Fn>
void visit_layer_cells(const TileLayer& layer, i32 cols, i32 rows, Fn&& fn) {
    for (i32 row = 0; row < rows; ++row) {
        for (i32 col = 0; col < cols; ++col) {
            const Vec2i cell{col, row};
            const std::size_t i = cell_index(cell, cols);
            if (i < layer.cells.size()) {
                fn(cell, layer.cells[i]);
            }
        }
    }
}

bool rects_overlap(Rectf a, Rectf b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

std::size_t chunk_index(i32 chunk_col, i32 chunk_row, i32 chunk_cols) {
    return static_cast<std::size_t>(chunk_row * chunk_cols + chunk_col);
}

TileRect chunk_bounds(i32 chunk_col, i32 chunk_row, Vec2i chunk_size, i32 cols, i32 rows) {
    const i32 x = chunk_col * chunk_size.x;
    const i32 y = chunk_row * chunk_size.y;
    return {
        x,
        y,
        std::min(chunk_size.x, cols - x),
        std::min(chunk_size.y, rows - y),
    };
}

TileRect tile_rect_to_chunk_rect(TileRect rect, Vec2i chunk_size, i32 chunk_cols, i32 chunk_rows) {
    rect = clipped_rect(rect, chunk_cols * chunk_size.x, chunk_rows * chunk_size.y);
    if (rect_empty(rect)) {
        return {};
    }
    const i32 x0 = std::clamp(rect.x / chunk_size.x, 0, chunk_cols - 1);
    const i32 y0 = std::clamp(rect.y / chunk_size.y, 0, chunk_rows - 1);
    const i32 x1 = std::clamp((rect.x + rect.w - 1) / chunk_size.x, 0, chunk_cols - 1);
    const i32 y1 = std::clamp((rect.y + rect.h - 1) / chunk_size.y, 0, chunk_rows - 1);
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

TileRect visible_tile_rect(const TileMap& map, const RenderView& view) {
    const Rectf visible = visible_world_rect(map, view);
    const i32 col0 = std::max(0, map.world_col(visible.x));
    const i32 row0 = std::max(0, map.world_row(visible.y));
    const i32 col1 = std::min(map.cols - 1, map.world_col(visible.x + visible.w + static_cast<f32>(map.tile_w - 1)));
    const i32 row1 = std::min(map.rows - 1, map.world_row(visible.y + visible.h + static_cast<f32>(map.tile_h - 1)));
    if (col1 < col0 || row1 < row0) {
        return {};
    }
    return {col0, row0, col1 - col0 + 1, row1 - row0 + 1};
}

bool same_rect_span(Rectf a, Rectf b) {
    return a.x == b.x && a.w == b.w && a.y + a.h == b.y;
}

std::string terrain_mask_text(u8 mask) {
    return std::to_string(static_cast<i32>(mask));
}

void write_color(JsonWriter& out, Color color) {
    out.begin_array().value(static_cast<i32>(color.r)).value(static_cast<i32>(color.g)).value(static_cast<i32>(color.b)).end_array();
}

void write_vec2(JsonWriter& out, Vec2f value) {
    out.begin_array().value(static_cast<f64>(value.x)).value(static_cast<f64>(value.y)).end_array();
}

void write_metadata_value(JsonWriter& out, const TileMetadataValue& value) {
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) {
            out.value(std::string_view{item});
        } else if constexpr (std::is_same_v<T, f32>) {
            out.value(static_cast<f64>(item));
        } else {
            out.value(item);
        }
    }, value);
}

TileMetadataValue metadata_value_from_json(const JsonValue& value) {
    if (value.is_bool()) {
        return value.as_bool();
    }
    if (value.is_number()) {
        const f64 number = value.as_number();
        const i32 integer = static_cast<i32>(number);
        return static_cast<f64>(integer) == number ? TileMetadataValue{integer} : TileMetadataValue{static_cast<f32>(number)};
    }
    if (value.is_string()) {
        return value.as_string();
    }
    return {};
}

void write_tile_definition(JsonWriter& out, const TileDefinition& definition, u16 tile_id) {
    out.begin_object()
        .field("tile_id", static_cast<i32>(tile_id))
        .field("id", std::string_view{definition.id})
        .field("solid", definition.solid)
        .field("cost", definition.cost)
        .field("blocks_sight", definition.blocks_sight);
    if (definition.walkable.has_value()) {
        out.field("walkable", *definition.walkable);
    }
    if (!definition.sprite_id.empty()) {
        out.field("sprite", std::string_view{definition.sprite_id});
    }
    if (definition.render_offset_px.x != 0.0f || definition.render_offset_px.y != 0.0f) {
        out.key("render_offset_px");
        write_vec2(out, definition.render_offset_px);
    }
    if (definition.render_size_px.x != 0.0f || definition.render_size_px.y != 0.0f) {
        out.key("render_size_px");
        write_vec2(out, definition.render_size_px);
    }
    if (!definition.terrain.empty()) {
        out.field("terrain", std::string_view{definition.terrain});
    }
    out.key("color");
    write_color(out, definition.color);
    out.key("top_edge_color");
    write_color(out, definition.top_edge_color);
    if (!definition.tags.empty()) {
        out.key("tags").begin_array();
        for (const std::string& tag : definition.tags) {
            out.value(std::string_view{tag});
        }
        out.end_array();
    }
    if (!definition.metadata.empty()) {
        out.key("metadata").begin_object();
        for (const auto& [key, value] : definition.metadata) {
            out.key(key);
            write_metadata_value(out, value);
        }
        out.end_object();
    }
    if (!definition.collision_polygon.empty()) {
        out.key("collision_polygon").begin_array();
        for (Vec2f point : definition.collision_polygon) {
            write_vec2(out, point);
        }
        out.end_array();
    }
    out.end_object();
}

void read_tile_definition(TileSet& tileset, const JsonValue& value) {
    if (!value.is_object()) {
        return;
    }
    const i64 raw_id = value.int_at("tile_id", 0);
    if (raw_id <= 0 || raw_id > std::numeric_limits<u16>::max()) {
        return;
    }
    const u16 tile_id = static_cast<u16>(raw_id);
    if (tileset.size() <= tile_id) {
        tileset.resize(static_cast<std::size_t>(tile_id) + 1);
    }
    TileDefinition& definition = tileset.tiles[tile_id];
    definition.id = value.string_at("id", definition.id);
    definition.solid = value.bool_at("solid", definition.solid);
    if (const JsonValue* walkable = value.find("walkable"); walkable && walkable->is_bool()) {
        definition.walkable = walkable->as_bool();
    }
    definition.cost = static_cast<i32>(value.int_at("cost", definition.cost));
    definition.blocks_sight = value.bool_at("blocks_sight", definition.blocks_sight);
    definition.sprite_id = value.string_at("sprite", definition.sprite_id);
    if (const JsonValue* render_offset = value.find("render_offset_px")) {
        definition.render_offset_px = vec2_from_json(*render_offset);
    }
    if (const JsonValue* render_size = value.find("render_size_px")) {
        definition.render_size_px = vec2_from_json(*render_size);
    }
    definition.terrain = value.string_at("terrain", definition.terrain);
    definition.color = color_from_json(value.find("color"), definition.color);
    definition.top_edge_color = color_from_json(value.find("top_edge_color"), definition.top_edge_color);
    definition.tags.clear();
    if (const JsonValue* tags = value.find("tags"); tags && tags->is_array()) {
        for (const JsonValue& tag : tags->items()) {
            if (tag.is_string()) {
                definition.tags.push_back(tag.as_string());
            }
        }
    }
    definition.metadata.clear();
    if (const JsonValue* metadata = value.find("metadata"); metadata && metadata->is_object()) {
        for (const auto& [key, member] : metadata->members()) {
            if (member.is_bool() || member.is_number() || member.is_string()) {
                definition.metadata.insert_or_assign(key, metadata_value_from_json(member));
            }
        }
    }
    definition.collision_polygon.clear();
    if (const JsonValue* polygon = value.find("collision_polygon"); polygon && polygon->is_array()) {
        for (const JsonValue& point : polygon->items()) {
            definition.collision_polygon.push_back(vec2_from_json(point));
        }
    }
}

TileCell read_cell_value(const JsonValue& value) {
    if (value.is_number()) {
        const i64 tile_id = value.as_int();
        if (tile_id > 0 && tile_id <= std::numeric_limits<u16>::max()) {
            return TileCell{.tile_id = static_cast<u16>(tile_id)};
        }
        return {};
    }
    if (!value.is_object()) {
        return {};
    }
    TileCell cell;
    const i64 tile_id = value.int_at("tile", 0);
    const i64 variant_id = value.int_at("variant", 0);
    const i64 flags = value.int_at("flags", 0);
    if (tile_id > 0 && tile_id <= std::numeric_limits<u16>::max()) {
        cell.tile_id = static_cast<u16>(tile_id);
    }
    if (variant_id > 0 && variant_id <= std::numeric_limits<u16>::max()) {
        cell.variant_id = static_cast<u16>(variant_id);
    }
    if (flags > 0 && flags <= std::numeric_limits<u16>::max()) {
        cell.flags = static_cast<u16>(flags);
    }
    return cell;
}

void write_cell_value(JsonWriter& out, TileCell cell) {
    if (cell.variant_id == 0 && cell.flags == 0) {
        out.value(static_cast<i32>(cell.tile_id));
        return;
    }
    out.begin_object()
        .field("tile", static_cast<i32>(cell.tile_id))
        .field("variant", static_cast<i32>(cell.variant_id))
        .field("flags", static_cast<i32>(cell.flags))
        .end_object();
}

void write_pattern(JsonWriter& out, const TilePattern& pattern) {
    out.begin_object()
        .field("id", std::string_view{pattern.id});
    out.key("bounds").begin_object()
        .field("x", pattern.bounds.x)
        .field("y", pattern.bounds.y)
        .field("w", pattern.bounds.w)
        .field("h", pattern.bounds.h)
        .end_object();
    out.key("cells").begin_array();
    for (const TilePatternCell& pattern_cell : pattern.cells) {
        out.begin_object();
        out.key("offset").begin_object()
            .field("x", pattern_cell.offset.x)
            .field("y", pattern_cell.offset.y)
            .end_object();
        out.key("cell");
        write_cell_value(out, pattern_cell.cell);
        out.end_object();
    }
    out.end_array().end_object();
}

void write_terrain_variant(JsonWriter& out, const TerrainVariant& variant) {
    out.begin_object()
        .field("terrain", std::string_view{variant.terrain})
        .field("mask", static_cast<i32>(variant.mask))
        .field("tile_id", static_cast<i32>(variant.tile_id))
        .field("fallback", variant.fallback)
        .end_object();
}

TerrainVariant read_terrain_variant(const JsonValue& value) {
    TerrainVariant variant;
    if (!value.is_object()) {
        return variant;
    }
    variant.terrain = value.string_at("terrain");
    const i64 mask = value.int_at("mask", 0);
    const i64 tile_id = value.int_at("tile_id", 0);
    variant.mask = static_cast<u8>(std::clamp<i64>(mask, 0, 15));
    if (tile_id > 0 && tile_id <= std::numeric_limits<u16>::max()) {
        variant.tile_id = static_cast<u16>(tile_id);
    }
    variant.fallback = value.bool_at("fallback", false);
    return variant;
}

TilePattern read_pattern(const JsonValue& value) {
    TilePattern pattern;
    if (!value.is_object()) {
        return pattern;
    }
    pattern.id = value.string_at("id");
    if (const JsonValue* bounds = value.find("bounds"); bounds && bounds->is_object()) {
        pattern.bounds = {
            static_cast<i32>(bounds->int_at("x", 0)),
            static_cast<i32>(bounds->int_at("y", 0)),
            static_cast<i32>(bounds->int_at("w", 0)),
            static_cast<i32>(bounds->int_at("h", 0)),
        };
    }
    if (const JsonValue* cells = value.find("cells"); cells && cells->is_array()) {
        for (const JsonValue& item : cells->items()) {
            if (!item.is_object()) {
                continue;
            }
            TilePatternCell pattern_cell;
            if (const JsonValue* offset = item.find("offset"); offset && offset->is_object()) {
                pattern_cell.offset = {
                    static_cast<i32>(offset->int_at("x", 0)),
                    static_cast<i32>(offset->int_at("y", 0)),
                };
            }
            if (const JsonValue* cell = item.find("cell")) {
                pattern_cell.cell = read_cell_value(*cell);
            }
            pattern.cells.push_back(pattern_cell);
        }
    }
    return pattern;
}

TileMap load_json_tilemap(const JsonValue& root) {
    TileMap map;
    if (!root.is_object()) {
        return map;
    }
    const JsonValue* size = root.find("size");
    const JsonValue* tile_size = root.find("tile_size");
    const i32 cols = static_cast<i32>(size ? size->int_at("cols", 0) : root.int_at("cols", 0));
    const i32 rows = static_cast<i32>(size ? size->int_at("rows", 0) : root.int_at("rows", 0));
    map.resize(cols, rows);
    map.tile_w = static_cast<i32>(tile_size ? tile_size->int_at("w", map.tile_w) : root.int_at("tile_w", map.tile_w));
    map.tile_h = static_cast<i32>(tile_size ? tile_size->int_at("h", map.tile_h) : root.int_at("tile_h", map.tile_h));

    if (const JsonValue* tileset = root.find("tileset"); tileset && tileset->is_object()) {
        if (const JsonValue* tiles = tileset->find("tiles"); tiles && tiles->is_array()) {
            for (const JsonValue& tile : tiles->items()) {
                read_tile_definition(map.tileset, tile);
            }
        }
        if (const JsonValue* terrain_variants = tileset->find("terrain_variants"); terrain_variants && terrain_variants->is_array()) {
            for (const JsonValue& variant : terrain_variants->items()) {
                map.tileset.terrain_variants.push_back(read_terrain_variant(variant));
            }
        }
    }

    map.layers.clear();
    if (const JsonValue* layers = root.find("layers"); layers && layers->is_array()) {
        for (const JsonValue& layer_value : layers->items()) {
            if (!layer_value.is_object()) {
                continue;
            }
            const std::string layer_id = layer_value.string_at("id", "ground");
            if (map.layer(layer_id)) {
                continue;
            }
            TileLayer& layer = map.add_layer(layer_id,
                                             static_cast<i32>(layer_value.int_at("order", 0)));
            layer.visible = layer_value.bool_at("visible", true);
            layer.y_sort = layer_value.bool_at("y_sort", false);
            layer.participates_in_collision = layer_value.bool_at("collision", true);
            layer.participates_in_navigation = layer_value.bool_at("navigation", true);
            if (const JsonValue* rows_value = layer_value.find("cells"); rows_value && rows_value->is_array()) {
                for (i32 row = 0; row < map.rows && static_cast<std::size_t>(row) < rows_value->items().size(); ++row) {
                    const JsonValue& row_value = rows_value->items()[static_cast<std::size_t>(row)];
                    if (!row_value.is_array()) {
                        continue;
                    }
                    for (i32 col = 0; col < map.cols && static_cast<std::size_t>(col) < row_value.items().size(); ++col) {
                        map.set_cell(layer.id, {col, row}, read_cell_value(row_value.items()[static_cast<std::size_t>(col)]));
                    }
                }
            }
        }
    }
    if (map.layers.empty() && map.cols > 0 && map.rows > 0) {
        map.add_layer("ground");
    }
    if (const JsonValue* patterns = root.find("patterns"); patterns && patterns->is_array()) {
        for (const JsonValue& pattern : patterns->items()) {
            map.patterns.push_back(read_pattern(pattern));
        }
    }
    map.clear_dirty();
    return map;
}

TileMap load_legacy_tilemap(std::string_view text) {
    TileMap map;
    std::istringstream in{std::string{text}};
    std::string current_layer;
    i32 read_row = 0;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        const auto parts = split_ws(line);
        if (parts.empty()) {
            continue;
        }

        if (parts[0] == "size" && parts.size() >= 3) {
            i32 cols = 0;
            i32 rows = 0;
            if (parse_i32(parts[1], cols) && parse_i32(parts[2], rows)) {
                map.resize(cols, rows);
            }
        } else if (parts[0] == "tile_size" && parts.size() >= 3) {
            parse_i32(parts[1], map.tile_w);
            parse_i32(parts[2], map.tile_h);
        } else if (parts[0] == "tile" && parts.size() >= 3) {
            u16 id = 0;
            if (!parse_u16(parts[1], id)) {
                continue;
            }
            if (map.tileset.size() <= id) {
                map.tileset.resize(static_cast<std::size_t>(id) + 1);
            }
            TileDefinition& definition = map.tileset.tiles[id];
            definition.id = std::string{parts[2]};
            for (std::size_t i = 3; i < parts.size(); ++i) {
                const std::string_view part = parts[i];
                const std::size_t eq = part.find('=');
                if (eq == std::string_view::npos) {
                    continue;
                }
                const std::string_view key = part.substr(0, eq);
                const std::string_view value = part.substr(eq + 1);
                if (key == "solid") {
                    definition.solid = truthy(value);
                } else if (key == "walkable") {
                    definition.walkable = truthy(value);
                } else if (key == "cost") {
                    parse_i32(value, definition.cost);
                } else if (key == "blocks_sight") {
                    definition.blocks_sight = truthy(value);
                } else if (key == "terrain") {
                    definition.terrain = std::string{value};
                } else if (key == "tags") {
                    definition.tags = split_csv(value);
                } else if (key == "color") {
                    definition.color = parse_color(value, definition.color);
                } else if (key == "edge") {
                    definition.top_edge_color = parse_color(value, definition.top_edge_color);
                } else if (key == "sprite") {
                    definition.sprite_id = std::string{value};
                } else if (key == "render_offset") {
                    definition.render_offset_px = parse_vec2f_text(value, definition.render_offset_px);
                } else if (key == "render_size") {
                    definition.render_size_px = parse_vec2f_text(value, definition.render_size_px);
                }
            }
        } else if (parts[0] == "layer" && parts.size() >= 2) {
            current_layer = std::string{parts[1]};
            i32 order = 0;
            for (std::size_t i = 2; i < parts.size(); ++i) {
                const std::string_view part = parts[i];
                const std::size_t eq = part.find('=');
                if (eq == std::string_view::npos) {
                    continue;
                }
                const std::string_view key = part.substr(0, eq);
                const std::string_view value = part.substr(eq + 1);
                if (key == "order") {
                    parse_i32(value, order);
                }
            }
            if (TileLayer* existing = map.layer(current_layer)) {
                existing->order = order;
            } else {
                map.add_layer(current_layer, order);
            }
            read_row = 0;
        } else if (parts[0] == "row" && !current_layer.empty()) {
            for (i32 col = 0; col < map.cols && static_cast<std::size_t>(col + 1) < parts.size(); ++col) {
                u16 tile_id = 0;
                if (parse_u16(parts[static_cast<std::size_t>(col + 1)], tile_id)) {
                    map.set(current_layer, col, read_row, tile_id);
                }
            }
            ++read_row;
        }
    }
    map.clear_dirty();
    return map;
}

} // namespace

void TileSet::clear() {
    tiles.clear();
}

void TileSet::resize(std::size_t count) {
    tiles.resize(count);
}

std::size_t TileSet::size() const {
    return tiles.size();
}

bool TileSet::empty() const {
    return tiles.empty();
}

TileDefinition* TileSet::tile(u16 tile_id) {
    if (tile_id == 0 || tile_id >= tiles.size()) {
        return nullptr;
    }
    return &tiles[tile_id];
}

const TileDefinition* TileSet::tile(u16 tile_id) const {
    if (tile_id == 0 || tile_id >= tiles.size()) {
        return nullptr;
    }
    return &tiles[tile_id];
}

u16 TileSet::tile_id(std::string_view id) const {
    if (id.empty()) {
        return 0;
    }
    for (std::size_t i = 1; i < tiles.size() && i <= std::numeric_limits<u16>::max(); ++i) {
        if (tiles[i].id == id) {
            return static_cast<u16>(i);
        }
    }
    return 0;
}

u16 TileSet::ensure_tile(std::string_view id) {
    if (const u16 existing = tile_id(id); existing != 0) {
        return existing;
    }
    if (tiles.size() > std::numeric_limits<u16>::max()) {
        return 0;
    }
    if (tiles.empty()) {
        tiles.resize(1);
    }
    const u16 next_id = static_cast<u16>(tiles.size());
    TileDefinition definition;
    definition.id = std::string{id};
    tiles.push_back(std::move(definition));
    return next_id;
}

void TileSet::add_terrain_variant(std::string_view terrain, u8 mask, u16 tile_id, bool fallback) {
    terrain_variants.push_back({
        .terrain = std::string{terrain},
        .mask = static_cast<u8>(mask & 0x0F),
        .tile_id = tile_id,
        .fallback = fallback,
    });
}

u16 TileSet::terrain_tile(std::string_view terrain, u8 mask) const {
    for (const TerrainVariant& variant : terrain_variants) {
        if (variant.terrain == terrain && variant.mask == (mask & 0x0F)) {
            return variant.tile_id;
        }
    }
    return 0;
}

u16 TileSet::terrain_fallback_tile(std::string_view terrain) const {
    u16 first = 0;
    for (const TerrainVariant& variant : terrain_variants) {
        if (variant.terrain != terrain) {
            continue;
        }
        if (first == 0) {
            first = variant.tile_id;
        }
        if (variant.fallback) {
            return variant.tile_id;
        }
    }
    return first;
}

void TileMap::resize(i32 c, i32 r, u16 fill) {
    if (c <= 0 || r <= 0) {
        cols = 0;
        rows = 0;
        for (TileLayer& tile_layer : layers) {
            tile_layer.cells.clear();
        }
        _render_caches.clear();
        return;
    }

    const std::size_t width = static_cast<std::size_t>(c);
    const std::size_t height = static_cast<std::size_t>(r);
    if (height > std::numeric_limits<std::size_t>::max() / width) {
        clear();
        return;
    }

    cols = c;
    rows = r;
    if (layers.empty()) {
        _render_caches.clear();
        add_layer("ground", 0, fill);
        return;
    }
    for (TileLayer& tile_layer : layers) {
        tile_layer.cells.assign(width * height, TileCell{.tile_id = fill});
        invalidate_render_cache(tile_layer.id);
    }
}

void TileMap::clear() {
    cols = 0;
    rows = 0;
    tile_w = 16;
    tile_h = 16;
    tileset.clear();
    layers.clear();
    patterns.clear();
    terrain_diagnostics.clear();
    _render_caches.clear();
}

TileLayer& TileMap::add_layer(std::string_view id, i32 order, u16 fill) {
    if (TileLayer* existing = layer(id)) {
        return *existing;
    }
    TileLayer next{
        .id = std::string{id},
        .order = order,
        .cells = {},
    };
    next.cells.assign(static_cast<std::size_t>(std::max(cols, 0)) * static_cast<std::size_t>(std::max(rows, 0)),
                      TileCell{.tile_id = fill});
    layers.push_back(std::move(next));
    return layers.back();
}

TileLayer* TileMap::layer(std::string_view id) {
    const auto found = std::ranges::find(layers, id, &TileLayer::id);
    return found == layers.end() ? nullptr : &*found;
}

const TileLayer* TileMap::layer(std::string_view id) const {
    const auto found = std::ranges::find(layers, id, &TileLayer::id);
    return found == layers.end() ? nullptr : &*found;
}

TileLayer& TileMap::default_layer() {
    if (layers.empty()) {
        add_layer("ground");
    }
    return layers.front();
}

const TileLayer& TileMap::default_layer() const {
    return layers.front();
}

void TileMap::set(i32 col, i32 row, u16 tile_id) {
    set(default_layer().id, col, row, tile_id);
}

void TileMap::set(std::string_view layer_id, i32 col, i32 row, u16 tile_id) {
    set_cell(layer_id, {col, row}, TileCell{.tile_id = tile_id});
}

bool TileMap::set_cell(Vec2i cell, std::string_view tile_id) {
    return set_cell(default_layer().id, cell, tile_id);
}

bool TileMap::set_cell(std::string_view layer_id, Vec2i cell, std::string_view id) {
    const u16 id_value = tileset.tile_id(id);
    if (id_value == 0) {
        return false;
    }
    return set_cell(layer_id, cell, TileCell{.tile_id = id_value});
}

bool TileMap::set_cell(Vec2i cell, TileCell tile) {
    return set_cell(default_layer().id, cell, tile);
}

bool TileMap::set_cell(std::string_view layer_id, Vec2i cell, TileCell tile) {
    TileLayer* tile_layer = layer(layer_id);
    if (!tile_layer) {
        return false;
    }
    DirtyAccumulator dirty;
    const CellWriteResult result = write_cell(*tile_layer, cols, rows, cell, tile, &dirty);
    if (dirty.dirty) {
        mark_dirty(*tile_layer, dirty.rect);
    }
    return result.accepted;
}

bool TileMap::clear_cell(Vec2i cell) {
    return clear_cell(default_layer().id, cell);
}

bool TileMap::clear_cell(std::string_view layer_id, Vec2i cell) {
    return set_cell(layer_id, cell, TileCell{});
}

i32 TileMap::set_cells(std::span<const Vec2i> cells, u16 tile_id) {
    return set_cells(default_layer().id, cells, tile_id);
}

i32 TileMap::set_cells(std::string_view layer_id, std::span<const Vec2i> cells, u16 tile_id) {
    TileLayer* tile_layer = layer(layer_id);
    if (!tile_layer) {
        return 0;
    }
    i32 changed = 0;
    DirtyAccumulator dirty;
    for (Vec2i cell : cells) {
        if (write_cell(*tile_layer, cols, rows, cell, TileCell{.tile_id = tile_id}, &dirty).accepted) {
            ++changed;
        }
    }
    if (dirty.dirty) {
        mark_dirty(*tile_layer, dirty.rect);
    }
    return changed;
}

i32 TileMap::set_cells(std::span<const Vec2i> cells, std::string_view tile_id) {
    return set_cells(default_layer().id, cells, tile_id);
}

i32 TileMap::set_cells(std::string_view layer_id, std::span<const Vec2i> cells, std::string_view id) {
    const u16 id_value = tileset.tile_id(id);
    return id_value == 0 ? 0 : set_cells(layer_id, cells, id_value);
}

i32 TileMap::fill_rect(TileRect rect, u16 tile_id) {
    return fill_rect(default_layer().id, rect, tile_id);
}

i32 TileMap::fill_rect(std::string_view layer_id, TileRect rect, u16 tile_id) {
    TileLayer* tile_layer = layer(layer_id);
    if (!tile_layer) {
        return 0;
    }
    rect = clipped_rect(rect, cols, rows);
    if (rect_empty(rect)) {
        return 0;
    }
    i32 changed = 0;
    DirtyAccumulator dirty;
    for (i32 row = rect.y; row < rect.y + rect.h; ++row) {
        for (i32 col = rect.x; col < rect.x + rect.w; ++col) {
            if (write_cell(*tile_layer, cols, rows, {col, row}, TileCell{.tile_id = tile_id}, &dirty).accepted) {
                ++changed;
            }
        }
    }
    if (dirty.dirty) {
        mark_dirty(*tile_layer, dirty.rect);
    }
    return changed;
}

i32 TileMap::replace_tile(u16 from_id, u16 to_id) {
    i32 changed = 0;
    for (const TileLayer& tile_layer : layers) {
        changed += replace_tile(tile_layer.id, from_id, to_id);
    }
    return changed;
}

i32 TileMap::replace_tile(std::string_view layer_id, u16 from_id, u16 to_id) {
    TileLayer* tile_layer = layer(layer_id);
    if (!tile_layer) {
        return 0;
    }
    i32 changed = 0;
    DirtyAccumulator dirty;
    for (i32 row = 0; row < rows; ++row) {
        for (i32 col = 0; col < cols; ++col) {
            const std::size_t i = index(col, row);
            if (i >= tile_layer->cells.size() || tile_layer->cells[i].tile_id != from_id) {
                continue;
            }
            if (tile_layer->cells[i].tile_id != to_id) {
                tile_layer->cells[i].tile_id = to_id;
                dirty.include({col, row});
            }
            ++changed;
        }
    }
    if (dirty.dirty) {
        mark_dirty(*tile_layer, dirty.rect);
    }
    return changed;
}

i32 TileMap::flood_fill(Vec2i start, u16 replacement_tile) {
    return flood_fill(default_layer().id, start, replacement_tile);
}

i32 TileMap::flood_fill(std::string_view layer_id, Vec2i start, u16 replacement_tile) {
    TileLayer* tile_layer = layer(layer_id);
    if (!tile_layer || !in_bounds(start.x, start.y)) {
        return 0;
    }
    const u16 target = get(layer_id, start.x, start.y);
    if (target == replacement_tile) {
        return 0;
    }

    std::queue<Vec2i> open;
    std::vector<bool> visited(static_cast<std::size_t>(std::max(cols, 0)) * static_cast<std::size_t>(std::max(rows, 0)), false);
    open.push(start);
    i32 changed = 0;
    DirtyAccumulator dirty;
    while (!open.empty()) {
        const Vec2i current = open.front();
        open.pop();
        if (!in_bounds(current.x, current.y)) {
            continue;
        }
        const std::size_t i = index(current.x, current.y);
        if (i >= visited.size() || visited[i]) {
            continue;
        }
        visited[i] = true;
        if (i >= tile_layer->cells.size() || tile_layer->cells[i].tile_id != target) {
            continue;
        }
        tile_layer->cells[i].tile_id = replacement_tile;
        ++changed;
        dirty.include(current);
        for (Vec2i neighbor : neighbor_cells(current)) {
            open.push(neighbor);
        }
    }
    if (dirty.dirty) {
        mark_dirty(*tile_layer, dirty.rect);
    }
    return changed;
}

i32 TileMap::set_terrain_connect(std::string_view layer_id, std::span<const Vec2i> cells, std::string_view terrain) {
    TileLayer* tile_layer = layer(layer_id);
    if (!tile_layer || terrain.empty()) {
        return 0;
    }
    const u16 fallback = tileset.terrain_fallback_tile(terrain);
    if (fallback == 0) {
        terrain_diagnostics.push_back("missing terrain fallback for '" + std::string{terrain} + "'");
        return 0;
    }

    i32 changed = 0;
    DirtyAccumulator dirty;
    std::vector<Vec2i> affected;
    affected.reserve(cells.size() * 5);
    for (Vec2i cell : cells) {
        if (!in_bounds(cell.x, cell.y)) {
            continue;
        }
        if (write_cell(*tile_layer, cols, rows, cell, TileCell{.tile_id = fallback}, &dirty).accepted) {
            ++changed;
        }
        affected.push_back(cell);
        for (Vec2i neighbor : neighbor_cells(cell)) {
            affected.push_back(neighbor);
        }
    }

    std::ranges::sort(affected, [](Vec2i a, Vec2i b) {
        return a.y == b.y ? a.x < b.x : a.y < b.y;
    });
    affected.erase(std::ranges::unique(affected).begin(), affected.end());

    for (Vec2i cell : affected) {
        const TileDefinition* definition = tile(get(layer_id, cell.x, cell.y));
        if (!definition || definition->terrain != terrain) {
            continue;
        }
        u8 mask = 0;
        const auto same_terrain = [&](Vec2i other) {
            const TileDefinition* other_definition = tile(get(layer_id, other.x, other.y));
            return other_definition && other_definition->terrain == terrain;
        };
        if (in_bounds(cell.x, cell.y - 1) && same_terrain({cell.x, cell.y - 1})) {
            mask |= TileTerrainNorth;
        }
        if (in_bounds(cell.x + 1, cell.y) && same_terrain({cell.x + 1, cell.y})) {
            mask |= TileTerrainEast;
        }
        if (in_bounds(cell.x, cell.y + 1) && same_terrain({cell.x, cell.y + 1})) {
            mask |= TileTerrainSouth;
        }
        if (in_bounds(cell.x - 1, cell.y) && same_terrain({cell.x - 1, cell.y})) {
            mask |= TileTerrainWest;
        }

        u16 variant_tile = tileset.terrain_tile(terrain, mask);
        if (variant_tile == 0) {
            variant_tile = fallback;
            terrain_diagnostics.push_back("missing terrain variant terrain='" + std::string{terrain} + "' mask=" + terrain_mask_text(mask));
        }
        if (write_cell(*tile_layer, cols, rows, cell, TileCell{.tile_id = variant_tile}, &dirty).accepted) {
            ++changed;
        }
    }
    if (dirty.dirty) {
        mark_dirty(*tile_layer, dirty.rect);
    }
    return changed;
}

void TileMap::clear_terrain_diagnostics() {
    terrain_diagnostics.clear();
}

u16 TileMap::get(i32 col, i32 row) const {
    return layers.empty() ? 0 : get(default_layer().id, col, row);
}

u16 TileMap::get(std::string_view layer_id, i32 col, i32 row) const {
    return cell(layer_id, {col, row}).tile_id;
}

TileCell TileMap::cell(Vec2i cell) const {
    return layers.empty() ? TileCell{} : this->cell(default_layer().id, cell);
}

TileCell TileMap::cell(std::string_view layer_id, Vec2i cell) const {
    const TileLayer* tile_layer = layer(layer_id);
    if (!tile_layer || !in_bounds(cell.x, cell.y)) {
        return {};
    }
    const std::size_t i = index(cell.x, cell.y);
    return i < tile_layer->cells.size() ? tile_layer->cells[i] : TileCell{};
}

std::vector<Vec2i> TileMap::used_cells(std::string_view layer_id) const {
    std::vector<Vec2i> result;
    visit_layers(*this, layer_id, [&](const TileLayer& tile_layer) {
        visit_layer_cells(tile_layer, cols, rows, [&](Vec2i cell, TileCell tile_cell) {
            if (!tile_cell.empty()) {
                result.push_back(cell);
            }
        });
    });
    return result;
}

TileRect TileMap::used_rect(std::string_view layer_id) const {
    RectAccumulator used;
    visit_layers(*this, layer_id, [&](const TileLayer& tile_layer) {
        visit_layer_cells(tile_layer, cols, rows, [&](Vec2i cell, TileCell tile_cell) {
            if (!tile_cell.empty()) {
                used.include(cell);
            }
        });
    });
    return used.any ? used.rect : TileRect{};
}

std::vector<Vec2i> TileMap::neighbor_cells(Vec2i cell) const {
    std::vector<Vec2i> result;
    result.reserve(4);
    const Vec2i candidates[] = {
        {cell.x + 1, cell.y},
        {cell.x - 1, cell.y},
        {cell.x, cell.y + 1},
        {cell.x, cell.y - 1},
    };
    for (Vec2i candidate : candidates) {
        if (in_bounds(candidate.x, candidate.y)) {
            result.push_back(candidate);
        }
    }
    return result;
}

const std::vector<TileRect>& TileMap::dirty_regions(std::string_view layer_id) const {
    static const std::vector<TileRect> empty;
    const TileLayer* tile_layer = layer(layer_id);
    return tile_layer ? tile_layer->dirty_regions : empty;
}

bool TileMap::layer_dirty(std::string_view layer_id) const {
    const TileLayer* tile_layer = layer(layer_id);
    return tile_layer && tile_layer->dirty;
}

void TileMap::clear_dirty(std::string_view layer_id) {
    const auto clear_layer = [](TileLayer& tile_layer) {
        tile_layer.dirty = false;
        tile_layer.dirty_regions.clear();
    };
    if (!layer_id.empty()) {
        if (TileLayer* tile_layer = layer(layer_id)) {
            clear_layer(*tile_layer);
        }
        return;
    }
    for (TileLayer& tile_layer : layers) {
        clear_layer(tile_layer);
    }
}

TilePattern TileMap::extract_pattern(std::string_view layer_id, TileRect rect, bool include_empty) const {
    TilePattern pattern;
    pattern.bounds = rect;
    rect = clipped_rect(rect, cols, rows);
    if (rect_empty(rect)) {
        return pattern;
    }
    const TileLayer* tile_layer = layer(layer_id);
    if (!tile_layer) {
        return pattern;
    }
    for (i32 row = rect.y; row < rect.y + rect.h; ++row) {
        for (i32 col = rect.x; col < rect.x + rect.w; ++col) {
            const TileCell tile_cell = cell(layer_id, {col, row});
            if (!include_empty && tile_cell.empty()) {
                continue;
            }
            pattern.cells.push_back({
                .offset = {col - rect.x, row - rect.y},
                .cell = tile_cell,
            });
        }
    }
    pattern.bounds = {0, 0, rect.w, rect.h};
    return pattern;
}

i32 TileMap::stamp_pattern(std::string_view layer_id, Vec2i target, const TilePattern& pattern, bool skip_empty) {
    TileLayer* tile_layer = layer(layer_id);
    if (!tile_layer) {
        return 0;
    }
    i32 changed = 0;
    DirtyAccumulator dirty;
    for (const TilePatternCell& pattern_cell : pattern.cells) {
        if (skip_empty && pattern_cell.cell.empty()) {
            continue;
        }
        const Vec2i cell{target.x + pattern_cell.offset.x, target.y + pattern_cell.offset.y};
        if (write_cell(*tile_layer, cols, rows, cell, pattern_cell.cell, &dirty).accepted) {
            ++changed;
        }
    }
    if (dirty.dirty) {
        mark_dirty(*tile_layer, dirty.rect);
    }
    return changed;
}

bool TileMap::solid(i32 col, i32 row) const {
    bool blocked = false;
    visit_layers(*this, {}, [&](const TileLayer& tile_layer) {
        if (!blocked && tile_layer.participates_in_collision && solid_in_layer(*this, tile_layer, col, row)) {
            blocked = true;
        }
    });
    return blocked;
}

bool TileMap::solid(std::string_view layer_id, i32 col, i32 row) const {
    const TileLayer* tile_layer = layer(layer_id);
    return tile_layer ? solid_in_layer(*this, *tile_layer, col, row) : false;
}

bool TileMap::walkable(i32 col, i32 row) const {
    bool saw_navigation_layer = false;
    bool all_walkable = true;
    visit_layers(*this, {}, [&](const TileLayer& tile_layer) {
        if (all_walkable && tile_layer.participates_in_navigation) {
            saw_navigation_layer = true;
            all_walkable = walkable_in_layer(*this, tile_layer, col, row);
        }
    });
    return saw_navigation_layer && all_walkable;
}

bool TileMap::walkable(std::string_view layer_id, i32 col, i32 row) const {
    const TileLayer* tile_layer = layer(layer_id);
    return tile_layer ? walkable_in_layer(*this, *tile_layer, col, row) : true;
}

i32 TileMap::movement_cost(i32 col, i32 row) const {
    i32 cost = 10;
    bool blocked = false;
    visit_layers(*this, {}, [&](const TileLayer& tile_layer) {
        if (!blocked && tile_layer.participates_in_navigation) {
            if (!walkable_in_layer(*this, tile_layer, col, row)) {
                blocked = true;
                return;
            }
            cost = std::max(cost, movement_cost_in_layer(*this, tile_layer, col, row));
        }
    });
    return blocked ? std::numeric_limits<i32>::max() : cost;
}

i32 TileMap::movement_cost(std::string_view layer_id, i32 col, i32 row) const {
    const TileLayer* tile_layer = layer(layer_id);
    return tile_layer ? movement_cost_in_layer(*this, *tile_layer, col, row) : 10;
}

bool TileMap::blocks_point(Vec2f world_pos) const {
    const Vec2i cell = world_to_map(world_pos);
    return solid(cell.x, cell.y);
}

bool TileMap::blocks_point(std::string_view layer_id, Vec2f world_pos) const {
    const TileLayer* tile_layer = layer(layer_id);
    if (!tile_layer || !tile_layer->participates_in_collision) {
        return false;
    }
    const Vec2i cell = world_to_map(world_pos);
    return solid_in_layer(*this, *tile_layer, cell.x, cell.y);
}

bool TileMap::blocks_rect(Rectf world_rect) const {
    bool blocked = false;
    visit_layers(*this, {}, [&](const TileLayer& tile_layer) {
        if (!blocked && tile_layer.participates_in_collision) {
            if (world_rect.w <= 0.0f || world_rect.h <= 0.0f) {
                return;
            }
            const i32 col0 = std::max(0, world_col(world_rect.x));
            const i32 row0 = std::max(0, world_row(world_rect.y));
            const i32 col1 = std::min(cols - 1, world_col(world_rect.x + world_rect.w - 0.001f));
            const i32 row1 = std::min(rows - 1, world_row(world_rect.y + world_rect.h - 0.001f));
            if (col1 < col0 || row1 < row0) {
                return;
            }
            for (i32 row = row0; row <= row1 && !blocked; ++row) {
                for (i32 col = col0; col <= col1; ++col) {
                    if (solid_in_layer(*this, tile_layer, col, row) && rects_overlap(world_rect, tile_rect(col, row))) {
                        blocked = true;
                        break;
                    }
                }
            }
        }
    });
    return blocked;
}

bool TileMap::blocks_rect(std::string_view layer_id, Rectf world_rect) const {
    const TileLayer* tile_layer = layer(layer_id);
    if (!tile_layer || !tile_layer->participates_in_collision) {
        return false;
    }
    if (world_rect.w <= 0.0f || world_rect.h <= 0.0f) {
        return false;
    }
    const i32 col0 = std::max(0, world_col(world_rect.x));
    const i32 row0 = std::max(0, world_row(world_rect.y));
    const i32 col1 = std::min(cols - 1, world_col(world_rect.x + world_rect.w - 0.001f));
    const i32 row1 = std::min(rows - 1, world_row(world_rect.y + world_rect.h - 0.001f));
    if (col1 < col0 || row1 < row0) {
        return false;
    }
    for (i32 row = row0; row <= row1; ++row) {
        for (i32 col = col0; col <= col1; ++col) {
            if (solid_in_layer(*this, *tile_layer, col, row) && rects_overlap(world_rect, tile_rect(col, row))) {
                return true;
            }
        }
    }
    return false;
}

std::vector<Rectf> TileMap::collision_rects(std::string_view layer_id) const {
    std::vector<Rectf> runs;
    visit_layers(*this, layer_id, [&](const TileLayer& tile_layer) {
        if (!tile_layer.participates_in_collision) {
            return;
        }
        for (i32 row = 0; row < rows; ++row) {
            i32 run_start = -1;
            for (i32 col = 0; col <= cols; ++col) {
                const bool blocked = col < cols && solid_in_layer(*this, tile_layer, col, row);
                if (blocked && run_start < 0) {
                    run_start = col;
                } else if (!blocked && run_start >= 0) {
                    Rectf rect{
                        static_cast<f32>(run_start * tile_w),
                        static_cast<f32>(row * tile_h),
                        static_cast<f32>((col - run_start) * tile_w),
                        static_cast<f32>(tile_h),
                    };
                    bool merged = false;
                    for (Rectf& existing : runs) {
                        if (same_rect_span(existing, rect)) {
                            existing.h += rect.h;
                            merged = true;
                            break;
                        }
                    }
                    if (!merged) {
                        runs.push_back(rect);
                    }
                    run_start = -1;
                }
            }
        }
    });
    return runs;
}

std::vector<std::vector<Vec2f>> TileMap::collision_polygons(std::string_view layer_id) const {
    std::vector<std::vector<Vec2f>> polygons;
    visit_layers(*this, layer_id, [&](const TileLayer& tile_layer) {
        if (!tile_layer.participates_in_collision) {
            return;
        }
        visit_layer_cells(tile_layer, cols, rows, [&](Vec2i cell, TileCell tile_cell) {
            const TileDefinition* definition = tile(tile_cell.tile_id);
            if (!definition || !definition->solid) {
                return;
            }
            const Vec2f origin = map_to_world(cell);
            std::vector<Vec2f> polygon;
            if (!definition->collision_polygon.empty()) {
                polygon.reserve(definition->collision_polygon.size());
                for (Vec2f point : definition->collision_polygon) {
                    polygon.push_back({origin.x + point.x, origin.y + point.y});
                }
            } else {
                polygon = {
                    {origin.x, origin.y},
                    {origin.x + static_cast<f32>(tile_w), origin.y},
                    {origin.x + static_cast<f32>(tile_w), origin.y + static_cast<f32>(tile_h)},
                    {origin.x, origin.y + static_cast<f32>(tile_h)},
                };
            }
            polygons.push_back(std::move(polygon));
        });
    });
    return polygons;
}

TileMap::LayerRenderCache* TileMap::render_cache(std::string_view layer_id) const {
    const auto found = std::ranges::find_if(_render_caches, [&](const LayerRenderCache& cache) {
        return cache.layer_id == layer_id;
    });
    return found == _render_caches.end() ? nullptr : &*found;
}

TileMap::LayerRenderCache& TileMap::render_cache_for(std::string_view layer_id) const {
    if (LayerRenderCache* cache = render_cache(layer_id)) {
        return *cache;
    }
    LayerRenderCache cache;
    cache.layer_id = std::string{layer_id};
    _render_caches.push_back(std::move(cache));
    return _render_caches.back();
}

void TileMap::reset_render_cache(LayerRenderCache& cache) const {
    cache.valid = false;
    cache.chunks.clear();
    cache.cols = 0;
    cache.rows = 0;
    cache.chunk_size = {};
}

void TileMap::invalidate_render_cache(std::string_view layer_id) const {
    if (!layer_id.empty()) {
        if (LayerRenderCache* cache = render_cache(layer_id)) {
            reset_render_cache(*cache);
        }
        return;
    }
    _render_caches.clear();
}

void TileMap::set_render_chunk_size(i32 cols, i32 rows) {
    const Vec2i next{std::max(1, cols), std::max(1, rows)};
    if (next == _render_chunk_size) {
        return;
    }
    _render_chunk_size = next;
    invalidate_render_cache();
}

Vec2i TileMap::render_chunk_size() const {
    return _render_chunk_size;
}

std::size_t TileMap::cached_render_chunk_count(std::string_view layer_id) const {
    if (!layer_id.empty()) {
        const LayerRenderCache* cache = render_cache(layer_id);
        return cache && cache->valid ? cache->chunks.size() : 0;
    }
    std::size_t count = 0;
    for (const LayerRenderCache& cache : _render_caches) {
        if (cache.valid) {
            count += cache.chunks.size();
        }
    }
    return count;
}

const TileDefinition* TileMap::tile(u16 tile_id) const {
    return tileset.tile(tile_id);
}

const TileDefinition* TileMap::tile(std::string_view id) const {
    return tile(tile_id(id));
}

u16 TileMap::tile_id(std::string_view id) const {
    return tileset.tile_id(id);
}

i32 TileMap::world_col(f32 wx) const {
    return tile_w > 0 ? static_cast<i32>(std::floor(wx / static_cast<f32>(tile_w))) : 0;
}

i32 TileMap::world_row(f32 wy) const {
    return tile_h > 0 ? static_cast<i32>(std::floor(wy / static_cast<f32>(tile_h))) : 0;
}

Vec2i TileMap::world_to_map(Vec2f world_pos) const {
    return {world_col(world_pos.x), world_row(world_pos.y)};
}

Vec2f TileMap::map_to_world(Vec2i cell) const {
    return {
        static_cast<f32>(cell.x * tile_w),
        static_cast<f32>(cell.y * tile_h),
    };
}

Vec2f TileMap::tile_center(Vec2i cell) const {
    return {
        (static_cast<f32>(cell.x) + 0.5f) * static_cast<f32>(tile_w),
        (static_cast<f32>(cell.y) + 0.5f) * static_cast<f32>(tile_h),
    };
}

Vec2f TileMap::world_size() const {
    return {static_cast<f32>(cols * tile_w), static_cast<f32>(rows * tile_h)};
}

Rectf TileMap::tile_rect(i32 col, i32 row) const {
    return {
        static_cast<f32>(col * tile_w),
        static_cast<f32>(row * tile_h),
        static_cast<f32>(tile_w),
        static_cast<f32>(tile_h),
    };
}

bool TileMap::in_bounds(i32 col, i32 row) const {
    return col >= 0 && col < cols && row >= 0 && row < rows;
}

void TileMap::submit(RenderQueue& queue, TilemapRenderOptions options) const {
    RenderView view;
    submit(queue, view, options);
}

void TileMap::submit(RenderQueue& queue, const RenderView& view, TilemapRenderOptions options) const {
    if (cols <= 0 || rows <= 0 || tile_w <= 0 || tile_h <= 0 || tileset.empty()) {
        return;
    }

    const TileRect visible_tiles = visible_tile_rect(*this, view);
    if (rect_empty(visible_tiles)) {
        return;
    }

    for (const TileLayer& tile_layer : layers) {
        if (!tile_layer.visible) {
            continue;
        }

        const RenderCacheKey cache_key = make_render_cache_key(tile_layer, options);
        const bool can_cache = options.static_renderable && options.use_cache && !tile_layer.y_sort;
        if (!can_cache) {
            submit_uncached_layer(queue, tile_layer, visible_tiles, cache_key);
            continue;
        }
        submit_cached_layer(queue, tile_layer, visible_tiles, cache_key, options);
    }
}

TileMap::RenderCacheKey TileMap::make_render_cache_key(const TileLayer& layer, TilemapRenderOptions options) const {
    return {
        .layer = options.layer,
        .order = options.order + layer.order,
        .draw_top_edge = options.draw_top_edge,
        .pass_mask = options.pass_mask ? options.pass_mask : pass_mask_for_layer(options.layer),
        .sprites = options.sprites,
    };
}

Vec2i TileMap::effective_render_chunk_size(TilemapRenderOptions options) const {
    Vec2i chunk_size = _render_chunk_size;
    if (options.chunk_cols != 16 || options.chunk_rows != 16) {
        chunk_size = {std::max(1, options.chunk_cols), std::max(1, options.chunk_rows)};
    }
    chunk_size.x = std::max(1, chunk_size.x);
    chunk_size.y = std::max(1, chunk_size.y);
    return chunk_size;
}

void TileMap::reset_render_cache_chunks(LayerRenderCache& cache,
                                        const RenderCacheKey& key,
                                        Vec2i chunk_size,
                                        i32 chunk_cols,
                                        i32 chunk_rows) const {
    cache.valid = true;
    cache.chunk_size = chunk_size;
    cache.cols = chunk_cols;
    cache.rows = chunk_rows;
    cache.key = key;
    cache.chunks.clear();
    cache.chunks.resize(static_cast<std::size_t>(chunk_cols * chunk_rows));
    for (i32 chunk_row = 0; chunk_row < chunk_rows; ++chunk_row) {
        for (i32 chunk_col = 0; chunk_col < chunk_cols; ++chunk_col) {
            RenderChunkCache& chunk = cache.chunks[chunk_index(chunk_col, chunk_row, chunk_cols)];
            chunk.bounds = chunk_bounds(chunk_col, chunk_row, chunk_size, cols, rows);
            chunk.dirty = true;
            chunk.commands.clear();
        }
    }
}

void TileMap::rebuild_render_chunk(RenderChunkCache& chunk, const TileLayer& layer, const RenderCacheKey& key) const {
    chunk.commands.clear();
    for (i32 row = chunk.bounds.y; row < chunk.bounds.y + chunk.bounds.h; ++row) {
        for (i32 col = chunk.bounds.x; col < chunk.bounds.x + chunk.bounds.w; ++col) {
            append_tile_render_commands(*this,
                                        layer,
                                        col,
                                        row,
                                        key.layer,
                                        key.order,
                                        key.draw_top_edge,
                                        key.pass_mask,
                                        key.sprites,
                                        chunk.commands);
        }
    }
    chunk.dirty = false;
}

void TileMap::submit_uncached_layer(RenderQueue& queue, const TileLayer& layer, TileRect visible_tiles, const RenderCacheKey& key) const {
    std::vector<RenderCommand> commands;
    for (i32 row = visible_tiles.y; row < visible_tiles.y + visible_tiles.h; ++row) {
        for (i32 col = visible_tiles.x; col < visible_tiles.x + visible_tiles.w; ++col) {
            append_tile_render_commands(*this,
                                        layer,
                                        col,
                                        row,
                                        key.layer,
                                        key.order,
                                        key.draw_top_edge,
                                        key.pass_mask,
                                        key.sprites,
                                        commands);
            for (const RenderCommand& command : commands) {
                queue.submit(command);
            }
            commands.clear();
        }
    }
}

void TileMap::submit_cached_layer(RenderQueue& queue,
                                  const TileLayer& layer,
                                  TileRect visible_tiles,
                                  const RenderCacheKey& key,
                                  TilemapRenderOptions options) const {
    const Vec2i chunk_size = effective_render_chunk_size(options);
    const i32 chunk_cols = (cols + chunk_size.x - 1) / chunk_size.x;
    const i32 chunk_rows = (rows + chunk_size.y - 1) / chunk_size.y;
    LayerRenderCache& cache = render_cache_for(layer.id);
    const bool rebuild_all = !cache.valid ||
                             cache.chunk_size != chunk_size ||
                             cache.cols != chunk_cols ||
                             cache.rows != chunk_rows ||
                             cache.key != key ||
                             cache.chunks.size() != static_cast<std::size_t>(chunk_cols * chunk_rows);
    if (rebuild_all) {
        reset_render_cache_chunks(cache, key, chunk_size, chunk_cols, chunk_rows);
    }

    const i32 col0 = visible_tiles.x;
    const i32 row0 = visible_tiles.y;
    const i32 col1 = visible_tiles.x + visible_tiles.w - 1;
    const i32 row1 = visible_tiles.y + visible_tiles.h - 1;
    const i32 visible_chunk_col0 = std::clamp(col0 / chunk_size.x, 0, chunk_cols - 1);
    const i32 visible_chunk_row0 = std::clamp(row0 / chunk_size.y, 0, chunk_rows - 1);
    const i32 visible_chunk_col1 = std::clamp(col1 / chunk_size.x, 0, chunk_cols - 1);
    const i32 visible_chunk_row1 = std::clamp(row1 / chunk_size.y, 0, chunk_rows - 1);
    for (i32 chunk_row = visible_chunk_row0; chunk_row <= visible_chunk_row1; ++chunk_row) {
        for (i32 chunk_col = visible_chunk_col0; chunk_col <= visible_chunk_col1; ++chunk_col) {
            RenderChunkCache& chunk = cache.chunks[chunk_index(chunk_col, chunk_row, chunk_cols)];
            if (chunk.dirty) {
                rebuild_render_chunk(chunk, layer, key);
            }
            for (const RenderCommand& command : chunk.commands) {
                queue.submit(command);
            }
        }
    }
}

void TileMap::render(Renderer2D& renderer, TilemapRenderOptions options) const {
    RenderQueue queue{RenderSortMode::LayerThenY};
    submit(queue, options);
    queue.flush(renderer);
}

void TileMap::render(Renderer2D& renderer, const RenderView& view, TilemapRenderOptions options) const {
    RenderQueue queue{RenderSortMode::LayerThenY};
    submit(queue, view, options);
    queue.flush(renderer, view);
}

std::size_t TileMap::index(i32 col, i32 row) const {
    return static_cast<std::size_t>(row * cols + col);
}

void TileMap::mark_dirty(TileLayer& layer, TileRect rect) {
    rect = clipped_rect(rect, cols, rows);
    if (rect_empty(rect)) {
        return;
    }
    layer.dirty = true;
    layer.dirty_regions.push_back(rect);
    invalidate_render_cache_for_dirty(layer, rect);
}

void TileMap::invalidate_render_cache_for_dirty(const TileLayer& layer, TileRect rect) const {
    LayerRenderCache* cache = render_cache(layer.id);
    if (!cache || !cache->valid || cache->chunks.empty() ||
        cache->chunk_size.x <= 0 || cache->chunk_size.y <= 0 ||
        cache->cols <= 0 || cache->rows <= 0) {
        return;
    }
    const TileRect chunks = tile_rect_to_chunk_rect(rect,
                                                    cache->chunk_size,
                                                    cache->cols,
                                                    cache->rows);
    if (rect_empty(chunks)) {
        return;
    }
    for (i32 row = chunks.y; row < chunks.y + chunks.h; ++row) {
        for (i32 col = chunks.x; col < chunks.x + chunks.w; ++col) {
            const std::size_t i = chunk_index(col, row, cache->cols);
            if (i < cache->chunks.size()) {
                cache->chunks[i].dirty = true;
            }
        }
    }
}

TileMap load_tilemap(const std::filesystem::path& path) {
    std::ifstream in{path};
    if (!in) {
        return {};
    }
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (looks_like_json(text)) {
        JsonParseResult parsed = parse_json(text);
        if (parsed.ok()) {
            return load_json_tilemap(*parsed.value);
        }
        return {};
    }
    return load_legacy_tilemap(text);
}

bool save_tilemap(const TileMap& map, const std::filesystem::path& path) {
    std::ofstream out{path};
    if (!out) {
        return false;
    }
    JsonWriter json{out};
    json.begin_object()
        .field("format", "kin.tilemap")
        .field("version", 1);
    json.key("size").begin_object()
        .field("cols", map.cols)
        .field("rows", map.rows)
        .end_object();
    json.key("tile_size").begin_object()
        .field("w", map.tile_w)
        .field("h", map.tile_h)
        .end_object();
    json.key("tileset").begin_object().key("tiles").begin_array();
    for (std::size_t id = 1; id < map.tileset.size(); ++id) {
        const TileDefinition& definition = map.tileset.tiles[id];
        if (definition.id.empty() &&
            !definition.solid &&
            !definition.walkable.has_value() &&
            definition.cost == 10 &&
            !definition.blocks_sight &&
            definition.terrain.empty() &&
            definition.tags.empty() &&
            definition.metadata.empty() &&
            definition.collision_polygon.empty() &&
            definition.sprite_id.empty()) {
            continue;
        }
        write_tile_definition(json, definition, static_cast<u16>(id));
    }
    json.end_array();
    if (!map.tileset.terrain_variants.empty()) {
        json.key("terrain_variants").begin_array();
        for (const TerrainVariant& variant : map.tileset.terrain_variants) {
            write_terrain_variant(json, variant);
        }
        json.end_array();
    }
    json.end_object();
    json.key("layers").begin_array();
    for (const TileLayer& tile_layer : map.layers) {
        json.begin_object()
            .field("id", std::string_view{tile_layer.id})
            .field("order", tile_layer.order)
            .field("visible", tile_layer.visible)
            .field("y_sort", tile_layer.y_sort)
            .field("collision", tile_layer.participates_in_collision)
            .field("navigation", tile_layer.participates_in_navigation);
        json.key("cells").begin_array();
        for (i32 row = 0; row < map.rows; ++row) {
            json.begin_array();
            for (i32 col = 0; col < map.cols; ++col) {
                const std::size_t i = static_cast<std::size_t>(row * map.cols + col);
                write_cell_value(json, i < tile_layer.cells.size() ? tile_layer.cells[i] : TileCell{});
            }
            json.end_array();
        }
        json.end_array().end_object();
    }
    json.end_array();
    if (!map.patterns.empty()) {
        json.key("patterns").begin_array();
        for (const TilePattern& pattern : map.patterns) {
            write_pattern(json, pattern);
        }
        json.end_array();
    }
    json.end_object();
    return true;
}

void register_tilemap_loader(AssetManager& assets) {
    assets.register_loader<TileMap>([](const std::filesystem::path& path) {
        return load_tilemap(path);
    });
}

} // namespace kin
