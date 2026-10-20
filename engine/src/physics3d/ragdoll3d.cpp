// Ragdolls (kin/physics3d/ragdoll3d.hpp): a body per bone joined by constraints, a state (animated, ragdoll, blend, get-up)
// and the pose a skinned character takes from it. Built on the world's public body and constraint calls.

#include "ragdoll3d_impl.hpp"

#include "bytes3d.hpp"

#include <kin/render3d/skeleton3d.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace kin {
namespace physics3d_detail {

namespace {

bool finite3(Vec3f v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

f32 smooth(f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The rotation of a matrix of translation and rotation (and maybe scale: columns are normalised).
Quatf quat_of(const Mat4f& m) {
    const Vec3f c0 = normalize(Vec3f{m.m[0], m.m[1], m.m[2]});
    const Vec3f c1 = normalize(Vec3f{m.m[4], m.m[5], m.m[6]});
    const Vec3f c2 = normalize(Vec3f{m.m[8], m.m[9], m.m[10]});
    const f32 r00 = c0.x, r10 = c0.y, r20 = c0.z;
    const f32 r01 = c1.x, r11 = c1.y, r21 = c1.z;
    const f32 r02 = c2.x, r12 = c2.y, r22 = c2.z;
    Quatf q;
    const f32 trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        const f32 k = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(r21 - r12) / k, (r02 - r20) / k, (r10 - r01) / k, 0.25f * k};
    } else if (r00 > r11 && r00 > r22) {
        const f32 k = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q = {0.25f * k, (r01 + r10) / k, (r02 + r20) / k, (r21 - r12) / k};
    } else if (r11 > r22) {
        const f32 k = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q = {(r01 + r10) / k, 0.25f * k, (r12 + r21) / k, (r02 - r20) / k};
    } else {
        const f32 k = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q = {(r02 + r20) / k, (r12 + r21) / k, 0.25f * k, (r10 - r01) / k};
    }
    return q.normalized();
}

// The shortest rotation taking unit `from` to unit `to`.
Quatf between(Vec3f from, Vec3f to) {
    const f32 d = dot(from, to);
    if (d < -0.999999f) {
        const Vec3f axis = normalize(cross(from, std::fabs(from.x) < 0.9f ? Vec3f{1.0f, 0.0f, 0.0f} : Vec3f{0.0f, 0.0f, 1.0f}));
        return {axis.x, axis.y, axis.z, 0.0f};
    }
    const Vec3f a = cross(from, to);
    return Quatf{a.x, a.y, a.z, 1.0f + d}.normalized();
}

constexpr u32 group_base = 0x40000000u;

} // namespace

// --- the set ---------------------------------------------------------------------------------------------------------

RagdollHandle3D RagdollSet::create(const RagdollDesc& desc, std::span<const Mat4f> bone_world, f32 scale) {
    const std::size_t n = desc.bones.size();
    if (n == 0 || bone_world.size() != n || !(scale > 0.0f) || !std::isfinite(scale) || desc.root_bone < 0 ||
        static_cast<std::size_t>(desc.root_bone) >= n || desc.layer >= layers3d::Count || desc.layer == layers3d::Static) {
        return {};
    }
    for (std::size_t i = 0; i < n; ++i) {
        const RagdollBoneDesc3D& b = desc.bones[i];
        if (b.parent >= static_cast<i32>(i) || !std::isfinite(b.radius) || !std::isfinite(b.half_height) || !(b.radius > 0.0f) ||
            !finite3(b.offset) || !finite3(b.half_extents) || !finite3(b.joint_axis) || !std::isfinite(b.mass) ||
            !std::isfinite(b.density)) {
            return {};
        }
        for (const f32 v : bone_world[i].m) {
            if (!std::isfinite(v)) {
                return {};
            }
        }
    }

    u32 index;
    if (!_free.empty()) {
        index = _free.back();
        _free.pop_back();
    } else {
        index = static_cast<u32>(_slots.size());
        _slots.emplace_back();
    }
    Slot& s = _slots[index];
    const u32 generation = s.generation;
    s = Slot{};
    s.generation = generation;
    s.desc = desc;
    s.scale = scale;
    s.bodies.assign(n, Body3D{});
    s.joints.assign(n, Constraint3D{});
    s.weight.resize(n);
    s.dynamic.assign(n, 0);
    s.target_position.resize(n);
    s.target_rotation.resize(n);
    s.snap_position.resize(n);
    s.snap_rotation.resize(n);
    const u32 group = group_base | (index + 1u);

    const auto fail = [&]() {
        for (const Body3D b : s.bodies) {
            if (b.valid()) {
                _world->owner_destroy_body(b);
            }
        }
        s.alive = false;
        s.bodies.clear();
        s.joints.clear();
        _free.push_back(index);
        return RagdollHandle3D{};
    };

    for (std::size_t i = 0; i < n; ++i) {
        const RagdollBoneDesc3D& b = desc.bones[i];
        const Mat4f& m = bone_world[i];
        const Vec3f position{m.m[12], m.m[13], m.m[14]};
        const Quatf rotation = quat_of(m);
        s.target_position[i] = position;
        s.target_rotation[i] = rotation;
        s.snap_position[i] = position;
        s.snap_rotation[i] = rotation;
        s.weight[i] = std::clamp(b.weight, 0.0f, 1.0f);

        Shape3D piece;
        switch (b.shape) {
        case RagdollShape3D::Capsule:
            piece = Shape3D::capsule(std::max(b.half_height * scale, 0.002f), b.radius * scale);
            break;
        case RagdollShape3D::Box:
            piece = Shape3D::box({std::max(b.half_extents.x * scale, 0.002f), std::max(b.half_extents.y * scale, 0.002f),
                                  std::max(b.half_extents.z * scale, 0.002f)});
            break;
        case RagdollShape3D::Sphere:
            piece = Shape3D::sphere(b.radius * scale);
            break;
        }
        const ShapeChild3D child{piece, b.offset * scale, b.rotation.normalized()};
        const Shape3D shape = Shape3D::compound(std::span<const ShapeChild3D>(&child, 1));
        if (!shape.valid()) {
            return fail();
        }
        Body3DDesc bd;
        bd.shape = shape;
        bd.motion = Body3DMotion::Dynamic;
        bd.position = position;
        bd.rotation = rotation;
        const f32 s3 = scale * scale * scale;
        bd.mass = b.mass > 0.0f ? b.mass * s3 : 0.0f;
        bd.density = b.density;
        bd.friction = b.friction;
        bd.restitution = b.restitution;
        bd.linear_damping = b.linear_damping;
        bd.angular_damping = b.angular_damping;
        bd.gravity_factor = desc.gravity_factor;
        bd.layer = desc.layer;
        bd.allow_sleeping = true;
        bd.continuous = desc.continuous;
        bd.start_active = false;
        bd.group = group;
        bd.group_self_collision = desc.self_collision;
        bd.velocity_steps = static_cast<u8>(std::min<u32>(desc.velocity_steps, 255));
        bd.position_steps = static_cast<u8>(std::min<u32>(desc.position_steps, 255));
        bd.user_id = desc.user_id;
        s.bodies[i] = _world->create_body(bd);
        if (!s.bodies[i].valid()) {
            return fail();
        }
        // The ragdoll's own: the game cannot destroy, reshape or switch a bone under it.
        _world->set_owner(s.bodies[i], BodyOwner3D::Ragdoll);
        // Animated to begin with: kinematic bodies at the pose.
        _world->owner_set_motion(s.bodies[i], Body3DMotion::Kinematic);
    }

    for (std::size_t i = 0; i < n; ++i) {
        const RagdollBoneDesc3D& b = desc.bones[i];
        if (b.parent < 0) {
            continue;
        }
        const Mat4f& m = bone_world[i];
        const Quatf rotation = s.target_rotation[i];
        Constraint3DDesc c;
        c.a = s.bodies[static_cast<std::size_t>(b.parent)];
        c.b = s.bodies[i];
        c.point = {m.m[12], m.m[13], m.m[14]};
        c.collide_connected = false;
        c.max_friction = std::max(b.joint_friction, 0.0f);
        c.user_id = desc.user_id;
        c.velocity_steps = desc.velocity_steps;
        c.position_steps = desc.position_steps;
        const Vec3f axis = length(b.joint_axis) > 0.0f ? rotation.rotate(normalize(b.joint_axis)) : rotation.rotate({0.0f, 1.0f, 0.0f});
        switch (b.joint_kind) {
        case RagdollJoint3D::Cone:
            c.kind = Constraint3DKind::Cone;
            c.axis = axis;
            c.swing_angle = b.swing_degrees;
            c.limits = true;
            c.min_angle = std::clamp(b.twist_min_degrees, -180.0f, 0.0f);
            c.max_angle = std::clamp(b.twist_max_degrees, 0.0f, 180.0f);
            break;
        case RagdollJoint3D::Hinge:
            c.kind = Constraint3DKind::Hinge;
            c.axis = axis;
            c.limits = true;
            c.min_angle = std::clamp(b.hinge_min_degrees, -180.0f, 0.0f);
            c.max_angle = std::clamp(b.hinge_max_degrees, 0.0f, 180.0f);
            break;
        case RagdollJoint3D::Fixed:
            c.kind = Constraint3DKind::Fixed;
            break;
        }
        s.joints[i] = _world->create_constraint(c);
        if (!s.joints[i].valid()) {
            // The constraints made so far go with the bodies.
            return fail();
        }
    }

    s.alive = true;
    s.has_targets = true;
    s.mode = RagdollMode3D::Animated;
    ++_live;
    apply_motion(s, false);
    return {index, s.generation};
}

void RagdollSet::destroy(RagdollHandle3D h) {
    Slot* s = find(h);
    if (s == nullptr) {
        return;
    }
    for (std::size_t i = s->bodies.size(); i-- > 0;) {
        if (s->bodies[i].valid() && _world->contains(s->bodies[i])) {
            _world->owner_destroy_body(s->bodies[i]);
        }
    }
    s->alive = false;
    ++s->generation;
    s->bodies.clear();
    s->joints.clear();
    _free.push_back(h.index);
    --_live;
}

std::vector<RagdollHandle3D> RagdollSet::list() const {
    std::vector<RagdollHandle3D> out;
    for (u32 i = 0; i < _slots.size(); ++i) {
        if (_slots[i].alive) {
            out.push_back({i, _slots[i].generation});
        }
    }
    return out;
}

std::vector<Body3D> RagdollSet::bodies(RagdollHandle3D h) const {
    const Slot* s = find(h);
    return s != nullptr ? s->bodies : std::vector<Body3D>{};
}

bool RagdollSet::has_body(RagdollHandle3D h,Body3D body)const {
    const auto* slot=find(h);return slot&&std::ranges::find(slot->bodies,body)!=slot->bodies.end();
}
const RagdollDesc* RagdollSet::desc(RagdollHandle3D h) const {
    const Slot* s = find(h);
    return s != nullptr ? &s->desc : nullptr;
}

void RagdollSet::push(const Slot& s, u32 index, RagdollEventKind3D kind) {
    RagdollEvent3D e;
    e.ragdoll = {index, s.generation};
    e.user_id = s.desc.user_id;
    e.kind = kind;
    _events.push_back(e);
}

// Changes what must change so that the bodies match the state: dynamic or kinematic, parked or not.
void RagdollSet::apply_motion(Slot& s, bool keep_velocity) {
    // A get-up has the bodies back on the clip (kinematic) from its first step: it is the pose that blends out.
    const bool dynamic_mode = (s.mode == RagdollMode3D::Ragdoll || s.mode == RagdollMode3D::Blend) && !s.frozen;
    const bool park = s.mode == RagdollMode3D::Animated && !s.desc.animated_bodies;
    for (std::size_t i = 0; i < s.bodies.size(); ++i) {
        const Body3D body = s.bodies[i];
        if (!body.valid() || !_world->contains(body)) {
            continue;
        }
        const bool want = dynamic_mode && s.weight[i] > 0.0f;
        if (want != (s.dynamic[i] != 0)) {
            const Vec3f v = _world->linear_velocity(body);
            const Vec3f w = _world->angular_velocity(body);
            if (_world->owner_set_motion(body, want ? Body3DMotion::Dynamic : Body3DMotion::Kinematic)) {
                s.dynamic[i] = want ? 1 : 0;
                if (want && keep_velocity) {
                    _world->set_linear_velocity(body, v);
                    _world->set_angular_velocity(body, w);
                } else if (!want) {
                    _world->set_linear_velocity(body, {});
                    _world->set_angular_velocity(body, {});
                }
            }
        }
    }
    if (park != s.parked) {
        for (std::size_t i = 0; i < s.bodies.size(); ++i) {
            const Body3D body = s.bodies[i];
            if (!body.valid() || !_world->contains(body)) {
                continue;
            }
            _world->set_no_collisions(body, park);
            if (park) {
                _world->deactivate(body);
            } else if (s.has_targets) {
                _world->teleport(body, s.target_position[i], s.target_rotation[i]);
            }
        }
        s.parked = park;
    }
    if (s.frozen) {
        for (const Body3D body : s.bodies) {
            if (body.valid() && _world->contains(body)) {
                _world->deactivate(body);
            }
        }
    }
}

void RagdollSet::set_targets(RagdollHandle3D h, std::span<const Mat4f> bone_world) {
    Slot* s = find(h);
    if (s == nullptr || bone_world.size() != s->bodies.size()) {
        return;
    }
    for (std::size_t i = 0; i < bone_world.size(); ++i) {
        const Mat4f& m = bone_world[i];
        const Vec3f p{m.m[12], m.m[13], m.m[14]};
        if (!finite3(p)) {
            return;
        }
        s->target_position[i] = p;
        s->target_rotation[i] = quat_of(m);
    }
    s->has_targets = true;
}

void RagdollSet::start(RagdollHandle3D h, const RagdollStart3D& start) {
    Slot* s = find(h);
    if (s == nullptr || !finite3(start.velocity) || !finite3(start.impulse) || !finite3(start.impulse_point)) {
        return;
    }
    if (s->mode == RagdollMode3D::GetUp) {
        // Turning a get-up round: the bodies go where the pose shows them now, so that nothing pops.
        for (std::size_t i = 0; i < s->bodies.size(); ++i) {
            if (s->bodies[i].valid() && _world->contains(s->bodies[i])) {
                // A normalised lerp of the rotations (square roots only: no trigonometry in the step).
                Quatf from = s->target_rotation[i], to = s->snap_rotation[i];
                if (from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w < 0.0f) {
                    to = {-to.x, -to.y, -to.z, -to.w};
                }
                const f32 u = s->alpha;
                const Quatf turned = Quatf{from.x + (to.x - from.x) * u, from.y + (to.y - from.y) * u, from.z + (to.z - from.z) * u,
                                           from.w + (to.w - from.w) * u}.normalized();
                _world->teleport(s->bodies[i], lerp(s->target_position[i], s->snap_position[i], s->alpha), turned);
            }
        }
    }
    s->mode = RagdollMode3D::Ragdoll;
    s->frozen = false;
    s->settled = false;
    s->started = true;
    s->settle_timer = 0.0f;
    s->from = s->alpha;
    s->to = 1.0f;
    s->t = 0.0f;
    s->duration = std::max(start.blend_seconds, 0.0f);
    if (s->duration <= 0.0f) {
        s->alpha = 1.0f;
    }
    // Un-park first: the bodies must be where the pose is before they are let go.
    apply_motion(*s, true);
    if (start.set_velocity) {
        for (std::size_t i = 0; i < s->bodies.size(); ++i) {
            if (s->dynamic[i] != 0) {
                _world->set_linear_velocity(s->bodies[i], start.velocity);
                _world->set_angular_velocity(s->bodies[i], {});
            }
        }
    }
    if (start.impulse_bone >= 0 && static_cast<std::size_t>(start.impulse_bone) < s->bodies.size()) {
        const Body3D body = s->bodies[static_cast<std::size_t>(start.impulse_bone)];
        if (body.valid() && _world->contains(body)) {
            if (start.point_given) {
                _world->add_impulse_at(body, start.impulse, start.impulse_point);
            } else {
                _world->add_impulse(body, start.impulse);
            }
        }
    }
    push(*s, h.index, RagdollEventKind3D::Started);
}

void RagdollSet::blend(RagdollHandle3D h, f32 alpha, f32 seconds) {
    Slot* s = find(h);
    if (s == nullptr || !std::isfinite(alpha) || !std::isfinite(seconds)) {
        return;
    }
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    s->frozen = false;
    s->settled = false;
    s->settle_timer = 0.0f;
    s->mode = RagdollMode3D::Blend;
    s->from = s->alpha;
    s->to = alpha;
    s->t = 0.0f;
    s->duration = std::max(seconds, 0.0f);
    if (s->duration <= 0.0f) {
        s->alpha = alpha;
        if (alpha <= 0.0f) {
            s->mode = RagdollMode3D::Animated;
        }
    }
    apply_motion(*s, true);
}

void RagdollSet::get_up(RagdollHandle3D h, f32 seconds) {
    Slot* s = find(h);
    if (s == nullptr || !std::isfinite(seconds)) {
        return;
    }
    if (s->mode == RagdollMode3D::Animated) {
        return;
    }
    // The pose it blends from: the bodies as they are now (a frozen one, or one that has been following a clip, is its own).
    for (std::size_t i = 0; i < s->bodies.size(); ++i) {
        if (s->bodies[i].valid() && _world->contains(s->bodies[i])) {
            s->snap_position[i] = _world->position(s->bodies[i]);
            s->snap_rotation[i] = _world->rotation(s->bodies[i]);
        }
    }
    s->frozen = false;
    s->settled = false;
    s->settle_timer = 0.0f;
    s->mode = RagdollMode3D::GetUp;
    s->from = s->alpha;
    s->to = 0.0f;
    s->t = 0.0f;
    s->duration = std::max(seconds, 0.0f);
    if (s->duration <= 0.0f) {
        s->alpha = 0.0f;
        s->mode = RagdollMode3D::Animated;
        s->started = false;
        push(*s, h.index, RagdollEventKind3D::Recovered);
    }
    apply_motion(*s, false);
    // The bodies are on the clip's pose at once (nothing is swept from the lying place to the standing one).
    if (s->has_targets && !s->parked) {
        for (std::size_t i = 0; i < s->bodies.size(); ++i) {
            if (s->bodies[i].valid() && _world->contains(s->bodies[i])) {
                _world->teleport(s->bodies[i], s->target_position[i], s->target_rotation[i]);
            }
        }
    }
}

void RagdollSet::animate(RagdollHandle3D h) {
    Slot* s = find(h);
    if (s == nullptr) {
        return;
    }
    s->mode = RagdollMode3D::Animated;
    s->alpha = s->from = s->to = 0.0f;
    s->t = s->duration = 0.0f;
    s->frozen = false;
    s->settled = false;
    s->settle_timer = 0.0f;
    s->started = false;
    apply_motion(*s, false);
    if (s->has_targets && !s->parked) {
        for (std::size_t i = 0; i < s->bodies.size(); ++i) {
            if (s->bodies[i].valid() && _world->contains(s->bodies[i])) {
                _world->teleport(s->bodies[i], s->target_position[i], s->target_rotation[i]);
            }
        }
    }
}

void RagdollSet::set_weights(RagdollHandle3D h, std::span<const f32> weights) {
    Slot* s = find(h);
    if (s == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < s->weight.size(); ++i) {
        const f32 w = i < weights.size() ? weights[i] : 1.0f;
        s->weight[i] = std::isfinite(w) ? std::clamp(w, 0.0f, 1.0f) : 0.0f;
    }
    apply_motion(*s, true);
}

void RagdollSet::set_subtree(RagdollHandle3D h, i32 bone, f32 weight) {
    Slot* s = find(h);
    if (s == nullptr || bone < 0 || static_cast<std::size_t>(bone) >= s->weight.size()) {
        return;
    }
    // Bones are parent first: a bone is below `bone` when its chain of parents reaches it.
    std::vector<u8> below(s->weight.size(), 0);
    below[static_cast<std::size_t>(bone)] = 1;
    for (std::size_t i = static_cast<std::size_t>(bone) + 1; i < below.size(); ++i) {
        const i32 p = s->desc.bones[i].parent;
        below[i] = p >= 0 && below[static_cast<std::size_t>(p)] != 0 ? 1 : 0;
    }
    const f32 w = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
    for (std::size_t i = 0; i < below.size(); ++i) {
        s->weight[i] = below[i] != 0 ? w : 0.0f;
    }
    apply_motion(*s, true);
}

RagdollState3D RagdollSet::state(RagdollHandle3D h) const {
    RagdollState3D out;
    const Slot* s = find(h);
    if (s == nullptr) {
        return out;
    }
    out.mode = s->mode;
    out.alpha = s->alpha;
    out.target_alpha = s->to;
    out.settled = s->settled;
    out.frozen = s->frozen;
    for (std::size_t i = 0; i < s->bodies.size(); ++i) {
        if (s->dynamic[i] == 0 || !s->bodies[i].valid() || !_world->contains(s->bodies[i])) {
            continue;
        }
        ++out.dynamic_bodies;
        if (_world->is_active(s->bodies[i])) {
            ++out.awake_bodies;
        }
        out.max_speed = std::max(out.max_speed, length(_world->linear_velocity(s->bodies[i])));
    }
    return out;
}

void RagdollSet::pose(RagdollHandle3D h, std::vector<RagdollBonePose3D>& out) const {
    out.clear();
    const Slot* s = find(h);
    if (s == nullptr) {
        return;
    }
    out.resize(s->bodies.size());
    const f32 alpha = s->mode == RagdollMode3D::Animated ? 0.0f : s->alpha;
    const bool snapshot = s->mode == RagdollMode3D::GetUp;
    for (std::size_t i = 0; i < s->bodies.size(); ++i) {
        const Body3D body = s->bodies[i];
        if (!body.valid() || !_world->contains(body)) {
            continue;
        }
        RagdollBonePose3D& o = out[i];
        o.position = snapshot ? s->snap_position[i] : _world->position(body);
        o.rotation = snapshot ? s->snap_rotation[i] : _world->rotation(body);
        o.weight = std::clamp(alpha * s->weight[i], 0.0f, 1.0f);
    }
}

RagdollPlace3D RagdollSet::place(RagdollHandle3D h, Vec3f up, Layer3DMask mask, f32 max_distance, Body3D ignore) const {
    RagdollPlace3D out;
    const Slot* s = find(h);
    if (s == nullptr || !finite3(up) || length(up) < 1.0e-6f) {
        return out;
    }
    const Body3D root = s->bodies[static_cast<std::size_t>(s->desc.root_bone)];
    if (!root.valid() || !_world->contains(root)) {
        return out;
    }
    const Vec3f u = normalize(up);
    const Vec3f position = _world->position(root);
    const Quatf rotation = _world->rotation(root);
    QueryFilter3D filter;
    filter.mask = mask;
    filter.ignore = ignore;
    const std::vector<RayHit3D> hits = _world->raycast_all(position + u * 0.2f, position - u * std::max(max_distance, 0.0f), filter);
    for (const RayHit3D& hit : hits) {
        if (std::find(s->bodies.begin(), s->bodies.end(), hit.body) != s->bodies.end()) {
            continue;
        }
        out.found = true;
        out.position = hit.point;
        out.height = dot(position - hit.point, u);
        break;
    }
    const Vec3f forward = rotation.rotate(length(s->desc.forward) > 0.0f ? normalize(s->desc.forward) : Vec3f{0.0f, 0.0f, 1.0f});
    const Vec3f head = rotation.rotate(length(s->desc.up) > 0.0f ? normalize(s->desc.up) : Vec3f{0.0f, 1.0f, 0.0f});
    const f32 along = dot(forward, u);
    out.lying = std::fabs(along) > 0.7f;
    out.face_down = along < -0.7f;
    Vec3f facing;
    if (!out.lying) {
        facing = forward - u * dot(forward, u);
    } else {
        // On the back or the belly: the head's direction (a person lying on their back stands facing their feet).
        facing = head - u * dot(head, u);
        if (!out.face_down) {
            facing = -facing;
        }
    }
    if (length(facing) < 1.0e-4f) {
        facing = cross(u, std::fabs(u.x) < 0.9f ? Vec3f{1.0f, 0.0f, 0.0f} : Vec3f{0.0f, 0.0f, 1.0f});
    }
    // A model faces +Z; look_rotation turns -Z.
    out.rotation = Quatf::look_rotation(-normalize(facing), u);
    return out;
}

void RagdollSet::pre_step(f32 dt) {
    for (u32 index = 0; index < _slots.size(); ++index) {
        Slot& s = _slots[index];
        if (!s.alive) {
            continue;
        }
        if (s.mode != RagdollMode3D::Animated) {
            if (s.t < s.duration) {
                s.t = std::min(s.duration, s.t + dt);
                s.alpha = s.from + (s.to - s.from) * smooth(s.t / s.duration);
            } else {
                s.alpha = s.to;
            }
            const bool done = s.t >= s.duration && s.alpha <= 0.0f;
            if (done && s.mode == RagdollMode3D::GetUp) {
                s.mode = RagdollMode3D::Animated;
                s.alpha = 0.0f;
                s.started = false;
                push(s, index, RagdollEventKind3D::Recovered);
            } else if (done && s.mode == RagdollMode3D::Blend) {
                s.mode = RagdollMode3D::Animated;
                s.alpha = 0.0f;
            }
        }
        apply_motion(s, true);
        if (!s.frozen && !s.parked && s.has_targets) {
            for (std::size_t i = 0; i < s.bodies.size(); ++i) {
                if (s.dynamic[i] == 0 && s.bodies[i].valid() && _world->contains(s.bodies[i])) {
                    _world->follow_transform(s.bodies[i], s.target_position[i], s.target_rotation[i], dt);
                }
            }
        }
    }
}

void RagdollSet::post_step(f32 dt) {
    for (u32 index = 0; index < _slots.size(); ++index) {
        Slot& s = _slots[index];
        if (!s.alive) {
            continue;
        }
        if ((s.mode == RagdollMode3D::Ragdoll || s.mode == RagdollMode3D::Blend) && !s.settled && !s.frozen && s.t >= s.duration) {
            bool any = false, all_asleep = true, quiet = true;
            for (std::size_t i = 0; i < s.bodies.size(); ++i) {
                if (s.dynamic[i] == 0 || !s.bodies[i].valid() || !_world->contains(s.bodies[i])) {
                    continue;
                }
                any = true;
                if (_world->is_active(s.bodies[i])) {
                    all_asleep = false;
                    if (length(_world->linear_velocity(s.bodies[i])) > s.desc.settle_speed ||
                        length(_world->angular_velocity(s.bodies[i])) > s.desc.settle_angular_speed) {
                        quiet = false;
                    }
                }
            }
            if (any && (quiet || all_asleep)) {
                s.settle_timer += dt;
            } else {
                s.settle_timer = 0.0f;
            }
            if (any && (all_asleep || s.settle_timer >= s.desc.settle_seconds)) {
                s.settled = true;
                push(s, index, RagdollEventKind3D::Settled);
                if (s.desc.freeze_on_settle) {
                    s.frozen = true;
                    apply_motion(s, false);
                }
            }
        }
    }
    // The batch since the last step (commands included) in a fixed order: by ragdoll, then by kind.
    std::stable_sort(_events.begin() + static_cast<std::ptrdiff_t>(_batch), _events.end(),
                     [](const RagdollEvent3D& a, const RagdollEvent3D& b) {
                         if (a.ragdoll.index != b.ragdoll.index) return a.ragdoll.index < b.ragdoll.index;
                         return static_cast<u8>(a.kind) < static_cast<u8>(b.kind);
                     });
    _batch = _events.size();
}

// --- state -----------------------------------------------------------------------------------------------------------

void RagdollSet::save_header(std::vector<u8>& out) const {
    ByteOut w{out};
    w.u32v(static_cast<u32>(_slots.size()));
    for (const Slot& s : _slots) {
        w.u32v(s.generation);
        w.b(s.alive);
        if (!s.alive) {
            continue;
        }
        w.u8v(static_cast<u8>(s.mode));
        w.f32v(s.alpha);
        w.f32v(s.from);
        w.f32v(s.to);
        w.f32v(s.t);
        w.f32v(s.duration);
        w.f32v(s.settle_timer);
        w.b(s.frozen);
        w.b(s.settled);
        w.b(s.has_targets);
        w.b(s.started);
        w.u32v(static_cast<u32>(s.weight.size()));
        for (const f32 v : s.weight) {
            w.f32v(v);
        }
        for (std::size_t i = 0; i < s.target_position.size(); ++i) {
            w.vec(s.target_position[i]);
            w.quat(s.target_rotation[i]);
            w.vec(s.snap_position[i]);
            w.quat(s.snap_rotation[i]);
        }
    }
}

std::unique_ptr<RagdollSet::Parsed> RagdollSet::parse_header(const u8*& cursor, const u8* end) const {
    ByteIn in{cursor, end};
    auto parsed = std::make_unique<Parsed>();
    const u32 count = in.u32v();
    if (in.failed || count != _slots.size()) {
        return nullptr;
    }
    parsed->slots.resize(count);
    for (u32 i = 0; i < count; ++i) {
        Parsed::One& o = parsed->slots[i];
        o.generation = in.u32v();
        o.alive = in.b();
        if (in.failed || o.generation != _slots[i].generation || o.alive != _slots[i].alive) {
            return nullptr;
        }
        if (!o.alive) {
            continue;
        }
        o.mode = static_cast<RagdollMode3D>(in.u8v());
        o.alpha = in.f32v();
        o.from = in.f32v();
        o.to = in.f32v();
        o.t = in.f32v();
        o.duration = in.f32v();
        o.settle_timer = in.f32v();
        o.frozen = in.b();
        o.settled = in.b();
        o.has_targets = in.b();
        o.started = in.b();
        const u32 bones = in.u32v();
        if (in.failed || bones != _slots[i].bodies.size()) {
            return nullptr;
        }
        o.weights.resize(bones);
        for (f32& v : o.weights) {
            v = in.f32v();
        }
        o.target_position.resize(bones);
        o.target_rotation.resize(bones);
        o.snap_position.resize(bones);
        o.snap_rotation.resize(bones);
        for (u32 b = 0; b < bones; ++b) {
            o.target_position[b] = in.vec();
            o.target_rotation[b] = in.quat();
            o.snap_position[b] = in.vec();
            o.snap_rotation[b] = in.quat();
        }
        if (in.failed) {
            return nullptr;
        }
    }
    cursor = in.p;
    return parsed;
}

void RagdollSet::apply(const Parsed& parsed) {
    for (u32 i = 0; i < parsed.slots.size() && i < _slots.size(); ++i) {
        const Parsed::One& o = parsed.slots[i];
        Slot& s = _slots[i];
        if (!s.alive) {
            continue;
        }
        s.mode = o.mode;
        s.alpha = o.alpha;
        s.from = o.from;
        s.to = o.to;
        s.t = o.t;
        s.duration = o.duration;
        s.settle_timer = o.settle_timer;
        s.frozen = o.frozen;
        s.settled = o.settled;
        s.has_targets = o.has_targets;
        s.started = o.started;
        s.weight = o.weights;
        s.target_position = o.target_position;
        s.target_rotation = o.target_rotation;
        s.snap_position = o.snap_position;
        s.snap_rotation = o.snap_rotation;
        apply_motion(s, false);
    }
}

} // namespace physics3d_detail

