#pragma once

// Collision shapes for kin/physics3d (Jolt Physics behind them; no Jolt header is
// included here). Experimental: the API may change in any release
// (docs/3d_plan.md, docs/physics_3d.md).
//
// A Shape3D is a cheap, shareable handle: copies point at the same immutable
// shape, any number of bodies in any number of worlds may use one, and a shape
// outlives the world it was first used in. Shapes are built in the body's own
// space (its origin at the body's position); a compound places children in it.
// A factory that cannot build its shape (a degenerate hull, a mesh with indices
// out of range, a size of zero) returns an invalid handle, never a half shape.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>

#include <memory>
#include <span>

namespace kin {

namespace physics3d_detail {
struct ShapeData;
}

enum class Shape3DKind : u8 {
    Box,
    Sphere,
    Capsule,
    Cylinder,
    ConvexHull,
    TriangleMesh,
    HeightField,
    Compound,
};

class Shape3D;

// One piece of a compound: a shape placed in the compound's space.
struct ShapeChild3D;

class Shape3D {
public:
    Shape3D() = default;

    // A box of half_extents along each axis. Its corners are rounded by 5 cm at
    // most (Jolt's convex radius, kept inside the shape: a face stays where it is).
    static Shape3D box(Vec3f half_extents);
    static Shape3D sphere(f32 radius);
    // Along +Y: a cylinder of `half_height` (half its straight part, so the
    // capsule is 2 * (half_height + radius) tall) capped by two hemispheres.
    static Shape3D capsule(f32 half_height, f32 radius);
    // Along +Y, `half_height` the half of its height.
    static Shape3D cylinder(f32 half_height, f32 radius);
    // The convex hull of the points (at least four, not all in a plane).
    static Shape3D convex_hull(std::span<const Vec3f> points);
    // A static triangle mesh: three indices a triangle. For static and kinematic
    // bodies only (a mesh has no volume, so a dynamic body of one is refused).
    // Hollow and double sided.
    static Shape3D triangle_mesh(std::span<const Vec3f> vertices, std::span<const u32> indices);
    // A height field: sample_count x sample_count heights, row major (sample
    // (x, z) at index z * sample_count + x), at x = offset.x + scale.x * x,
    // y = offset.y + scale.y * height, z = offset.z + scale.z * z. sample_count is
    // even and at least 4. A height of FLT_MAX is a hole. Heights are stored in
    // 8 bits per sample relative to their block, so a surface is within about
    // a 256th of its block's height range. Static and kinematic bodies only.
    static Shape3D height_field(std::span<const f32> heights, i32 sample_count, Vec3f offset = {},
                                Vec3f scale = {1.0f, 1.0f, 1.0f});
    // Several shapes as one rigid shape (at least one). Its centre of mass is
    // the volume-weighted mean of the children's.
    static Shape3D compound(std::span<const ShapeChild3D> children);

    bool valid() const { return _data != nullptr; }
    explicit operator bool() const { return valid(); }
    Shape3DKind kind() const;

    // Of the solid (zero for a mesh or a height field, which are surfaces).
    f32 volume() const;
    // In the shape's own space, where a body of it has its origin.
    Vec3f centre_of_mass() const;
    // The box that holds the shape in its own space.
    Vec3f bounds_min() const;
    Vec3f bounds_max() const;

    friend bool operator==(const Shape3D& a, const Shape3D& b) { return a._data == b._data; }

private:
    friend class World3D;
    friend class DebugDraw3D;
    std::shared_ptr<const physics3d_detail::ShapeData> _data;
};

struct ShapeChild3D {
    Shape3D shape;
    Vec3f position{};
    Quatf rotation{};
};

} // namespace kin
