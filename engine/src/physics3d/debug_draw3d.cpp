#include <kin/physics3d/debug_draw3d.hpp>

#include "jolt_runtime.hpp"

#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace kin {

namespace {

using physics3d_detail::from_jolt;

constexpr f32 two_pi = 6.28318530717958647692f;

// A direction in a unit's own space turned into the world.
Vec3f along(Quatf q, Vec3f v) { return q.rotate(v); }

} // namespace

void DebugDraw3D::line(Vec3f a, Vec3f b, Color color) {
    if (_lines.size() >= _options.max_lines) {
        _truncated = true;
        return;
    }
    _lines.push_back({a, b, color});
}

void DebugDraw3D::cross(Vec3f p, Color color) {
    const f32 s = _options.point_size;
    line(p - Vec3f{s, 0, 0}, p + Vec3f{s, 0, 0}, color);
    line(p - Vec3f{0, s, 0}, p + Vec3f{0, s, 0}, color);
    line(p - Vec3f{0, 0, s}, p + Vec3f{0, 0, s}, color);
}

void DebugDraw3D::arc(Vec3f centre, Vec3f u, Vec3f v, f32 radius, f32 from_angle, f32 to_angle, Color color) {
    const u32 segments = std::max<u32>(4, _options.circle_segments);
    Vec3f previous = centre + (u * std::cos(from_angle) + v * std::sin(from_angle)) * radius;
    for (u32 i = 1; i <= segments; ++i) {
        const f32 t = from_angle + (to_angle - from_angle) * static_cast<f32>(i) / static_cast<f32>(segments);
        const Vec3f next = centre + (u * std::cos(t) + v * std::sin(t)) * radius;
        line(previous, next, color);
        previous = next;
    }
}

void DebugDraw3D::circle(Vec3f centre, Vec3f u, Vec3f v, f32 radius, Color color) {
    arc(centre, u, v, radius, 0.0f, two_pi, color);
}

void DebugDraw3D::shape(const Shape3D& s, Vec3f position, Quatf rotation, Color color) {
    if (!s.valid()) {
        return;
    }
    const Quatf q = rotation.normalized();
    jolt_shape(s._data->shape.GetPtr(), position + q.rotate(s.centre_of_mass()), q, color, 0);
}

