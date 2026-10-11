#include <kin/physics3d/shape3d.hpp>

#include "jolt_runtime.hpp"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace kin {

namespace physics3d_detail {

void ensure_jolt_runtime() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

} // namespace physics3d_detail

namespace {

using physics3d_detail::ShapeData;
using physics3d_detail::to_jolt;

// Jolt's own default for the rounded edge of boxes, cylinders and hulls.
constexpr f32 max_convex_radius = 0.05f;

bool positive_finite(f32 v) { return std::isfinite(v) && v > 0.0f; }
bool finite(Vec3f v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

std::shared_ptr<const ShapeData> make_data(const JPH::ShapeSettings::ShapeResult& result, Shape3DKind kind) {
    if (result.HasError() || !result.IsValid()) {
        return nullptr;
    }
    auto data = std::make_shared<ShapeData>();
    data->shape = result.Get();
    data->kind = kind;
    return data;
}

} // namespace

Shape3D Shape3D::box(Vec3f half_extents) {
    Shape3D out;
    if (!finite(half_extents) || !positive_finite(half_extents.x) || !positive_finite(half_extents.y)
        || !positive_finite(half_extents.z)) {
        return out;
    }
    physics3d_detail::ensure_jolt_runtime();
    const f32 radius = std::min({max_convex_radius, half_extents.x, half_extents.y, half_extents.z});
    JPH::BoxShapeSettings settings(to_jolt(half_extents), radius);
    out._data = make_data(settings.Create(), Shape3DKind::Box);
    return out;
}

Shape3D Shape3D::sphere(f32 radius) {
    Shape3D out;
    if (!positive_finite(radius)) {
        return out;
    }
    physics3d_detail::ensure_jolt_runtime();
    JPH::SphereShapeSettings settings(radius);
    out._data = make_data(settings.Create(), Shape3DKind::Sphere);
    return out;
}

Shape3D Shape3D::capsule(f32 half_height, f32 radius) {
    Shape3D out;
    if (!positive_finite(half_height) || !positive_finite(radius)) {
        return out;
    }
    physics3d_detail::ensure_jolt_runtime();
    JPH::CapsuleShapeSettings settings(half_height, radius);
    out._data = make_data(settings.Create(), Shape3DKind::Capsule);
    return out;
}

Shape3D Shape3D::cylinder(f32 half_height, f32 radius) {
    Shape3D out;
    if (!positive_finite(half_height) || !positive_finite(radius)) {
        return out;
    }
    physics3d_detail::ensure_jolt_runtime();
    JPH::CylinderShapeSettings settings(half_height, radius, std::min({max_convex_radius, half_height, radius}));
    out._data = make_data(settings.Create(), Shape3DKind::Cylinder);
    return out;
}

Shape3D Shape3D::convex_hull(std::span<const Vec3f> points) {
    Shape3D out;
    if (points.size() < 4) {
        return out;
    }
    JPH::Array<JPH::Vec3> jolt_points;
    jolt_points.reserve(points.size());
    Vec3f lo = points[0];
    Vec3f hi = points[0];
    for (const Vec3f& p : points) {
        if (!finite(p)) {
            return out;
        }
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        jolt_points.push_back(to_jolt(p));
    }
    physics3d_detail::ensure_jolt_runtime();
    // A rounded edge as wide as a quarter of the thinnest side, at most 5 cm.
    const Vec3f size = hi - lo;
    const f32 radius = std::min(max_convex_radius, 0.25f * std::min({size.x, size.y, size.z}));
    JPH::ConvexHullShapeSettings settings(jolt_points.data(), static_cast<int>(jolt_points.size()), radius);
    out._data = make_data(settings.Create(), Shape3DKind::ConvexHull);
    return out;
}

Shape3D Shape3D::triangle_mesh(std::span<const Vec3f> vertices, std::span<const u32> indices) {
    Shape3D out;
    if (vertices.empty() || indices.size() < 3 || indices.size() % 3 != 0) {
        return out;
    }
    for (const u32 i : indices) {
        if (i >= vertices.size()) {
            return out;
        }
    }
    JPH::VertexList verts;
    verts.reserve(vertices.size());
    for (const Vec3f& v : vertices) {
        if (!finite(v)) {
            return out;
        }
        verts.push_back(JPH::Float3(v.x, v.y, v.z));
    }
    JPH::IndexedTriangleList triangles;
    triangles.reserve(indices.size() / 3);
    for (std::size_t i = 0; i < indices.size(); i += 3) {
        triangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2]));
    }
    physics3d_detail::ensure_jolt_runtime();
    JPH::MeshShapeSettings settings(std::move(verts), std::move(triangles));
    out._data = make_data(settings.Create(), Shape3DKind::TriangleMesh);
    return out;
}

Shape3D Shape3D::height_field(std::span<const f32> heights, i32 sample_count, Vec3f offset, Vec3f scale) {
    Shape3D out;
    if (sample_count < 4 || sample_count % 2 != 0
        || heights.size() != static_cast<std::size_t>(sample_count) * static_cast<std::size_t>(sample_count)
        || !finite(offset) || !finite(scale)) {
        return out;
    }
    physics3d_detail::ensure_jolt_runtime();
    JPH::HeightFieldShapeSettings settings(heights.data(), to_jolt(offset), to_jolt(scale),
                                           static_cast<JPH::uint32>(sample_count));
    out._data = make_data(settings.Create(), Shape3DKind::HeightField);
    return out;
}

Shape3D Shape3D::compound(std::span<const ShapeChild3D> children) {
    Shape3D out;
    if (children.empty()) {
        return out;
    }
    physics3d_detail::ensure_jolt_runtime();
    JPH::StaticCompoundShapeSettings settings;
    // The child's index goes in as its user data: it is what a ray or shape hit reports as the sub shape.
    JPH::uint32 index = 0;
    for (const ShapeChild3D& child : children) {
        if (!child.shape.valid() || !finite(child.position)) {
            return out;
        }
        settings.AddShape(to_jolt(child.position), to_jolt(child.rotation.normalized()), child.shape._data->shape, index++);
    }
    out._data = make_data(settings.Create(), Shape3DKind::Compound);
    return out;
}

Shape3DKind Shape3D::kind() const {
    return _data ? _data->kind : Shape3DKind::Box;
}

f32 Shape3D::volume() const {
    return _data ? _data->shape->GetVolume() : 0.0f;
}

Vec3f Shape3D::centre_of_mass() const {
    return _data ? physics3d_detail::from_jolt(_data->shape->GetCenterOfMass()) : Vec3f{};
}

Vec3f Shape3D::bounds_min() const {
    return _data ? physics3d_detail::from_jolt(_data->shape->GetLocalBounds().mMin + _data->shape->GetCenterOfMass()) : Vec3f{};
}

Vec3f Shape3D::bounds_max() const {
    return _data ? physics3d_detail::from_jolt(_data->shape->GetLocalBounds().mMax + _data->shape->GetCenterOfMass()) : Vec3f{};
}

} // namespace kin
