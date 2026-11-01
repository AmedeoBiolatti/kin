#pragma once

#include <kin/assets/asset_manager.hpp>
#include <kin/core/hex.hpp>
#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/render_layer.hpp>
#include <kin/renderer/render_queue.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite_catalog.hpp>
#include <kin/renderer/sprite_sheet.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace kin {

struct ShapeMesh;

using TileMetadataValue = std::variant<bool, i32, f32, std::string>;

// The sides a cell's terrain joins its neighbours' at (TerrainVariant::mask):
// a square's four, these; a hexagon's six, side d toward neighbour direction
// d (bit 1 << d: tile_terrain_hex_side). With HexDirections those are, for
// pointy-top hexes, east, north-east, north-west, west, south-west and
// south-east; for flat-top ones, south-east, north-east, north, north-west,
// south-west and south.
enum TileTerrainMask : u8 {
    TileTerrainNorth = 1 << 0,
    TileTerrainEast = 1 << 1,
    TileTerrainSouth = 1 << 2,
    TileTerrainWest = 1 << 3,
};

constexpr u8 tile_terrain_hex_side(i32 direction) {
    return static_cast<u8>(1u << hex_wrap_direction(direction));
}

// Every side of a square (0x0F) or of a hexagon (0x3F).
constexpr u8 tile_terrain_sides(bool hex) {
    return hex ? 0x3F : 0x0F;
}

// The shape of a map's cells: squares in rows and columns; or hexagons,
// pointy-top (in rows, every other one half a hex right or left) or flat-top
// (in columns), kept as the cols x rows offset cells of HexLayout. A hex
// cell's box (tile_w x tile_h, what map_to_world() and tile_rect() give)
// bounds its hexagon, and its tile's image is drawn in it.
enum class TileShape : u8 { Square, Hex };

struct TileGrid {
    TileShape shape = TileShape::Square;
    HexOrientation orientation = HexOrientation::PointyTop;
    HexOffset offset = HexOffset::Odd; // which rows (pointy) or columns (flat) sit half a hex further on

    bool hex() const { return shape == TileShape::Hex; }
    friend constexpr bool operator==(TileGrid, TileGrid) = default;
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
    // Its frame of its tileset's image (TileSet::image, cut as TileSet::sheet
    // says), from 0 at the top left, along the rows; -1: none. A sprite (in
    // TilemapRenderOptions::sprites) is drawn before it; else its colour.
    i32 frame = -1;
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
    // Which tile a terrain's cell is for each set of sides it joins at (its
    // mask); the one marked fallback when none is near (else its first).
    std::vector<TerrainVariant> terrain_variants;
    // The cells its tiles are drawn for (their variants' sides: four or six),
    // for the editors; a map uses its own grid. Square unless said.
    TileGrid grid{};
    // The image its tiles' frames are cut from: a content path, as scenes name
    // assets ("images/tiles.png"), in frames of sheet.tile_w x sheet.tile_h
    // (sheet.margin around them, sheet.spacing between). Empty: none.
    std::string image;
    SpriteSheetGrid sheet{};

    void clear();
    void resize(std::size_t count);
    std::size_t size() const;
    bool empty() const;

    TileDefinition* tile(u16 tile_id);
    const TileDefinition* tile(u16 tile_id) const;
    u16 tile_id(std::string_view id) const;
    u16 ensure_tile(std::string_view id);
    void add_terrain_variant(std::string_view terrain, u8 mask, u16 tile_id, bool fallback = false);
    // The variant of `terrain` joining exactly `mask`'s sides; 0: none.
    u16 terrain_tile(std::string_view terrain, u8 mask) const;
    u16 terrain_fallback_tile(std::string_view terrain) const;
    // The tile a cell of `terrain` joining `mask`'s sides is drawn with: the
    // variant joining exactly those; else the one joining most of them and no
    // other (the first listed of those); else the fallback. 0: the terrain has
    // no variants. `exact`, when given: whether its own variant was found.
    u16 terrain_match(std::string_view terrain, u8 mask, bool* exact = nullptr) const;
    // The terrains its tiles and variants name, each once, in order.
    std::vector<std::string> terrains() const;
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
    // The tileset's image cut in frames (TileDefinition::frame), when it has one.
    const SpriteSheet* sheet = nullptr;
    // Where the map's top left is drawn (its cells' world positions plus this).
    Vec2f origin{};
};

// What a map's solid cells stop, as shapes to collide with (from the map's
// top left): `loops` around each run of touching solid cells (those without a
// collision polygon of their own), joined, straight sides one; wound clockwise
// on screen (y down), the solid on the right going along it; a hole's loop the
// other way round, so its solid is on the right too. `polygons`: each cell
// with a collision polygon, its own.
struct TileCollisionShapes {
    std::vector<std::vector<Vec2f>> loops;
    std::vector<std::vector<Vec2f>> polygons;
};