void DebugDraw3D::jolt_shape(const void* erased, Vec3f com, Quatf q, Color color, int depth) {
    const auto* shape = static_cast<const JPH::Shape*>(erased);
    const Vec3f x = along(q, {1, 0, 0}), y = along(q, {0, 1, 0}), z = along(q, {0, 0, 1});
    switch (shape->GetSubType()) {
    case JPH::EShapeSubType::Box: {
        const Vec3f h = from_jolt(static_cast<const JPH::BoxShape*>(shape)->GetHalfExtent());
        std::array<Vec3f, 8> c;
        for (int i = 0; i < 8; ++i) {
            c[static_cast<std::size_t>(i)] = com + x * ((i & 1) != 0 ? h.x : -h.x) + y * ((i & 2) != 0 ? h.y : -h.y)
                                           + z * ((i & 4) != 0 ? h.z : -h.z);
        }
        for (int i = 0; i < 8; ++i) {
            for (int bit = 1; bit <= 4; bit <<= 1) {
                if ((i & bit) == 0) {
                    line(c[static_cast<std::size_t>(i)], c[static_cast<std::size_t>(i | bit)], color);
                }
            }
        }
        return;
    }
    case JPH::EShapeSubType::Sphere: {
        const f32 r = static_cast<const JPH::SphereShape*>(shape)->GetRadius();
        circle(com, x, y, r, color);
        circle(com, y, z, r, color);
        circle(com, z, x, r, color);
        return;
    }
    case JPH::EShapeSubType::Capsule: {
        const auto* capsule = static_cast<const JPH::CapsuleShape*>(shape);
        const f32 r = capsule->GetRadius();
        const f32 h = capsule->GetHalfHeightOfCylinder();
        const Vec3f top = com + y * h, bottom = com - y * h;
        circle(top, x, z, r, color);
        circle(bottom, x, z, r, color);
        for (const Vec3f& side : {x, z, -x, -z}) {
            line(top + side * r, bottom + side * r, color);
        }
        arc(top, x, y, r, 0.0f, two_pi * 0.5f, color);
        arc(top, z, y, r, 0.0f, two_pi * 0.5f, color);
        arc(bottom, x, y, r, two_pi * 0.5f, two_pi, color);
        arc(bottom, z, y, r, two_pi * 0.5f, two_pi, color);
        return;
    }
    case JPH::EShapeSubType::Cylinder: {
        const auto* cylinder = static_cast<const JPH::CylinderShape*>(shape);
        const f32 r = cylinder->GetRadius();
        const f32 h = cylinder->GetHalfHeight();
        const Vec3f top = com + y * h, bottom = com - y * h;
        circle(top, x, z, r, color);
        circle(bottom, x, z, r, color);
        for (const Vec3f& side : {x, z, -x, -z}) {
            line(top + side * r, bottom + side * r, color);
        }
        return;
    }
    case JPH::EShapeSubType::ConvexHull: {
        const auto* hull = static_cast<const JPH::ConvexHullShape*>(shape);
        std::vector<JPH::uint> face;
        for (JPH::uint f = 0; f < hull->GetNumFaces(); ++f) {
            face.resize(hull->GetNumVerticesInFace(f));
            const JPH::uint n = hull->GetFaceVertices(f, static_cast<JPH::uint>(face.size()), face.data());
            for (JPH::uint i = 0; i < n; ++i) {
                const Vec3f a = com + q.rotate(from_jolt(hull->GetPoint(face[i])));
                const Vec3f b = com + q.rotate(from_jolt(hull->GetPoint(face[(i + 1) % n])));
                line(a, b, color);
            }
        }
        return;
    }
    case JPH::EShapeSubType::RotatedTranslated: {
        // A character's capsule (its feet are the origin): the inner shape has the same centre of mass, turned by the
        // wrapper's own rotation.
        if (depth >= 4) {
            return;
        }
        const auto* wrapped = static_cast<const JPH::RotatedTranslatedShape*>(shape);
        jolt_shape(wrapped->GetInnerShape(), com, (q * from_jolt(wrapped->GetRotation())).normalized(), color, depth + 1);
        return;
    }
    case JPH::EShapeSubType::StaticCompound:
    case JPH::EShapeSubType::MutableCompound: {
        if (depth >= 4) {
            return;
        }
        const auto* compound = static_cast<const JPH::CompoundShape*>(shape);
        for (const auto& child : compound->GetSubShapes()) {
            const Quatf cq = (q * from_jolt(child.GetRotation())).normalized();
            jolt_shape(child.mShape.GetPtr(), com + q.rotate(from_jolt(child.GetPositionCOM())), cq, color, depth + 1);
        }
        return;
    }
    default:
        break;
    }
    // Triangle meshes, height fields and anything else: its triangles, an even stride of them when over the cap.
    const u32 total = std::max<u32>(1, shape->GetStats().mNumTriangles);
    const u32 stride = std::max<u32>(1, (total + _options.max_triangles_per_shape - 1) / _options.max_triangles_per_shape);
    JPH::Shape::GetTrianglesContext context;
    shape->GetTrianglesStart(context, JPH::AABox::sBiggest(), physics3d_detail::to_jolt(com),
                             physics3d_detail::to_jolt(q), JPH::Vec3::sReplicate(1.0f));
    constexpr int batch = 64;
    std::array<JPH::Float3, 3 * batch> verts;
    u32 index = 0;
    for (;;) {
        const int n = shape->GetTrianglesNext(context, batch, verts.data());
        if (n == 0) {
            break;
        }
        for (int t = 0; t < n; ++t, ++index) {
            if (index % stride != 0) {
                continue;
            }
            const auto p = [&](int k) {
                const JPH::Float3& f = verts[static_cast<std::size_t>(3 * t + k)];
                return Vec3f{f.x, f.y, f.z};
            };
            line(p(0), p(1), color);
            line(p(1), p(2), color);
            line(p(2), p(0), color);
        }
        if (_truncated) {
            break;
        }
    }
}

