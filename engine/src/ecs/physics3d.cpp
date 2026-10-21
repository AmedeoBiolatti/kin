#include <kin/ecs/physics3d.hpp>

#include <kin/platform/log.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/render3d/skeleton3d.hpp>
#include <kin/physics3d/assets3d.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace kin {

namespace {

// An ancestor chain's world matrix as propagate_transforms makes it (a parent without Transform3D ends the chain),
// worked out here and now: the world a body is made in is not the render's, which may be a frame old.
Mat4f world_matrix(flecs::entity entity) {
    const Transform3D* own = entity.get<Transform3D>();
    Mat4f m = own != nullptr ? own->matrix() : Mat4f{};
    for (flecs::entity p = entity.parent(); p.is_valid() && p.is_alive(); p = p.parent()) {
        const Transform3D* t = p.get<Transform3D>();
        if (t == nullptr) {
            break;
        }
        m = t->matrix() * m;
    }
    return m;
}

Mat4f parent_matrix(flecs::entity entity) {
    const flecs::entity p = entity.parent();
    return p.is_valid() && p.is_alive() && p.get<Transform3D>() != nullptr ? world_matrix(p) : Mat4f{};
}

Quatf quat_of(const Mat4f& m, Vec3f s) {
    // The rotation part with the scale divided out (a reflection is not representable: it comes out as some rotation).
    const f32 sx = s.x != 0.0f ? s.x : 1.0f, sy = s.y != 0.0f ? s.y : 1.0f, sz = s.z != 0.0f ? s.z : 1.0f;
    const f32 r00 = m.m[0] / sx, r10 = m.m[1] / sx, r20 = m.m[2] / sx;
    const f32 r01 = m.m[4] / sy, r11 = m.m[5] / sy, r21 = m.m[6] / sy;
    const f32 r02 = m.m[8] / sz, r12 = m.m[9] / sz, r22 = m.m[10] / sz;
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

struct Placed {
    Vec3f position{};
    Quatf rotation{};
    Vec3f scale{1.0f, 1.0f, 1.0f};
};

Placed decompose(const Mat4f& m) {
    Placed p;
    p.position = {m.m[12], m.m[13], m.m[14]};
    p.scale = {length(Vec3f{m.m[0], m.m[1], m.m[2]}), length(Vec3f{m.m[4], m.m[5], m.m[6]}),
               length(Vec3f{m.m[8], m.m[9], m.m[10]})};
    p.rotation = quat_of(m, p.scale);
    return p;
}

Vec3f abs3(Vec3f v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }

bool hand_active(const TraversalState3D& traversal, u32 limb) {
    const auto phase = traversal.grips[limb].phase;
    const bool reaching = phase == GripPhase3D::Reaching;
    const bool holding = phase == GripPhase3D::Held || phase == GripPhase3D::Slipping;
    return (reaching || holding) && !(traversal.mode == TraversalMode3D::Mantling && traversal.mantle_progress > 0.6f);
}

Quatf hand_rotation(const GripState3D& grip, const CharacterAnimationLink3D& link) {
    Mat4f m;
    const Vec3f x = -grip.right, y = grip.surface_up, z = -grip.normal;
    m.m[0] = x.x;
    m.m[1] = x.y;
    m.m[2] = x.z;
    m.m[4] = y.x;
    m.m[5] = y.y;
    m.m[6] = y.z;
    m.m[8] = z.x;
    m.m[9] = z.y;
    m.m[10] = z.z;
    return quat_of(m, {1, 1, 1}) * link.hand_offset;
}

void drive_traversal_hand(IKConstraint3D& hand, const TraversalState3D& traversal, const CharacterAnimationLink3D& link,
                          u32 limb) {
    if (hand_active(traversal, limb)) {
        const auto& grip = traversal.grips[limb];
        hand.set_target(grip.point + grip.normal * link.hand_clearance);
        if (link.align_hands)
            hand.set_target_rotation(hand_rotation(grip, link), 1.f);
        hand.set_weight(1.f);
    } else {
        hand.set_weight(0.f);
    }
}

// The singleton `S` (events of one kind) made to hold `events`, in place: its vector keeps its room, and the change is announced
// as set() announces it.
template <class S, class E>
void publish(flecs::world& ecs, const std::vector<E>& events) {
    ecs.ensure<S>().events = events;
    ecs.modified<S>();
}

// More spare body slots than the most that one thing takes (a rope of 512 segments and its anchors): a refusal with that much
// room was not for want of it.
constexpr i64 ample_bodies = 520;

// The asset cooker's test for a scale that is the same on every axis (assets3d.cpp), with room for the few ulps that a scale
// taken from a matrix is off by.
bool uniform_scale(Vec3f s) {
    const f32 slack = 1.0e-5f * std::max({1.0f, s.x, s.y, s.z});
    return std::fabs(s.x - s.y) <= slack && std::fabs(s.y - s.z) <= slack;
}

// A collider (or compound child) that is turned under a scale that differs between axes. build_shape scales a shape along its own
// axes and then turns it, which is not the entity's scale (along the entity's axes) applied to a turned shape: a beam 2 m long
// along X, turned to Z on an entity stretched three times along Z, would stay 2 m where it should be 6. It is refused, as the asset
// cooker refuses it, and not made wrong. A ready shape is used as it is (no scale reaches it), so turning it is fine.
bool skewed_rotation(const Collider3D& c, Vec3f entity_scale, int depth = 0) {
    const bool ready = c.asset.id.empty() && c.shape.valid();
    if (!ready && c.rotation != Quatf{} && !uniform_scale(abs3(entity_scale))) {
        return true;
    }
    if (ready || !c.asset.id.empty() || c.kind != Collider3D::Kind::Compound || depth >= 4) {
        return false;
    }
    return std::ranges::any_of(c.children, [&](const Collider3D& child) {
        return skewed_rotation(child, entity_scale * c.scale, depth + 1);
    });
}

// The shape for a collider at the given scale (entity's world scale times its own), in the entity's space.
Shape3D build_shape(const Collider3D& c, Vec3f entity_scale, int depth = 0) {
    const Vec3f s = abs3(entity_scale * c.scale);
    Shape3D shape;
    if (!c.asset.id.empty()) {
        // Never silently substitute a box for an unresolved authored recipe.
        shape = c.asset_binding && c.asset_binding->scale == s ? c.asset_binding->shape : Shape3D{};
    } else if (c.shape.valid()) {
        shape = c.shape;
    } else {
        switch (c.kind) {
        case Collider3D::Kind::Box:
            shape = Shape3D::box(c.half_extents * s);
            break;
        case Collider3D::Kind::Sphere:
            shape = Shape3D::sphere(c.radius * std::max({s.x, s.y, s.z}));
            break;
        case Collider3D::Kind::Capsule:
            shape = Shape3D::capsule(c.half_height * s.y, c.radius * std::max(s.x, s.z));
            break;
        case Collider3D::Kind::Cylinder:
            shape = Shape3D::cylinder(c.half_height * s.y, c.radius * std::max(s.x, s.z));
            break;
        case Collider3D::Kind::ConvexHull:
        case Collider3D::Kind::TriangleMesh: {
            if (!c.mesh || c.mesh->vertices.empty()) {
                break;
            }
            std::vector<Vec3f> points;
            points.reserve(c.mesh->vertices.size());
            for (const MeshVertex3D& v : c.mesh->vertices) {
                points.push_back(v.position * s);
            }
            if (c.kind == Collider3D::Kind::ConvexHull) {
                shape = Shape3D::convex_hull(points);
            } else if (c.mesh->indices.empty()) {
                std::vector<u32> sequential(points.size());
                for (u32 i = 0; i < sequential.size(); ++i) {
                    sequential[i] = i;
                }
                shape = Shape3D::triangle_mesh(points, sequential);
            } else {
                shape = Shape3D::triangle_mesh(points, c.mesh->indices);
            }
            break;
        }
        case Collider3D::Kind::Compound: {
            if (depth >= 4 || c.children.empty()) {
                break;
            }
            std::vector<ShapeChild3D> children;
            for (const Collider3D& child : c.children) {
                Shape3D inner = build_shape(child, entity_scale * c.scale, depth + 1);
                if (!inner.valid()) {
                    return {};
                }
                children.push_back({inner, child.offset * s, child.rotation});
            }
            shape = Shape3D::compound(children);
            break;
        }
        }
    }
    return shape;
}

// The collider's shape with its offset and rotation applied (a one child compound when it has any).
Shape3D build_placed_shape(const Collider3D& c, Vec3f entity_scale) {
    if (skewed_rotation(c, entity_scale)) {
        return {};
    }
    Shape3D shape = build_shape(c, entity_scale);
    if (!shape.valid()) {
        return shape;
    }
    const Vec3f offset = c.offset * abs3(entity_scale);
    if (offset == Vec3f{} && c.rotation == Quatf{}) {
        return shape;
    }
    const ShapeChild3D one{shape, offset, c.rotation};
    return Shape3D::compound(std::span<const ShapeChild3D>(&one, 1));
}

// FNV-1a over the values that a body, character, ragdoll or rope is made from, to tell a refused entity whose components changed
// from one whose did not. Floats go in by their bits; a mesh or a shape by identity (and size).
struct Stamp {
    u64 hash = 14695981039346656037ull;
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash = (hash ^ p[i]) * 1099511628211ull;
        }
    }
    template <class T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    void add(T v) {
        bytes(&v, sizeof v);
    }
    void add(Vec3f v) {
        add(v.x);
        add(v.y);
        add(v.z);
    }
    void add(Quatf q) {
        add(q.x);
        add(q.y);
        add(q.z);
        add(q.w);
    }
    void add(const std::string& s) {
        add(s.size());
        bytes(s.data(), s.size());
    }
    void add(const void* p) { add(reinterpret_cast<std::uintptr_t>(p)); }
    template <class... T>
    void operator()(const T&... values) {
        (add(values), ...);
    }
};

bool finite_pose(const Placed& p) {
    return std::isfinite(p.position.x) && std::isfinite(p.position.y) && std::isfinite(p.position.z) &&
           std::isfinite(p.rotation.x) && std::isfinite(p.rotation.y) && std::isfinite(p.rotation.z) && std::isfinite(p.rotation.w);
}

void stamp_shape(Stamp& s, const Shape3D& shape) {
    s(shape.valid());
    if (shape.valid()) {
        s(shape.kind(), shape.bounds_min(), shape.bounds_max());
    }
}

void stamp_collider(Stamp& s, const Collider3D& c, int depth = 0) {
    s(c.kind, c.half_extents, c.radius, c.half_height, c.offset, c.rotation, c.scale, c.asset.id,
      static_cast<const void*>(c.mesh.get()), static_cast<const void*>(c.asset_binding.get()), c.children.size());
    if (c.mesh) {
        s(c.mesh->vertices.size(), c.mesh->indices.size());
    }
    stamp_shape(s, c.shape);
    if (c.asset_binding) {
        s(c.asset_binding->scale, c.asset_binding->contains_surface);
        stamp_shape(s, c.asset_binding->shape);
    }
    if (depth < 4) {
        for (const Collider3D& child : c.children) {
            stamp_collider(s, child, depth + 1);
        }
    }
}

// What create_body reads, bar the pose (an entity that cannot be made must not be tried again each time it moves): the world
// scale, whether the pose is usable, the collider and the body's settings.
u64 body_stamp(flecs::entity entity) {
    const Placed pose = decompose(world_matrix(entity));
    Stamp s;
    s(pose.scale, finite_pose(pose));
    stamp_collider(s, *entity.get<Collider3D>());
    const RigidBody3D* rb = entity.get<RigidBody3D>();
    s(rb != nullptr);
    if (rb != nullptr) {
        s(rb->motion, rb->mass, rb->density, rb->friction, rb->restitution, rb->linear_damping, rb->angular_damping,
          rb->gravity_factor, rb->layer, rb->sensor, rb->allow_sleeping, rb->continuous, rb->linear_velocity,
          rb->angular_velocity);
    }
    return s.hash;
}

// The fields that create_character checks (the grips' tuning is left out: `rebuild(entity)` reads it again).
u64 character_stamp(const CharacterDesc3D& d, const Placed& pose) {
    Stamp s;
    s(finite_pose(pose), d.up, d.radius, d.height, d.crouch_height, d.mass, d.push_strength, d.max_slope_degrees,
      d.max_step_height, d.step_down_distance, d.walk_speed, d.sprint_speed, d.crouch_speed, d.acceleration, d.deceleration,
      d.air_acceleration, d.ground_friction, d.wall_friction, d.gravity_scale, d.jump_speed, d.coyote_seconds,
      d.jump_buffer_seconds, d.up_blend_rate, d.layer, d.collides_with, d.traversal.enabled, d.traversal.limb_count);
    return s.hash;
}

