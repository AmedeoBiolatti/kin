#pragma once

#include <kin/assets/asset_manager.hpp>
#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/render_layer.hpp>
#include <kin/renderer/render_queue.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace kin {

using TileMetadataValue = std::variant<bool, i32, f32, std::string>;

enum TileTerrainMask : u8 {
    TileTerrainNorth = 1 << 0,
    TileTerrainEast = 1 << 1,
    TileTerrainSouth = 1 << 2,
    TileTerrainWest = 1 << 3,
};

struct TileDefinition {
    std::string id;
    bool solid = false;
    std::optional<bool> walkable;
    i32 cost = 10;
    bool blocks_sight = false;
    std::string terrain;
    std::vector<std::string> tags;
    std::unordered_map<std::string, TileMetadataValue> metadata;
    std::vector<Vec2f> collision_polygon;
    Color color = Color::rgb(90, 90, 90);
    Color top_edge_color = Color::rgb(120, 120, 120);
    std::string sprite_id;
    Vec2f render_offset_px{};
    Vec2f render_size_px{};

    bool is_walkable() const { return walkable.value_or(!solid); }
};

struct TerrainVariant {
    std::string terrain;
    u8 mask = 0;
    u16 tile_id = 0;
    bool fallback = false;
};

struct TileSet {
    std::vector<TileDefinition> tiles;
    std::vector<TerrainVariant> terrain_variants;

    void clear();
    void resize(std::size_t count);
    std::size_t size() const;
    bool empty() const;

    TileDefinition* tile(u16 tile_id);
    const TileDefinition* tile(u16 tile_id) const;
    u16 tile_id(std::string_view id) const;
    u16 ensure_tile(std::string_view id);
    void add_terrain_variant(std::string_view terrain, u8 mask, u16 tile_id, bool fallback = false);
    u16 terrain_tile(std::string_view terrain, u8 mask) const;
    u16 terrain_fallback_tile(std::string_view terrain) const;
};

struct TileCell {
    u16 tile_id = 0;
    u16 variant_id = 0;
    u16 flags = 0;

    bool empty() const { return tile_id == 0; }
    friend constexpr bool operator==(TileCell, TileCell) = default;
};

struct TileRect {
    i32 x = 0;
    i32 y = 0;
    i32 w = 0;
    i32 h = 0;

    friend constexpr bool operator==(TileRect, TileRect) = default;
};

struct TileLayer {
    std::string id = "ground";
    i32 order = 0;
    bool visible = true;
    bool y_sort = false;
    bool participates_in_collision = true;
    bool participates_in_navigation = true;
    std::vector<TileCell> cells;
    bool dirty = false;
    std::vector<TileRect> dirty_regions;
};

struct TilePatternCell {
    Vec2i offset{};
    TileCell cell{};
};

struct TilePattern {
    std::string id;
    TileRect bounds{};
    std::vector<TilePatternCell> cells;
};

struct TilemapRenderOptions {
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool draw_top_edge = true;
    bool static_renderable = true;
    bool use_cache = true;
    i32 chunk_cols = 16;
    i32 chunk_rows = 16;
    u64 pass_mask = render_pass_mask::world;
    const SpriteCatalog* sprites = nullptr;
};

class TileMap {
public:
    i32 cols = 0;
    i32 rows = 0;
    i32 tile_w = 16;
    i32 tile_h = 16;
    TileSet tileset;
    std::vector<TileLayer> layers;
    std::vector<TilePattern> patterns;
    std::vector<std::string> terrain_diagnostics;

    void resize(i32 c, i32 r, u16 fill = 0);
    void clear();

    TileLayer& add_layer(std::string_view id, i32 order = 0, u16 fill = 0);
    TileLayer* layer(std::string_view id);
    const TileLayer* layer(std::string_view id) const;
    TileLayer& default_layer();
    const TileLayer& default_layer() const;