class TileMap {
public:
    i32 cols = 0;
    i32 rows = 0;
    i32 tile_w = 16;
    i32 tile_h = 16;
    TileGrid grid;
    TileSet tileset;
    // The tileset file (.kintileset, a content path) its tiles are from,
    // shared with other maps; empty when the map keeps its own. A map naming
    // one is saved naming it (not with its tiles).
    std::string tileset_source;
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
    // Cells set to `terrain`, each with the variant joining its neighbours of
    // that terrain (four sides, or a hexagon's six), and their neighbours
    // fitted again (each to its own terrain). Cells changed, those around them
    // included; a missing variant (the nearest used) or fallback is said in
    // terrain_diagnostics.
    i32 set_terrain_connect(std::string_view layer_id, std::span<const Vec2i> cells, std::string_view terrain);
    // Cells emptied, the terrain cells around them fitted again.
    i32 clear_terrain_connect(std::string_view layer_id, std::span<const Vec2i> cells);
    // The sides of `cell` with a neighbour of `terrain` (in the map).
    u8 terrain_mask(std::string_view layer_id, Vec2i cell, std::string_view terrain) const;
    void clear_terrain_diagnostics();
    u16 get(i32 col, i32 row) const;
    u16 get(std::string_view layer_id, i32 col, i32 row) const;
    TileCell cell(Vec2i cell) const;
    TileCell cell(std::string_view layer_id, Vec2i cell) const;
    std::vector<Vec2i> used_cells(std::string_view layer_id = {}) const;
    TileRect used_rect(std::string_view layer_id = {}) const;
    // Its neighbours in the map: four (square cells) or six (hexes).
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
    TileCollisionShapes collision_shapes(std::string_view layer_id = {}) const;
    void invalidate_render_cache(std::string_view layer_id = {}) const;
    void set_render_chunk_size(i32 cols, i32 rows);
    Vec2i render_chunk_size() const;
    std::size_t cached_render_chunk_count(std::string_view layer_id = {}) const;
    const TileDefinition* tile(u16 tile_id) const;
    const TileDefinition* tile(std::string_view id) const;
    u16 tile_id(std::string_view id) const;

    // A square map's column and row at a world x and y (a hex map's cells are
    // not in straight columns: world_to_map() finds them).
    i32 world_col(f32 wx) const;
    i32 world_row(f32 wy) const;
    // The cell at a point (a hex map's: the hexagon it is in).
    Vec2i world_to_map(Vec2f world_pos) const;
    // A cell's box: its top left, its centre, all of it.
    Vec2f map_to_world(Vec2i cell) const;
    Vec2f tile_center(Vec2i cell) const;
    Rectf tile_rect(i32 col, i32 row) const;
    // A hex cell's hexagon's corners (a square cell's four).
    std::vector<Vec2f> tile_outline(Vec2i cell) const;
    // Around every cell, from (0, 0).
    Vec2f world_size() const;
    bool in_bounds(i32 col, i32 row) const;
    // How a hex map's cells are laid out (their centres, sizes, offsets).
    HexLayout hex_layout() const;

    // Drawn: each tile its sprite, its frame of the tileset's image, or its
    // colour (a hex map's, its hexagon filled).
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
        const SpriteSheet* sheet = nullptr;

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

    i32 connect_terrain(TileLayer& layer, std::span<const Vec2i> painted, u16 tile);
    void mark_dirty(TileLayer& layer, TileRect rect);
    void invalidate_render_cache_for_dirty(const TileLayer& layer, TileRect rect) const;
    RenderCacheKey make_render_cache_key(const TileLayer& layer, TilemapRenderOptions options) const;
    Vec2i effective_render_chunk_size(TilemapRenderOptions options) const;
    void reset_render_cache_chunks(LayerRenderCache& cache, const RenderCacheKey& key, Vec2i chunk_size, i32 chunk_cols, i32 chunk_rows) const;
    void rebuild_render_chunk(RenderChunkCache& chunk, const TileLayer& layer, const RenderCacheKey& key) const;
    void submit_uncached_layer(RenderQueue& queue, const TileLayer& layer, TileRect visible_tiles, const RenderCacheKey& key,
                               Vec2f origin) const;
    void submit_cached_layer(RenderQueue& queue, const TileLayer& layer, TileRect visible_tiles, const RenderCacheKey& key, TilemapRenderOptions options) const;
    // A hexagon filling a hex cell's box (centred on the origin), shared by every colour tile.
    std::shared_ptr<const ShapeMesh> hex_tile_mesh() const;
    LayerRenderCache* render_cache(std::string_view layer_id) const;
    LayerRenderCache& render_cache_for(std::string_view layer_id) const;
    void reset_render_cache(LayerRenderCache& cache) const;
    std::size_t index(i32 col, i32 row) const;

    Vec2i _render_chunk_size{16, 16};
    mutable std::vector<LayerRenderCache> _render_caches;
    mutable std::shared_ptr<const ShapeMesh> _hex_mesh;
    mutable Vec2i _hex_mesh_size{};
    mutable HexOrientation _hex_mesh_orientation = HexOrientation::PointyTop;
};

// A map file (.kintilemap: JSON, format kin.tilemap; or the older text form).
// A map naming a tileset file (tileset_source) is read without its tiles; the
// second form reads them too, from `content_root`; `error`, when given, says
// why either could not be read (an empty map then).
TileMap load_tilemap(const std::filesystem::path& path);
TileMap load_tilemap(const std::filesystem::path& path, const std::filesystem::path& content_root, std::string* error = nullptr);
bool save_tilemap(const TileMap& map, const std::filesystem::path& path);
void register_tilemap_loader(AssetManager& assets);

// A tileset in a file of its own (.kintileset: JSON, format kin.tileset), which
// maps name (TileMap::tileset_source) to share it: its tiles, its terrain
// variants, the image its tiles' frames are cut from. Empty, and `error` says
// why, when it can't be read.
TileSet load_tileset(const std::filesystem::path& path, std::string* error = nullptr);
bool save_tileset(const TileSet& tileset, const std::filesystem::path& path);

} // namespace kin