u64 ragdoll_stamp(const Ragdoll3D& r, const Model3D& model, const Placed& pose) {
    Stamp s;
    const RagdollDesc& d = r.desc;
    s(pose.scale, finite_pose(pose), static_cast<const void*>(&model), model.skeletons[0].joint_count(), d.bones.size(),
      d.layer, d.self_collision, d.continuous, d.animated_bodies, d.settle_speed, d.settle_angular_speed, d.settle_seconds,
      d.freeze_on_settle, d.velocity_steps, d.position_steps, d.gravity_factor, d.root_bone, d.forward, d.up);
    for (const RagdollBoneDesc3D& b : d.bones) {
        s(b.joint, b.parent, b.shape, b.offset, b.rotation, b.radius, b.half_height, b.half_extents, b.mass, b.density,
          b.friction, b.restitution, b.linear_damping, b.angular_damping, b.joint_kind, b.joint_axis, b.swing_degrees,
          b.twist_min_degrees, b.twist_max_degrees, b.hinge_min_degrees, b.hinge_max_degrees, b.joint_friction, b.weight);
    }
    // The skeleton's own bones, when `desc` has none.
    const AutoRagdollOptions& o = r.auto_options;
    s(o.radius_ratio, o.min_radius, o.max_radius, o.leaf_length, o.leaf_radius_ratio, o.min_bone_length, o.total_mass,
      o.swing_degrees, o.twist_degrees, o.joint_friction, o.forward, o.up, o.layer, o.self_collision, o.root_name,
      o.overrides.size(), o.skip.size());
    for (const AutoRagdollOptions::Override& v : o.overrides) {
        s(v.pattern, v.kind, v.axis_model, v.min_degrees, v.max_degrees);
    }
    for (const std::string& name : o.skip) {
        s(name);
    }
    return s.hash;
}

// `start_body` and `end_body` are the bodies of the entities that the ends are fixed to.
u64 rope_stamp(const Rope3D& c, const Placed& pose, Body3D start_body, Body3D end_body) {
    Stamp s;
    s(finite_pose(pose), start_body.index, start_body.generation, end_body.index, end_body.generation, c.start.kind,
      c.start.point, c.start.entity, c.end.kind, c.end.point, c.end.entity, c.length, c.segments, c.segment_length, c.radius,
      c.mass_per_metre, c.friction, c.linear_damping, c.angular_damping, c.sag_direction, c.hang_direction, c.layer,
      c.collide_world, c.self_collision, c.continuous, c.velocity_steps, c.position_steps, c.break_tension);
    return s.hash;
}

} // namespace

Shape3D cook_collider3d(const Collider3D& collider,Vec3f scale) { return build_placed_shape(collider,scale); }

WorldPhysicsState3D::WorldPhysicsState3D(flecs::world& world, World3D& physics, WorldPhysicsState3DConfig config)
    : _ecs(world)
    , _physics(physics)
    , _config(config)
    // Cached, and with no data read: `changed()` then says whether entities came, went or moved between tables since the query was
    // last run through, whatever their components hold (a Transform3D written every step is not a change here).
    , _query(world.query_builder().with<Transform3D>().inout_none().with<Collider3D>().inout_none().cached().build())
    , _character_query(world.query_builder().with<Transform3D>().inout_none().with<CharacterController3D>().inout_none().cached().build())
    , _ragdoll_query(world.query_builder<const Transform3D, const SkinnedMeshRenderer3D, const Ragdoll3D>().cached().build())
    , _rope_query(world.query_builder().with<Rope3D>().inout_none().cached().build())
    , _debug(config.debug) {
    // This readable empty singleton exists even when collection is disabled.
    // No contacts are collected or copied on the disabled path.
    _ecs.set<PhysicsContacts3D>({});
}

void WorldPhysicsState3D::create_body(flecs::entity entity, Bound& out, u64 user_id) {
    const Collider3D& collider = *entity.get<Collider3D>();
    const RigidBody3D* rb = entity.get<RigidBody3D>();
    const Placed pose = decompose(world_matrix(entity));
    Body3DDesc desc;
    desc.shape = build_placed_shape(collider, pose.scale);
    desc.position = pose.position;
    desc.rotation = pose.rotation;
    desc.user_id = user_id ? user_id : entity.id();
    desc.motion = Body3DMotion::Static;
    if (rb != nullptr) {
        desc.motion = rb->motion;
        desc.mass = rb->mass;
        desc.density = rb->density;
        desc.friction = rb->friction;
        desc.restitution = rb->restitution;
        desc.linear_damping = rb->linear_damping;
        desc.angular_damping = rb->angular_damping;
        desc.gravity_factor = rb->gravity_factor;
        desc.layer = rb->layer;
        desc.sensor = rb->sensor;
        desc.allow_sleeping = rb->allow_sleeping;
        desc.continuous = rb->continuous;
        desc.linear_velocity = rb->linear_velocity;
        desc.angular_velocity = rb->angular_velocity;
    }
    if (collider.asset_binding && collider.asset_binding->contains_surface && desc.motion == Body3DMotion::Dynamic)
        desc.shape = {};
    out = {};
    out.entity = desc.user_id;
    out.body = _physics.create_body(desc);
    out.motion = desc.motion;
    out.scale = pose.scale;
    out.position = out.previous_position = pose.position;
    out.rotation = out.previous_rotation = pose.rotation;
    if (!out.body.valid()) {
        KIN_LOG_WARN_F("physics3d",
                       skewed_rotation(collider, pose.scale) ? "entity has no valid body (rotated collider children require uniform parent scale)"
                                                             : "entity has no valid body (shape, mass or layer refused)",
                       (LogFields{{.name = "entity", .value = std::to_string(entity.id())}}));
    } else {
        // What a hand finds in it (grab and climb): its material and, if it is one, its hold.
        if (const SurfaceMaterial3D* m = entity.get<SurfaceMaterial3D>()) {
            _physics.set_surface_material(out.body, *m);
        }
        if (const Hold3D* h = entity.get<Hold3D>()) {
            HoldDesc3D hold;
            hold.quality = h->quality;
            hold.drain = h->drain;
            hold.snap = h->snap;
            hold.anchor = h->anchor;
            hold.anchor_normal = h->anchor_normal;
            hold.slide = h->slide;
            _physics.set_hold(out.body, hold);
        }
    }
    const Transform3D* local = entity.get<Transform3D>();
    out.last_position = desc.motion == Body3DMotion::Dynamic ? local->position : pose.position;
    out.last_rotation = desc.motion == Body3DMotion::Dynamic ? local->rotation : pose.rotation;
}

bool WorldPhysicsState3D::sync_selected(EcsId id) const {
    return !_selective_sync || std::ranges::binary_search(_sync_entities,id);
}

void WorldPhysicsState3D::sync_entities(std::span<const EcsId> entities) {
    if(entities.empty())return;
    if(_selective_sync)throw std::logic_error("recursive selective physics reconciliation");
    std::vector<EcsId> selected(entities.begin(),entities.end());
    std::ranges::sort(selected);selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
    _selective_sync=true;_sync_entities=selected;
    try {sync_bodies();}
    catch(...) {_selective_sync=false;_sync_entities={};throw;}
    _selective_sync=false;_sync_entities={};
}

// The first refusal not before (entity, what) in the order of _refused.
std::vector<WorldPhysicsState3D::Refused>::iterator WorldPhysicsState3D::find_refused(flecs::entity_t entity, Made what) {
    return std::ranges::lower_bound(_refused, std::pair{entity, static_cast<u8>(what)}, {}, [](const Refused& r) {
        return std::pair{r.entity, static_cast<u8>(r.what)};
    });
}

i64 WorldPhysicsState3D::spare_bodies() const {
    return static_cast<i64>(_physics.config().max_bodies) - _physics.body_count();
}

bool WorldPhysicsState3D::refused_before(flecs::entity_t entity, Made what, u64 stamp) {
    // Named by sync_entities: asked again.
    if (_refused.empty() || (_selective_sync && sync_selected(entity))) {
        return false;
    }
    const auto found = find_refused(entity, what);
    return found != _refused.end() && found->entity == entity && found->what == what && found->stamp == stamp &&
           !(found->spare >= 0 && spare_bodies() > found->spare);
}

void WorldPhysicsState3D::refuse(flecs::entity_t entity, Made what, u64 stamp) {
    const auto found = find_refused(entity, what);
    const i64 spare = spare_bodies() < ample_bodies ? spare_bodies() : -1;
    if (found != _refused.end() && found->entity == entity && found->what == what) {
        found->stamp = stamp;
        found->spare = spare;
    } else {
        _refused.insert(found, Refused{entity, what, stamp, spare});
    }
}

void WorldPhysicsState3D::forgive(flecs::entity_t entity, Made what) {
    if (_refused.empty()) {
        return;
    }
    const auto found = find_refused(entity, what);
    if (found != _refused.end() && found->entity == entity && found->what == what) {
        _refused.erase(found);
    }
}

void WorldPhysicsState3D::prune_refused(Made what, const std::vector<flecs::entity>& wanted) {
    std::erase_if(_refused, [&](const Refused& r) {
        return r.what == what && sync_selected(r.entity) &&
               !std::ranges::binary_search(wanted, r.entity, {}, [](const flecs::entity& e) { return e.id(); });
    });
}

bool WorldPhysicsState3D::has_refused(Made what) const {
    return std::ranges::any_of(_refused, [&](const Refused& r) { return r.what == what; });
}

namespace {

// `v` by entity id; the entities of one table come in id order, so it is mostly sorted already.
void sort_by_id(std::vector<flecs::entity>& v) {
    const auto id = [](const flecs::entity& e) { return e.id(); };
    if (!std::ranges::is_sorted(v, {}, id)) {
        std::ranges::sort(v, {}, id);
    }
}

struct ReconciledEntities {
    std::vector<flecs::entity> to_create;
    std::size_t retained = 0;
};

// Both inputs are in entity-id order. Finish every destruction/compaction before
// the caller creates replacements, so native handle allocation keeps its order.
// Characters and ragdolls move their history vectors; other records retain their copy semantics.
template <bool MoveRetained = false, class Bound, class Selected, class Replace, class Destroy>
ReconciledEntities reconcile_entities(std::vector<Bound>& bound, const std::vector<flecs::entity>& wanted,
                                      Selected selected, Replace replace, Destroy destroy) {
    ReconciledEntities result;
    std::size_t i = 0, j = 0, w = 0;
    const auto keep = [&](std::size_t from) {
        if (w != from) {
            if constexpr (MoveRetained)
                bound[w] = std::move(bound[from]);
            else
                bound[w] = bound[from];
        }
        ++w;
    };
    while (i < bound.size() || j < wanted.size()) {
        const bool have_bound = i < bound.size();
        const bool have_wanted = j < wanted.size();
        if (have_bound && !selected(bound[i].entity)) {
            keep(i++);
            continue;
        }
        if (have_bound && (!have_wanted || bound[i].entity < wanted[j].id())) {
            destroy(bound[i]);
            ++i;
        } else if (have_wanted && (!have_bound || wanted[j].id() < bound[i].entity)) {
            result.to_create.push_back(wanted[j]);
            ++j;
        } else {
            if (replace(bound[i], wanted[j])) {
                destroy(bound[i]);
                result.to_create.push_back(wanted[j]);
            } else {
                keep(i);
            }
            ++i;
            ++j;
        }
    }
    bound.erase(bound.begin() + static_cast<std::ptrdiff_t>(w), bound.end());
    result.retained = w;
    return result;
}

template <class Bound> void merge_created_entities(std::vector<Bound>& bound, std::size_t retained) {
    if (bound.size() > retained)
        std::inplace_merge(bound.begin(), bound.begin() + static_cast<std::ptrdiff_t>(retained), bound.end(),
                           [](const Bound& a, const Bound& b) { return a.entity < b.entity; });
}

} // namespace

