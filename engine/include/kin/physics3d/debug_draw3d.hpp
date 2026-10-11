#pragma once

// Lines for looking at a World3D (experimental, docs/physics_3d.md): the shapes of its bodies as wireframes,
// awake and sleeping bodies in different colours, constraints, contacts and normals, a query's ray and where it hit.
// Plain data, no Jolt header and no renderer: a list of coloured segments in world space. kin/ecs/physics3d.hpp has the
// opt-in that draws them into a 3D view; a tool can also project them itself.
//
// The shapes come from Jolt's own geometry (a box's twelve edges, a sphere's three circles, a hull's faces, a mesh's or
// height field's triangles up to a cap), so no debug renderer is built into Jolt for this.

#include <kin/core/math3d.hpp>
#include <kin/physics3d/world3d.hpp>
#include <kin/renderer/color.hpp>

#include <span>
#include <vector>

namespace kin {

struct DebugLine3D {
    Vec3f a{};
    Vec3f b{};
    Color color = colors::white;
};

struct DebugDraw3DOptions {
    bool bodies = true;
    bool constraints = true;
    // Sleeping bodies in `sleeping`, awake ones in the colour of their motion.
    bool colour_by_sleep = true;
    // A triangle mesh or height field is drawn with at most this many triangles (an even stride through them).
    u32 max_triangles_per_shape = 600;
    // The list stops growing here (`truncated()` says so).
    u32 max_lines = 400000;
    // Segments of a circle (sphere, capsule and cylinder outlines).
    u32 circle_segments = 16;
    // A contact's normal and a hit's normal, in metres; the half size of a point's cross.
    f32 normal_length = 0.3f;
    f32 point_size = 0.04f;

    Color static_body = Color::rgb(150, 150, 165);
    Color kinematic_body = Color::rgb(70, 200, 255);
    Color dynamic_body = Color::rgb(90, 230, 90);
    Color sleeping_body = Color::rgb(100, 110, 190);
    Color sensor_body = Color::rgb(255, 220, 60);
    Color constraint = Color::rgb(255, 80, 220);
    Color broken_constraint = Color::rgb(255, 40, 40);
    Color contact_point = Color::rgb(255, 60, 60);
    Color contact_normal = Color::rgb(255, 170, 0);
    Color ray = Color::rgb(255, 255, 255);
    Color ray_hit = Color::rgb(0, 255, 255);
};

class DebugDraw3D {
public:
    explicit DebugDraw3D(DebugDraw3DOptions options = {})
        : _options(options) {}

    DebugDraw3DOptions& options() { return _options; }
    const DebugDraw3DOptions& options() const { return _options; }

    void clear() {
        _lines.clear();
        _truncated = false;
    }
    std::span<const DebugLine3D> lines() const { return _lines; }
    bool truncated() const { return _truncated; }

    void line(Vec3f a, Vec3f b, Color color);
    // A small three axis cross at `point`.
    void cross(Vec3f point, Color color);
    // `shape` with its origin at `position` (a body's origin, not its centre of mass).
    void shape(const Shape3D& shape, Vec3f position, Quatf rotation, Color color);
    // Every body (by its colour: motion, sensor, asleep) and constraint of the world, in body number order.
    void world(const World3D& world);
    // A cross at each contact point and a line along its normal (Begin and Persist events; End has no point).
    void contacts(std::span<const ContactEvent3D> events);
    // The ray's segment up to its hit (or all of it), a cross and the normal at the hit.
    void ray(Vec3f from, Vec3f to, const std::optional<RayHit3D>& hit);
    // A swept shape: it at the start, at the end (or where it hit), and the hit's point and normal.
    void shape_cast(const Shape3D& shape, Vec3f position, Quatf rotation, Vec3f displacement,
                    const std::optional<ShapeHit3D>& hit);

private:
    void circle(Vec3f centre, Vec3f u, Vec3f v, f32 radius, Color color);
    void arc(Vec3f centre, Vec3f u, Vec3f v, f32 radius, f32 from_angle, f32 to_angle, Color color);
    void jolt_shape(const void* shape, Vec3f centre_of_mass, Quatf rotation, Color color, int depth);

    DebugDraw3DOptions _options;
    std::vector<DebugLine3D> _lines;
    bool _truncated = false;
};

} // namespace kin
