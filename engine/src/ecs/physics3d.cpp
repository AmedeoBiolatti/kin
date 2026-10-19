#include <kin/ecs/physics3d.hpp>

#include <kin/platform/log.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/render3d/skeleton3d.hpp>
#include <kin/physics3d/assets3d.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

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

Shape3D cook_collider3d(const Collider3D& collider,Vec3f scale) { return build_placed_shape(collider,scale); }

WorldPhysicsState3D::WorldPhysicsState3D(flecs::world& world, World3D& physics, WorldPhysicsState3DConfig config)
    : _ecs(world)
    , _physics(physics)
    , _config(config)
    , _query(world.query_builder<const Transform3D, const Collider3D, const RigidBody3D*>().build())
    , _character_query(world.query_builder<const Transform3D, const CharacterController3D>().build())
    , _ragdoll_query(world.query_builder<const Transform3D, const SkinnedMeshRenderer3D, const Ragdoll3D>().build())
    , _rope_query(world.query_builder<const Rope3D>().build())
    , _debug(config.debug) {}

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

void WorldPhysicsState3D::sync_bodies() {
    // The entities that should have a body, by id.
    std::vector<flecs::entity> wanted;
    const auto consider=[&](flecs::entity e) {
        // A character is its own capsule: a collider beside it would make a second body for the same thing.
        if (!e.has<CharacterController3D>()) {
            wanted.push_back(e);
        }
    };
    if(_selective_sync)for(auto id:_sync_entities) {
        flecs::entity e{_ecs,id};
        if(e.is_alive()&&e.enabled()&&!e.has(flecs::Prefab)&&e.has<Transform3D>()&&e.has<Collider3D>())consider(e);
    } else _query.each([&](flecs::entity e,const Transform3D&,const Collider3D&,const RigidBody3D*){consider(e);});
    std::ranges::sort(wanted, [](flecs::entity a, flecs::entity b) { return a.id() < b.id(); });

    // Destroy first (ascending), then create (ascending): the order is a function of the ids only.
    std::vector<Bound> kept;
    kept.reserve(_bound.size());
    std::vector<flecs::entity> to_create;
    std::size_t i = 0, j = 0;
    while (i < _bound.size() || j < wanted.size()) {
        const bool have_bound = i < _bound.size();
        const bool have_wanted = j < wanted.size();
        if(have_bound&&!sync_selected(_bound[i].entity)){kept.push_back(_bound[i++]);continue;}
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
    sync_ragdolls();
    sync_ropes();
}

void WorldPhysicsState3D::sync_characters() {
    std::vector<flecs::entity> wanted;
    if(_selective_sync)for(auto id:_sync_entities) {
        flecs::entity e{_ecs,id};
        if(e.is_alive()&&e.enabled()&&!e.has(flecs::Prefab)&&e.has<Transform3D>()&&e.has<CharacterController3D>())wanted.push_back(e);
    } else _character_query.each([&](flecs::entity e, const Transform3D&, const CharacterController3D&) { wanted.push_back(e); });
    std::ranges::sort(wanted, [](flecs::entity a, flecs::entity b) { return a.id() < b.id(); });

    // Destroyed first (ascending), then made (ascending), as bodies are.
    std::vector<BoundCharacter> kept;
    kept.reserve(_characters.size());
    std::vector<flecs::entity> to_create;
    std::size_t i = 0, j = 0;
    while (i < _characters.size() || j < wanted.size()) {
        const bool have_bound = i < _characters.size();
        const bool have_wanted = j < wanted.size();
        if(have_bound&&!sync_selected(_characters[i].entity)){kept.push_back(_characters[i++]);continue;}
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
    // Scene-prefab transforms can be inherited. Own the local pose before
    // writing simulation results; raw Flecs get_mut only accepts owned data.
    Transform3D* t = EcsEntity{e}.get_mut<Transform3D>();
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
    _ragdoll_events.clear();
    _rope_events.clear();
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

    ragdolls_to_world();
    ropes_to_world();
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

    // Ragdolls before the characters: a character handed over follows its pelvis, and the entity's Transform3D follows that.
    ragdolls_from_world();
    ropes_from_world();
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
    for (const RagdollEvent3D& e : _physics.drain_ragdoll_events()) {
        _ragdoll_events.push_back(e);
    }
    if (!_ragdolls.empty() || !_ragdoll_events.empty()) {
        RagdollEvents3D events;
        events.events = _ragdoll_events;
        _ecs.set<RagdollEvents3D>(std::move(events));
    }
    for (const RopeEvent3D& e : _physics.drain_rope_events()) {
        _rope_events.push_back(e);
    }
    if (!_ropes.empty() || !_rope_events.empty()) {
        RopeEvents3D events;
        events.events = _rope_events;
        _ecs.set<RopeEvents3D>(std::move(events));
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
    for (BoundRagdoll& r : _ragdolls) {
        write_pose_override(r, alpha);
    }
}

bool WorldPhysicsState3D::replace_bodies(std::span<const BodyEdit> edits,
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
        if (error) error->clear();
        return true;
    } catch (...) { cleanup(); throw; }
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

void WorldPhysicsState3D::sync_ragdolls() {
    std::vector<flecs::entity> wanted;
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
    std::ranges::sort(wanted, [](flecs::entity a, flecs::entity b) { return a.id() < b.id(); });

    std::vector<BoundRagdoll> kept;
    kept.reserve(_ragdolls.size());
    std::vector<flecs::entity> to_create;
    std::size_t i = 0, j = 0;
    while (i < _ragdolls.size() || j < wanted.size()) {
        const bool have_bound = i < _ragdolls.size();
        const bool have_wanted = j < wanted.size();
        if(have_bound&&!sync_selected(_ragdolls[i].entity)){kept.push_back(std::move(_ragdolls[i++]));continue;}
        if (have_bound && (!have_wanted || _ragdolls[i].entity < wanted[j].id())) {
            // The entity (or its component) is gone: its character, if it was handed over, gets its place back as it is.
            _physics.destroy_ragdoll(_ragdolls[i].ragdoll);
            ++i;
        } else if (have_wanted && (!have_bound || wanted[j].id() < _ragdolls[i].entity)) {
            to_create.push_back(wanted[j]);
            ++j;
        } else {
            if (_ragdolls[i].rebuild || !_physics.contains(_ragdolls[i].ragdoll)) {
                _physics.destroy_ragdoll(_ragdolls[i].ragdoll);
                to_create.push_back(wanted[j]);
            } else {
                kept.push_back(_ragdolls[i]);
            }
            ++i;
            ++j;
        }
    }
    for (const flecs::entity e : to_create) {
        const Ragdoll3D& component = *e.get<Ragdoll3D>();
        const Model3D& model = *e.get<SkinnedMeshRenderer3D>()->model;
        const Skeleton3D& skeleton = model.skeletons[0];
        const Mat4f entity_matrix = world_matrix(e);
        const Placed placed = decompose(entity_matrix);
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
        } else {
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
        kept.push_back(std::move(b));
    }
    std::ranges::sort(kept, [](const BoundRagdoll& a, const BoundRagdoll& b) { return a.entity < b.entity; });
    std::erase_if(kept, [](const BoundRagdoll& r) { return !r.ragdoll.valid(); });
    _ragdolls = std::move(kept);
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
        const bool bound = animator != nullptr && animator->bound == renderer->model.get() && animator->player.skeleton() == &skeleton;
        if (bound) {
            compute_world_matrices(skeleton, animator->player.pose(), _pose_scratch);
        } else {
            compute_world_matrices(skeleton, skeleton.rest_pose(), _pose_scratch);
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
    bool any = false;
    for (const RagdollBonePose3D& b : r.current) {
        any = any || b.weight > 0.0f;
    }
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
        o.weights[j] = now.weight;
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
                const RagdollPlace3D place = _physics.ragdoll_get_up_place(r.ragdoll, state.up, all_layers3d, 4.0f);
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
    out.place = _physics.ragdoll_get_up_place(r->ragdoll, up, options.mask, options.max_ground_distance);
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

void WorldPhysicsState3D::sync_ropes() {
    std::vector<flecs::entity> wanted;
    if(_selective_sync)for(auto id:_sync_entities) {
        flecs::entity e{_ecs,id};
        if(e.is_alive()&&e.enabled()&&!e.has(flecs::Prefab)&&e.has<Rope3D>())wanted.push_back(e);
    } else _rope_query.each([&](flecs::entity e, const Rope3D&) { wanted.push_back(e); });
    std::ranges::sort(wanted, [](flecs::entity a, flecs::entity b) { return a.id() < b.id(); });

    std::vector<BoundRope> kept;
    kept.reserve(_ropes.size());
    std::vector<flecs::entity> to_create;
    std::size_t i = 0, j = 0;
    while (i < _ropes.size() || j < wanted.size()) {
        const bool have_bound = i < _ropes.size();
        const bool have_wanted = j < wanted.size();
        if(have_bound&&!sync_selected(_ropes[i].entity)){kept.push_back(std::move(_ropes[i++]));continue;}
        if (have_bound && (!have_wanted || _ropes[i].entity < wanted[j].id())) {
            _physics.destroy_rope(_ropes[i].rope);
            ++i;
        } else if (have_wanted && (!have_bound || wanted[j].id() < _ropes[i].entity)) {
            to_create.push_back(wanted[j]);
            ++j;
        } else {
            if (_ropes[i].rebuild || !_physics.contains(_ropes[i].rope)) {
                _physics.destroy_rope(_ropes[i].rope);
                to_create.push_back(wanted[j]);
            } else {
                kept.push_back(_ropes[i]);
            }
            ++i;
            ++j;
        }
    }
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
            continue;
        }
        kept.push_back(b);
    }
    std::ranges::sort(kept, [](const BoundRope& a, const BoundRope& b) { return a.entity < b.entity; });
    _ropes = std::move(kept);
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
        c->sampled = _physics.rope_points(r.rope);
        c->tension_start = _physics.rope_tension(r.rope, RopeEnd3D::Start);
        c->tension_end = _physics.rope_tension(r.rope, RopeEnd3D::End);
    }
}

void WorldPhysicsState3D::collect_ropes(Renderer3D& renderer, Scene3D& scene) {
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
        const std::vector<RopePoint3D>& pts = c->sampled;
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
        Mesh3D mesh = renderer.create_mesh(g.data);
        if (mesh.valid()) {
            scene.draw(mesh, Mat4f{}, g.material);
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
    Mesh3D mesh = renderer.create_mesh(data);
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