// Whether the bodies are the bodies the entities ask for, so that there is nothing to make, destroy or rebuild: no entity with a
// Transform3D and a Collider3D came, went or changed tables (flecs says), nothing was refused to be asked again, nobody called
// rebuild, and no body was lost to the world or has the wrong motion for its RigidBody3D.
bool WorldPhysicsState3D::bodies_stand() {
    // Asked every time: it is also what starts the query's monitors.
    const bool moved = _query.changed();
    if (_selective_sync || _force_sync || _body_components_dirty || moved) {
        return false;
    }
    for (const auto& refused : _refused) {
        if (refused.what != Made::Body)
            continue;
        flecs::entity e{_ecs, refused.entity};
        if (!e.is_alive() || !e.enabled() || !e.has<Transform3D>() || !e.has<Collider3D>() ||
            body_stamp(e) != refused.stamp || (refused.spare >= 0 && spare_bodies() > refused.spare))
            return false;
    }
    for (const Bound& b : _bound) {
        if (b.rebuild || !_physics.contains(b.body)) {
            return false;
        }
        const RigidBody3D* rb = b.inherited_rigid ? _ecs.entity(b.entity).get<RigidBody3D>() : b.rigid;
        if ((rb != nullptr ? rb->motion : Body3DMotion::Static) != b.motion) {
            return false;
        }
    }
    return true;
}

void WorldPhysicsState3D::sync_bodies() {
    if (!bodies_stand()) {
        reconcile_bodies();
    }
    sync_characters();
    sync_ragdolls();
    sync_ropes();
    if (_body_components_dirty || _query.changed()) {
        for (Bound& b : _bound) {
            const flecs::entity entity{_ecs, b.entity};
            b.local = entity.owns<Transform3D>() ? entity.get<Transform3D>() : nullptr;
            b.rigid = entity.owns<RigidBody3D>() ? entity.get<RigidBody3D>() : nullptr;
            // A prefab can move, gain or lose RigidBody3D without changing the instance's table or this query's terms.
            b.inherited_rigid = !entity.owns<RigidBody3D>() && entity.has(flecs::IsA, flecs::Wildcard);
        }
        // Record the tables after Body3D and other native components have been installed. Data writes do not
        // invalidate these pointers; table moves and growth do, and changed() catches them before the next step.
        _query.each([](flecs::entity) {});
        _body_components_dirty = false;
    }
    if (!_selective_sync) {
        _force_sync = false;
    }
}

void WorldPhysicsState3D::reconcile_bodies() {
    _body_components_dirty = true;
    // The entities that should have a body, by id.
    std::vector<flecs::entity>& wanted = _wanted;
    wanted.clear();
    const auto consider=[&](flecs::entity e) {
        // A character is its own capsule: a collider beside it would make a second body for the same thing.
        if (!e.has<CharacterController3D>()) {
            wanted.push_back(e);
        }
    };
    if(_selective_sync)for(auto id:_sync_entities) {
        flecs::entity e{_ecs,id};
        if(e.is_alive()&&e.enabled()&&!e.has(flecs::Prefab)&&e.has<Transform3D>()&&e.has<Collider3D>())consider(e);
    } else _query.each([&](flecs::entity e){consider(e);});
    sort_by_id(wanted);
    prune_refused(Made::Body, wanted);

    // Destroy first (ascending), then create (ascending): the order is a function of the ids only. The bodies that stay are moved
    // down over the ones that went, in place.
    auto [to_create, w] = reconcile_entities(
        _bound, wanted, [&](EcsId id) { return sync_selected(id); },
        [&](const Bound& b, flecs::entity e) {
            const RigidBody3D* rb = e.get<RigidBody3D>();
            const Body3DMotion motion = rb != nullptr ? rb->motion : Body3DMotion::Static;
            return b.rebuild || motion != b.motion || !_physics.contains(b.body);
        },
        [&](const Bound& b) { _physics.destroy_body(b.body); });
    // The new ones go after the ones that stayed, each run ascending: merge them into order. Refused ones are left out.
    for (const flecs::entity e : to_create) {
        // Refused before and nothing changed since: the same answer, without cooking the shape and warning at every step.
        if (refused_before(e.id(), Made::Body, _refused.empty() ? 0 : body_stamp(e))) {
            continue;
        }
        Bound b;
        create_body(e, b);
        if (b.body.valid()) {
            forgive(e.id(), Made::Body);
            _bound.push_back(b);
        } else {
            refuse(e.id(), Made::Body, body_stamp(e));
        }
    }
    merge_created_entities(_bound, w);
    // Hand the new bodies to their entities as a component.
    for (const flecs::entity e : to_create) {
        const auto found = std::ranges::lower_bound(_bound, e.id(), {}, &Bound::entity);
        if (found != _bound.end() && found->entity == e.id()) {
            e.set<PhysicsBody3D>({found->body});
        }
    }
    // Something changed archetype: an entity may have a parent now, or not, and may own its transform or inherit it.
    for (Bound& b : _bound) {
        b.stamped = false;
        b.parented = -1;
        b.owns_transform = -1;
    }
}

// As bodies_stand, for characters: they have no motion to differ in.
bool WorldPhysicsState3D::characters_stand() {
    const bool moved = _character_query.changed();
    if (_selective_sync || _force_sync || moved || has_refused(Made::Character)) {
        return false;
    }
    return std::ranges::none_of(_characters, [&](const BoundCharacter& c) { return c.rebuild || !_physics.contains(c.character); });
}

void WorldPhysicsState3D::sync_characters() {
    if (characters_stand()) {
        return;
    }
    std::vector<flecs::entity>& wanted = _wanted;
    wanted.clear();
    if(_selective_sync)for(auto id:_sync_entities) {
        flecs::entity e{_ecs,id};
        if(e.is_alive()&&e.enabled()&&!e.has(flecs::Prefab)&&e.has<Transform3D>()&&e.has<CharacterController3D>())wanted.push_back(e);
    } else _character_query.each([&](flecs::entity e) { wanted.push_back(e); });
    sort_by_id(wanted);
    prune_refused(Made::Character, wanted);

    // Destroyed first (ascending), then made (ascending), as bodies are.
    auto [to_create, w] = reconcile_entities<true>(
        _characters, wanted, [&](EcsId id) { return sync_selected(id); },
        [&](const BoundCharacter& c, flecs::entity) { return c.rebuild || !_physics.contains(c.character); },
        [&](const BoundCharacter& c) { _physics.destroy_character(c.character); });
    for (const flecs::entity e : to_create) {
        if (refused_before(e.id(), Made::Character, _refused.empty() ? 0 : character_stamp(e.get<CharacterController3D>()->desc, decompose(world_matrix(e))))) {
            continue;
        }
        // Runtime controller/state and transforms must never mutate a shared
        // prefab definition. Materialize before taking pointers into the table.
        EcsEntity{e}.get_mut<Transform3D>();
        EcsEntity{e}.get_mut<CharacterController3D>();
        EcsEntity{e}.get_mut<Animator3D>();
        const CharacterController3D& controller = *e.get<CharacterController3D>();
        const Placed pose = decompose(world_matrix(e));
        CharacterDesc3D desc = controller.desc;
        desc.position = pose.position;
        desc.user_id = e.id();
        BoundCharacter c;
        c.entity = e.id();
        c.character = _physics.create_character(desc);
        c.scale = pose.scale;
        c.last_position = e.get<Transform3D>()->position;
        c.position = c.previous_position = pose.position;
        if (c.character.valid()) {
            forgive(e.id(), Made::Character);
            _characters.push_back(c);
        } else {
            KIN_LOG_WARN_F("physics3d", "entity has no valid character (descriptor refused)",
                           (LogFields{{.name = "entity", .value = std::to_string(e.id())}}));
            refuse(e.id(), Made::Character, character_stamp(controller.desc, pose));
        }
    }
    merge_created_entities(_characters, w);
    for (const flecs::entity e : to_create) {
        const auto found = std::ranges::lower_bound(_characters, e.id(), {}, &BoundCharacter::entity);
        if (found != _characters.end() && found->entity == e.id()) {
            e.set<PhysicsCharacter3D>({found->character});
        }
    }
}

void WorldPhysicsState3D::write_character(BoundCharacter& c, Vec3f feet) {
    flecs::entity e(_ecs, c.entity);
    if (!e.is_alive()) {
        return;
    }
    Transform3D* t = EcsEntity{e}.get_mut<Transform3D>();
    if (t == nullptr) {
        return;
    }
    if (!e.parent().is_valid() || parent_matrix(e) == Mat4f{}) {
        t->position = feet;
    } else {
        const Mat4f local = parent_matrix(e).inverse() * Mat4f::translation(feet);
        t->position = {local.m[12], local.m[13], local.m[14]};
    }
    c.last_position = t->position;
}

void WorldPhysicsState3D::characters_to_world(f32) {
    for (BoundCharacter& c : _characters) {
        flecs::entity e(_ecs, c.entity);
        if (!e.is_alive()) {
            continue;
        }
        const Transform3D* t = e.get<Transform3D>();
        const CharacterController3D* controller = e.get<CharacterController3D>();
        if (t == nullptr || controller == nullptr) {
            continue;
        }
        // Moved by game code since we wrote it: a teleport to where it was put (its world place now).
        if (t->position != c.last_position) {
            const Placed p = decompose(world_matrix(e));
            _physics.teleport_character(c.character, p.position);
            c.position = c.previous_position = p.position;
            c.last_position = t->position;
            c.hands.clear();
        }
        _physics.set_character_input(c.character, controller->input);
        if (controller->desc.traversal.enabled) {
            _physics.set_traversal_input(c.character, controller->traversal);
        }
    }
}

void WorldPhysicsState3D::characters_from_world() {
    for (BoundCharacter& c : _characters) {
        flecs::entity e(_ecs, c.entity);
        if (!e.is_alive()) {
            continue;
        }
        const CharacterState3D state = _physics.character(c.character);
        c.previous_position = c.position;
        c.position = state.position;
        write_character(c, state.position);
        CharacterController3D* controller = e.get_mut<CharacterController3D>();
        TraversalState3D traversal;
        if (controller != nullptr) {
            controller->state = state;
            if (controller->desc.traversal.enabled) {
                traversal = _physics.traversal(c.character);
                controller->traversal_state = traversal;
                // Hanging, it faces what it holds (turning to it over a few steps, then keeping the way it faced).
                if (controller->apply_facing && traversal.hanging() && length(traversal.surface_normal) > 0.0f) {
                    const Vec3f across = traversal.surface_normal - state.up * dot(traversal.surface_normal, state.up);
                    if (length(across) > 0.2f) {
                        // The model faces +Z; look_rotation turns -Z.
                        const Quatf wanted = Quatf::look_rotation(normalize(across), state.up);
                        controller->facing = slerp(controller->facing, wanted, 0.25f);
                    }
                }
            }
            if (controller->apply_facing) {
                if (Transform3D* t = e.get_mut<Transform3D>()) {
                    t->rotation = controller->facing;
                }
            }
        }
        // The animator, when the entity has the link.
        bool sampled_hands = false;
        const CharacterAnimationLink3D* link = e.get<CharacterAnimationLink3D>();
        if (link != nullptr) {
            if (Animator3D* animator = e.get_mut<Animator3D>()) {
                drive_character_animator(*animator, state, _physics, *link);
                if (traversal.enabled) {
                    drive_traversal_animator(*animator, traversal, *link, state.up);
                    sample_hands(c, traversal, *link, *animator);
                    sampled_hands = true;
                }
            }
        }
        if (!sampled_hands) c.hands.clear();
    }
}

void WorldPhysicsState3D::sample_hands(BoundCharacter& c, const TraversalState3D& traversal,
                                       const CharacterAnimationLink3D& link, const Animator3D& animator) {
    c.hands.resize(std::min<std::size_t>(link.hands.size(), max_grip_limbs3d));
    for (u32 h = 0; h < c.hands.size(); ++h) {
        auto& history = c.hands[h];
        const auto* hand = animator.find_ik(link.hands[h]);
        const auto& grip = traversal.grips[h];
        const bool retained = history.active && history.constraint == link.hands[h] &&
                              history.clearance == link.hand_clearance && history.align == link.align_hands &&
                              history.offset == link.hand_offset;
        history.active = hand && hand_active(traversal, h) && _physics.contains(grip.body) &&
                         hand->target.kind == IKTarget3D::Kind::World &&
                         hand->target.position == grip.point + grip.normal * link.hand_clearance &&
                         (!link.align_hands || hand->target_rotation == hand_rotation(grip, link));
        history.constraint = link.hands[h];
        if (!history.active)
            continue;
        history.previous = history.current;
        auto& sample = history.current;
        sample = {};
        sample.body = grip.body;
        sample.entity = grip.body_user_id;
        sample.position = _physics.position(grip.body);
        sample.rotation = _physics.rotation(grip.body);
        const auto inverse = sample.rotation.conjugate();
        sample.local_point = inverse.rotate(grip.point - sample.position);
        sample.local_normal = inverse.rotate(grip.normal);
        sample.local_rotation = inverse * hand->target_rotation;
        if (grip.kind == GripKind3D::Rope) {
            if (const auto segment = _physics.rope_segment_of(grip.body)) {
                sample.rope = segment->rope;
                sample.along = static_cast<f32>(segment->segment) + grip.local_point.y + 0.5f;
                sample.segments = segment->segments;
            }
        }
        // A new/replaced grip has no trajectory from the previous object. Moving
        // along one rope may change capsule, while retaining the same grip.
        const bool same_rope =
            sample.rope.valid() && sample.rope == history.previous.rope && sample.segments == history.previous.segments;
        if (!retained || (!same_rope && sample.body != history.previous.body))
            history.previous = sample;
        history.target = {hand->target.position, hand->target_rotation};
        history.offset = link.hand_offset;
        history.clearance = link.hand_clearance;
        history.align = link.align_hands;
    }
}