// --- from a skeleton ---------------------------------------------------------------------------------------------------

namespace {

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool contains_name(const std::string& name, const std::string& pattern) {
    return !pattern.empty() && lower(name).find(lower(pattern)) != std::string::npos;
}

Vec3f translation_of(const Mat4f& m) { return {m.m[12], m.m[13], m.m[14]}; }

Quatf rotation_of_matrix(const Mat4f& m) {
    // Columns normalised: the rotation of a joint that may carry a scale.
    Mat4f r = m;
    for (int c = 0; c < 3; ++c) {
        const Vec3f col = normalize(Vec3f{m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]});
        r.m[c * 4] = col.x;
        r.m[c * 4 + 1] = col.y;
        r.m[c * 4 + 2] = col.z;
    }
    const f32 trace = r.m[0] + r.m[5] + r.m[10];
    Quatf q;
    if (trace > 0.0f) {
        const f32 k = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(r.m[6] - r.m[9]) / k, (r.m[8] - r.m[2]) / k, (r.m[1] - r.m[4]) / k, 0.25f * k};
    } else if (r.m[0] > r.m[5] && r.m[0] > r.m[10]) {
        const f32 k = std::sqrt(1.0f + r.m[0] - r.m[5] - r.m[10]) * 2.0f;
        q = {0.25f * k, (r.m[4] + r.m[1]) / k, (r.m[8] + r.m[2]) / k, (r.m[6] - r.m[9]) / k};
    } else if (r.m[5] > r.m[10]) {
        const f32 k = std::sqrt(1.0f + r.m[5] - r.m[0] - r.m[10]) * 2.0f;
        q = {(r.m[4] + r.m[1]) / k, 0.25f * k, (r.m[9] + r.m[6]) / k, (r.m[8] - r.m[2]) / k};
    } else {
        const f32 k = std::sqrt(1.0f + r.m[10] - r.m[0] - r.m[5]) * 2.0f;
        q = {(r.m[8] + r.m[2]) / k, (r.m[9] + r.m[6]) / k, 0.25f * k, (r.m[1] - r.m[4]) / k};
    }
    return q.normalized();
}

