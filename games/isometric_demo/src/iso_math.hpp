#pragma once

#include <kin/core/types.hpp>

namespace isometric_demo {

constexpr kin::f32 DefaultTileW = 64.0f;
constexpr kin::f32 DefaultTileH = 32.0f;

struct IsoView {
    kin::Vec2f origin{};
    kin::Vec2f camera{};
    kin::f32 zoom = 1.0f;
    kin::f32 tile_w = DefaultTileW;
    kin::f32 tile_h = DefaultTileH;

    kin::f32 half_tile_w() const { return tile_w * 0.5f; }
    kin::f32 half_tile_h() const { return tile_h * 0.5f; }

    kin::Vec2f world_to_iso(kin::Vec2f world) const {
        return {
            (world.x - world.y) * half_tile_w(),
            (world.x + world.y) * half_tile_h(),
        };
    }

    kin::Vec2f iso_to_world(kin::Vec2f iso) const {
        const kin::f32 x = iso.x / half_tile_w();
        const kin::f32 y = iso.y / half_tile_h();
        return {(x + y) * 0.5f, (y - x) * 0.5f};
    }

    kin::Vec2f world_to_screen(kin::Vec2f world) const {
        const kin::Vec2f iso = world_to_iso(world);
        return {
            origin.x + (iso.x - camera.x) * zoom,
            origin.y + (iso.y - camera.y) * zoom,
        };
    }

    kin::Vec2f screen_to_world(kin::Vec2f screen) const {
        const kin::Vec2f iso{
            (screen.x - origin.x) / zoom + camera.x,
            (screen.y - origin.y) / zoom + camera.y,
        };
        return iso_to_world(iso);
    }
};

} // namespace isometric_demo