void WorldPhysicsState3D::prepare_hand_presentation(f32 alpha) {
    _presentation_hands.clear();
    for (const auto& c : _characters) {
        const flecs::entity actor{_ecs, c.entity};
        const auto* animator = actor.is_alive() ? actor.get<Animator3D>() : nullptr;
        const auto* link = actor.is_alive() ? actor.get<CharacterAnimationLink3D>() : nullptr;
        const auto* controller = actor.is_alive() ? actor.get<CharacterController3D>() : nullptr;
        if (!animator || !link || !controller || !controller->desc.traversal.enabled)
            continue;
        for (u32 h = 0; h < c.hands.size(); ++h) {
            const auto& history = c.hands[h];
            const auto* hand = animator->find_ik(history.constraint);
            if (!history.active || !hand || h >= link->hands.size() || link->hands[h] != history.constraint ||
                !_physics.contains(history.current.body) || hand->target.kind != IKTarget3D::Kind::World ||
                hand->target.position != history.target.position || hand->target_rotation != history.target.rotation ||
                link->hand_clearance != history.clearance || link->align_hands != history.align ||
                link->hand_offset != history.offset)
                continue;
            const auto& before = history.previous;
            const auto& now = history.current;
            Vec3f position = lerp(before.position, now.position, alpha);
            Quatf rotation = slerp(before.rotation, now.rotation, alpha);
            const auto bound = std::ranges::lower_bound(_bound, now.entity, {}, &Bound::entity);
            if (bound != _bound.end() && bound->entity == now.entity && bound->body == now.body) {
                // Reuse the rendered body's pose, including teleport resets and
                // parent transforms. The grip's native local point is already scaled.
                const flecs::entity entity{_ecs, now.entity};
                const auto place = decompose(presentation_transform(entity).value_or(world_matrix(entity)));
                position = place.position;
                rotation = place.rotation;
            }
            Vec3f point = position + rotation.rotate(lerp(before.local_point, now.local_point, alpha));
            Vec3f normal = rotation.rotate(normalize(lerp(before.local_normal, now.local_normal, alpha)));
            Quatf hand_rotation = rotation * slerp(before.local_rotation, now.local_rotation, alpha);
            if (now.rope.valid()) {
                // Ropes render an averaged joint polyline, not separate capsules.
                // Sample that same polyline even when climbing crosses a segment.
                const auto rope = std::ranges::lower_bound(_ropes, now.entity, {}, &BoundRope::entity);
                const auto* component = rope != _ropes.end() && rope->entity == now.entity && rope->rope == now.rope
                                            ? flecs::entity{_ecs, rope->entity}.get<Rope3D>()
                                            : nullptr;
                if (component && component->sampled.size() == static_cast<std::size_t>(now.segments) + 1) {
                    const auto& points = component->sampled;
                    const f32 along =
                        std::clamp(std::lerp(before.along, now.along, alpha), 0.f, static_cast<f32>(now.segments));
                    const auto segment = std::min(static_cast<u32>(along), now.segments - 1);
                    const auto sample = [&](u32 i) {
                        return rope->previous.size() == points.size()
                                   ? lerp(rope->previous[i].position, points[i].position, alpha)
                                   : points[i].position;
                    };
                    point = lerp(sample(segment), sample(segment + 1), along - static_cast<f32>(segment));
                }
                // A rope grip faces the character; capsule twist does not turn it.
                normal = normalize(
                    lerp(before.rotation.rotate(before.local_normal), now.rotation.rotate(now.local_normal), alpha));
                hand_rotation =
                    slerp(before.rotation * before.local_rotation, now.rotation * now.local_rotation, alpha);
            }
            _presentation_hands.emplace_back(hand,
                                             IKTargetPose3D{point + normal * history.clearance,
                                                            history.align ? hand_rotation : hand->target_rotation});
        }
    }
    std::ranges::sort(_presentation_hands, [](const auto& a, const auto& b) {
        return std::less<const IKConstraint3D*>{}(a.first, b.first);
    });
}

std::optional<IKTargetPose3D> WorldPhysicsState3D::presentation_ik_target(const IKConstraint3D& constraint) const {
    const auto found = std::lower_bound(
        _presentation_hands.begin(), _presentation_hands.end(), &constraint,
        [](const auto& entry, const auto* key) { return std::less<const IKConstraint3D*>{}(entry.first, key); });
    return found != _presentation_hands.end() && found->first == &constraint ? std::optional{found->second}
                                                                             : std::nullopt;
}

TraversalState3D WorldPhysicsState3D::traversal_of(flecs::entity entity) const {
    const Character3D c = character_of(entity);
    return c.valid() ? _physics.traversal(c) : TraversalState3D{};
}

Character3D WorldPhysicsState3D::character_of(flecs::entity entity) const {
    const auto found = std::ranges::lower_bound(_characters, entity.id(), {}, &BoundCharacter::entity);
    return found != _characters.end() && found->entity == entity.id() ? found->character : Character3D{};
}

void WorldPhysicsState3D::write_back(Bound& b, Vec3f position, Quatf rotation, bool in_step) {
    flecs::entity e(_ecs, b.entity);
    if (!e.is_alive()) {
        return;
    }
    // Scene-prefab transforms can be inherited. Own the local pose before
    // writing simulation results; raw Flecs get_mut only accepts owned data.
    Transform3D* t = in_step && b.owns_transform > 0 ? e.get_mut<Transform3D>() : EcsEntity{e}.get_mut<Transform3D>();
    if (t == nullptr) {
        return;
    }
    if (in_step) {
        b.owns_transform = 1;
        if (b.parented < 0) {
            b.parented = e.parent().is_valid() ? 1 : 0;
        }
    }
    const bool rootless = in_step ? b.parented == 0 : !e.parent().is_valid();
    if (rootless || parent_matrix(e) == Mat4f{}) {
        t->position = position;
        t->rotation = rotation;
    } else {
        // The body's place in the world seen from the parent now.
        const Mat4f local = parent_matrix(e).inverse() * Mat4f::trs(position, rotation, b.scale);
        const Placed p = decompose(local);
        t->position = p.position;
        t->rotation = p.rotation;
    }
    b.last_position = t->position;
    b.last_rotation = t->rotation;
}

void WorldPhysicsState3D::step(f32 dt) {
    if (!(dt > 0.0f)) {
        return;
    }
    _events.clear();
    _character_events.clear();
    _ragdoll_events.clear();
    _rope_events.clear();
    sync_bodies();

    // Entities to bodies.
    for (Bound& b : _bound) {
        flecs::entity e(_ecs, b.entity);
        if (!e.is_alive()) {
            continue;
        }
        const Transform3D* local = b.local ? b.local : e.get<Transform3D>();
        if (b.motion == Body3DMotion::Dynamic) {
            // Moved by game code since we wrote it: take the body there (the world pose now, velocities kept).
            if (local != nullptr && (local->position != b.last_position || local->rotation != b.last_rotation)) {
                const Placed p = decompose(world_matrix(e));
                _physics.set_position(b.body, p.position);
                _physics.set_rotation(b.body, p.rotation);
                // A gameplay teleport starts a new interpolation segment at its destination.
                b.position = b.previous_position = p.position;
                b.rotation = b.previous_rotation = p.rotation;
                b.last_position = local->position;
                b.last_rotation = local->rotation;
            }
        } else {
            Placed p;
            if (b.stamped && local != nullptr && local->position == b.stamp_position && local->rotation == b.stamp_rotation &&
                local->scale == b.stamp_scale) {
                // Not moved since the pose was taken: the same pose.
                p.position = b.last_position;
                p.rotation = b.last_rotation;
            } else {
                p = decompose(world_matrix(e));
                b.stamped = local != nullptr && !e.parent().is_valid();
                if (b.stamped) {
                    b.stamp_position = local->position;
                    b.stamp_rotation = local->rotation;
                    b.stamp_scale = local->scale;
                }
            }
            if (b.motion == Body3DMotion::Kinematic) {
                _physics.follow_transform(b.body, p.position, p.rotation, dt);
            } else if (p.position != b.last_position || p.rotation != b.last_rotation) {
                _physics.follow_transform(b.body, p.position, p.rotation, dt);
            }
            b.last_position = p.position;
            b.last_rotation = p.rotation;
        }
    }

    ragdolls_to_world();
    ropes_to_world();
    characters_to_world(dt);
    _physics.step(dt);

    // Bodies to entities (the awake dynamic ones: a sleeping body has not moved).
    for (Bound& b : _bound) {
        b.previous_position = b.position;
        b.previous_rotation = b.rotation;
        if (b.motion == Body3DMotion::Static)
            continue;
        if (b.motion == Body3DMotion::Kinematic) {
            b.position = _physics.position(b.body);
            b.rotation = _physics.rotation(b.body);
            continue;
        }
        if (!_physics.is_active(b.body)) {
            continue;
        }
        b.position = _physics.position(b.body);
        b.rotation = _physics.rotation(b.body);
        write_back(b, b.position, b.rotation, true);
    }

    // Ragdolls before the characters: a character handed over follows its pelvis, and the entity's Transform3D follows that.
    ragdolls_from_world();
    ropes_from_world();
    characters_from_world();

    // Events: out of the world, in its order. The singletons are written in place, so that their vectors keep their room.
    if (_physics.config().contact_events) {
        _physics.drain_events(_events);
        publish<PhysicsContacts3D>(_ecs, _events);
    }
    _physics.drain_character_events(_character_events);
    _physics.drain_ragdoll_events(_native_ragdoll_events);
    for (const RagdollEvent3D& e : _native_ragdoll_events) {
        _ragdoll_events.push_back(e);
    }
    if (!_ragdolls.empty() || !_ragdoll_events.empty()) {
        publish<RagdollEvents3D>(_ecs, _ragdoll_events);
    }
    _physics.drain_rope_events(_native_rope_events);
    for (const RopeEvent3D& e : _native_rope_events) {
        _rope_events.push_back(e);
    }
    if (!_ropes.empty() || !_rope_events.empty()) {
        publish<RopeEvents3D>(_ecs, _rope_events);
    }
    if (!_characters.empty() || !_character_events.empty()) {
        publish<CharacterEvents3D>(_ecs, _character_events);
    }
}

i32 WorldPhysicsState3D::advance(f32 frame_seconds) {
    if (!(_config.fixed_dt > 0.0f) || !(frame_seconds > 0.0f)) {
        return 0;
    }
    _accumulator += frame_seconds;
    i32 steps = 0;
    auto& all = _advance_events;
    auto& character_all = _advance_character_events;
    all.clear();
    character_all.clear();
    while (_accumulator >= _config.fixed_dt && steps < std::max(1, _config.max_steps)) {
        step(_config.fixed_dt);
        if (steps == 0) {
            // The first step's events are taken, not copied: most advances take one.
            all.swap(_events);
            character_all.swap(_character_events);
        } else {
            all.insert(all.end(), _events.begin(), _events.end());
            character_all.insert(character_all.end(), _character_events.begin(), _character_events.end());
        }
        _accumulator -= _config.fixed_dt;
        ++steps;
    }
    if (steps == std::max(1, _config.max_steps)) {
        _accumulator = std::min(_accumulator, _config.fixed_dt); // drop the backlog
    }
    // One advance's events: all of its steps' batches.
    _events.swap(all);
    _character_events.swap(character_all);
    if (steps > 1) {
        if (_physics.config().contact_events)
            publish<PhysicsContacts3D>(_ecs, _events);
        if (!_characters.empty() || !_character_events.empty()) {
            publish<CharacterEvents3D>(_ecs, _character_events);
        }
    }
    return steps;
}

