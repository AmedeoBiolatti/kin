#include <kin/physics3d/shape3d.hpp>

#include "jolt_runtime.hpp"

#include <kin/assets/bake_cache.hpp>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
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
#include <cstring>
#include <mutex>
#include <string_view>

namespace kin {

namespace physics3d_detail {

#ifdef JPH_ENABLE_ASSERTS
namespace {
JPH::AssertFailedFunction previous_assert = nullptr;
bool jolt_assert(const char* expression, const char* message, const char* file, JPH::uint line) {
    // Jolt reports recoverable capacity exhaustion twice: this assertion and
    // Update's return value. World3D consumes the latter as StepReport3D and
    // logs it once per world, including in Debug. Preserve every other assert.
    if (std::strcmp(expression, "errors == EPhysicsUpdateError::None") == 0 &&
        std::string_view{file}.ends_with("PhysicsSystem.cpp"))
        return false;
    return previous_assert ? previous_assert(expression, message, file, line) : true;
}
} // namespace
#endif

void ensure_jolt_runtime() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
#ifdef JPH_ENABLE_ASSERTS
        previous_assert = JPH::AssertFailed;
        JPH::AssertFailed = jolt_assert;
#endif
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

// save_binary's header: magic "KSH3", this layout's version, binary_version(), the kind, the payload's length and its
// FNV-1a 64; then the payload, Jolt's SaveWithChildren.
constexpr u32 binary_magic = 0x3348534bu;
constexpr u32 binary_layout = 1;
constexpr std::size_t binary_header = 4 + 4 + 8 + 1 + 8 + 8;

class BytesOut final : public JPH::StreamOut {
public:
    explicit BytesOut(std::vector<u8>& out) : _out(out) {}
    void WriteBytes(const void* data, size_t size) override {
        const auto* p = static_cast<const u8*>(data);
        _out.insert(_out.end(), p, p + size);
    }
    bool IsFailed() const override { return false; }

private:
    std::vector<u8>& _out;
};

// Reads past the end give zeros and set the end flag, which Jolt checks after every piece it reads.
class BytesIn final : public JPH::StreamIn {
public:
    explicit BytesIn(std::span<const u8> bytes) : _bytes(bytes) {}
    void ReadBytes(void* data, size_t size) override {
        if (size > _bytes.size() - _at) {
            std::memset(data, 0, size);
            _at = _bytes.size();
            _end = true;
            return;
        }
        std::memcpy(data, _bytes.data() + _at, size);
        _at += size;
    }
    bool IsEOF() const override { return _end; }
    bool IsFailed() const override { return false; }
    bool whole() const { return !_end && _at == _bytes.size(); }

private:
    std::span<const u8> _bytes;
    std::size_t _at = 0;
    bool _end = false;
};

// The Jolt shapes the factories make for each kind (Jolt makes a compound of one child a rotated-translated shape).
bool made_as(Shape3DKind kind, JPH::EShapeSubType type) {
    using T = JPH::EShapeSubType;
    switch (kind) {
    case Shape3DKind::Box: return type == T::Box;
    case Shape3DKind::Sphere: return type == T::Sphere;
    case Shape3DKind::Capsule: return type == T::Capsule;
    case Shape3DKind::Cylinder: return type == T::Cylinder;
    case Shape3DKind::ConvexHull: return type == T::ConvexHull;
    case Shape3DKind::TriangleMesh: return type == T::Mesh;
    case Shape3DKind::HeightField: return type == T::HeightField;
    case Shape3DKind::Compound:
        // Jolt collapses an identity one-child compound to its child. The saved
        // header already checks the authored kind, so retain that logical kind.
        return type==T::StaticCompound || type==T::MutableCompound || type==T::RotatedTranslated || type==T::Scaled ||
               type==T::Box || type==T::Sphere || type==T::Capsule || type==T::Cylinder || type==T::ConvexHull ||
               type==T::Mesh || type==T::HeightField;
    }
    return false;
}

} // namespace

u64 Shape3D::binary_version() {
    using JPH::uint64; // JPH_VERSION_ID's macro names it unqualified
    // Jolt's id: its version in the low 24 bits, its build features (double precision, determinism, ...) above them.
    constexpr u64 jolt = JPH_VERSION_ID;
    static_assert(jolt < (u64{1} << 48));
    return (static_cast<u64>(binary_layout) << 48) | jolt;
}

bool Shape3D::save_binary(std::vector<u8>& out) const {
    if (!_data) {
        return false;
    }
    std::vector<u8> payload;
    {
        BytesOut stream{payload};
        JPH::Shape::ShapeToIDMap shapes;
        JPH::Shape::MaterialToIDMap materials;
        _data->shape->SaveWithChildren(stream, shapes, materials);
    }
    BakeWriter header;
    header.value(binary_magic);
    header.value(binary_layout);
    header.value(binary_version());
    header.value(static_cast<u8>(_data->kind));
    header.value(static_cast<u64>(payload.size()));
    header.value(bake_hash(payload.data(), payload.size()));
    out.insert(out.end(), header.bytes().begin(), header.bytes().end());
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

Shape3D Shape3D::restore_binary(std::span<const u8> bytes, Shape3DKind kind) {
    Shape3D out;
    BakeReader header{bytes.first(std::min(bytes.size(), binary_header))};
    u32 magic = 0, layout = 0;
    u64 version = 0, length = 0, checksum = 0;
    u8 stored_kind = 0;
    header.value(magic);
    header.value(layout);
    header.value(version);
    header.value(stored_kind);
    header.value(length);
    header.value(checksum);
    // Jolt trusts the lengths in its state (it sizes arrays by them), so it reads nothing that the checksum has not passed.
    if (!header.done() || magic != binary_magic || layout != binary_layout || version != binary_version() || stored_kind != static_cast<u8>(kind) ||
        length != bytes.size() - binary_header) {
        return out;
    }
    const std::span<const u8> payload = bytes.subspan(binary_header);
    if (bake_hash(payload.data(), payload.size()) != checksum) {
        return out;
    }
    physics3d_detail::ensure_jolt_runtime();
    BytesIn stream{payload};
    JPH::Shape::IDToShapeMap shapes;
    JPH::Shape::IDToMaterialMap materials;
    const JPH::Shape::ShapeResult result = JPH::Shape::sRestoreWithChildren(stream, shapes, materials);
    if (!stream.whole() || result.HasError() || !result.IsValid() || result.Get() == nullptr || !made_as(kind, result.Get()->GetSubType())) {
        return out;
    }
    auto data = std::make_shared<ShapeData>();
    data->shape = result.Get();
    data->kind = kind;
    out._data = std::move(data);
    return out;
}

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
    // Jolt containers allocate before the shape is constructed, including when this is the first physics call.
    physics3d_detail::ensure_jolt_runtime();
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
    physics3d_detail::ensure_jolt_runtime();
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
