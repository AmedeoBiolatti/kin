#pragma once

// Jolt's headers and the few conversions between its types and kin's, for the
// sources of kin/physics3d only: no public header includes this.

#include <kin/core/math3d.hpp>
#include <kin/physics3d/shape3d.hpp>

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace kin::physics3d_detail {

struct ShapeData {
    JPH::RefConst<JPH::Shape> shape;
    Shape3DKind kind = Shape3DKind::Box;
};

// Registers Jolt's allocator and shape types once per process (never undone:
// shapes may outlive every world). Thread safe; cheap after the first call.
void ensure_jolt_runtime();

inline JPH::Vec3 to_jolt(Vec3f v) { return JPH::Vec3(v.x, v.y, v.z); }
inline JPH::Quat to_jolt(Quatf q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
inline Vec3f from_jolt(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
inline Quatf from_jolt(JPH::QuatArg q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

} // namespace kin::physics3d_detail