std::optional<Mat4f> WorldPhysicsState3D::render_transform(flecs::entity entity, f32 alpha) const {
    if (!entity.is_valid() || !std::isfinite(alpha) || entity.world().c_ptr() != _ecs.c_ptr())
        return {};
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    Mat4f relative;
    for (u32 depth = 0; entity.is_valid() && entity.is_alive() && depth < 64; ++depth, entity = entity.parent()) {
        const auto* local = entity.get<Transform3D>();
        if (!local)
            break;
        const auto b = std::ranges::lower_bound(_bound, entity.id(), {}, &Bound::entity);
        if (b != _bound.end() && b->entity == entity.id() && b->motion != Body3DMotion::Static) {
            // A gameplay edit since the last step is a teleport, not a render lerp.
            const auto authoritative = world_matrix(entity);
            const auto simulated = Mat4f::trs(b->position, b->rotation, b->scale);
            for (u32 i = 0; i < 16; ++i)
                if (std::abs(authoritative.m[i] - simulated.m[i]) > 1e-4f)
                    return authoritative * relative;
            return Mat4f::trs(lerp(b->previous_position, b->position, alpha),
                              slerp(b->previous_rotation, b->rotation, alpha), b->scale) *
                   relative;
        }
        const auto c = std::ranges::lower_bound(_characters, entity.id(), {}, &BoundCharacter::entity);
        if (c != _characters.end() && c->entity == entity.id()) {
            auto pose = decompose(world_matrix(entity));
            if (length_squared(pose.position - c->position) < 1e-8f)
                pose.position = lerp(c->previous_position, c->position, alpha);
            return Mat4f::trs(pose.position, pose.rotation, pose.scale) * relative;
        }
        relative = local->matrix() * relative;
    }
    return {};
}

void WorldPhysicsState3D::prepare_presentation(f32 alpha) {
    _presentation_roots.clear();
    _presentation_transforms.clear();
    _presentation_poses.resize(_ragdolls.size());
    alpha = std::clamp(alpha, 0.f, 1.f);
    const auto seed = [&](EcsId id) {
        const flecs::entity e{_ecs, id};
        if (const auto pose = render_transform(e, alpha))
            _presentation_roots.emplace_back(id, *pose);
    };
    for (const auto& b : _bound)
        if (b.motion != Body3DMotion::Static) seed(b.entity);
    for (const auto& c : _characters) seed(c.entity);
    const auto by_id = [](const auto& a, const auto& b) { return a.first < b.first; };
    std::ranges::sort(_presentation_roots, by_id);
    _presentation_transforms.assign(_presentation_roots.begin(), _presentation_roots.end());
    const auto descendants = [&](auto&& self, flecs::entity parent, const Mat4f& place, u32 depth) -> void {
        if (depth >= 64) return;
        parent.children([&](flecs::entity child) {
            const auto root = std::ranges::lower_bound(_presentation_roots, child.id(), {}, [](const auto& e) { return e.first; });
            if (root != _presentation_roots.end() && root->first == child.id()) return;
            if (const auto* local = child.get<Transform3D>()) {
                const auto world = place * local->matrix();
                _presentation_transforms.emplace_back(child.id(), world);
                self(self, child, world, depth + 1);
            }
        });
    };
    for (const auto& [id, pose] : _presentation_roots)
        descendants(descendants, flecs::entity{_ecs, id}, pose, 0);
    std::ranges::sort(_presentation_transforms, by_id);
    for (std::size_t i = 0; i < _ragdolls.size(); ++i) {
        _presentation_poses[i].first = _ragdolls[i].entity;
        fill_pose_override(_ragdolls[i], alpha, _presentation_poses[i].second);
    }
    prepare_hand_presentation(alpha);
}

std::optional<Mat4f> WorldPhysicsState3D::presentation_transform(flecs::entity entity) const {
    const auto found = std::ranges::lower_bound(_presentation_transforms, entity.id(), {}, [](const auto& e) { return e.first; });
    if (found != _presentation_transforms.end() && found->first == entity.id()) return found->second;
    return {};
}
const PoseOverride3D* WorldPhysicsState3D::presentation_pose(flecs::entity entity) const {
    const auto found = std::ranges::lower_bound(_presentation_poses, entity.id(), {}, [](const auto& e) { return e.first; });
    return found != _presentation_poses.end() && found->first == entity.id() ? &found->second : nullptr;
}

void WorldPhysicsState3D::interpolate(f32 alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    for (Bound& b : _bound) {
        if (b.motion != Body3DMotion::Dynamic || !_physics.is_active(b.body)) {
            continue;
        }
        write_back(b, lerp(b.previous_position, b.position, alpha), slerp(b.previous_rotation, b.rotation, alpha));
    }
    for (BoundCharacter& c : _characters) {
        write_character(c, lerp(c.previous_position, c.position, alpha));
    }
    for (BoundRagdoll& r : _ragdolls) {
        write_pose_override(r, alpha);
    }
}

void WorldPhysicsState3D::resync_from_world() {
    // step() writes back only the awake dynamic bodies and interpolate() lerps from the pose before the last step: after a
    // restore_state both are the poses of before the rollback. Everything is taken from the world as it is, asleep or not.
    for (Bound& b : _bound) {
        if (!_physics.contains(b.body)) {
            continue;
        }
        b.position = b.previous_position = _physics.position(b.body);
        b.rotation = b.previous_rotation = _physics.rotation(b.body);
        // A static body is moved by its entity and a restore leaves it where it was. write_back also notes the Transform3D it
        // wrote, so that the next step does not read it as a move by game code.
        if (b.motion != Body3DMotion::Static) {
            write_back(b, b.position, b.rotation);
        }
    }
    for (BoundCharacter& c : _characters) {
        flecs::entity e(_ecs, c.entity);
        if (!e.is_alive() || !_physics.contains(c.character)) {
            continue;
        }
        const CharacterState3D state = _physics.character(c.character);
        c.position = c.previous_position = state.position;
        write_character(c, state.position);
        if (CharacterController3D* controller = e.get_mut<CharacterController3D>()) {
            controller->state = state;
            if (controller->desc.traversal.enabled) {
                controller->traversal_state = _physics.traversal(c.character);
                if (const auto* link = e.get<CharacterAnimationLink3D>()) {
                    if (auto* animator = e.get_mut<Animator3D>()) {
                        // Refresh bridge-owned targets only. A manual target survives rollback.
                        for (u32 h = 0; h < c.hands.size(); ++h) {
                            auto& history = c.hands[h];
                            auto* hand = animator->find_ik(history.constraint);
                            if (history.active && hand && h < link->hands.size() && link->hands[h] == history.constraint &&
                                hand->target.kind == IKTarget3D::Kind::World && hand->target.position == history.target.position &&
                                hand->target_rotation == history.target.rotation)
                                drive_traversal_hand(*hand, controller->traversal_state, *link, h);
                            history.active = false;
                        }
                        sample_hands(c, controller->traversal_state, *link, *animator);
                    }
                }
            }
        }
    }
    for (BoundRagdoll& r : _ragdolls) {
        flecs::entity e(_ecs, r.entity);
        if (!e.is_alive() || !_physics.contains(r.ragdoll)) {
            continue;
        }
        _physics.ragdoll_pose(r.ragdoll, r.current);
        r.previous = r.current;
        write_pose_override(r, 1.0f);
        if (Ragdoll3D* component = e.get_mut<Ragdoll3D>()) {
            component->state = _physics.ragdoll_state(r.ragdoll);
        }
    }
    for (auto& r : _ropes) r.sampled = false;
    ropes_from_world();
}

Status3D WorldPhysicsState3D::replace_bodies(std::span<const BodyEdit> edits, const std::function<void()>& commit) {
    std::string why;
    const bool success = replace_bodies_impl(edits, commit, &why);
    if (success) return {};
    return {false, {"replace_bodies: " + why}};
}

bool WorldPhysicsState3D::replace_bodies_impl(std::span<const BodyEdit> edits,
                                        const std::function<void()>& commit, std::string* error) {
    std::vector<Bound> replacements;
    replacements.reserve(edits.size());
    const auto cleanup = [&] { for (const auto& b : replacements) _physics.destroy_body(b.body); };
    const auto fail = [&](const char* why) { cleanup(); if (error) *error = why; return false; };
    try {
        for (const auto& edit : edits) {
            if (edit.target.world().c_ptr() != _ecs.c_ptr() || !edit.target.is_alive() || !edit.source.is_alive() ||
                edit.target.has<CharacterController3D>() || edit.target.has<Ragdoll3D>() || edit.target.has<Rope3D>() ||
                !edit.source.has<Transform3D>() || !edit.source.has<Collider3D>())
                return fail("live body edit requires an ordinary rigid collider");
            const auto old = std::ranges::lower_bound(_bound, edit.target.id(), {}, &Bound::entity);
            if (old == _bound.end() || old->entity != edit.target.id() || !_physics.contains(old->body))
                return fail("live body edit requires an existing body");
            if (std::ranges::any_of(replacements, [&](const auto& b) { return b.entity == edit.target.id(); }))
                return fail("duplicate live body edit");
            for (const auto c : _physics.constraints()) {
                const auto info = _physics.constraint_info(c);
                if (info.a == old->body || info.b == old->body)
                    return fail("constrained body edits require scene reload");
            }
            Bound replacement;
            create_body(edit.source, replacement, edit.target.id());
            if (!replacement.body.valid()) return fail("replacement body refused (invalid settings or no spare body capacity)");
            replacements.push_back(replacement);
            auto& b = replacements.back();
            if (edit.keep_pose && b.motion == old->motion) {
                _physics.set_position(b.body, _physics.position(old->body));
                _physics.set_rotation(b.body, _physics.rotation(old->body));
                b.position = old->position; b.previous_position = old->previous_position;
                b.rotation = old->rotation; b.previous_rotation = old->previous_rotation;
            }
            if (b.motion == old->motion && b.motion == Body3DMotion::Dynamic) {
                if (edit.keep_linear_velocity) _physics.set_linear_velocity(b.body, _physics.linear_velocity(old->body));
                if (edit.keep_angular_velocity) _physics.set_angular_velocity(b.body, _physics.angular_velocity(old->body));
                if (edit.keep_pose && edit.keep_linear_velocity && edit.keep_angular_velocity && !_physics.is_active(old->body))
                    _physics.deactivate(b.body);
            }
        }
        commit();
        for (const auto& b : replacements) {
            auto old = std::ranges::lower_bound(_bound, b.entity, {}, &Bound::entity);
            _physics.destroy_body(old->body);
            *old = b;
            _ecs.entity(b.entity).set<PhysicsBody3D>({b.body});
        }
        _body_components_dirty = true;
        if (error) error->clear();
        return true;
    } catch (...) { cleanup(); throw; }
}

void WorldPhysicsState3D::rebuild(flecs::entity entity) {
    _force_sync = true;
    std::erase_if(_refused, [&](const Refused& r) { return r.entity == entity.id(); });
    const auto found = std::ranges::lower_bound(_bound, entity.id(), {}, &Bound::entity);
    if (found != _bound.end() && found->entity == entity.id()) {
        found->rebuild = true;
    }
    const auto character = std::ranges::lower_bound(_characters, entity.id(), {}, &BoundCharacter::entity);
    if (character != _characters.end() && character->entity == entity.id()) {
        character->rebuild = true;
    }
    if (BoundRagdoll* r = bound_ragdoll(entity)) {
        r->rebuild = true;
    }
    const auto rope = std::ranges::lower_bound(_ropes, entity.id(), {}, &BoundRope::entity);
    if (rope != _ropes.end() && rope->entity == entity.id()) {
        rope->rebuild = true;
    }
}

flecs::entity WorldPhysicsState3D::entity_of(u64 user_id) const {
    const flecs::entity e(_ecs, static_cast<flecs::entity_t>(user_id));
    return e.is_alive() ? e : flecs::entity{};
}

flecs::entity WorldPhysicsState3D::entity_of_body(Body3D body)const {
    if(!_physics.contains(body))return {};
    const auto entity=entity_of(_physics.user_id(body));if(!entity)return {};
    if(body_of(entity)==body)return entity;
    const auto character=character_of(entity);
    if(character&&_physics.character(character).body==body)return entity;
    const auto ragdoll=ragdoll_of(entity);
    if(ragdoll&&_physics.ragdoll_has_body(ragdoll,body))return entity;
    if(const auto segment=_physics.rope_segment_of(body);segment&&segment->rope==rope_of(entity))return entity;
    return {};
}