    void set(i32 col, i32 row, u16 tile_id);
    void set(std::string_view layer_id, i32 col, i32 row, u16 tile_id);
    bool set_cell(Vec2i cell, std::string_view tile_id);
    bool set_cell(std::string_view layer_id, Vec2i cell, std::string_view tile_id);
    bool set_cell(Vec2i cell, TileCell tile);
    bool set_cell(std::string_view layer_id, Vec2i cell, TileCell tile);
    bool clear_cell(Vec2i cell);
    bool clear_cell(std::string_view layer_id, Vec2i cell);
    i32 set_cells(std::span<const Vec2i> cells, u16 tile_id);
    i32 set_cells(std::string_view layer_id, std::span<const Vec2i> cells, u16 tile_id);
    i32 set_cells(std::span<const Vec2i> cells, std::string_view tile_id);
    i32 set_cells(std::string_view layer_id, std::span<const Vec2i> cells, std::string_view tile_id);
    i32 fill_rect(TileRect rect, u16 tile_id);
    i32 fill_rect(std::string_view layer_id, TileRect rect, u16 tile_id);
    i32 replace_tile(u16 from_id, u16 to_id);
    i32 replace_tile(std::string_view layer_id, u16 from_id, u16 to_id);
    i32 flood_fill(Vec2i start, u16 replacement_tile);
    i32 flood_fill(std::string_view layer_id, Vec2i start, u16 replacement_tile);
    i32 set_terrain_connect(std::string_view layer_id, std::span<const Vec2i> cells, std::string_view terrain);
    void clear_terrain_diagnostics();
    u16 get(i32 col, i32 row) const;
    u16 get(std::string_view layer_id, i32 col, i32 row) const;
    TileCell cell(Vec2i cell) const;
    TileCell cell(std::string_view layer_id, Vec2i cell) const;
    std::vector<Vec2i> used_cells(std::string_view layer_id = {}) const;
    TileRect used_rect(std::string_view layer_id = {}) const;
    std::vector<Vec2i> neighbor_cells(Vec2i cell) const;
    const std::vector<TileRect>& dirty_regions(std::string_view layer_id) const;
    bool layer_dirty(std::string_view layer_id) const;
    void clear_dirty(std::string_view layer_id = {});
    TilePattern extract_pattern(std::string_view layer_id, TileRect rect, bool include_empty = true) const;
    i32 stamp_pattern(std::string_view layer_id, Vec2i target, const TilePattern& pattern, bool skip_empty = false);
    bool solid(i32 col, i32 row) const;
    bool solid(std::string_view layer_id, i32 col, i32 row) const;
    bool walkable(i32 col, i32 row) const;
    bool walkable(std::string_view layer_id, i32 col, i32 row) const;
    i32 movement_cost(i32 col, i32 row) const;
    i32 movement_cost(std::string_view layer_id, i32 col, i32 row) const;
    bool blocks_point(Vec2f world_pos) const;
    bool blocks_point(std::string_view layer_id, Vec2f world_pos) const;
    bool blocks_rect(Rectf world_rect) const;
    bool blocks_rect(std::string_view layer_id, Rectf world_rect) const;
    std::vector<Rectf> collision_rects(std::string_view layer_id = {}) const;
    std::vector<std::vector<Vec2f>> collision_polygons(std::string_view layer_id = {}) const;
    void invalidate_render_cache(std::string_view layer_id = {}) const;
    void set_render_chunk_size(i32 cols, i32 rows);
    Vec2i render_chunk_size() const;
    std::size_t cached_render_chunk_count(std::string_view layer_id = {}) const;
    const TileDefinition* tile(u16 tile_id) const;
    const TileDefinition* tile(std::string_view id) const;
    u16 tile_id(std::string_view id) const;

    i32 world_col(f32 wx) const;
    i32 world_row(f32 wy) const;
    Vec2i world_to_map(Vec2f world_pos) const;
    Vec2f map_to_world(Vec2i cell) const;
    Vec2f tile_center(Vec2i cell) const;
    Vec2f world_size() const;
    Rectf tile_rect(i32 col, i32 row) const;
    bool in_bounds(i32 col, i32 row) const;

    void submit(RenderQueue& queue, TilemapRenderOptions options = {}) const;
    void submit(RenderQueue& queue, const RenderView& view, TilemapRenderOptions options = {}) const;
    void render(Renderer2D& renderer, TilemapRenderOptions options = {}) const;
    void render(Renderer2D& renderer, const RenderView& view, TilemapRenderOptions options = {}) const;

private:
    struct RenderCacheKey {
        i32 layer = layer_value(RenderLayer::World);
        i32 order = 0;
        bool draw_top_edge = true;
        u64 pass_mask = render_pass_mask::world;
        const SpriteCatalog* sprites = nullptr;

        friend constexpr bool operator==(RenderCacheKey, RenderCacheKey) = default;
    };

    struct RenderChunkCache {
        TileRect bounds{};
        bool dirty = true;
        std::vector<RenderCommand> commands;
    };

    struct LayerRenderCache {
        std::string layer_id;
        Vec2i chunk_size{0, 0};
        i32 cols = 0;
        i32 rows = 0;
        bool valid = false;
        RenderCacheKey key{};
        std::vector<RenderChunkCache> chunks;
    };

    void mark_dirty(TileLayer& layer, TileRect rect);
    void invalidate_render_cache_for_dirty(const TileLayer& layer, TileRect rect) const;
    RenderCacheKey make_render_cache_key(const TileLayer& layer, TilemapRenderOptions options) const;
    Vec2i effective_render_chunk_size(TilemapRenderOptions options) const;
    void reset_render_cache_chunks(LayerRenderCache& cache, const RenderCacheKey& key, Vec2i chunk_size, i32 chunk_cols, i32 chunk_rows) const;
    void rebuild_render_chunk(RenderChunkCache& chunk, const TileLayer& layer, const RenderCacheKey& key) const;
    void submit_uncached_layer(RenderQueue& queue, const TileLayer& layer, TileRect visible_tiles, const RenderCacheKey& key) const;
    void submit_cached_layer(RenderQueue& queue, const TileLayer& layer, TileRect visible_tiles, const RenderCacheKey& key, TilemapRenderOptions options) const;
    LayerRenderCache* render_cache(std::string_view layer_id) const;
    LayerRenderCache& render_cache_for(std::string_view layer_id) const;
    void reset_render_cache(LayerRenderCache& cache) const;
    std::size_t index(i32 col, i32 row) const;

    Vec2i _render_chunk_size{16, 16};
    mutable std::vector<LayerRenderCache> _render_caches;
};

TileMap load_tilemap(const std::filesystem::path& path);
bool save_tilemap(const TileMap& map, const std::filesystem::path& path);
void register_tilemap_loader(AssetManager& assets);

} // namespace kin