void DebugDraw3D::world(const World3D& world) {
    if (_options.bodies) {
        for (const Body3D body : world.bodies()) {
            Color colour = _options.dynamic_body;
            const Body3DMotion motion = world.motion(body);
            if (world.is_sensor(body)) {
                colour = _options.sensor_body;
            } else if (motion == Body3DMotion::Static) {
                colour = _options.static_body;
            } else if (_options.colour_by_sleep && !world.is_active(body)) {
                colour = _options.sleeping_body;
            } else if (motion == Body3DMotion::Kinematic) {
                colour = _options.kinematic_body;
            }
            shape(world.shape(body), world.position(body), world.rotation(body), colour);
        }
    }
    if (_options.constraints) {
        for (const Constraint3D handle : world.constraints()) {
            const Constraint3DInfo c = world.constraint_info(handle);
            const Color colour = c.broken ? _options.broken_constraint : _options.constraint;
            switch (c.kind) {
            case Constraint3DKind::Hinge:
            case Constraint3DKind::Slider:
            case Constraint3DKind::Cone:
                line(c.point_a - c.axis * 0.2f, c.point_a + c.axis * 0.2f, colour);
                break;
            default: break;
            }
            cross(c.point_a, colour);
            if (c.kind == Constraint3DKind::Distance) {
                cross(c.point_b, colour);
                line(c.point_a, c.point_b, colour);
            }
            // The two bodies' origins to the anchor: which bodies it holds.
            if (c.a.valid()) {
                line(world.position(c.a), c.point_a, colour);
            }
            if (c.b.valid()) {
                line(world.position(c.b), c.kind == Constraint3DKind::Distance ? c.point_b : c.point_a, colour);
            }
        }
    }
}

void DebugDraw3D::contacts(std::span<const ContactEvent3D> events) {
    for (const ContactEvent3D& e : events) {
        if (e.kind == ContactKind3D::End) {
            continue;
        }
        cross(e.point, _options.contact_point);
        line(e.point, e.point + e.normal * _options.normal_length, _options.contact_normal);
    }
}

void DebugDraw3D::grips(const World3D& world, Character3D character) {
    const TraversalState3D t = world.traversal(character);
    if (!t.enabled) {
        return;
    }
    const CharacterState3D s = world.character(character);
    for (u32 h = 0; h < t.limb_count; ++h) {
        const GripState3D& g = t.grips[h];
        if (g.phase == GripPhase3D::Free || g.phase == GripPhase3D::Released) {
            continue;
        }
        const Color colour = g.phase == GripPhase3D::Reaching ? _options.grip_reaching
                           : g.phase == GripPhase3D::Slipping ? _options.grip_slipping : _options.grip_held;
        cross(g.point, colour);
        line(g.point, g.point + g.normal * _options.normal_length, colour);
        // The surface's frame at the anchor: up it, along it.
        line(g.point, g.point + g.surface_up * (0.5f * _options.normal_length), _options.candidate_surface);
        line(g.point, g.point + g.right * (0.5f * _options.normal_length), _options.candidate_surface);
        // To the character: a line from the anchor to the shoulder's height over the feet.
        line(g.point, s.position + s.up * (0.8f * s.height), colour);
    }
    if (t.mode != TraversalMode3D::Walking) {
        cross(t.hang_place, _options.hang_place);
        line(t.hang_place, s.position, _options.hang_place);
    }
}

void DebugDraw3D::candidates(std::span<const GripCandidate3D> candidates) {
    bool first = true;
    for (const GripCandidate3D& c : candidates) {
        const Color colour = c.kind == GripKind3D::Hold ? _options.candidate_hold
                           : c.kind == GripKind3D::Ledge ? _options.candidate_ledge : _options.candidate_surface;
        cross(c.point, colour);
        line(c.point, c.point + c.normal * (first ? 2.0f : 1.0f) * _options.normal_length, colour);
        if (c.ledge) {
            cross(c.ledge_top, c.clear ? _options.grip_held : _options.grip_slipping);
        }
        first = false;
    }
}

void DebugDraw3D::ray(Vec3f from, Vec3f to, const std::optional<RayHit3D>& hit) {
    if (!hit) {
        line(from, to, _options.ray);
        return;
    }
    line(from, hit->point, _options.ray);
    cross(hit->point, _options.ray_hit);
    line(hit->point, hit->point + hit->normal * _options.normal_length, _options.contact_normal);
}

void DebugDraw3D::shape_cast(const Shape3D& s, Vec3f position, Quatf rotation, Vec3f displacement,
                             const std::optional<ShapeHit3D>& hit) {
    shape(s, position, rotation, _options.ray);
    const Vec3f end = hit ? hit->position : position + displacement;
    shape(s, end, rotation, hit ? _options.ray_hit : _options.ray);
    line(position, end, _options.ray);
    if (hit) {
        cross(hit->point, _options.contact_point);
        line(hit->point, hit->point + hit->normal * _options.normal_length, _options.contact_normal);
    }
}

} // namespace kin