Body3D WorldPhysicsState3D::body_of(flecs::entity entity) const {
    const auto found = std::ranges::lower_bound(_bound, entity.id(), {}, &Bound::entity);
    return found != _bound.end() && found->entity == entity.id() ? found->body : Body3D{};
}

void WorldPhysicsState3D::collect_debug(Renderer3D& renderer, Scene3D& scene) {
    if (!_config.debug_draw) {
        return;
    }
    // The caller's own lines (rays, sweeps) were added since the last call; the world's go in after them.
    DebugDraw3D& d = _debug;
    d.options() = _config.debug;
    d.world(_physics);
    if (_config.debug_contacts) {
        d.contacts(_events);
    }
    if (_config.debug_grips) {
        for (const BoundCharacter& c : _characters) {
            d.grips(_physics, c.character);
        }
    }
    _debug_mesh = draw_debug_lines(renderer, scene, d.lines());
    d.clear();
}

// --- ragdolls ----------------------------------------------------------------------------------------------------------------

WorldPhysicsState3D::BoundRagdoll* WorldPhysicsState3D::bound_ragdoll(flecs::entity entity) {
    const auto found = std::ranges::lower_bound(_ragdolls, entity.id(), {}, &BoundRagdoll::entity);
    return found != _ragdolls.end() && found->entity == entity.id() ? &*found : nullptr;
}

const WorldPhysicsState3D::BoundRagdoll* WorldPhysicsState3D::bound_ragdoll(flecs::entity entity) const {
    return const_cast<WorldPhysicsState3D*>(this)->bound_ragdoll(entity);
}

RagdollHandle3D WorldPhysicsState3D::ragdoll_of(flecs::entity entity) const {
    const BoundRagdoll* r = bound_ragdoll(entity);
    return r != nullptr ? r->ragdoll : RagdollHandle3D{};
}

// Ragdolls are the entities of a query that also asks for a model with a skeleton, which a component write can change without
// any entity moving: they are read again at every sync unless there is no entity that could have one.
bool WorldPhysicsState3D::ragdolls_stand() {
    return !_selective_sync && !_force_sync && _ragdolls.empty() && !has_refused(Made::Ragdoll) && !_ragdoll_query.is_true();
}

void WorldPhysicsState3D::sync_ragdolls() {
    if (ragdolls_stand()) {
        return;
    }
    std::vector<flecs::entity>& wanted = _wanted;
    wanted.clear();
    const auto consider=[&](flecs::entity e,const SkinnedMeshRenderer3D& renderer) {
        if (renderer.model && !renderer.model->skeletons.empty()) {
            wanted.push_back(e);
        }
    };
    if(_selective_sync)for(auto id:_sync_entities) {
        flecs::entity e{_ecs,id};
        if(e.is_alive()&&e.enabled()&&!e.has(flecs::Prefab)&&e.has<Transform3D>()&&e.has<Ragdoll3D>())
            if(const auto* renderer=e.get<SkinnedMeshRenderer3D>())consider(e,*renderer);
    } else _ragdoll_query.each([&](flecs::entity e,const Transform3D&,const SkinnedMeshRenderer3D& renderer,const Ragdoll3D&){consider(e,renderer);});
    sort_by_id(wanted);
    prune_refused(Made::Ragdoll, wanted);

    auto [to_create, w] = reconcile_entities<true>(
        _ragdolls, wanted, [&](EcsId id) { return sync_selected(id); },
        [&](const BoundRagdoll& r, flecs::entity) { return r.rebuild || !_physics.contains(r.ragdoll); },
        [&](const BoundRagdoll& r) { _physics.destroy_ragdoll(r.ragdoll); });
    for (const flecs::entity e : to_create) {
        const Ragdoll3D& component = *e.get<Ragdoll3D>();
        const Model3D& model = *e.get<SkinnedMeshRenderer3D>()->model;
        const Skeleton3D& skeleton = model.skeletons[0];
        const Mat4f entity_matrix = world_matrix(e);
        const Placed placed = decompose(entity_matrix);
        if (refused_before(e.id(), Made::Ragdoll, _refused.empty() ? 0 : ragdoll_stamp(component, model, placed))) {
            continue;
        }
        const Pose3D rest = skeleton.rest_pose();
        RagdollDesc desc = component.desc;
        if (desc.bones.empty()) {
            desc = auto_ragdoll_desc(skeleton, rest, component.auto_options);
        }
        desc.user_id = e.id();
        std::vector<Mat4f> model_world;
        compute_world_matrices(skeleton, rest, model_world);
        std::vector<Mat4f> bone_world(desc.bones.size());
        BoundRagdoll b;
        b.entity = e.id();
        b.joints = skeleton.joint_count();
        b.joint.resize(desc.bones.size(), -1);
        bool ok = !desc.bones.empty();
        for (std::size_t k = 0; k < desc.bones.size() && ok; ++k) {
            const i32 joint = desc.bones[k].joint;
            if (joint < 0 || static_cast<u32>(joint) >= skeleton.joint_count()) {
                ok = false;
                break;
            }
            b.joint[k] = joint;
            bone_world[k] = entity_matrix * model_world[static_cast<std::size_t>(joint)];
        }
        if (ok) {
            const f32 scale = std::max({placed.scale.x, placed.scale.y, placed.scale.z});
            b.ragdoll = _physics.create_ragdoll(desc, bone_world, scale);
        }
        if (!b.ragdoll.valid()) {
            KIN_LOG_WARN_F("physics3d", "entity has no valid ragdoll (skeleton or desc refused)",
                           (LogFields{{.name = "entity", .value = std::to_string(e.id())}}));
            refuse(e.id(), Made::Ragdoll, ragdoll_stamp(component, model, placed));
        } else {
            forgive(e.id(), Made::Ragdoll);
            // A character on the same entity does not meet its own bones: its movement and queries skip the ragdoll's group.
            if (const PhysicsCharacter3D* pc = e.get<PhysicsCharacter3D>(); pc != nullptr && _physics.contains(pc->character)) {
                const std::vector<Body3D> bodies = _physics.ragdoll_bodies(b.ragdoll);
                if (!bodies.empty() && bodies[0].valid()) {
                    _physics.set_group(_physics.character(pc->character).body, _physics.group(bodies[0]));
                }
            }
            // The pose is written into the animator: an entity with a ragdoll has one.
            if (e.get<Animator3D>() == nullptr) {
                Animator3D made;
                e.set<Animator3D>(std::move(made));
            }
            b.previous.resize(desc.bones.size());
            b.current.resize(desc.bones.size());
            _physics.ragdoll_pose(b.ragdoll, b.current);
            b.previous = b.current;
        }
        if (b.ragdoll.valid()) {
            _ragdolls.push_back(std::move(b));
        }
    }
    merge_created_entities(_ragdolls, w);
    for (const flecs::entity e : to_create) {
        if (const BoundRagdoll* r = bound_ragdoll(e)) {
            e.set<PhysicsRagdoll3D>({r->ragdoll});
        }
    }
}

void WorldPhysicsState3D::ragdolls_to_world() {
    for (BoundRagdoll& r : _ragdolls) {
        flecs::entity e(_ecs, r.entity);
        if (!e.is_alive() || !_physics.contains(r.ragdoll)) {
            continue;
        }
        const SkinnedMeshRenderer3D* renderer = e.get<SkinnedMeshRenderer3D>();
        if (renderer == nullptr || !renderer->model || renderer->model->skeletons.empty()) {
            continue;
        }
        const Skeleton3D& skeleton = renderer->model->skeletons[0];
        const Animator3D* animator = e.get<Animator3D>();
        // Where the clip puts the bones now, in the world: what the kinematic bodies follow. (The clip's pose: constraints
        // are solved at draw time and are the renderer's.)
        const bool bound = animator != nullptr && animator->bound_to(renderer->model) && animator->player.skeleton() == &skeleton;
        if (bound) {
            compute_world_matrices(skeleton, animator->player.pose(), _pose_scratch);
        } else {
            _rest_scratch.joints = skeleton.rest;
            compute_world_matrices(skeleton, _rest_scratch, _pose_scratch);
        }
        const Mat4f entity_matrix = world_matrix(e);
        std::vector<Mat4f> bone_world(r.joint.size());
        for (std::size_t k = 0; k < r.joint.size(); ++k) {
            bone_world[k] = entity_matrix * _pose_scratch[static_cast<std::size_t>(r.joint[k])];
        }
        _physics.set_ragdoll_targets(r.ragdoll, bone_world);
    }
}

void WorldPhysicsState3D::write_pose_override(BoundRagdoll& r, f32 alpha) {
    flecs::entity e(_ecs, r.entity);
    if (!e.is_alive()) {
        return;
    }
    Animator3D* animator = e.get_mut<Animator3D>();
    if (animator == nullptr) {
        return;
    }
    PoseOverride3D& o = animator->pose_override;
    fill_pose_override(r, alpha, o);
}

void WorldPhysicsState3D::fill_pose_override(const BoundRagdoll& r, f32 alpha, PoseOverride3D& o) const {
    bool any = false;
    for (const RagdollBonePose3D& b : r.current) {
        any = any || b.weight > 0.0f;
    }
    if (alpha < 1)
        for (const auto& before : r.previous) any |= before.weight > 0;
    if (!any) {
        // All clip: the animator is exactly what it is without a ragdoll.
        if (!o.empty()) {
            o.clear();
        }
        return;
    }
    o.weights.assign(r.joints, 0.0f);
    o.position.assign(r.joints, Vec3f{});
    o.rotation.assign(r.joints, Quatf{});
    for (std::size_t k = 0; k < r.joint.size(); ++k) {
        const std::size_t j = static_cast<std::size_t>(r.joint[k]);
        const RagdollBonePose3D& now = r.current[k];
        const RagdollBonePose3D& before = r.previous[k];
        o.weights[j] = alpha >= 1 ? now.weight : before.weight + (now.weight - before.weight) * alpha;
        o.position[j] = alpha >= 1.0f ? now.position : lerp(before.position, now.position, alpha);
        o.rotation[j] = alpha >= 1.0f ? now.rotation : slerp(before.rotation, now.rotation, alpha);
    }
}

void WorldPhysicsState3D::ragdolls_from_world() {
    for (BoundRagdoll& r : _ragdolls) {
        flecs::entity e(_ecs, r.entity);
        if (!e.is_alive() || !_physics.contains(r.ragdoll)) {
            continue;
        }
        r.previous = r.current;
        _physics.ragdoll_pose(r.ragdoll, r.current);
        write_pose_override(r, 1.0f);
        if (Ragdoll3D* component = e.get_mut<Ragdoll3D>()) {
            component->state = _physics.ragdoll_state(r.ragdoll);
        }
        // A character handed over follows the pelvis's ground point.
        if (r.handed_over) {
            const PhysicsCharacter3D* pc = e.get<PhysicsCharacter3D>();
            if (pc != nullptr && _physics.contains(pc->character) && !_physics.character_enabled(pc->character)) {
                const CharacterState3D state = _physics.character(pc->character);
                const RagdollPlace3D place = _physics.ragdoll_get_up_place(r.ragdoll, state.up, all_layers3d, 4.0f, state.body);
                if (place.found) {
                    _physics.teleport_character(pc->character, place.position);
                }
            }
        }
    }
}

bool WorldPhysicsState3D::start_ragdoll(flecs::entity entity, const RagdollStart3D& start) {
    const BoundRagdoll* r = bound_ragdoll(entity);
    if (r == nullptr) {
        return false;
    }
    _physics.start_ragdoll(r->ragdoll, start);
    return true;
}

bool WorldPhysicsState3D::blend_ragdoll(flecs::entity entity, f32 alpha, f32 seconds) {
    const BoundRagdoll* r = bound_ragdoll(entity);
    if (r == nullptr) {
        return false;
    }
    _physics.blend_ragdoll(r->ragdoll, alpha, seconds);
    return true;
}

bool WorldPhysicsState3D::ragdoll_get_up(flecs::entity entity, f32 seconds) {
    const BoundRagdoll* r = bound_ragdoll(entity);
    if (r == nullptr) {
        return false;
    }
    _physics.ragdoll_get_up(r->ragdoll, seconds);
    return true;
}

