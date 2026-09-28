#pragma once

#include <kin/physics/physics.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/render_layer.hpp>
#include <kin/renderer/render_queue.hpp>

namespace kin {

struct PhysicsDebugRenderOptions {
    PhysicsFilter filter{};
    RenderKey key{
        .layer = layer_value(RenderLayer::Debug),
        .order = 0,
        .pass_mask = render_pass_mask::debug,
    };
    Color static_color = Color::rgb(126, 132, 148);
    Color dynamic_color = Color::rgb(84, 216, 132);
    Color kinematic_color = Color::rgb(88, 166, 255);
    Color sensor_color = Color::rgb(245, 204, 72);
    Color sleeping_color = Color::rgb(72, 76, 86);
    Color contact_color = Color::rgb(238, 92, 92);
    i32 circle_segments = 32;
    bool draw_contacts = true;
};

void submit_physics_debug(RenderQueue& queue, const PhysicsWorld& physics, PhysicsDebugRenderOptions options = {});

} // namespace kin
