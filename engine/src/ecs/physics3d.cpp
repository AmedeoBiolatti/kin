#include <kin/ecs/physics3d.hpp>

#include <kin/platform/log.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/scene3d.hpp>

#include <algorithm>
#include <cmath>

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

// The shape for a collider at the given scale (entity's world scale times its own), in the entity's space.
Shape3D build_shape(const Collider3D& c, Vec3f entity_scale, int depth = 0) {
    const Vec3f s = abs3(entity_scale * c.scale);
    Shape3D shape;
    if (c.shape.valid()) {
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
            for (const MeshVertex& v : c.mesh->vertices) {
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

} // namespace

WorldPhysicsState3D::WorldPhysicsState3D(flecs::world& world, World3D& physics, WorldPhysicsState3DConfig config)
    : _ecs(world)
    , _physics(physics)
    , _config(config)
    , _query(world.query_builder<const Transform3D, const Collider3D, const RigidBody3D*>().build())
    , _character_query(world.query_builder<const Transform3D, const CharacterController3D>().build())
    , _debug(config.debug) {}

void WorldPhysicsState3D::create_body(flecs::entity entity, Bound& out) {
    const Collider3D& collider = *entity.get<Collider3D>();
    const RigidBody3D* rb = entity.get<RigidBody3D>();
    const Placed pose = decompose(world_matrix(entity));
    Body3DDesc desc;
    desc.shape = build_placed_shape(collider, pose.scale);
    desc.position = pose.position;
    desc.rotation = pose.rotation;
    desc.user_id = entity.id();
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
    out = {};
    out.entity = entity.id();
    out.body = _physics.create_body(desc);
    out.motion = desc.motion;
    out.scale = pose.scale;
    out.position = out.previous_position = pose.position;
    out.rotation = out.previous_rotation = pose.rotation;
    if (!out.body.valid()) {
        KIN_LOG_WARN_F("physics3d", "entity has no valid body (shape, mass or layer refused)",
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

void WorldPhysicsState3D::sync_bodies() {
    // The entities that should have a body, by id.
    std::vector<flecs::entity> wanted;
    _query.each([&](flecs::entity e, const Transform3D&, const Collider3D&, const RigidBody3D*) {
        // A character is its own capsule: a collider beside it would make a second body for the same thing.
        if (!e.has<CharacterController3D>()) {
            wanted.push_back(e);
        }
    });
    std::ranges::sort(wanted, [](flecs::entity a, flecs::entity b) { return a.id() < b.id(); });

    // Destroy first (ascending), then create (ascending): the order is a function of the ids only.
    std::vector<Bound> kept;
    kept.reserve(_bound.size());
    std::vector<flecs::entity> to_create;
    std::size_t i = 0, j = 0;
    while (i < _bound.size() || j < wanted.size()) {
        const bool have_bound = i < _bound.size();
        const bool have_wanted = j < wanted.size();
        if (have_bound && (!have_wanted || _bound[i].entity < wanted[j].id())) {
            _physics.destroy_body(_bound[i].body);
            ++i;
        } else if (have_wanted && (!have_bound || wanted[j].id() < _bound[i].entity)) {
            to_create.push_back(wanted[j]);
            ++j;
        } else {
            // Both: kept, unless asked to be rebuilt or its motion changed.
            const RigidBody3D* rb = wanted[j].get<RigidBody3D>();
            const Body3DMotion motion = rb != nullptr ? rb->motion : Body3DMotion::Static;
            if (_bound[i].rebuild || motion != _bound[i].motion || !_physics.contains(_bound[i].body)) {
                _physics.destroy_body(_bound[i].body);
                to_create.push_back(wanted[j]);
            } else {
                kept.push_back(_bound[i]);
            }
            ++i;
            ++j;
        }
    }
    // Ascending ids interleave kept and new ones: put them back in order.
    for (const flecs::entity e : to_create) {
        Bound b;
        create_body(e, b);
        kept.push_back(b);
    }
    std::ranges::sort(kept, [](const Bound& a, const Bound& b) { return a.entity < b.entity; });
    // Drop the entries whose body was refused; hand the others to the entity as a component.
    std::erase_if(kept, [](const Bound& b) { return !b.body.valid(); });
    _bound = std::move(kept);
    for (const flecs::entity e : to_create) {
        const auto found = std::ranges::lower_bound(_bound, e.id(), {}, &Bound::entity);
        if (found != _bound.end() && found->entity == e.id()) {
            e.set<PhysicsBody3D>({found->body});
        }
    }
    sync_characters();
}

void WorldPhysicsState3D::sync_characters() {
    std::vector<flecs::entity> wanted;
    _character_query.each([&](flecs::entity e, const Transform3D&, const CharacterController3D&) { wanted.push_back(e); });
    std::ranges::sort(wanted, [](flecs::entity a, flecs::entity b) { return a.id() < b.id(); });

    // Destroyed first (ascending), then made (ascending), as bodies are.
    std::vector<BoundCharacter> kept;
    kept.reserve(_characters.size());
    std::vector<flecs::entity> to_create;
    std::size_t i = 0, j = 0;
    while (i < _characters.size() || j < wanted.size()) {
        const bool have_bound = i < _characters.size();
        const bool have_wanted = j < wanted.size();
        if (have_bound && (!have_wanted || _characters[i].entity < wanted[j].id())) {
            _physics.destroy_character(_characters[i].character);
            ++i;
        } else if (have_wanted && (!have_bound || wanted[j].id() < _characters[i].entity)) {
            to_create.push_back(wanted[j]);
            ++j;
        } else {
            if (_characters[i].rebuild || !_physics.contains(_characters[i].character)) {
                _physics.destroy_character(_characters[i].character);
                to_create.push_back(wanted[j]);
            } else {
                kept.push_back(_characters[i]);
            }
            ++i;
            ++j;
        }
    }
    for (const flecs::entity e : to_create) {
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
        if (!c.character.valid()) {
            KIN_LOG_WARN_F("physics3d", "entity has no valid character (descriptor refused)",
                           (LogFields{{.name = "entity", .value = std::to_string(e.id())}}));
        }
        kept.push_back(c);
    }
    std::ranges::sort(kept, [](const BoundCharacter& a, const BoundCharacter& b) { return a.entity < b.entity; });
    std::erase_if(kept, [](const BoundCharacter& c) { return !c.character.valid(); });
    _characters = std::move(kept);
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
    Transform3D* t = e.get_mut<Transform3D>();
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
        const CharacterAnimationLink3D* link = e.get<CharacterAnimationLink3D>();
        if (link != nullptr) {
            if (Animator3D* animator = e.get_mut<Animator3D>()) {
                drive_character_animator(*animator, state, _physics, *link);
                if (traversal.enabled) {
                    drive_traversal_animator(*animator, traversal, *link, state.up);
                }
            }
        }
    }
}

TraversalState3D WorldPhysicsState3D::traversal_of(flecs::entity entity) const {
    const Character3D c = character_of(entity);
    return c.valid() ? _physics.traversal(c) : TraversalState3D{};
}

Character3D WorldPhysicsState3D::character_of(flecs::entity entity) const {
    const auto found = std::ranges::lower_bound(_characters, entity.id(), {}, &BoundCharacter::entity);
    return found != _characters.end() && found->entity == entity.id() ? found->character : Character3D{};
}

void WorldPhysicsState3D::write_back(Bound& b, Vec3f position, Quatf rotation) {
    flecs::entity e(_ecs, b.entity);
    if (!e.is_alive()) {
        return;
    }
    Transform3D* t = e.get_mut<Transform3D>();
    if (t == nullptr) {
        return;
    }
    if (!e.parent().is_valid() || parent_matrix(e) == Mat4f{}) {
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
    sync_bodies();

    // Entities to bodies.
    for (Bound& b : _bound) {
        flecs::entity e(_ecs, b.entity);
        if (!e.is_alive()) {
            continue;
        }
        const Transform3D* local = e.get<Transform3D>();
        if (b.motion == Body3DMotion::Dynamic) {
            // Moved by game code since we wrote it: take the body there (the world pose now, velocities kept).
            if (local != nullptr && (local->position != b.last_position || local->rotation != b.last_rotation)) {
                const Placed p = decompose(world_matrix(e));
                _physics.set_position(b.body, p.position);
                _physics.set_rotation(b.body, p.rotation);
                b.last_position = local->position;
                b.last_rotation = local->rotation;
            }
        } else {
            const Placed p = decompose(world_matrix(e));
            if (b.motion == Body3DMotion::Kinematic) {
                _physics.follow_transform(b.body, p.position, p.rotation, dt);
            } else if (p.position != b.last_position || p.rotation != b.last_rotation) {
                _physics.follow_transform(b.body, p.position, p.rotation, dt);
            }
            b.last_position = p.position;
            b.last_rotation = p.rotation;
        }
    }

    characters_to_world(dt);
    _physics.step(dt);

    // Bodies to entities (the awake dynamic ones: a sleeping body has not moved).
    for (Bound& b : _bound) {
        b.previous_position = b.position;
        b.previous_rotation = b.rotation;
        if (b.motion != Body3DMotion::Dynamic) {
            continue;
        }
        if (!_physics.is_active(b.body)) {
            continue;
        }
        b.position = _physics.position(b.body);
        b.rotation = _physics.rotation(b.body);
        write_back(b, b.position, b.rotation);
    }

    characters_from_world();

    // Events: out of the world, in its order.
    for (const ContactEvent3D& e : _physics.drain_events()) {
        _events.push_back(e);
    }
    PhysicsContacts3D contacts;
    contacts.events = _events;
    _ecs.set<PhysicsContacts3D>(std::move(contacts));
    for (const CharacterEvent3D& e : _physics.drain_character_events()) {
        _character_events.push_back(e);
    }
    if (!_characters.empty() || !_character_events.empty()) {
        CharacterEvents3D events;
        events.events = _character_events;
        _ecs.set<CharacterEvents3D>(std::move(events));
    }
}

i32 WorldPhysicsState3D::advance(f32 frame_seconds) {
    if (!(_config.fixed_dt > 0.0f) || !(frame_seconds > 0.0f)) {
        return 0;
    }
    _accumulator += frame_seconds;
    i32 steps = 0;
    std::vector<ContactEvent3D> all;
    std::vector<CharacterEvent3D> character_all;
    while (_accumulator >= _config.fixed_dt && steps < std::max(1, _config.max_steps)) {
        step(_config.fixed_dt);
        all.insert(all.end(), _events.begin(), _events.end());
        character_all.insert(character_all.end(), _character_events.begin(), _character_events.end());
        _accumulator -= _config.fixed_dt;
        ++steps;
    }
    if (steps == std::max(1, _config.max_steps)) {
        _accumulator = std::min(_accumulator, _config.fixed_dt); // drop the backlog
    }
    // One advance's events: all of its steps' batches.
    _events = std::move(all);
    _character_events = std::move(character_all);
    if (steps > 1) {
        PhysicsContacts3D contacts;
        contacts.events = _events;
        _ecs.set<PhysicsContacts3D>(std::move(contacts));
        if (!_characters.empty() || !_character_events.empty()) {
            CharacterEvents3D events;
            events.events = _character_events;
            _ecs.set<CharacterEvents3D>(std::move(events));
        }
    }
    return steps;
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
}

void WorldPhysicsState3D::rebuild(flecs::entity entity) {
    const auto found = std::ranges::lower_bound(_bound, entity.id(), {}, &Bound::entity);
    if (found != _bound.end() && found->entity == entity.id()) {
        found->rebuild = true;
    }
    const auto character = std::ranges::lower_bound(_characters, entity.id(), {}, &BoundCharacter::entity);
    if (character != _characters.end() && character->entity == entity.id()) {
        character->rebuild = true;
    }
}

flecs::entity WorldPhysicsState3D::entity_of(u64 user_id) const {
    const flecs::entity e(_ecs, static_cast<flecs::entity_t>(user_id));
    return e.is_alive() ? e : flecs::entity{};
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
        if (IKConstraint* feet = animator.find_ik(link.feet)) {
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
        IKConstraint* hand = animator.find_ik(link.hands[h]);
        if (hand == nullptr) {
            continue;
        }
        const GripState3D& g = traversal.grips[h];
        const bool reaching = g.phase == GripPhase3D::Reaching;
        const bool holding = g.phase == GripPhase3D::Held || g.phase == GripPhase3D::Slipping;
        // Over a ledge the hands are let go of half way up (they would be pulled through the chest).
        const bool over = traversal.mode == TraversalMode3D::Mantling && traversal.mantle_progress > 0.6f;
        if ((reaching || holding) && !over) {
            hand->set_target(g.point + g.normal * link.hand_clearance);
            if (link.align_hands) {
                // The hand's own axes: +Z into the surface, +Y up it, +X to the character's left.
                Mat4f m;
                const Vec3f x = -g.right, y = g.surface_up, z = -g.normal;
                m.m[0] = x.x; m.m[1] = x.y; m.m[2] = x.z;
                m.m[4] = y.x; m.m[5] = y.y; m.m[6] = y.z;
                m.m[8] = z.x; m.m[9] = z.y; m.m[10] = z.z;
                hand->set_target_rotation(quat_of(m, {1.0f, 1.0f, 1.0f}) * link.hand_offset, 1.0f);
            }
            hand->set_weight(1.0f);
        } else {
            hand->set_weight(0.0f);
        }
    }
}

GroundQuery ground_query_from_world(const World3D& world, Layer3DMask mask, Body3D ignore) {
    return [&world, mask, ignore](Vec3f from, Vec3f to) -> std::optional<GroundHit> {
        QueryFilter3D filter;
        filter.mask = mask;
        filter.ignore = ignore;
        const std::optional<RayHit3D> hit = world.raycast(from, to, filter);
        if (!hit) {
            return std::nullopt;
        }
        return GroundHit{hit->point, hit->normal};
    };
}

Collider3D collider_from_bounds(const Bounds3& bounds) {
    Collider3D c;
    c.kind = Collider3D::Kind::Box;
    c.half_extents = bounds.size() * 0.5f;
    c.offset = bounds.center();
    return c;
}

std::optional<Collider3D> collider_from_mesh(flecs::entity entity) {
    if (const MeshRenderer* r = entity.get<MeshRenderer>(); r != nullptr && r->mesh.valid()) {
        return collider_from_bounds(r->mesh.bounds());
    }
    if (const SkinnedMeshRenderer* r = entity.get<SkinnedMeshRenderer>(); r != nullptr && r->model) {
        return collider_from_bounds(r->model->bounds);
    }
    return std::nullopt;
}

Collider3D collider_hull_from_mesh_data(std::shared_ptr<const MeshData> mesh) {
    Collider3D c;
    c.kind = Collider3D::Kind::ConvexHull;
    c.mesh = std::move(mesh);
    return c;
}

Collider3D collider_triangles_from_mesh_data(std::shared_ptr<const MeshData> mesh) {
    Collider3D c;
    c.kind = Collider3D::Kind::TriangleMesh;
    c.mesh = std::move(mesh);
    return c;
}

Mesh draw_debug_lines(Renderer3D& renderer, Scene3D& scene, std::span<const DebugLine3D> lines) {
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
    MeshData data;
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
        MeshVertex va, vb, vc;
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
    Mesh mesh = renderer.create_mesh(data);
    if (mesh.valid()) {
        Material3D material;
        material.unlit = true;
        material.wireframe = true;
        material.double_sided = true;
        material.casts_shadow = false;
        material.receives_shadow = false;
        scene.draw(mesh, Mat4f{}, material);
    }
    return mesh;
}

} // namespace kin