bool WorldPhysicsState3D::animate_ragdoll(flecs::entity entity) {
    BoundRagdoll* r = bound_ragdoll(entity);
    if (r == nullptr) {
        return false;
    }
    _physics.animate_ragdoll(r->ragdoll);
    r->handed_over = false;
    return true;
}

bool WorldPhysicsState3D::set_ragdoll_subtree(flecs::entity entity, std::string_view joint_name, f32 weight) {
    const BoundRagdoll* r = bound_ragdoll(entity);
    const SkinnedMeshRenderer3D* renderer = entity.get<SkinnedMeshRenderer3D>();
    if (r == nullptr || renderer == nullptr || !renderer->model || renderer->model->skeletons.empty()) {
        return false;
    }
    const i32 joint = renderer->model->skeletons[0].find(joint_name);
    for (std::size_t k = 0; k < r->joint.size(); ++k) {
        if (r->joint[k] == joint) {
            _physics.set_ragdoll_subtree(r->ragdoll, static_cast<i32>(k), weight);
            return true;
        }
    }
    return false;
}

bool WorldPhysicsState3D::hand_over_to_ragdoll(flecs::entity entity, const RagdollHandOver3D& options) {
    BoundRagdoll* r = bound_ragdoll(entity);
    const Character3D c = character_of(entity);
    if (r == nullptr) {
        return false;
    }
    RagdollStart3D start;
    start.blend_seconds = options.blend_seconds;
    start.impulse_bone = options.impulse_bone;
    start.impulse = options.impulse;
    start.impulse_point = options.impulse_point;
    start.point_given = options.point_given;
    if (c.valid() && _physics.contains(c)) {
        if (options.inherit_velocity) {
            start.set_velocity = true;
            start.velocity = _physics.character(c).velocity;
        }
        // The capsule gives its place up (it follows the pelvis's ground point from now on).
        _physics.set_character_enabled(c, false);
        r->handed_over = true;
    }
    _physics.start_ragdoll(r->ragdoll, start);
    return true;
}

RagdollRecoverResult3D WorldPhysicsState3D::recover_from_ragdoll(flecs::entity entity, const RagdollRecover3D& options) {
    RagdollRecoverResult3D out;
    BoundRagdoll* r = bound_ragdoll(entity);
    const Character3D c = character_of(entity);
    if (r == nullptr) {
        return out;
    }
    const auto blocked = [&] {
        out.blocked = true;
        RagdollEvent3D e;
        e.ragdoll = r->ragdoll;
        e.user_id = entity.id();
        e.kind = RagdollEventKind3D::Blocked;
        _ragdoll_events.push_back(e);
        return out;
    };
    Vec3f up{0.0f, 1.0f, 0.0f};
    f32 radius = 0.3f, height = 1.8f;
    if (c.valid() && _physics.contains(c)) {
        const CharacterState3D state = _physics.character(c);
        up = state.up;
        const CharacterController3D* controller = entity.get<CharacterController3D>();
        if (controller != nullptr) {
            radius = controller->desc.radius;
            height = controller->desc.height;
        }
    } else if (length(_physics.gravity()) > 0.0f) {
        up = normalize(-_physics.gravity());
    }
    // A disabled controller still participates in queries. It cannot be its own ground.
    const Body3D capsule_body = c.valid() && _physics.contains(c) ? _physics.character(c).body : Body3D{};
    out.place = _physics.ragdoll_get_up_place(r->ragdoll, up, options.mask, options.max_ground_distance, capsule_body);
    if (!out.place.found) {
        return blocked();
    }
    // The standing capsule over the place, against what is not the ragdoll's own (nor the character's own capsule).
    const std::vector<Body3D> own = _physics.ragdoll_bodies(r->ragdoll);
    QueryFilter3D filter;
    filter.mask = options.mask;
    if (c.valid() && _physics.contains(c)) {
        filter.ignore = _physics.character(c).body;
    }
    const Shape3D capsule = Shape3D::capsule(std::max(0.5f * (height - 2.0f * radius), 0.01f), radius);
    // A capsule's own axis is +Y: turn it to `up`.
    const Quatf turn = up.y < -0.999999f ? Quatf{1.0f, 0.0f, 0.0f, 0.0f} : Quatf{up.z, 0.0f, -up.x, 1.0f + up.y}.normalized();
    const std::vector<OverlapHit3D> overlaps =
        _physics.overlap(capsule, out.place.position + up * (0.5f * height + 0.01f), turn, filter);
    for (const OverlapHit3D& o : overlaps) {
        if (std::find(own.begin(), own.end(), o.body) != own.end()) {
            continue;
        }
        if (o.penetration > options.allowed_penetration) {
            return blocked();
        }
    }
    if (c.valid() && _physics.contains(c)) {
        _physics.teleport_character(c, out.place.position);
        _physics.set_character_enabled(c, true);
        if (CharacterController3D* controller = entity.get_mut<CharacterController3D>()) {
            controller->facing = out.place.rotation;
        }
    }
    r->handed_over = false;
    _physics.ragdoll_get_up(r->ragdoll, options.get_up_seconds);
    out.recovered = true;
    return out;
}

// --- ropes -------------------------------------------------------------------------------------------------------------------

RopeHandle3D WorldPhysicsState3D::rope_of(flecs::entity entity) const {
    const auto found = std::ranges::lower_bound(_ropes, entity.id(), {}, &BoundRope::entity);
    return found != _ropes.end() && found->entity == entity.id() ? found->rope : RopeHandle3D{};
}

std::optional<RopeClosest3D> Rope3D::closest_point(Vec3f point) const {
    if (sampled.size() < 2) {
        return std::nullopt;
    }
    RopeClosest3D best;
    f32 best_d2 = 3.0e38f;
    const u32 n = static_cast<u32>(sampled.size() - 1);
    for (u32 k = 0; k < n; ++k) {
        const Vec3f a = sampled[k].position, b = sampled[k + 1].position;
        const Vec3f ab = b - a;
        const f32 len2 = dot(ab, ab);
        const f32 t = len2 > 1.0e-12f ? std::clamp(dot(point - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        const Vec3f q = a + ab * t;
        const f32 d2 = length_squared(point - q);
        if (d2 < best_d2) {
            best_d2 = d2;
            best.segment = k;
            best.t = t;
            best.point = q;
            best.tangent = len2 > 1.0e-12f ? ab / std::sqrt(len2) : Vec3f{0.0f, -1.0f, 0.0f};
        }
    }
    best.distance = std::sqrt(best_d2);
    best.along = (static_cast<f32>(best.segment) + best.t) / static_cast<f32>(n);
    return best;
}

// As bodies_stand, for ropes; one that waits for the body of an entity it is fixed to is tried again at every sync.
bool WorldPhysicsState3D::ropes_stand() {
    const bool moved = _rope_query.changed();
    if (_selective_sync || _force_sync || moved || _ropes_waiting || has_refused(Made::Rope)) {
        return false;
    }
    return std::ranges::none_of(_ropes, [&](const BoundRope& r) { return r.rebuild || !_physics.contains(r.rope); });
}

void WorldPhysicsState3D::sync_ropes() {
    if (ropes_stand()) {
        return;
    }
    _ropes_waiting = false;
    std::vector<flecs::entity>& wanted = _wanted;
    wanted.clear();
    if(_selective_sync)for(auto id:_sync_entities) {
        flecs::entity e{_ecs,id};
        if(e.is_alive()&&e.enabled()&&!e.has(flecs::Prefab)&&e.has<Rope3D>())wanted.push_back(e);
    } else _rope_query.each([&](flecs::entity e) { wanted.push_back(e); });
    sort_by_id(wanted);
    prune_refused(Made::Rope, wanted);

    auto [to_create, w] = reconcile_entities(
        _ropes, wanted, [&](EcsId id) { return sync_selected(id); },
        [&](const BoundRope& r, flecs::entity) { return r.rebuild || !_physics.contains(r.rope); },
        [&](const BoundRope& r) { _physics.destroy_rope(r.rope); });
    for (const flecs::entity e : to_create) {
        const Rope3D& c = *e.get<Rope3D>();
        const Mat4f entity_matrix = world_matrix(e);
        const auto anchor = [&](const RopeAttach3D& a, RopeAnchor3D& out) {
            switch (a.kind) {
            case RopeAttach3D::Kind::Free:
                out = RopeAnchor3D::free(entity_matrix.apply(a.point));
                return true;
            case RopeAttach3D::Kind::World:
                out = RopeAnchor3D::at(a.point);
                return true;
            case RopeAttach3D::Kind::Entity: {
                const flecs::entity target(_ecs, a.entity);
                const Body3D body = target.is_alive() ? body_of(target) : Body3D{};
                if (!body.valid()) {
                    return false; // not made yet: tried again at the next sync
                }
                out = RopeAnchor3D::on(body, a.point);
                return true;
            }
            }
            return false;
        };
        RopeDesc3D desc;
        if (!anchor(c.start, desc.start) || !anchor(c.end, desc.end)) {
            _ropes_waiting = true;
            continue;
        }
        const auto anchored = [&](const RopeAttach3D& a) {
            return a.kind == RopeAttach3D::Kind::Entity ? body_of(flecs::entity(_ecs, a.entity)) : Body3D{};
        };
        const u64 stamp = rope_stamp(c, decompose(entity_matrix), anchored(c.start), anchored(c.end));
        if (refused_before(e.id(), Made::Rope, stamp)) {
            continue;
        }
        desc.length = c.length;
        desc.segments = c.segments;
        desc.segment_length = c.segment_length;
        desc.radius = c.radius;
        desc.mass_per_metre = c.mass_per_metre;
        desc.friction = c.friction;
        desc.linear_damping = c.linear_damping;
        desc.angular_damping = c.angular_damping;
        desc.sag_direction = c.sag_direction;
        desc.hang_direction = c.hang_direction;
        desc.layer = c.layer;
        desc.collide_world = c.collide_world;
        desc.self_collision = c.self_collision;
        desc.continuous = c.continuous;
        desc.velocity_steps = c.velocity_steps;
        desc.position_steps = c.position_steps;
        desc.break_tension = c.break_tension;
        desc.user_id = e.id();
        BoundRope b;
        b.entity = e.id();
        b.rope = _physics.create_rope(desc);
        if (!b.rope.valid()) {
            KIN_LOG_WARN_F("physics3d", "entity has no valid rope (anchors, length or descriptor refused)",
                           (LogFields{{.name = "entity", .value = std::to_string(e.id())}}));
            refuse(e.id(), Made::Rope, stamp);
            continue;
        }
        forgive(e.id(), Made::Rope);
        _ropes.push_back(b);
    }
    merge_created_entities(_ropes, w);
    for (const flecs::entity e : to_create) {
        const RopeHandle3D h = rope_of(e);
        if (h.valid()) {
            e.set<PhysicsRope3D>({h});
        }
    }
}

void WorldPhysicsState3D::ropes_to_world() {
    for (BoundRope& r : _ropes) {
        flecs::entity e(_ecs, r.entity);
        if (!e.is_alive() || !_physics.contains(r.rope)) {
            continue;
        }
        const Rope3D* c = e.get<Rope3D>();
        if (c == nullptr) {
            continue;
        }
        if (c->target_length >= 0.0f && (c->target_length != r.applied_target || c->reel_speed != r.applied_speed)) {
            _physics.reel_rope(r.rope, c->target_length, c->reel_speed);
            r.applied_target = c->target_length;
            r.applied_speed = c->reel_speed;
        }
    }
}

void WorldPhysicsState3D::ropes_from_world() {
    for (BoundRope& r : _ropes) {
        flecs::entity e(_ecs, r.entity);
        if (!e.is_alive() || !_physics.contains(r.rope)) {
            continue;
        }
        Rope3D* c = e.get_mut<Rope3D>();
        if (c == nullptr) {
            continue;
        }
        c->state = _physics.rope_state(r.rope);
        if (r.sampled) r.previous = c->sampled;
        else r.previous.clear();
        r.sampled = true;
        c->sampled = _physics.rope_points(r.rope);
        c->tension_start = _physics.rope_tension(r.rope, RopeEnd3D::Start);
        c->tension_end = _physics.rope_tension(r.rope, RopeEnd3D::End);
    }
}

void WorldPhysicsState3D::collect_ropes(Renderer3D& renderer, Scene3D& scene, f32 alpha) {
    alpha = std::clamp(alpha, 0.f, 1.f);
    _rope_meshes.clear();
    struct Group {
        Material3D material;
        Color tint;
        MeshData3D data;
    };
    std::vector<Group> groups;
    for (const BoundRope& r : _ropes) {
        flecs::entity e(_ecs, r.entity);
        if (!e.is_alive()) {
            continue;
        }
        const Rope3D* c = e.get<Rope3D>();
        if (c == nullptr || !c->render || c->sampled.size() < 2) {
            continue;
        }
        Group* group = nullptr;
        for (Group& g : groups) {
            if (g.material == c->material && g.tint == c->tint) {
                group = &g;
                break;
            }
        }
        if (group == nullptr) {
            groups.push_back({c->material, c->tint, {}});
            group = &groups.back();
        }
        MeshData3D& data = group->data;
        const u32 sides = std::clamp<u32>(c->sides, 3u, 16u);
        const f32 radius = c->render_radius > 0.0f ? c->render_radius : c->radius;
        const std::vector<RopePoint3D>* points = &c->sampled;
        if (alpha < 1 && r.previous.size() == c->sampled.size()) {
            _rope_render_points = c->sampled;
            for (std::size_t i = 0; i < c->sampled.size(); ++i) {
                _rope_render_points[i].position = lerp(r.previous[i].position, c->sampled[i].position, alpha);
                const auto tangent = lerp(r.previous[i].tangent, c->sampled[i].tangent, alpha);
                _rope_render_points[i].tangent = length_squared(tangent) > 0 ? normalize(tangent) : c->sampled[i].tangent;
            }
            points = &_rope_render_points;
        }
        const std::vector<RopePoint3D>& pts = *points;
        // Rings along the points, the frame carried from one to the next (no twist about the rope).
        const u32 first = static_cast<u32>(data.vertices.size());
        Vec3f normal{};
        f32 along = 0.0f;
        for (std::size_t k = 0; k < pts.size(); ++k) {
            const Vec3f t = pts[k].tangent;
            if (k == 0) {
                normal = cross(t, std::fabs(t.y) < 0.9f ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f});
            } else {
                normal = normal - t * dot(normal, t);
                along += length(pts[k].position - pts[k - 1].position);
            }
            normal = length(normal) > 1.0e-6f ? normalize(normal) : Vec3f{1.0f, 0.0f, 0.0f};
            const Vec3f binormal = cross(t, normal);
            for (u32 s = 0; s < sides; ++s) {
                const f32 a = 6.2831853f * static_cast<f32>(s) / static_cast<f32>(sides);
                const Vec3f out = normal * std::cos(a) + binormal * std::sin(a);
                MeshVertex3D v;
                v.position = pts[k].position + out * radius;
                v.normal = out;
                v.uv = {static_cast<f32>(s) / static_cast<f32>(sides), along / std::max(6.2831853f * radius, 1.0e-3f)};
                v.color = group->tint;
                data.vertices.push_back(v);
            }
        }
        // Winding from the first quad: whichever way makes the face normal agree with the vertex normal.
        const auto idx = [&](std::size_t ring, u32 s) { return first + static_cast<u32>(ring) * sides + (s % sides); };
        bool flip = false;
        {
            const Vec3f p0 = data.vertices[idx(0, 0)].position, p1 = data.vertices[idx(1, 0)].position,
                        p2 = data.vertices[idx(1, 1)].position;
            flip = dot(cross(p1 - p0, p2 - p0), data.vertices[idx(0, 0)].normal) < 0.0f;
        }
        for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
            for (u32 s = 0; s < sides; ++s) {
                const u32 a = idx(k, s), b = idx(k + 1, s), c2 = idx(k + 1, s + 1), d = idx(k, s + 1);
                if (!flip) {
                    data.indices.insert(data.indices.end(), {a, b, c2, a, c2, d});
                } else {
                    data.indices.insert(data.indices.end(), {a, c2, b, a, d, c2});
                }
            }
        }
    }
    for (Group& g : groups) {
        if (g.data.vertices.empty()) {
            continue;
        }
        Mesh3D mesh = renderer.create_mesh(g.data).value_or({});
        if (mesh.valid()) {
            (void)scene.draw(mesh, Mat4f{}, g.material);
            _rope_meshes.push_back(std::move(mesh));
        }
    }
}

