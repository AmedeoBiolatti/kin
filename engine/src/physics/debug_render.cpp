#include <kin/physics/debug_render.hpp>

#include <algorithm>
#include <cmath>

namespace kin {
namespace {

constexpr f32 pi = 3.1415926535f;

Vec2f add(Vec2f a, Vec2f b) {
    return {a.x + b.x, a.y + b.y};
}

Vec2f mul(Vec2f value, f32 scalar) {
    return {value.x * scalar, value.y * scalar};
}

Color color_for(const PhysicsDebugShape& shape, const PhysicsDebugRenderOptions& options) {
    if (shape.sensor) {
        return options.sensor_color;
    }
    if (!shape.awake && shape.body_type == PhysicsBodyType::Dynamic) {
        return options.sleeping_color;
    }

    switch (shape.body_type) {
    case PhysicsBodyType::Static: return options.static_color;
    case PhysicsBodyType::Dynamic: return options.dynamic_color;
    case PhysicsBodyType::Kinematic: return options.kinematic_color;
    }
    return options.static_color;
}

void submit_circle(RenderQueue& queue, RenderKey key, Vec2f center, f32 radius, i32 segments, Color color) {
    const i32 clamped_segments = std::max(8, segments);
    Vec2f previous{center.x + radius, center.y};
    for (i32 i = 1; i <= clamped_segments; ++i) {
        const f32 t = (static_cast<f32>(i) / static_cast<f32>(clamped_segments)) * pi * 2.0f;
        const Vec2f next{center.x + std::cos(t) * radius, center.y + std::sin(t) * radius};
        queue.draw_line(key, previous, next, color);
        previous = next;
    }
}

} // namespace

void submit_physics_debug(RenderQueue& queue, const PhysicsWorld& physics, PhysicsDebugRenderOptions options) {
    for (const PhysicsDebugShape& shape : physics.debug_shapes(options.filter)) {
        const Color color = color_for(shape, options);
        switch (shape.type) {
        case PhysicsDebugShapeType::Polygon:
            for (std::size_t i = 0; i < shape.points.size(); ++i) {
                queue.draw_line(options.key, shape.points[i], shape.points[(i + 1) % shape.points.size()], color);
            }
            break;
        case PhysicsDebugShapeType::Circle:
            submit_circle(queue, options.key, shape.center, shape.radius, options.circle_segments, color);
            break;
        }
    }

    if (!options.draw_contacts) {
        return;
    }

    for (const PhysicsContactEvent& event : physics.contacts()) {
        if (!event.has_point) {
            continue;
        }
        queue.draw_line(options.key, {event.point.x - 5.0f, event.point.y}, {event.point.x + 5.0f, event.point.y}, options.contact_color);
        queue.draw_line(options.key, {event.point.x, event.point.y - 5.0f}, {event.point.x, event.point.y + 5.0f}, options.contact_color);
        queue.draw_line(options.key, event.point, add(event.point, mul(event.normal, 18.0f)), options.contact_color);
    }
}

} // namespace kin
