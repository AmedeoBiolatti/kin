#pragma once

// A tilemap in a 2D world: an entity showing a map file (.kintilemap), its
// cells square or hexagons, drawn by WorldRenderState with the rest. Scene
// files name the map (TilemapComponent's `asset`); Assets2D reads it, the
// tileset it names (.kintileset) and the image that tileset's tiles are cut
// from, and reads them again when they change.
//
//     kin::EcsEntity level = world.entity();
//     level.set(kin::Transform2D{.pos = {0, 0}});
//     level.set(kin::TilemapComponent{.asset = {"maps/level.kintilemap"}});
//     // later: the map itself, to ask it (walkable cells, paths)
//     if (const auto* tiles = level.get<kin::TilemapComponent>(); tiles && tiles->tilemap) {
//         const kin::Vec2i cell = tiles->tilemap->map.world_to_map(point - position);
//     }

#include <kin/ecs/component.hpp>
#include <kin/renderer/render_layer.hpp>
#include <kin/renderer/sprite_sheet.hpp>
#include <kin/tilemap/tilemap.hpp>

#include <memory>

namespace kin {

// A map as Assets2D reads it: its cells and tiles, and its tileset's image
// cut in frames (invalid when the tileset has none).
struct TilemapAsset {
    TileMap map;
    SpriteSheet sheet;
};

// A map drawn with its top left where its entity is (its world position;
// rotation and scale are not used), on `layer` (behind the world's sprites,
// unless set: RenderLayer::TileFront is before them), its layers' orders
// after `order`.
struct TilemapComponent {
    ComponentAssetRef asset; // the map file
    i32 layer = layer_value(RenderLayer::TileBack);
    i32 order = 0;
    bool visible = true;
    bool static_renderable = true; // drawn into a scene's cached static queue
    // What `asset` names, read (Assets2D's binder sets it). Null: nothing drawn.
    std::shared_ptr<const TilemapAsset> tilemap;
};

} // namespace kin