Animator3D locomotion_animator(const LocomotionClips3D& clips) {
    Animator3D a;
    auto machine = std::make_shared<AnimationStateMachine>();
    machine->initial = "ground";
    machine->states = {{"ground", ""}, {"air", clips.fall}};
    AnimTransition up;
    up.from = "ground";
    up.to = "air";
    up.when = {{clips.airborne_param, CondOp::IsTrue, 0.0f}};
    AnimTransition down;
    down.from = "air";
    down.to = "ground";
    down.when = {{clips.airborne_param, CondOp::IsFalse, 0.0f}};
    machine->transitions = {up, down};
    if (!clips.hang.empty()) {
        // Hanging by the hands: from the ground or the air while the parameter is true (listed first: they win), and back to
        // whichever the character is in when it is not.
        machine->states["hang"] = clips.hang;
        AnimTransition from_ground, from_air, to_air, to_ground;
        from_ground.from = "ground";
        from_ground.to = "hang";
        from_ground.when = {{clips.hanging_param, CondOp::IsTrue, 0.0f}};
        from_air = from_ground;
        from_air.from = "air";
        to_air.from = "hang";
        to_air.to = "air";
        to_air.when = {{clips.hanging_param, CondOp::IsFalse, 0.0f}, {clips.airborne_param, CondOp::IsTrue, 0.0f}};
        to_ground = to_air;
        to_ground.to = "ground";
        to_ground.when = {{clips.hanging_param, CondOp::IsFalse, 0.0f}, {clips.airborne_param, CondOp::IsFalse, 0.0f}};
        machine->transitions = {from_ground, from_air, to_air, to_ground, up, down};
    }
    a.machine = machine;
    Animator3D::BlendSpace space;
    space.parameter = clips.speed_param;
    if (!clips.idle.empty()) {
        space.entries.push_back({clips.idle, 0.0f});
    }
    if (!clips.walk.empty()) {
        space.entries.push_back({clips.walk, clips.walk_speed});
    }
    if (!clips.run.empty()) {
        space.entries.push_back({clips.run, clips.run_speed});
    }
    a.blend_spaces["ground"] = space;
    a.fade_seconds = clips.fade_seconds;
    a.remove_root_motion = true;
    return a;
}

void drive_character_animator(Animator3D& animator, const CharacterState3D& state, const World3D& world,
                              const CharacterAnimationLink3D& link) {
    const bool on_ground = state.ground == CharacterGround3D::OnGround;
    if (!link.speed.empty()) {
        animator.params.set_float(link.speed, on_ground ? state.ground_speed : length(Vec3f{state.velocity.x, 0.0f, state.velocity.z}));
    }
    if (!link.airborne.empty()) {
        animator.params.set_bool(link.airborne, !on_ground);
    }
    if (!link.crouching.empty()) {
        animator.params.set_bool(link.crouching, state.crouching);
    }
    if (!link.vertical.empty()) {
        animator.params.set_float(link.vertical, state.vertical_speed);
    }
    if (!link.hanging.empty()) {
        animator.params.set_bool(link.hanging, state.traversal_mode != TraversalMode3D::Walking);
    }
    if (!link.feet.empty()) {
        if (IKConstraint3D* feet = animator.find_ik(link.feet)) {
            // The feet go where the physics world's floor is, never onto the character's own capsule; in the air the
            // clip's feet are the right ones (the weight fades by the constraint's own time).
            feet->ground = ground_query_from_world(world, link.ground_mask, state.body);
            feet->set_weight(on_ground ? 1.0f : 0.0f);
        }
    }
}

void drive_traversal_animator(Animator3D& animator, const TraversalState3D& traversal, const CharacterAnimationLink3D& link,
                              Vec3f up) {
    (void)up;
    const u32 limbs = std::min<u32>(static_cast<u32>(link.hands.size()), max_grip_limbs3d);
    for (u32 h = 0; h < limbs; ++h) {
        IKConstraint3D* hand = animator.find_ik(link.hands[h]);
        if (hand == nullptr) {
            continue;
        }
        drive_traversal_hand(*hand, traversal, link, h);
    }
}

GroundQuery3D ground_query_from_world(const World3D& world, Layer3DMask mask, Body3D ignore) {
    return [&world, mask, ignore](Vec3f from, Vec3f to) -> std::optional<GroundHit3D> {
        QueryFilter3D filter;
        filter.mask = mask;
        filter.ignore = ignore;
        const std::optional<RayHit3D> hit = world.raycast(from, to, filter);
        if (!hit) {
            return std::nullopt;
        }
        return GroundHit3D{hit->point, hit->normal};
    };
}

Collider3D collider_from_bounds(const Bounds3D& bounds) {
    Collider3D c;
    c.kind = Collider3D::Kind::Box;
    c.half_extents = bounds.size() * 0.5f;
    c.offset = bounds.center();
    return c;
}

std::optional<Collider3D> collider_from_mesh(flecs::entity entity) {
    if (const MeshRenderer3D* r = entity.get<MeshRenderer3D>(); r != nullptr && r->mesh.valid()) {
        return collider_from_bounds(r->mesh.bounds());
    }
    if (const SkinnedMeshRenderer3D* r = entity.get<SkinnedMeshRenderer3D>(); r != nullptr && r->model) {
        return collider_from_bounds(r->model->bounds);
    }
    return std::nullopt;
}

Collider3D collider_hull_from_mesh_data(std::shared_ptr<const MeshData3D> mesh) {
    Collider3D c;
    c.kind = Collider3D::Kind::ConvexHull;
    c.mesh = std::move(mesh);
    return c;
}

Collider3D collider_triangles_from_mesh_data(std::shared_ptr<const MeshData3D> mesh) {
    Collider3D c;
    c.kind = Collider3D::Kind::TriangleMesh;
    c.mesh = std::move(mesh);
    return c;
}

Mesh3D draw_debug_lines(Renderer3D& renderer, Scene3D& scene, std::span<const DebugLine3D> lines) {
    if (lines.empty()) {
        return {};
    }
    // A line is a sliver triangle: the wireframe material draws its edges. Each end is moved toward the eye along its
    // view ray (it shows at the same pixel, a hair nearer): a line that edges a face then wins the depth test on every
    // device; the wireframe material's own depth bias is not enough on a software rasteriser.
    const Camera3D& camera = scene.camera;
    const bool orthographic = camera.projection == Projection3D::Orthographic;
    const Vec3f toward_eye = -camera.forward();
    const auto nearer = [&](Vec3f p) {
        const Vec3f to_eye = orthographic ? toward_eye : camera.position - p;
        const f32 distance = orthographic ? 1.0f : length(to_eye);
        if (!(distance > 0.0f)) {
            return p;
        }
        const f32 shift = orthographic ? 0.004f : std::min(0.5f * distance, std::max(0.003f, 0.002f * distance));
        return p + to_eye * (shift / distance);
    };
    MeshData3D data;
    data.vertices.reserve(lines.size() * 3);
    data.indices.reserve(lines.size() * 3);
    for (const DebugLine3D& l : lines) {
        const u32 first = static_cast<u32>(data.vertices.size());
        const Vec3f a = nearer(l.a), b = nearer(l.b);
        // A triangle (a, b, c) with c a hair off b, across the line as the eye sees it: software rasterisers drop a
        // triangle of no area before they draw its edges, so (a, b, b) would vanish on them. Its two short edges are a
        // pixel or so long.
        const Vec3f to_eye = orthographic ? toward_eye : camera.position - b;
        Vec3f across = cross(b - a, to_eye);
        across = length(across) > 0.0f ? normalize(across) : Vec3f{0.0f, 1.0f, 0.0f};
        const f32 hair = std::max(0.002f, 0.0015f * length(camera.position - b));
        MeshVertex3D va, vb, vc;
        va.position = a;
        vb.position = b;
        vc.position = b + across * hair;
        va.color = vb.color = vc.color = l.color;
        data.vertices.push_back(va);
        data.vertices.push_back(vb);
        data.vertices.push_back(vc);
        data.indices.push_back(first);
        data.indices.push_back(first + 1);
        data.indices.push_back(first + 2);
    }
    Mesh3D mesh = renderer.create_mesh(data).value_or({});
    if (mesh.valid()) {
        Material3D material;
        material.unlit = true;
        material.wireframe = true;
        material.double_sided = true;
        material.casts_shadow = false;
        material.receives_shadow = false;
        (void)scene.draw(mesh, Mat4f{}, material);
    }
    return mesh;
}

} // namespace kin
