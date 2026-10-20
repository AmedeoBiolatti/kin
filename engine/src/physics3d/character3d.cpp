#include "character3d_impl.hpp"

#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace kin {
namespace physics3d_detail {

namespace {

bool finite(Vec3f v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool positive(f32 v) { return std::isfinite(v) && v > 0.0f; }
bool non_negative(f32 v) { return std::isfinite(v) && v >= 0.0f; }

// The grab and climb numbers of a descriptor: finite, and the sizes that must be positive.
bool valid_traversal(const TraversalDesc3D& t) {
    if (!t.enabled) {
        return true;
    }
    if (t.limb_count < 1 || t.limb_count > max_grip_limbs3d || !positive(t.reach) || !non_negative(t.probe_radius) ||
        !non_negative(t.probe_spread) || !non_negative(t.reach_seconds) || !non_negative(t.min_hand_separation) ||
        !positive(t.resource.max) || !non_negative(t.resource.regen_rate) || !non_negative(t.resource.regen_delay) ||
        !non_negative(t.mantle_seconds) || !non_negative(t.exhaust_interval) || !non_negative(t.exhaust_fade)) {
        return false;
    }
    for (const GripLimbDesc3D& l : t.limbs) {
        if (!finite(l.shoulder)) {
            return false;
        }
    }
    return finite(Vec3f{t.hang_drop, t.wall_gap, t.swing_frequency}) && finite(Vec3f{t.swing_damping, t.max_swing, t.grab_momentum}) &&
           finite(Vec3f{t.shimmy_speed, t.climb_speed, t.slip_speed}) && finite(Vec3f{t.jump_off_speed, t.jump_up_speed, t.drain_one_hand}) &&
           finite(Vec3f{t.drain_two_hands, t.drain_moving, t.drain_sprint}) && finite(Vec3f{t.grab_cost, t.pull_up_cost, t.pull_up_min}) &&
           finite(Vec3f{t.slip_release_seconds, t.mantle_inset, t.ground_drain_factor}) &&
           finite(Vec3f{t.swing_vertical_frequency, t.swing_vertical_damping, t.ledge_search_height});
}

// The rotation that takes +Y to the unit vector `up` (a capsule is round about its axis, so which twist does not matter,
// only that it is a function of `up` alone). Antiparallel: a half turn about X. Square roots only: no trigonometry, so it
// is the same bits on every platform.
Quatf y_to(Vec3f up) {
    const f32 d = up.y;
    if (d < -0.999999f) {
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    // The half-way quaternion: axis = Y x up, w = 1 + Y.up, normalised.
    const Vec3f axis{up.z, 0.0f, -up.x};
    return Quatf{axis.x, axis.y, axis.z, 1.0f + d}.normalized();
}

// `v` with its part along `up` taken off.
Vec3f flat(Vec3f v, Vec3f up) { return v - up * dot(v, up); }

// From `a` toward `b` by at most `max_delta` (a vector approach: how a velocity gains speed at a fixed acceleration).
Vec3f approach(Vec3f a, Vec3f b, f32 max_delta) {
    const Vec3f d = b - a;
    const f32 n = length(d);
    if (!(n > max_delta)) {
        return b;
    }
    return a + d * (max_delta / n);
}

// Little endian, fixed width, for the saved header (which the hash reads too).
struct Out {
    std::vector<u8>& bytes;
    void u32v(u32 v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void u64v(u64 v) {
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void f32v(f32 v) { u32v(std::bit_cast<u32>(v)); }
    void vec(Vec3f v) {
        f32v(v.x);
        f32v(v.y);
        f32v(v.z);
    }
    void b(bool v) { bytes.push_back(v ? 1 : 0); }
};

struct In {
    const u8* p;
    const u8* end;
    bool failed = false;
    u32 u32v() {
        if (end - p < 4) {
            failed = true;
            return 0;
        }
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(p[i]) << (8 * i);
        p += 4;
        return v;
    }
    u64 u64v() {
        const u64 lo = u32v();
        const u64 hi = u32v();
        return lo | hi << 32;
    }
    f32 f32v() { return std::bit_cast<f32>(u32v()); }
    Vec3f vec() {
        Vec3f v;
        v.x = f32v();
        v.y = f32v();
        v.z = f32v();
        return v;
    }
    bool b() {
        if (end - p < 1) {
            failed = true;
            return false;
        }
        return *p++ != 0;
    }
};

void save_trav(Out& out, const TravData& t) {
    out.bytes.push_back(static_cast<u8>(t.mode));
    for (const bool g : t.input.grab) out.b(g);
    out.vec(t.input.reach);
    out.b(t.input.pull_up);
    out.b(t.input.jump_off);
    out.b(t.input.drop);
    out.b(t.prev_pull);
    out.b(t.prev_jump_off);
    out.f32v(t.resource.value);
    out.f32v(t.resource.max);
    out.f32v(t.resource.delay);
    out.b(t.resource.exhausted);
    out.vec(t.swing);
    out.vec(t.swing_velocity);
    out.vec(t.hang_place);
    out.b(t.has_hang_place);
    out.f32v(t.hang_time);
    out.vec(t.facing);
    out.f32v(t.mantle_time);
    out.u32v(t.mantle_body.index);
    out.u32v(t.mantle_body.generation);
    for (const Vec3f& p : t.mantle_path) out.vec(p);
    for (const GripData& g : t.grips) {
        out.bytes.push_back(static_cast<u8>(g.phase));
        out.bytes.push_back(static_cast<u8>(g.kind));
        out.u32v(g.body.index);
        out.u32v(g.body.generation);
        out.vec(g.local_point);
        out.vec(g.local_normal);
        out.f32v(g.quality);
        out.f32v(g.drain);
        out.f32v(g.slip);
        out.u32v(g.tag);
        out.b(g.fixed);
        out.b(g.ledge);
        out.f32v(g.reach_t);
        out.f32v(g.slip_time);
        out.b(g.slipping);
        out.f32v(g.exhaust_t);
        out.f32v(g.exhaust_delay);
        out.f32v(g.fade);
        out.b(g.latch);
    }
}

bool parse_trav(In& in, TravData& t) {
    if (in.end - in.p < 1) {
        return false;
    }
    t.mode = static_cast<TraversalMode3D>(*in.p++);
    for (bool& g : t.input.grab) g = in.b();
    t.input.reach = in.vec();
    t.input.pull_up = in.b();
    t.input.jump_off = in.b();
    t.input.drop = in.b();
    t.prev_pull = in.b();
    t.prev_jump_off = in.b();
    t.resource.value = in.f32v();
    t.resource.max = in.f32v();
    t.resource.delay = in.f32v();
    t.resource.exhausted = in.b();
    t.swing = in.vec();
    t.swing_velocity = in.vec();
    t.hang_place = in.vec();
    t.has_hang_place = in.b();
    t.hang_time = in.f32v();
    t.facing = in.vec();
    t.mantle_time = in.f32v();
    t.mantle_body.index = in.u32v();
    t.mantle_body.generation = in.u32v();
    for (Vec3f& p : t.mantle_path) p = in.vec();
    for (GripData& g : t.grips) {
        if (in.end - in.p < 2) {
            return false;
        }
        g.phase = static_cast<GripPhase3D>(*in.p++);
        g.kind = static_cast<GripKind3D>(*in.p++);
        g.body.index = in.u32v();
        g.body.generation = in.u32v();
        g.local_point = in.vec();
        g.local_normal = in.vec();
        g.quality = in.f32v();
        g.drain = in.f32v();
        g.slip = in.f32v();
        g.tag = in.u32v();
        g.fixed = in.b();
        g.ledge = in.b();
        g.reach_t = in.f32v();
        g.slip_time = in.f32v();
        g.slipping = in.b();
        g.exhaust_t = in.f32v();
        g.exhaust_delay = in.f32v();
        g.fade = in.f32v();
        g.latch = in.b();
    }
    return !in.failed;
}

} // namespace

Layer3DMask CharacterSet::mask_of(const Slot& s) const {
    // Its own mask and what the world's layer table lets its layer meet.
    return s.desc.collides_with & (*_env.table)[std::min<u32>(s.desc.layer, 31)];
}

void CharacterSet::apply_desc(Slot& s) {
    s.ch->SetMaxSlopeAngle(JPH::DegreesToRadians(std::clamp(s.desc.max_slope_degrees, 0.0f, 89.0f)));
    s.ch->SetMass(s.desc.mass);
    s.ch->SetMaxStrength(s.desc.push_strength);
}

Character3D CharacterSet::create(const CharacterDesc3D& d) {
    if (!finite(d.position) || !finite(d.up) || !positive(d.radius) || !positive(d.height) || d.height < 2.0f * d.radius
        || !finite(Vec3f{d.crouch_height, d.mass, d.push_strength}) || !positive(d.mass) || !non_negative(d.push_strength)
        || !non_negative(d.max_step_height) || !non_negative(d.step_down_distance) || d.layer >= layers3d::Count
        || !std::isfinite(d.max_slope_degrees) || !finite(Vec3f{d.walk_speed, d.sprint_speed, d.crouch_speed})
        || !finite(Vec3f{d.acceleration, d.deceleration, d.air_acceleration})
        || !finite(Vec3f{d.ground_friction, d.wall_friction, d.gravity_scale}) || !std::isfinite(d.jump_speed)
        || !finite(Vec3f{d.coyote_seconds, d.jump_buffer_seconds, d.up_blend_rate}) || d.layer == layers3d::Static
        || !valid_traversal(d.traversal)) {
        return {};
    }
    const f32 crouch_height = std::clamp(d.crouch_height, 2.0f * d.radius, d.height);
    const auto capsule = [&](f32 height) -> JPH::RefConst<JPH::Shape> {
        const JPH::CapsuleShapeSettings capsule_settings(0.5f * (height - 2.0f * d.radius), d.radius);
        const JPH::RotatedTranslatedShapeSettings placed(JPH::Vec3(0.0f, 0.5f * height, 0.0f), JPH::Quat::sIdentity(),
                                                         capsule_settings.Create().Get());
        const auto made = placed.Create();
        return made.IsValid() ? made.Get() : JPH::RefConst<JPH::Shape>();
    };
    JPH::RefConst<JPH::Shape> stand = capsule(d.height), crouch = capsule(crouch_height);
    if (stand == nullptr || crouch == nullptr) {
        return {};
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

    Vec3f up = normalize(d.up);
    bool explicit_up = up != Vec3f{};
    if (!explicit_up) {
        const Vec3f g = _env.gravity();
        up = length(g) > 0.0f ? normalize(-g) : Vec3f{0.0f, 1.0f, 0.0f};
    }
    const Quatf rotation = y_to(up);

    JPH::CharacterVirtualSettings settings;
    settings.mID = JPH::CharacterID(index);
    settings.mUp = to_jolt(up);
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -d.radius);
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(std::clamp(d.max_slope_degrees, 0.0f, 89.0f));
    settings.mShape = stand;
    settings.mMass = d.mass;
    settings.mMaxStrength = d.push_strength;
    settings.mInnerBodyShape = stand;
    settings.mInnerBodyLayer = static_cast<JPH::ObjectLayer>(d.layer);
    s.ch = new JPH::CharacterVirtual(&settings, JPH::RVec3(to_jolt(d.position)), to_jolt(rotation), d.user_id, _env.system);
    s.stand = stand;
    s.crouch = crouch;
    s.desc = d;
    s.desc.up = explicit_up ? up : Vec3f{};
    s.input = {};
    s.inner = {};
    s.prev_jump = false;
    s.jump_buffer = 0.0f;
    s.coyote = 0.0f;
    s.up = up;
    s.up_target = up;
    s.up_explicit = explicit_up;
    s.crouching = false;
    s.stand_blocked = false;
    s.velocity = {};
    s.control = {};
    s.vertical = 0.0f;
    s.relative = {};
    s.air_time = 0.0f;
    s.jumped = false;
    s.ground = CharacterGround3D::InAir;
    s.touching_ceiling = false;
    s.touched.clear();
    s.contacts.clear();
    s.trav = {};
    s.external_set = false;
    s.external_velocity = {};
    s.external_impulse = {};
    if (d.traversal.enabled) {
        s.trav.resource.max = std::max(0.0f, d.traversal.resource.max);
        s.trav.resource.value = s.trav.resource.max;
    }
    s.alive = true;
    apply_desc(s);
    if (s.ch->GetInnerBodyID().IsInvalid()) {
        s.ch = nullptr;
        s.alive = false;
        _free.push_back(index);
        return {};
    }
    s.inner = _env.register_inner(s.ch->GetInnerBodyID(), d.user_id);
    ++_live;
    // Where it is: the ground under it, so that the first step starts supported.
    const MaskFilter layer_filter(mask_of(s));
    const OwnBodyFilter body_filter(s.ch->GetInnerBodyID(), _env);
    s.ch->RefreshContacts(_env.system->GetDefaultBroadPhaseLayerFilter(d.layer), layer_filter, body_filter,
                          JPH::ShapeFilter(), *_env.temp);
    return {index, s.generation};
}

void CharacterSet::destroy(Character3D c) {
    Slot* s = find(c);
    if (s == nullptr) {
        return;
    }
    // The world forgets the inner body first (its pairs end), then Jolt's character takes the Jolt body with it.
    _env.unregister_inner(s->inner);
    s->ch = nullptr;
    s->stand = nullptr;
    s->crouch = nullptr;
    s->alive = false;
    ++s->generation;
    s->touched.clear();
    s->contacts.clear();
    _free.push_back(c.index);
    --_live;
}

std::vector<Character3D> CharacterSet::list() const {
    std::vector<Character3D> out;
    for (u32 i = 0; i < _slots.size(); ++i) {
        if (_slots[i].alive) {
            out.push_back({i, _slots[i].generation});
        }
    }
    return out;
}

void CharacterSet::set_input(Character3D c, const CharacterInput3D& input) {
    Slot* s = find(c);
    if (s == nullptr || !std::isfinite(input.move.x) || !std::isfinite(input.move.y) || !finite(input.forward)) {
        return;
    }
    s->input = input;
}

CharacterInput3D CharacterSet::input(Character3D c) const {
    const Slot* s = find(c);
    return s != nullptr ? s->input : CharacterInput3D{};
}

CharacterState3D CharacterSet::state(Character3D c) const {
    CharacterState3D out;
    const Slot* s = find(c);
    if (s == nullptr) {
        return out;
    }
    const JPH::CharacterVirtual& ch = *s->ch;
    out.position = from_jolt(ch.GetPosition());
    out.velocity = s->velocity;
    out.up = s->up;
    out.ground = s->ground;
    if (ch.IsSupported() && !ch.GetGroundBodyID().IsInvalid()) {
        out.ground_body = _env.body_of(ch.GetGroundBodyID());
        out.ground_user_id = out.ground_body.valid() ? _env.user_id_of(out.ground_body) : 0;
        out.ground_normal = from_jolt(ch.GetGroundNormal());
        out.ground_velocity = from_jolt(ch.GetGroundVelocity());
    }
    // Along the ground it stands on (the plane of its normal), not across the plan.
    const Vec3f relative = s->relative;
    const Vec3f plane_normal = out.ground == CharacterGround3D::OnGround ? out.ground_normal : s->up;
    out.ground_speed = length(relative - plane_normal * dot(relative, plane_normal));
    out.vertical_speed = dot(s->velocity, s->up);
    out.crouching = s->crouching;
    out.stand_blocked = s->stand_blocked;
    out.air_time = s->air_time;
    out.height = s->crouching ? std::clamp(s->desc.crouch_height, 2.0f * s->desc.radius, s->desc.height) : s->desc.height;
    out.body = s->inner;
    out.jumped = s->jumped;
    out.traversal_mode = s->trav.mode;
    return out;
}

std::vector<CharacterContact3D> CharacterSet::contacts(Character3D c) const {
    const Slot* s = find(c);
    return s != nullptr ? s->contacts : std::vector<CharacterContact3D>{};
}

void CharacterSet::teleport(Character3D c, Vec3f feet) {
    Slot* s = find(c);
    if (s == nullptr || !finite(feet)) {
        return;
    }
    s->ch->SetPosition(JPH::RVec3(to_jolt(feet)));
    s->ch->SetLinearVelocity(JPH::Vec3::sZero());
    s->velocity = {};
    s->control = {};
    s->vertical = 0.0f;
    s->relative = {};
    s->air_time = 0.0f;
    s->coyote = 0.0f;
    s->jump_buffer = 0.0f;
    // A teleport stops it: a velocity or impulses given before it are dropped (those given after it count).
    s->external_set = false;
    s->external_velocity = {};
    s->external_impulse = {};
    if (s->desc.traversal.enabled) {
        // Taken somewhere else: nothing it held is within reach any more. The resource and the command stay.
        const Resource3D resource = s->trav.resource;
        const TraversalInput3D input = s->trav.input;
        s->trav = {};
        s->trav.resource = resource;
        s->trav.input = input;
        s->trav.prev_pull = input.pull_up;
        s->trav.prev_jump_off = input.jump_off;
    }
    const MaskFilter layer_filter(mask_of(*s));
    const OwnBodyFilter body_filter(s->ch->GetInnerBodyID(), _env);
    s->ch->RefreshContacts(_env.system->GetDefaultBroadPhaseLayerFilter(s->desc.layer), layer_filter, body_filter,
                           JPH::ShapeFilter(), *_env.temp);
    // Where it is now is where it was: no Begin or End for what the move changed, no landing.
    s->touched.clear();
    collect_contacts(c.index, *s);
    s->ground = s->ch->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround ? CharacterGround3D::OnGround
              : CharacterGround3D::InAir;
}

void CharacterSet::set_up(Character3D c, Vec3f up, bool instant) {
    Slot* s = find(c);
    if (s == nullptr || !finite(up)) {
        return;
    }
    const Vec3f n = normalize(up);
    if (n == Vec3f{}) {
        s->up_explicit = false;
        s->desc.up = {};
        return;
    }
    s->up_explicit = true;
    s->up_target = n;
    s->desc.up = n;
    if (instant) {
        s->up = n;
        s->ch->SetUp(to_jolt(n));
        s->ch->SetRotation(to_jolt(y_to(n)));
    }
}

void CharacterSet::swap_shape(Slot& s, bool crouch) {
    const JPH::Shape* shape = crouch ? s.crouch.GetPtr() : s.stand.GetPtr();
    s.ch->SetInnerBodyShape(shape);
    s.crouching = crouch;
}

bool CharacterSet::set_crouch(Character3D c, bool crouch) {
    Slot* s = find(c);
    if (s == nullptr) {
        return false;
    }
    if (crouch == s->crouching) {
        return true;
    }
    const MaskFilter layer_filter(mask_of(*s));
    const OwnBodyFilter body_filter(s->ch->GetInnerBodyID(), _env);
    // Crouching always fits; standing is tried with a clearance check (a little penetration is accepted, as the
    // character's padding is).
    const f32 allowed = crouch ? FLT_MAX : 1.5f * s->ch->GetCharacterPadding();
    const bool ok = s->ch->SetShape(crouch ? s->crouch.GetPtr() : s->stand.GetPtr(), allowed,
                                    _env.system->GetDefaultBroadPhaseLayerFilter(s->desc.layer), layer_filter,
                                    body_filter, JPH::ShapeFilter(), *_env.temp);
    if (!ok) {
        return false;
    }
    swap_shape(*s, crouch);
    s->stand_blocked = false;
    return true;
}

void CharacterSet::set_desc(Character3D c, const CharacterDesc3D& d) {
    Slot* s = find(c);
    if (s == nullptr) {
        return;
    }
    // The shape, layer, position and up are not changed here. Grab and climb numbers that are not valid are not taken.
    const CharacterDesc3D keep = s->desc;
    s->desc = d;
    if (!valid_traversal(d.traversal)) {
        s->desc.traversal = keep.traversal;
    }
    s->desc.radius = keep.radius;
    s->desc.height = keep.height;
    s->desc.crouch_height = keep.crouch_height;
    s->desc.layer = keep.layer;
    s->desc.position = keep.position;
    s->desc.up = keep.up;
    s->desc.user_id = keep.user_id;
    // Whether grab and climb is on is fixed at creation: it is in the saved state's layout.
    s->desc.traversal.enabled = keep.traversal.enabled;
    apply_desc(*s);
}

void CharacterSet::collect_contacts(u32 index, Slot& s) {
    (void)index;
    s.contacts.clear();
    const Vec3f up = s.up;
    const f32 cos_slope = std::cos(JPH::DegreesToRadians(std::clamp(s.desc.max_slope_degrees, 0.0f, 89.0f)));
    for (const JPH::CharacterVirtual::Contact& c : s.ch->GetActiveContacts()) {
        if (!c.mHadCollision || c.mBodyB.IsInvalid() || c.mBodyB == s.ch->GetInnerBodyID()) {
            continue;
        }
        const Body3D body = _env.body_of(c.mBodyB);
        if (!body.valid()) {
            continue;
        }
        CharacterContact3D out;
        out.body = body;
        out.user_id = _env.user_id_of(body);
        out.point = from_jolt(c.mPosition);
        out.normal = from_jolt(c.mContactNormal);
        out.sensor = c.mIsSensorB;
        out.walkable = dot(from_jolt(c.mSurfaceNormal), up) >= cos_slope;
        s.contacts.push_back(out);
    }
    // A body once (the lowest sub shape's contact first as Jolt sorted them), by body number.
    std::stable_sort(s.contacts.begin(), s.contacts.end(),
                     [](const CharacterContact3D& a, const CharacterContact3D& b) { return a.body.index < b.body.index; });
    s.contacts.erase(std::unique(s.contacts.begin(), s.contacts.end(),
                                 [](const CharacterContact3D& a, const CharacterContact3D& b) { return a.body == b.body; }),
                     s.contacts.end());
}

void CharacterSet::update(f32 dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        return;
    }
    for (u32 i = 0; i < _slots.size(); ++i) {
        if (_slots[i].alive && !_slots[i].disabled) {
            update_one(i, _slots[i], dt);
        }
    }
}

void CharacterSet::set_enabled(Character3D c, bool enabled) {
    Slot* s = find(c);
    if (s == nullptr || s->disabled == !enabled) {
        return;
    }
    s->disabled = !enabled;
    _env.world->set_no_collisions(s->inner, s->disabled);
    // Whatever it was doing is over: no speed, nothing held, no ground, until it is moved and updated again.
    s->velocity = {};
    s->control = {};
    s->vertical = 0.0f;
    s->relative = {};
    s->air_time = 0.0f;
    s->jumped = false;
    s->ground = CharacterGround3D::InAir;
    s->external_set = false;
    s->external_velocity = {};
    s->external_impulse = {};
    s->trav.mode = TraversalMode3D::Walking;
    s->trav.swing = {};
    s->trav.swing_velocity = {};
    s->trav.has_hang_place = false;
    for (GripData& g : s->trav.grips) {
        g = GripData{};
    }
}

bool CharacterSet::enabled(Character3D c) const {
    const Slot* s = find(c);
    return s != nullptr && !s->disabled;
}

void CharacterSet::add_impulse(Character3D c, Vec3f impulse) {
    Slot* s = find(c);
    if (s == nullptr || s->disabled || !finite(impulse)) {
        return;
    }
    s->external_impulse += impulse;
}

void CharacterSet::set_velocity(Character3D c, Vec3f velocity) {
    Slot* s = find(c);
    if (s == nullptr || s->disabled || !finite(velocity)) {
        return;
    }
    // A velocity replaces: what was pushed before it is gone, as a body's velocity set after an impulse is.
    s->external_set = true;
    s->external_velocity = velocity;
    s->external_impulse = {};
}

void CharacterSet::take_external(Slot& s) {
    const Vec3f up = s.up;
    // (set_character_desc does not check the mass: none, no push.)
    const Vec3f dv = s.desc.mass > 0.0f && std::isfinite(s.desc.mass) ? s.external_impulse / s.desc.mass : Vec3f{};
    TravData& t = s.trav;
    if (s.desc.traversal.enabled && t.mode == TraversalMode3D::Mantling) {
        // A pull-up is a set move over the edge: nothing from outside changes it.
    } else if (s.desc.traversal.enabled && t.mode == TraversalMode3D::Hanging) {
        // Hanging: the swing about the hang place takes it (the grips' spring brings it back), and so does the speed it has if
        // it lets go this step. The place's own motion (a platform) is the part of the last velocity the swing is not.
        const Vec3f place_motion = s.velocity - t.swing_velocity;
        if (s.external_set) {
            s.velocity = s.external_velocity;
            t.swing_velocity = s.external_velocity - place_motion;
        }
        s.velocity += dv;
        t.swing_velocity += dv;
    } else {
        // Walking: the controller's own velocity, in its three parts: along up, across it in the world, across it over the
        // ground (in the frame of the ground's velocity that the last step measured it in).
        if (s.external_set) {
            const Vec3f ground = s.ch->IsSupported() ? from_jolt(s.ch->GetGroundVelocity()) : Vec3f{};
            s.velocity = s.external_velocity;
            s.vertical = dot(s.external_velocity, up);
            s.control = s.external_velocity;
            s.relative = s.external_velocity - ground;
        }
        s.velocity += dv;
        s.vertical += dot(dv, up);
        s.control += dv;
        s.relative += dv;
    }
    s.external_set = false;
    s.external_velocity = {};
    s.external_impulse = {};
}

void CharacterSet::update_one(u32 index, Slot& s, f32 dt) {
    JPH::CharacterVirtual& ch = *s.ch;
    const CharacterDesc3D& d = s.desc;
    const Vec3f world_gravity = _env.gravity();

    // --- up: toward its target (the world's gravity, or the explicit vector), smoothly
    Vec3f target = s.up_target;
    if (!s.up_explicit && length(world_gravity) > 0.0f) {
        target = normalize(-world_gravity);
    }
    if (target != s.up) {
        Vec3f next = target;
        if (d.up_blend_rate > 0.0f) {
            const f32 k = std::min(1.0f, d.up_blend_rate * dt);
            // Turning exactly upside down has no shortest way: aim at a vector across it first (always the same one),
            // and the normalised blend then carries on round to the target.
            Vec3f aim = target;
            if (dot(s.up, target) < -0.9999f) {
                const Vec3f side = std::fabs(s.up.x) < 0.9f ? Vec3f{1.0f, 0.0f, 0.0f} : Vec3f{0.0f, 0.0f, 1.0f};
                aim = normalize(cross(s.up, side));
            }
            const Vec3f blended = lerp(s.up, aim, k);
            next = normalize(blended);
            // Close enough: arrive (so that a turn ends and the state stops changing).
            if (length_squared(target - next) < 1.0e-10f) {
                next = target;
            }
        }
        s.up = next;
        ch.SetUp(to_jolt(next));
        ch.SetRotation(to_jolt(y_to(next)));
    }
    const Vec3f up = s.up;
    const JPH::Vec3 up_j = to_jolt(up);

    // The filters: what it collides with, never its own body.
    const MaskFilter layer_filter(mask_of(s));
    const OwnBodyFilter body_filter(ch.GetInnerBodyID(), _env);
    const JPH::BroadPhaseLayerFilter& bp_filter_ref = _env.system->GetDefaultBroadPhaseLayerFilter(d.layer);

    // --- what was given from outside since the last step (set_velocity, add_impulse): the velocity the step starts from
    const bool external = s.has_external();
    if (external) {
        take_external(s);
    }

    // --- grab and climb: the grips, the resource and, while it hangs or mantles, the move itself
    if (d.traversal.enabled) {
        _traversal_events.clear();
        if (update_traversal(index, s, dt)) {
            // The character was moved by the grips: what it touches now, then the events (contacts first, as walking does).
            const std::vector<std::pair<u32, bool>> before = std::move(s.touched);
            collect_contacts(index, s);
            s.touched.clear();
            for (const CharacterContact3D& c : s.contacts) {
                s.touched.push_back({c.body.index, c.sensor});
            }
            s.touching_ceiling = false;
            emit_contact_changes(index, s, before);
            flush_traversal_events(index, s);
            return;
        }
    }

    // --- stance: crouching is immediate; standing needs the room (tried every step while it is wanted and blocked)
    const bool want_crouch = s.input.crouch;
    if (want_crouch != s.crouching) {
        const f32 allowed = want_crouch ? FLT_MAX : 1.5f * ch.GetCharacterPadding();
        if (ch.SetShape(want_crouch ? s.crouch.GetPtr() : s.stand.GetPtr(), allowed, bp_filter_ref, layer_filter,
                        body_filter, JPH::ShapeFilter(), *_env.temp)) {
            swap_shape(s, want_crouch);
        }
    }
    s.stand_blocked = !want_crouch && s.crouching;

    // --- the ground it is on, with its velocity as it is now (a platform's changed since the last step)
    if (ch.IsSupported()) {
        ch.UpdateGroundVelocity();
    }
    const CharacterGround3D previous_ground = s.ground;
    const bool on_ground = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    const Vec3f ground_velocity = on_ground ? from_jolt(ch.GetGroundVelocity()) : Vec3f{};
    const f32 vertical_previous = s.vertical;
    const f32 ground_vertical = dot(ground_velocity, up);
    const bool moving_toward = vertical_previous - ground_vertical < 0.1f;
    const bool standing = on_ground && moving_toward;
    // Launched: what came from outside sends it away from the ground it is on. That is a jump that was not asked for: it is
    // not standing, is not put back on the floor, makes no jump of its own this step and has no coyote time after it.
    const bool launched = external && on_ground && !moving_toward;

    // --- jump: a press (rising edge), buffered if asked; allowed on the ground or inside the coyote time
    const bool pressed = s.input.jump && !s.prev_jump;
    s.prev_jump = s.input.jump;
    if (pressed) {
        s.jump_buffer = d.jump_buffer_seconds;
    } else {
        s.jump_buffer = std::max(0.0f, s.jump_buffer - dt);
    }
    if (standing) {
        s.coyote = d.coyote_seconds;
    } else {
        s.coyote = std::max(0.0f, s.coyote - dt);
    }
    if (launched) {
        s.coyote = 0.0f;
    }
    const bool wants_jump = pressed || s.jump_buffer > 0.0f;
    const bool jump = wants_jump && (standing || s.coyote > 0.0f);
    if (jump) {
        s.jump_buffer = 0.0f;
        s.coyote = 0.0f;
    }
    s.jumped = jump;

    // --- the velocity it asks for
    const f32 g_magnitude = length(world_gravity) * d.gravity_scale;
    const Vec3f gravity = -up * g_magnitude;
    // The wish: the command's move across `up`, at the speed of the stance.
    Vec3f wish_dir{};
    {
        const Vec3f forward = normalize(flat(s.input.forward, up));
        const Vec3f right = normalize(cross(forward, up));
        const Vec3f dir = right * s.input.move.x + forward * s.input.move.y;
        const f32 n = length(dir);
        wish_dir = n > 1.0f ? dir / n : dir;
    }
    const bool has_wish = length_squared(wish_dir) > 1.0e-8f;
    // A character with a resource that has run out cannot sprint.
    const bool sprinting = s.input.sprint && !(d.traversal.enabled && d.traversal.sprint_needs_resource && s.trav.resource.exhausted);
    const f32 top_speed = s.crouching ? d.crouch_speed : (sprinting ? d.sprint_speed : d.walk_speed);
    Vec3f wish = wish_dir * top_speed;

    // The speed along `up` that is its own (not the ground's slope under it): what it carries into the air.
    f32 vertical_cmd = 0.0f;
    Vec3f vel;
    const bool supported = ch.IsSupported();
    // Launched, it is in the air from this step on: the ground does not brake what sent it off.
    if (standing || (supported && !launched)) {
        // On the ground (or against a slope too steep to stand on, which it still pushes off with its feet): the ground's
        // own velocity, plus a velocity along its surface (the speed is held on a slope as it is on the flat: along the
        // surface, not across the plan), approached at the acceleration. Gravity is not added while it stands: the
        // stick-to-ground below holds it down, and a walk up a slope would lose g sin(angle) a step.
        // A ground normal steeper than it may stand on (a stair walk that put it on the corner of a step says it is
        // on the ground) is not a plane to walk along: the speed stays across `up`.
        Vec3f normal = up;
        if (standing) {
            normal = from_jolt(ch.GetGroundNormal());
            if (ch.IsSlopeTooSteep(to_jolt(normal))) {
                normal = up;
            }
        }
        const auto along = [&](Vec3f v) { return v - normal * dot(v, normal); };
        Vec3f target = along(wish);
        const f32 target_length = length(target);
        if (target_length > 1.0e-6f) {
            target = target * (length(wish) / target_length);
        }
        // Relative to the ground as it was when that speed was achieved (a platform that has just got going is not
        // a character that is moving backwards on it).
        const Vec3f relative = along(s.relative);
        const f32 rate = (has_wish ? d.acceleration : d.deceleration) * std::max(0.0f, d.ground_friction);
        const Vec3f moving = approach(relative, target, rate * dt);
        // Going up a slope, what is sent to Jolt has no part along `up` (a velocity that leaves the ground is a
        // character that is leaving it: it would not be held to it): the horizontal vector whose slide along the surface
        // is `moving`. Going down it is `moving` itself (a horizontal one would run off the surface).
        const f32 tilt = dot(normal, up);
        const Vec3f command = tilt > 0.1f && dot(moving, up) > 0.0f ? moving - normal * (dot(moving, up) / tilt)
                            : tilt > 0.1f ? moving : flat(moving, up);
        if (standing) {
            vel = ground_velocity + command;
            vertical_cmd = ground_vertical;
            if (jump) {
                // The jump replaces what went up or down with the speed the descriptor says (the ground's own on top).
                vertical_cmd = ground_vertical + d.jump_speed + dot(gravity, up) * dt;
                vel = flat(vel, up) + up * vertical_cmd;
            }
        } else {
            vertical_cmd = (jump ? d.jump_speed : vertical_previous) + dot(gravity, up) * dt;
            vel = flat(from_jolt(ch.GetGroundVelocity()), up) + command + up * vertical_cmd;
        }
    } else {
        // In the air: vertical speed as it was (a ceiling has stopped it, if it hit one) plus gravity, across `up` the
        // approach to the wish at the air acceleration.
        vertical_cmd = (jump ? d.jump_speed : vertical_previous) + dot(gravity, up) * dt; // jump: coyote time
        vel = up * vertical_cmd;
        const Vec3f horizontal_previous = flat(s.control, up);
        if (has_wish && d.air_acceleration > 0.0f) {
            vel += approach(horizontal_previous, wish, d.air_acceleration * dt);
        } else {
            vel += horizontal_previous;
        }
    }
    const f32 falling_speed = -(dot(vel, up) - ground_vertical);

    // Friction against the walls it is pressed to (found by the last step): Coulomb, the speed along the wall loses up to
    // wall_friction times the speed into it. Zero friction leaves a slide along a wall at full speed. (Jolt's solver
    // keeps no friction of its own, and its hook is not called for a contact the character already rests on.)
    if (d.wall_friction > 0.0f) {
        const Vec3f frame = supported ? from_jolt(ch.GetGroundVelocity()) : Vec3f{};
        for (const CharacterContact3D& c : s.contacts) {
            if (c.sensor || std::fabs(dot(c.normal, up)) > 0.5f) {
                continue;
            }
            const f32 into = dot(vel - frame, c.normal);
            if (!(into < 0.0f)) {
                continue;
            }
            const Vec3f relative = vel - frame;
            const Vec3f tangent = relative - c.normal * dot(relative, c.normal);
            const f32 speed = length(tangent);
            if (speed > 1.0e-6f) {
                vel -= tangent * (std::min(speed, d.wall_friction * -into) / speed);
            }
        }
    }

    ch.SetLinearVelocity(to_jolt(vel));

    // --- move it: Jolt's ExtendedUpdate (slide, stick to the floor, walk stairs), written out here so that the move
    // of the slide itself can be told from the snaps that follow it (a step down to the floor is not a speed).
    const JPH::Vec3 gravity_j = to_jolt(gravity);
    const JPH::Vec3 desired_velocity = to_jolt(vel);
    ch.SetLinearVelocity(ch.CancelVelocityTowardsSteepSlopes(desired_velocity));
    const JPH::RVec3 old_position_j = ch.GetPosition();
    bool ground_to_air = ch.IsSupported();
    ch.Update(dt, gravity_j, bp_filter_ref, layer_filter, body_filter, JPH::ShapeFilter(), *_env.temp);
    const JPH::RVec3 slid_position_j = ch.GetPosition();
    if (ch.IsSupported()) {
        ground_to_air = false;
    }
    const f32 step_down = d.step_down_distance;
    if (ground_to_air && step_down > 0.0f) {
        // Not moving up: put it back on the floor it has just left (a hump's far side, a ramp down).
        if (JPH::Vec3(slid_position_j - old_position_j).Dot(up_j) / dt <= 1.0e-6f) {
            ch.StickToFloor(-up_j * step_down, bp_filter_ref, layer_filter, body_filter, JPH::ShapeFilter(), *_env.temp);
        }
    }
    if (d.max_step_height > 0.0f) {
        JPH::Vec3 desired_step = desired_velocity * dt;
        desired_step -= desired_step.Dot(up_j) * up_j;
        const f32 desired_length = desired_step.Length();
        if (desired_length > 0.0f) {
            JPH::Vec3 achieved = JPH::Vec3(ch.GetPosition() - old_position_j);
            achieved -= achieved.Dot(up_j) * up_j;
            const JPH::Vec3 forward = desired_step / desired_length;
            achieved = std::max(0.0f, achieved.Dot(forward)) * forward;
            const f32 achieved_length = achieved.Length();
            if (achieved_length + 1.0e-4f < desired_length && ch.CanWalkStairs(desired_velocity)) {
                // At least 2 cm forward, however short the step (a small dt must still clear the edge).
                const JPH::Vec3 step_forward = forward * std::max(0.02f, desired_length - achieved_length);
                JPH::Vec3 test = -JPH::Vec3(ch.GetGroundNormal());
                test -= test.Dot(up_j) * up_j;
                test = test.NormalizedOr(forward);
                if (test.Dot(forward) < 0.25881905f) { // cos 75 degrees
                    test = forward;
                }
                test *= 0.15f;
                ch.WalkStairs(dt, up_j * d.max_step_height, step_forward, test, JPH::Vec3::sZero(), bp_filter_ref,
                              layer_filter, body_filter, JPH::ShapeFilter(), *_env.temp);
            }
        }
    }
    const Vec3f old_position = from_jolt(old_position_j);
    const Vec3f new_position = from_jolt(ch.GetPosition());
    // The velocity it achieved, and (for the next step's control) the same without the move along `up` that stairs and
    // snaps add: a correction of height, not a speed.
    const Vec3f slid = from_jolt(slid_position_j) - old_position;
    s.control = (slid + flat(new_position - from_jolt(slid_position_j), up)) / dt;
    s.velocity = (new_position - old_position) / dt;
    // Its speed over the ground it stands on after this step (the ground's own velocity taken off), for the next.
    s.relative = s.control - (ch.IsSupported() ? from_jolt(ch.GetGroundVelocity()) : Vec3f{});

    // --- what it is now
    const auto ground_state = ch.GetGroundState();
    if (ground_state == JPH::CharacterBase::EGroundState::OnGround) {
        s.ground = CharacterGround3D::OnGround;
    } else if (ground_state == JPH::CharacterBase::EGroundState::OnSteepGround) {
        // Sliding: going down along the slope at more than half a metre a second.
        const f32 down = -(dot(s.velocity, up) - dot(from_jolt(ch.GetGroundVelocity()), up));
        s.ground = down > 0.5f ? CharacterGround3D::Sliding : CharacterGround3D::OnSteepSlope;
    } else {
        s.ground = CharacterGround3D::InAir;
    }
    const bool supported_now = s.ground == CharacterGround3D::OnGround;
    // The vertical speed the next step starts from: while it stands, the ground's own (walking up a slope or a step up a
    // stair is not "moving away from the ground"); otherwise the less of what it achieved and what it asked for: a
    // ceiling stops a jump (achieved is less), but rolling over the edge of a step shoots it up faster than it asked
    // and it must not carry that into the air as a hop (asked is less).
    s.vertical = supported_now && !jump && !launched ? dot(from_jolt(ch.GetGroundVelocity()), up)
                                                     : std::min(dot(s.control, up), vertical_cmd);
    s.air_time = supported_now ? 0.0f : s.air_time + dt;

    // --- events: in the order of the kinds, for this character
    const Character3D handle{index, s.generation};
    const auto emit = [&](CharacterEvent3D e) {
        e.character = handle;
        e.user_id = d.user_id;
        _events.push_back(e);
    };
    const bool was_ground = previous_ground == CharacterGround3D::OnGround;
    if (!was_ground && supported_now) {
        CharacterEvent3D e;
        e.kind = CharacterEventKind3D::Landed;
        e.speed = std::max(0.0f, falling_speed);
        e.normal = from_jolt(ch.GetGroundNormal());
        e.point = new_position;
        const Body3D g = _env.body_of(ch.GetGroundBodyID());
        e.body = g;
        e.body_user_id = g.valid() ? _env.user_id_of(g) : 0;
        emit(e);
    }
    if (was_ground && !supported_now) {
        CharacterEvent3D e;
        e.kind = CharacterEventKind3D::LeftGround;
        e.speed = dot(s.velocity, up);
        e.jumped = jump;
        e.point = old_position;
        emit(e);
    }
    const std::vector<std::pair<u32, bool>> before = std::move(s.touched);
    collect_contacts(index, s);
    s.touched.clear();
    bool ceiling = false;
    Vec3f ceiling_point{}, ceiling_normal{};
    for (const CharacterContact3D& c : s.contacts) {
        s.touched.push_back({c.body.index, c.sensor});
        if (!c.sensor && dot(c.normal, up) < -0.7f) {
            ceiling = true;
            ceiling_point = c.point;
            ceiling_normal = c.normal;
        }
    }
    if (ceiling && !s.touching_ceiling && dot(vel, up) - ground_vertical > 0.1f) {
        CharacterEvent3D e;
        e.kind = CharacterEventKind3D::HitHead;
        e.speed = dot(vel, up) - ground_vertical;
        e.point = ceiling_point;
        e.normal = ceiling_normal;
        emit(e);
    }
    s.touching_ceiling = ceiling;
    // Contacts begun (in the order of body numbers), then ended.
    emit_contact_changes(index, s, before);
    if (d.traversal.enabled) {
        flush_traversal_events(index, s);
    }
}

void CharacterSet::push_event(u32 index, const Slot& s, CharacterEvent3D e) {
    e.character = {index, s.generation};
    e.user_id = s.desc.user_id;
    _events.push_back(e);
}

void CharacterSet::emit_contact_changes(u32 index, Slot& s, const std::vector<std::pair<u32, bool>>& before) {
    for (const CharacterContact3D& c : s.contacts) {
        const bool known = std::any_of(before.begin(), before.end(), [&](const auto& p) { return p.first == c.body.index; });
        if (!known) {
            CharacterEvent3D e;
            e.kind = CharacterEventKind3D::ContactBegin;
            e.body = c.body;
            e.body_user_id = c.user_id;
            e.sensor = c.sensor;
            e.point = c.point;
            e.normal = c.normal;
            push_event(index, s, e);
        }
    }
    for (const auto& [slot_index, sensor] : before) {
        const bool still = std::any_of(s.touched.begin(), s.touched.end(), [&](const auto& p) { return p.first == slot_index; });
        if (!still) {
            CharacterEvent3D e;
            e.kind = CharacterEventKind3D::ContactEnd;
            // The generation of the body that was there: unknown now if it has gone; the user id is looked up if it is not.
            const Body3D live = _env.body_of_slot(slot_index);
            e.body = live;
            e.body_user_id = live.valid() ? _env.user_id_of(live) : 0;
            e.sensor = sensor;
            push_event(index, s, e);
        }
    }
}

void CharacterSet::save_header(std::vector<u8>& bytes) const {
    Out out{bytes};
    out.u32v(static_cast<u32>(_slots.size()));
    for (const Slot& s : _slots) {
        out.u32v(s.generation);
        // 0 free slot, 1 a character, 2 a character handed over to a ragdoll (a world with none hashes as before); 4 added while
        // a velocity or an impulse waits for the next step (its bytes are at the end of the character's).
        const bool external = s.alive && s.has_external();
        out.bytes.push_back(s.alive ? static_cast<u8>((s.disabled ? 2 : 1) | (external ? 4 : 0)) : 0);
        if (!s.alive) {
            continue;
        }
        out.u64v(s.desc.user_id);
        out.u32v(s.inner.index);
        out.u32v(s.inner.generation);
        out.f32v(s.input.move.x);
        out.f32v(s.input.move.y);
        out.vec(s.input.forward);
        out.b(s.input.jump);
        out.b(s.input.crouch);
        out.b(s.input.sprint);
        out.b(s.prev_jump);
        out.f32v(s.jump_buffer);
        out.f32v(s.coyote);
        out.vec(s.up);
        out.vec(s.up_target);
        out.b(s.up_explicit);
        out.b(s.crouching);
        out.b(s.stand_blocked);
        out.vec(s.velocity);
        out.vec(s.control);
        out.f32v(s.vertical);
        out.vec(s.relative);
        out.f32v(s.air_time);
        out.b(s.jumped);
        out.bytes.push_back(static_cast<u8>(s.ground));
        out.b(s.touching_ceiling);
        out.u32v(static_cast<u32>(s.touched.size()));
        for (const auto& [slot_index, sensor] : s.touched) {
            out.u32v(slot_index);
            out.b(sensor);
        }
        // The character's own position and what it stands on: in Jolt's state too, but the hash reads this.
        out.vec(from_jolt(s.ch->GetPosition()));
        out.vec(from_jolt(s.ch->GetLinearVelocity()));
        out.bytes.push_back(static_cast<u8>(s.ch->GetGroundState()));
        out.u32v(s.ch->GetGroundBodyID().GetIndexAndSequenceNumber());
        out.vec(from_jolt(s.ch->GetGroundNormal()));
        out.vec(from_jolt(s.ch->GetGroundVelocity()));
        // Grab and climb: only for a character that has it (the others hash as they always did).
        if (s.desc.traversal.enabled) {
            save_trav(out, s.trav);
        }
        if (external) {
            out.b(s.external_set);
            out.vec(s.external_velocity);
            out.vec(s.external_impulse);
        }
    }
}

std::unique_ptr<CharacterSet::Parsed> CharacterSet::parse_header(const u8*& cursor, const u8* end) const {
    In in{cursor, end};
    const u32 count = in.u32v();
    if (in.failed || count != _slots.size()) {
        return nullptr;
    }
    auto parsed = std::make_unique<Parsed>();
    parsed->slots.resize(count);
    for (u32 i = 0; i < count; ++i) {
        Parsed::One& o = parsed->slots[i];
        o.generation = in.u32v();
        if (end - in.p < 1) {
            return nullptr;
        }
        bool external = false;
        {
            const u8 flag = *in.p++;
            o.alive = (flag & 3u) != 0;
            o.disabled = (flag & 3u) == 2;
            external = (flag & 4u) != 0;
        }
        if (in.failed || o.generation != _slots[i].generation || o.alive != _slots[i].alive) {
            return nullptr;
        }
        if (!o.alive) {
            continue;
        }
        o.user_id = in.u64v();
        const u32 inner_index = in.u32v();
        const u32 inner_generation = in.u32v();
        if (inner_index != _slots[i].inner.index || inner_generation != _slots[i].inner.generation) {
            return nullptr;
        }
        o.input.move.x = in.f32v();
        o.input.move.y = in.f32v();
        o.input.forward = in.vec();
        o.input.jump = in.b();
        o.input.crouch = in.b();
        o.input.sprint = in.b();
        o.prev_jump = in.b();
        o.jump_buffer = in.f32v();
        o.coyote = in.f32v();
        o.up = in.vec();
        o.up_target = in.vec();
        o.up_explicit = in.b();
        o.crouching = in.b();
        o.stand_blocked = in.b();
        o.velocity = in.vec();
        o.control = in.vec();
        o.vertical = in.f32v();
        o.relative = in.vec();
        o.air_time = in.f32v();
        o.jumped = in.b();
        if (end - in.p < 1) {
            return nullptr;
        }
        o.ground = *in.p++;
        o.touching_ceiling = in.b();
        const u32 touched = in.u32v();
        if (in.failed || touched > (1u << 20)) {
            return nullptr;
        }
        for (u32 t = 0; t < touched; ++t) {
            const u32 slot_index = in.u32v();
            const bool sensor = in.b();
            o.touched.push_back({slot_index, sensor});
        }
        // The hash-only tail: position (3), velocity (3), ground state (1), ground body (4), normal (3), velocity (3).
        in.vec();
        in.vec();
        if (end - in.p < 1) {
            return nullptr;
        }
        ++in.p;
        in.u32v();
        in.vec();
        in.vec();
        if (in.failed) {
            return nullptr;
        }
        if (_slots[i].desc.traversal.enabled) {
            o.trav = TravData{};
            if (!parse_trav(in, o.trav)) {
                return nullptr;
            }
        }
        if (external) {
            o.external_set = in.b();
            o.external_velocity = in.vec();
            o.external_impulse = in.vec();
            if (in.failed) {
                return nullptr;
            }
        }
    }
    cursor = in.p;
    return parsed;
}

void CharacterSet::save_jolt(JPH::StateRecorder& out) const {
    for (const Slot& s : _slots) {
        if (s.alive) {
            s.ch->SaveState(out);
        }
    }
}

void CharacterSet::prepare_restore(const Parsed& parsed) {
    // The stance first, before the physics system restores: a body's saved place is its centre of mass, and which
    // centre it has depends on the shape (restoring into the other stance would move the capsule by the difference).
    for (u32 i = 0; i < _slots.size(); ++i) {
        Slot& s = _slots[i];
        const Parsed::One& o = parsed.slots[i];
        if (!s.alive || o.crouching == s.crouching) {
            continue;
        }
        const JPH::Shape* shape = o.crouching ? s.crouch.GetPtr() : s.stand.GetPtr();
        s.ch->SetInnerBodyShape(shape);
        // Its own shape, with no clearance check (the state restored is one that fitted).
        s.ch->SetShape(shape, FLT_MAX, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter(), JPH::BodyFilter(),
                       JPH::ShapeFilter(), *_env.temp);
        s.crouching = o.crouching;
    }
}

bool CharacterSet::restore(Parsed& parsed, JPH::StateRecorder& in) {
    for (u32 i = 0; i < _slots.size(); ++i) {
        Slot& s = _slots[i];
        const Parsed::One& o = parsed.slots[i];
        if (!s.alive) {
            continue;
        }
        if (s.disabled != o.disabled) {
            s.disabled = o.disabled;
            _env.world->set_no_collisions(s.inner, s.disabled);
        }
        s.input = o.input;
        s.prev_jump = o.prev_jump;
        s.jump_buffer = o.jump_buffer;
        s.coyote = o.coyote;
        s.up = o.up;
        s.up_target = o.up_target;
        s.up_explicit = o.up_explicit;
        s.stand_blocked = o.stand_blocked;
        s.velocity = o.velocity;
        s.control = o.control;
        s.vertical = o.vertical;
        s.relative = o.relative;
        s.air_time = o.air_time;
        s.jumped = o.jumped;
        s.ground = static_cast<CharacterGround3D>(o.ground);
        s.touching_ceiling = o.touching_ceiling;
        s.touched = o.touched;
        if (s.desc.traversal.enabled) {
            s.trav = o.trav;
        }
        s.external_set = o.external_set;
        s.external_velocity = o.external_velocity;
        s.external_impulse = o.external_impulse;
        // Up is a property of the Jolt character that its own state does not carry.
        s.ch->SetUp(to_jolt(s.up));
        s.ch->RestoreState(in);
        s.ch->SetUp(to_jolt(s.up));
        collect_contacts(i, s);
    }
    return !static_cast<const JPH::StreamIn&>(in).IsFailed();
}

} // namespace physics3d_detail
} // namespace kin
