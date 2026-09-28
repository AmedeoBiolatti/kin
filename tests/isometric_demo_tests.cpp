#include "../games/isometric_demo/src/iso_math.hpp"
#include "../games/isometric_demo/src/iso_order.hpp"

#include <cassert>
#include <cmath>

namespace {

bool near(kin::f32 a, kin::f32 b, kin::f32 epsilon = 0.001f) {
    return std::abs(a - b) <= epsilon;
}

void test_projection_round_trip() {
    isometric_demo::IsoView view{
        .origin = {480.0f, 116.0f},
        .camera = {32.0f, -18.0f},
        .zoom = 1.35f,
    };

    const kin::Vec2f world{5.25f, 8.75f};
    const kin::Vec2f screen = view.world_to_screen(world);
    const kin::Vec2f round_trip = view.screen_to_world(screen);

    assert(near(round_trip.x, world.x));
    assert(near(round_trip.y, world.y));
}

void test_picking_cell_from_center() {
    isometric_demo::IsoView view{
        .origin = {480.0f, 116.0f},
        .camera = {},
        .zoom = 1.0f,
    };

    const kin::Vec2i cell{3, 9};
    const kin::Vec2f screen = view.world_to_screen({
        static_cast<kin::f32>(cell.x) + 0.5f,
        static_cast<kin::f32>(cell.y) + 0.5f,
    });
    const kin::Vec2f world = view.screen_to_world(screen);
    const kin::Vec2i picked{
        static_cast<kin::i32>(std::floor(world.x)),
        static_cast<kin::i32>(std::floor(world.y)),
    };

    assert(picked == cell);
}

void test_cell_order_progresses_by_position() {
    constexpr kin::i32 map_cols = 16;
    assert(isometric_demo::cell_order_key({3, 8}, map_cols) <
           isometric_demo::cell_order_key({3, 9}, map_cols));

    assert(isometric_demo::cell_order_key({2, 9}, map_cols) <
           isometric_demo::cell_order_key({3, 8}, map_cols));
    assert(isometric_demo::cell_order_key({3, 8}, map_cols) <
           isometric_demo::cell_order_key({4, 7}, map_cols));
}

void test_content_depth_uses_cell_order_first() {
    constexpr kin::i32 map_cols = 16;
    const kin::i32 base_3_8 = isometric_demo::render_depth({3, 8}, isometric_demo::BaseContentOffset, map_cols);
    const kin::i32 object_3_8 = isometric_demo::render_depth({3, 8}, isometric_demo::MovableObjectOffset, map_cols);
    const kin::i32 base_3_9 = isometric_demo::render_depth({3, 9}, isometric_demo::BaseContentOffset, map_cols);
    const kin::i32 object_3_9 = isometric_demo::render_depth({3, 9}, isometric_demo::MovableObjectOffset, map_cols);

    assert(base_3_8 < object_3_8);
    assert(object_3_8 < base_3_9);
    assert(base_3_9 < object_3_9);
}

} // namespace

int main() {
    test_projection_round_trip();
    test_picking_cell_from_center();
    test_cell_order_progresses_by_position();
    test_content_depth_uses_cell_order_first();
    return 0;
}