Quatf y_to(Vec3f dir) {
    const f32 d = dir.y;
    if (d < -0.999999f) {
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    return Quatf{dir.z, 0.0f, -dir.x, 1.0f + d}.normalized();
}

} // namespace

RagdollDesc auto_ragdoll_desc(const Skeleton3D& skeleton, const Pose3D& pose, const AutoRagdollOptions& o) {
    RagdollDesc desc;
    desc.layer = o.layer;
    desc.self_collision = o.self_collision;
    const u32 n = skeleton.joint_count();
    if (n == 0 || pose.joints.size() != n) {
        return desc;
    }
    std::vector<Mat4f> world;
    compute_world_matrices(skeleton, pose, world);
    std::vector<std::vector<u32>> children(n);
    for (u32 j = 0; j < n; ++j) {
        if (skeleton.parents[j] >= 0) {
            children[static_cast<std::size_t>(skeleton.parents[j])].push_back(j);
        }
    }
    // The ragdoll's root joint: the one called `root_name`, else the first joint with two or more children (the pelvis of
    // a rig with a helper root above it), else the skeleton's first root.
    i32 root = -1;
    if (!o.root_name.empty()) {
        for (u32 j = 0; j < n && root < 0; ++j) {
            if (contains_name(skeleton.names[j], o.root_name)) {
                root = static_cast<i32>(j);
            }
        }
    }
    if (root < 0) {
        for (u32 j = 0; j < n && root < 0; ++j) {
            if (children[j].size() >= 2) {
                root = static_cast<i32>(j);
            }
        }
    }
    if (root < 0) {
        root = skeleton.root_joint();
    }
    if (root < 0) {
        return desc;
    }
    std::vector<u8> below = skeleton.subtree(root);

    struct Plan {
        u32 joint = 0;
        Vec3f from{}, to{};
        bool leaf = false;
        f32 length = 0.0f;
        f32 radius = 0.0f;
        f32 volume = 0.0f;
        std::string name;
    };
    std::vector<Plan> plans;
    std::vector<i32> bone_of(n, -1);
    for (u32 j = 0; j < n; ++j) {
        if (below[j] == 0) {
            continue;
        }
        const std::string& name = skeleton.names[j];
        bool skipped = false;
        for (const std::string& pattern : o.skip) {
            skipped = skipped || contains_name(name, pattern);
        }
        if (skipped) {
            continue;
        }
        const Vec3f from = translation_of(world[j]);
        // The direction the parent bone runs in (none for the root).
        Vec3f incoming{};
        if (static_cast<i32>(j) != root && skeleton.parents[j] >= 0) {
            const Vec3f d = from - translation_of(world[static_cast<std::size_t>(skeleton.parents[j])]);
            if (length(d) > 1.0e-5f) {
                incoming = normalize(d);
            }
        }
        // The main child: the one that continues the incoming direction; the longest for a root or a bend.
        i32 pick = -1;
        f32 best = -2.0f;
        for (const u32 c : children[j]) {
            const Vec3f d = translation_of(world[c]) - from;
            const f32 len = length(d);
            if (len < o.min_bone_length) {
                continue;
            }
            bool skip_child = false;
            for (const std::string& pattern : o.skip) {
                skip_child = skip_child || contains_name(skeleton.names[c], pattern);
            }
            if (skip_child) {
                continue;
            }
            const f32 score = incoming == Vec3f{} ? len : dot(d / len, incoming) + 0.001f * len;
            if (score > best) {
                best = score;
                pick = static_cast<i32>(c);
            }
        }
        Plan p;
        p.joint = j;
        p.from = from;
        p.name = name;
        if (pick >= 0) {
            p.to = translation_of(world[static_cast<std::size_t>(pick)]);
            p.length = length(p.to - from);
            p.radius = std::clamp(o.radius_ratio * p.length, o.min_radius, o.max_radius);
        } else {
            // A leaf: a short capsule along the way the parent came.
            Vec3f d = incoming != Vec3f{} ? incoming : Vec3f{0.0f, 1.0f, 0.0f};
            // A foot: the bone above points down, so the capsule goes forward (along the ground) instead.
            if (length(o.up) > 0.0f && length(o.forward) > 0.0f && dot(d, -normalize(o.up)) > 0.85f) {
                d = normalize(o.forward);
            }
            p.leaf = true;
            p.length = o.leaf_length;
            p.to = from + d * p.length;
            p.radius = std::clamp(o.leaf_radius_ratio * p.length, o.min_radius, o.max_radius);
        }
        if (p.length < o.min_bone_length) {
            continue;
        }
        // A capsule's cylinder is the length less its two caps.
        const f32 straight = std::max(p.length - 2.0f * p.radius, 0.01f);
        p.volume = 3.14159265f * p.radius * p.radius * straight + 4.0f / 3.0f * 3.14159265f * p.radius * p.radius * p.radius;
        bone_of[j] = static_cast<i32>(plans.size());
        plans.push_back(p);
    }
    f32 total_volume = 0.0f;
    for (const Plan& p : plans) {
        total_volume += p.volume;
    }
    for (const Plan& p : plans) {
        RagdollBoneDesc3D b;
        b.name = p.name;
        b.joint = static_cast<i32>(p.joint);
        // The nearest ancestor with a body.
        i32 parent = -1;
        for (i32 a = skeleton.parents[p.joint]; a >= 0; a = skeleton.parents[static_cast<std::size_t>(a)]) {
            if (bone_of[static_cast<std::size_t>(a)] >= 0) {
                parent = bone_of[static_cast<std::size_t>(a)];
                break;
            }
        }
        b.parent = parent;
        const Mat4f& w = world[p.joint];
        const Quatf rot = rotation_of_matrix(w);
        const Quatf inverse = rot.conjugate();
        const Vec3f axis_world = normalize(p.to - p.from);
        const Vec3f axis_local = inverse.rotate(axis_world);
        b.shape = RagdollShape3D::Capsule;
        b.radius = p.radius;
        b.half_height = std::max(0.5f * p.length - p.radius, 0.005f);
        b.offset = inverse.rotate((p.to - p.from) * 0.5f);
        b.rotation = y_to(axis_local);
        b.mass = total_volume > 0.0f ? o.total_mass * p.volume / total_volume : 0.0f;
        b.joint_axis = axis_local;
        // A little friction at the joints (N m a kg of the bone): a limb that has stopped does not keep drifting.
        b.joint_friction = o.joint_friction * b.mass;
        b.joint_kind = RagdollJoint3D::Cone;
        b.swing_degrees = o.swing_degrees;
        b.twist_min_degrees = -o.twist_degrees;
        b.twist_max_degrees = o.twist_degrees;
        for (const AutoRagdollOptions::Override& ov : o.overrides) {
            if (contains_name(p.name, ov.pattern)) {
                b.joint_kind = ov.kind;
                if (length(ov.axis_model) > 0.0f) {
                    b.joint_axis = inverse.rotate(normalize(ov.axis_model));
                }
                b.hinge_min_degrees = ov.min_degrees;
                b.hinge_max_degrees = ov.max_degrees;
                break;
            }
        }
        desc.bones.push_back(b);
    }
    // The root of the desc: the bone of the ragdoll's root joint (the first, as parents come first).
    desc.root_bone = 0;
    if (!plans.empty()) {
        const Quatf inverse = rotation_of_matrix(world[plans[0].joint]).conjugate();
        if (length(o.forward) > 0.0f) {
            desc.forward = inverse.rotate(normalize(o.forward));
        }
        if (length(o.up) > 0.0f) {
            desc.up = inverse.rotate(normalize(o.up));
        }
    }
    return desc;
}

} // namespace kin
