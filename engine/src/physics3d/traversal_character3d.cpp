// Grab and climb on the character controller (kin/physics3d/traversal3d.hpp): the grips of each limb, the resource, the
// hanging and mantling modes. Written over the same public questions a game can ask (rays, sweeps, a body's place and
// velocity) plus Jolt's CharacterVirtual for the move itself, so that it stays a pure function of the character's state,
// its commands and dt.

#include "character3d_impl.hpp"

#include <Jolt/Physics/Collision/ShapeFilter.h>

#include <algorithm>
#include <cmath>

namespace kin {
namespace physics3d_detail {

namespace {

constexpr f32 two_pi = 6.2831853f;

bool finite3(Vec3f v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
Vec3f flat(Vec3f v, Vec3f up) { return v - up * dot(v, up); }
f32 smooth(f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
Vec3f any_perpendicular(Vec3f up) {
    return normalize(cross(up, std::fabs(up.x) < 0.9f ? Vec3f{1.0f, 0.0f, 0.0f} : Vec3f{0.0f, 0.0f, 1.0f}));
}

bool holding(const GripData& g) { return g.phase == GripPhase3D::Held || g.phase == GripPhase3D::Slipping; }

GripData grip_of(const GripAnchor3D& a) {
    GripData g;
    g.phase = GripPhase3D::Reaching;
    g.kind = a.kind;
    g.body = a.body;
    g.local_point = a.local_point;
    g.local_normal = a.local_normal;
    g.quality = a.quality;
    g.drain = a.drain;
    g.slip = a.slip;
    g.tag = a.surface_tag;
    g.fixed = a.fixed;
    g.ledge = a.ledge;
    return g;
}

GripAnchor3D anchor_of(const GripData& g) {
    GripAnchor3D a;
    a.body = g.body;
    a.kind = g.kind;
    a.local_point = g.local_point;
    a.local_normal = g.local_normal;
    a.quality = g.quality;
    a.surface_tag = g.tag;
    a.drain = g.drain;
    a.slip = g.slip;
    a.fixed = g.fixed;
    a.ledge = g.ledge;
    return a;
}

} // namespace

// --- the public face ---------------------------------------------------------------------------------------------------

void CharacterSet::set_traversal_input(Character3D c, const TraversalInput3D& input) {
    Slot* s = find(c);
    if (s == nullptr || !s->desc.traversal.enabled || !finite3(input.reach)) {
        return;
    }
    s->trav.input = input;
}

TraversalInput3D CharacterSet::traversal_input(Character3D c) const {
    const Slot* s = find(c);
    return s != nullptr && s->desc.traversal.enabled ? s->trav.input : TraversalInput3D{};
}

void CharacterSet::set_traversal_resource(Character3D c, f32 value) {
    Slot* s = find(c);
    if (s == nullptr || !s->desc.traversal.enabled || !std::isfinite(value)) {
        return;
    }
    Resource3D& r = s->trav.resource;
    r.max = s->desc.traversal.resource.max;
    r.value = std::clamp(value, 0.0f, r.max);
    r.exhausted = r.value <= 0.0f;
    // As if it had just been spent: it comes back after its delay.
    r.delay = s->desc.traversal.resource.regen_delay;
}

TraversalState3D CharacterSet::traversal(Character3D c) const {
    TraversalState3D out;
    const Slot* sp = find(c);
    if (sp == nullptr || !sp->desc.traversal.enabled) {
        return out;
    }
    const Slot& s = *sp;
    const TravData& t = s.trav;
    const TraversalDesc3D& td = s.desc.traversal;
    out.enabled = true;
    out.mode = t.mode;
    out.limb_count = std::clamp<u32>(td.limb_count, 1u, max_grip_limbs3d);
    out.resource = t.resource;
    out.swing = t.swing;
    out.hang_place = t.hang_place;
    out.hang_time = t.hang_time;
    out.mantle_progress = t.mode == TraversalMode3D::Mantling ? std::min(1.0f, t.mantle_time / std::max(td.mantle_seconds, 1.0e-4f)) : 0.0f;
    const Vec3f forward = t.facing != Vec3f{} ? t.facing : normalize(flat(s.input.forward, s.up));
    Vec3f centre{}, normal_sum{};
    u32 held = 0;
    for (u32 h = 0; h < max_grip_limbs3d; ++h) {
        const GripData& g = t.grips[h];
        GripState3D& o = out.grips[h];
        o.phase = g.phase;
        o.kind = g.kind;
        o.body = g.body;
        o.local_point = g.local_point;
        o.local_normal = g.local_normal;
        o.strength = holding(g) ? std::clamp(g.quality * g.fade, 0.0f, 1.0f) : 0.0f;
        o.reach_progress = g.phase == GripPhase3D::Reaching ? g.reach_t : (holding(g) ? 1.0f : 0.0f);
        o.slip_time = g.slip_time;
        o.slip_speed = g.slipping ? g.slip * td.slip_speed : 0.0f;
        o.surface_tag = g.tag;
        o.drain = g.drain;
        o.fixed = g.fixed;
        o.ledge = g.ledge;
        o.quality = g.quality;
        if (g.phase == GripPhase3D::Free) {
            continue;
        }
        Vec3f p{}, n{};
        if (anchor_world(s, g, false, 0.0f, p, n)) {
            o.body_user_id = _env.user_id_of(g.body);
            o.point = p;
            o.normal = normalize(n);
            const SurfaceFrame3D f = surface_frame(o.normal, s.up, forward);
            o.right = f.right;
            o.surface_up = f.up;
            if (holding(g)) {
                centre += p;
                normal_sum += o.normal;
                ++held;
            }
        }
    }
    out.held = held;
    if (held > 0) {
        out.anchor_centre = centre / static_cast<f32>(held);
        out.surface_normal = normalize(normal_sum);
    }
    return out;
}

bool CharacterSet::anchor_world(const Slot& s, const GripData& g, bool predicted, f32 dt, Vec3f& point, Vec3f& normal) const {
    const World3D& world = *_env.world;
    if (!g.body.valid() || !world.contains(g.body)) {
        return false;
    }
    const Vec3f position = world.position(g.body);
    const Quatf rotation = world.rotation(g.body);
    point = position + rotation.rotate(g.local_point);
    normal = rotation.rotate(g.local_normal);
    if (g.kind == GripKind3D::Rope) {
        // A rope's anchor is a share of a segment's length from its middle (so that it keeps its place along a rope that
        // changes length), and a rope has no face: the hand is on the side the character faces it from.
        Vec3f tangent{};
        const auto ref = world.rope_segment_of(g.body);
        if (!ref || !world.rope_point_at(ref->rope, ref->segment, g.local_point.y, point, tangent)) {
            return false;
        }
        if (s.trav.facing != Vec3f{}) {
            normal = -s.trav.facing;
        }
    }
    if (predicted) {
        // Where the body's velocity (a platform's, a crate's) takes the point over this step.
        const Vec3f arm = point - world.centre_of_mass_position(g.body);
        point += (world.linear_velocity(g.body) + cross(world.angular_velocity(g.body), arm)) * dt;
    }
    return true;
}

void CharacterSet::add_traversal_event(CharacterEventKind3D kind, u32 hand, u8 reason, Body3D body, Vec3f point,
                                       Vec3f normal, f32 speed) {
    CharacterEvent3D e;
    e.kind = kind;
    e.hand = static_cast<u8>(hand);
    e.reason = reason;
    e.body = body;
    e.body_user_id = body.valid() ? _env.user_id_of(body) : 0;
    e.point = point;
    e.normal = normal;
    e.speed = speed;
    _traversal_events.push_back(e);
}

void CharacterSet::release_grip(Slot& s, u32 hand, GripRelease3D reason) {
    GripData& g = s.trav.grips[hand];
    if (holding(g)) {
        Vec3f p{}, n{};
        anchor_world(s, g, false, 0.0f, p, n);
        add_traversal_event(CharacterEventKind3D::GripReleased, hand, static_cast<u8>(reason), g.body, p, normalize(n), 0.0f);
    }
    // Let go by the character (not by the player's command): the command must be given up before this limb grabs again.
    const bool latch = reason != GripRelease3D::Input && s.trav.input.grab[hand];
    g = GripData{};
    g.phase = GripPhase3D::Released;
    g.latch = latch;
}

void CharacterSet::leave_hang(Slot& s, bool fell, GripRelease3D why) {
    TravData& t = s.trav;
    t.mode = TraversalMode3D::Walking;
    t.swing = {};
    t.has_hang_place = false;
    t.hang_time = 0.0f;
    t.mantle_time = 0.0f;
    // The walking controller carries on with the speed the hanging gave it, in the air.
    const Vec3f up = s.up;
    s.control = flat(s.velocity, up);
    s.vertical = dot(s.velocity, up);
    s.relative = {};
    s.ground = CharacterGround3D::InAir;
    s.air_time = 0.0f;
    s.prev_jump = s.input.jump;
    s.jump_buffer = 0.0f;
    s.coyote = 0.0f;
    if (fell) {
        add_traversal_event(CharacterEventKind3D::Fell, 0, static_cast<u8>(why), {}, from_jolt(s.ch->GetPosition()), up, s.vertical);
    }
}

void CharacterSet::flush_traversal_events(u32 index, const Slot& s) {
    // Deterministic order: by kind, then by hand (the order the grips were processed in is not part of the contract).
    std::stable_sort(_traversal_events.begin(), _traversal_events.end(), [](const CharacterEvent3D& a, const CharacterEvent3D& b) {
        if (a.kind != b.kind) return static_cast<u8>(a.kind) < static_cast<u8>(b.kind);
        return a.hand < b.hand;
    });
    for (const CharacterEvent3D& e : _traversal_events) {
        push_event(index, s, e);
    }
    _traversal_events.clear();
}

// --- the step ------------------------------------------------------------------------------------------------------------

namespace {

// Drives the character to `target` (feet) this step, by the swept move of Jolt's own Update so that it does not pass
// through what is in the way. Returns the place it got to.
Vec3f move_to(JPH::CharacterVirtual& ch, JPH::PhysicsSystem& system, u8 layer, const MaskFilter& layer_filter,
              const JPH::BodyFilter& body_filter, JPH::TempAllocator& temp, Vec3f target, f32 dt) {
    const Vec3f from = from_jolt(ch.GetPosition());
    ch.SetLinearVelocity(to_jolt((target - from) / dt));
    ch.Update(dt, JPH::Vec3::sZero(), system.GetDefaultBroadPhaseLayerFilter(layer), layer_filter, body_filter,
              JPH::ShapeFilter(), temp);
    return from_jolt(ch.GetPosition());
}

} // namespace

bool CharacterSet::update_traversal(u32 index, Slot& s, f32 dt) {
    (void)index;
    TravData& t = s.trav;
    const TraversalDesc3D& td = s.desc.traversal;
    const CharacterDesc3D& d = s.desc;
    JPH::CharacterVirtual& ch = *s.ch;
    World3D& world = *_env.world;
    const Vec3f up = s.up;
    const u32 limbs = std::clamp<u32>(td.limb_count, 1u, max_grip_limbs3d);
    const TraversalInput3D& in = t.input;
    const ResourceConfig3D& rc = td.resource;
    const Vec3f feet = from_jolt(ch.GetPosition());
    // What it was doing when the step began (the hanging move changes `s.velocity` below).
    const Vec3f velocity_in = s.velocity;

    // The commands' edges and the latches (a limb the character let go of waits for its command to be given up).
    const bool pull_edge = in.pull_up && !t.prev_pull;
    const bool jump_off_edge = in.jump_off && !t.prev_jump_off;
    t.prev_pull = in.pull_up;
    t.prev_jump_off = in.jump_off;
    for (u32 h = 0; h < max_grip_limbs3d; ++h) {
        if (!in.grab[h]) {
            t.grips[h].latch = false;
        }
    }

    if (t.mode == TraversalMode3D::Mantling) {
        return mantle_step(index, s, dt);
    }

    // Which way it faces and where its shoulders are. Hanging: toward the surface it hangs from.
    Vec3f facing = normalize(flat(s.input.forward, up));
    if (t.mode != TraversalMode3D::Walking && t.facing != Vec3f{}) {
        facing = t.facing;
    }
    if (facing == Vec3f{}) {
        facing = any_perpendicular(up);
    }
    const Vec3f right = normalize(cross(facing, up));
    const Vec3f reach_dir = length(in.reach) > 1.0e-6f ? normalize(in.reach) : facing;
    const auto shoulder = [&](u32 h) {
        const Vec3f sh = td.limbs[h].shoulder;
        return feet + right * sh.x + up * sh.y + facing * sh.z;
    };
    const auto held_count = [&] {
        u32 n = 0;
        for (u32 h = 0; h < limbs; ++h) {
            n += holding(t.grips[h]) ? 1u : 0u;
        }
        return n;
    };

    GripRelease3D last_reason = GripRelease3D::Lost;
    bool released_any = false;
    const auto release = [&](u32 h, GripRelease3D reason) {
        if (holding(t.grips[h])) {
            released_any = true;
            last_reason = reason;
        }
        release_grip(s, h, reason);
    };

    // A limb the previous step let go of is free again now.
    for (u32 h = 0; h < max_grip_limbs3d; ++h) {
        if (t.grips[h].phase == GripPhase3D::Released) {
            const bool latch = t.grips[h].latch;
            t.grips[h] = GripData{};
            t.grips[h].latch = latch;
        }
    }

    // --- drop: everything let go (a level: while it is given, nothing is taken either)
    if (in.drop) {
        for (u32 h = 0; h < limbs; ++h) {
            GripData& g = t.grips[h];
            if (holding(g)) {
                release(h, GripRelease3D::Drop);
            } else if (g.phase == GripPhase3D::Reaching) {
                g = GripData{};
            }
            t.grips[h].latch = t.grips[h].latch || in.grab[h];
        }
    }

    // --- the limbs, in number order
    const Layer3DMask own_mask = mask_of(s);
    for (u32 h = 0; h < limbs; ++h) {
        GripData& g = t.grips[h];
        switch (g.phase) {
        case GripPhase3D::Held:
        case GripPhase3D::Slipping:
            if (!world.contains(g.body)) {
                release(h, GripRelease3D::Lost);
            } else if (!in.grab[h]) {
                release(h, GripRelease3D::Input);
            }
            break;
        case GripPhase3D::Reaching: {
            Vec3f p{}, n{};
            if (!in.grab[h] || !anchor_world(s, g, false, 0.0f, p, n) || length(p - shoulder(h)) > td.reach * 1.6f + 0.25f) {
                g = GripData{};
                break;
            }
            g.reach_t += dt / std::max(td.reach_seconds, 1.0e-4f);
            if (g.reach_t >= 1.0f) {
                g.reach_t = 1.0f;
                g.phase = GripPhase3D::Held;
                if (t.resource.take(rc, td.grab_cost)) {
                    add_traversal_event(CharacterEventKind3D::Exhausted, 0, 0, {}, feet, up, t.resource.value);
                }
                add_traversal_event(CharacterEventKind3D::Gripped, h, 0, g.body, p, normalize(n), 0.0f);
            }
            break;
        }
        case GripPhase3D::Free:
        case GripPhase3D::Released: {
            if (!in.grab[h] || g.latch || in.drop || t.resource.exhausted) {
                break;
            }
            GripProbeDesc3D probe;
            probe.origin = shoulder(h);
            probe.direction = reach_dir;
            probe.up = up;
            probe.forward = facing;
            probe.reach = td.reach;
            probe.radius = td.probe_radius;
            probe.spread = td.probe_spread;
            probe.mask = td.grip_mask;
            probe.ignore = s.inner;
            probe.min_grip = td.min_grip;
            probe.ledge_search_height = td.ledge_search_height;
            probe.clearance_radius = d.radius;
            probe.clearance_height = d.height;
            const std::vector<GripCandidate3D> found = world.probe_grips(probe);
            for (const GripCandidate3D& c : found) {
                // Not on top of what another hand has (or is going for).
                bool near_other = false;
                for (u32 o = 0; o < limbs && !near_other; ++o) {
                    Vec3f op{}, on{};
                    if (o != h && t.grips[o].phase != GripPhase3D::Free && t.grips[o].phase != GripPhase3D::Released &&
                        anchor_world(s, t.grips[o], false, 0.0f, op, on)) {
                        near_other = length(op - c.point) < td.min_hand_separation;
                    }
                }
                if (near_other) {
                    continue;
                }
                const GripAnchor3D anchor = grip_provider(c.kind).attach(world, c);
                g = grip_of(anchor);
                if (td.reach_seconds <= 0.0f) {
                    g.phase = GripPhase3D::Held;
                    g.reach_t = 1.0f;
                    if (t.resource.take(rc, td.grab_cost)) {
                        add_traversal_event(CharacterEventKind3D::Exhausted, 0, 0, {}, feet, up, t.resource.value);
                    }
                    add_traversal_event(CharacterEventKind3D::Gripped, h, 0, g.body, c.point, c.normal, 0.0f);
                }
                break;
            }
            break;
        }
        }
    }
    // --- what is the character doing now
    bool just_hung = false;
    const auto walking_tick = [&] {
        // The resource on the ground: sprinting spends it, otherwise it comes back after its delay.
        const bool sprinting = s.input.sprint && s.input.move.x * s.input.move.x + s.input.move.y * s.input.move.y > 0.01f && !s.crouching &&
                               s.ground == CharacterGround3D::OnGround && !t.resource.exhausted;
        const f32 before = t.resource.value;
        if (t.resource.step(rc, dt, sprinting ? td.drain_sprint : 0.0f)) {
            add_traversal_event(CharacterEventKind3D::Exhausted, 0, 0, {}, feet, up, before);
        }
    };

    u32 held = held_count();
    if (t.mode == TraversalMode3D::Walking) {
        if (held == 0) {
            walking_tick();
            return false;
        }
        // The first hold: it hangs. A grab in the air keeps some of the speed as the start of the swing.
        just_hung = true;
        t.mode = TraversalMode3D::Hanging;
        t.has_hang_place = false;
        t.hang_time = 0.0f;
        t.swing_velocity = s.velocity * td.grab_momentum;
        s.prev_jump = s.input.jump;
    } else if (held == 0) {
        // Everything let go (or lost): it falls with the speed it had.
        leave_hang(s, true, released_any ? last_reason : GripRelease3D::Lost);
        walking_tick();
        return false;
    }

    // The surface the hands are on, as the last step left the bodies (for the commands below).
    const auto held_surface = [&](Vec3f& centre, Vec3f& normal) {
        centre = {};
        normal = {};
        u32 n = 0;
        for (u32 h = 0; h < limbs; ++h) {
            Vec3f p{}, nrm{};
            if (holding(t.grips[h]) && anchor_world(s, t.grips[h], false, 0.0f, p, nrm)) {
                centre += p;
                normal += nrm;
                ++n;
            }
        }
        if (n > 0) {
            centre = centre / static_cast<f32>(n);
        }
        normal = normalize(normal);
    };

    // --- jump off: away from the surface and up, with the speed of what it was holding
    if (jump_off_edge) {
        Vec3f centre{}, normal{};
        held_surface(centre, normal);
        Vec3f away = normalize(flat(normal, up));
        if (away == Vec3f{}) {
            away = -facing;
        }
        Vec3f carried{};
        u32 n = 0;
        bool on_rope = false;
        for (u32 h = 0; h < limbs; ++h) {
            if (holding(t.grips[h]) && world.contains(t.grips[h].body)) {
                carried += world.linear_velocity(t.grips[h].body);
                on_rope = on_rope || t.grips[h].kind == GripKind3D::Rope;
                ++n;
            }
        }
        if (n > 0) {
            carried = carried / static_cast<f32>(n);
        }
        if (on_rope) {
            // Off a rope it goes with the speed it has (the swing), not away from a face it has none of.
            carried = velocity_in;
            away = {};
        }
        for (u32 h = 0; h < limbs; ++h) {
            release(h, GripRelease3D::JumpOff);
            t.grips[h].latch = t.grips[h].latch || in.grab[h];
        }
        leave_hang(s, false, GripRelease3D::JumpOff);
        s.control = flat(carried, up) + away * td.jump_off_speed;
        s.vertical = dot(carried, up) + td.jump_up_speed;
        walking_tick();
        return false;
    }

    // --- pull up: over the ledge, if there is one and the capsule fits
    if (pull_edge) {
        s.prev_jump = s.input.jump;
        // The grip to go over from: a ledge's, else the first one held.
        int pick = -1;
        for (u32 h = 0; h < limbs; ++h) {
            if (holding(t.grips[h]) && t.grips[h].ledge) {
                pick = static_cast<int>(h);
                break;
            }
            if (pick < 0 && holding(t.grips[h])) {
                pick = static_cast<int>(h);
            }
        }
        const auto blocked = [&](MantleBlock3D why) {
            add_traversal_event(CharacterEventKind3D::MantleBlocked, 0, static_cast<u8>(why), {}, feet, up, 0.0f);
        };
        Vec3f p{}, n{};
        if (pick >= 0 && anchor_world(s, t.grips[static_cast<u32>(pick)], false, 0.0f, p, n)) {
            const GripData& g = t.grips[static_cast<u32>(pick)];
            if (t.resource.value < td.pull_up_min) {
                blocked(MantleBlock3D::Tired);
            } else {
                // Over the middle of the hands (they may be a shoulder's width apart along the edge).
                Vec3f centre{};
                u32 hands = 0;
                for (u32 h = 0; h < limbs; ++h) {
                    Vec3f hp{}, hn{};
                    if (holding(t.grips[h]) && anchor_world(s, t.grips[h], false, 0.0f, hp, hn)) {
                        centre += hp;
                        ++hands;
                    }
                }
                const Vec3f over_point = hands > 0 ? centre / static_cast<f32>(hands) : p;
                LedgeInfo ledge = find_ledge(world, g.body, over_point, normalize(n), up, td.ledge_search_height, 0.3f,
                                             td.grip_mask, s.inner);
                if (!ledge.found) {
                    blocked(MantleBlock3D::NoLedge);
                } else {
                    ledge_room(world, ledge, up, d.radius, d.height, td.mantle_inset, own_mask, s.inner);
                    bool room = ledge.clear;
                    const Vec3f over = feet + up * dot(ledge.stand - feet, up);
                    if (room) {
                        // Straight up the wall to the height of the top, then over the edge: the standing capsule must be
                        // free where it comes over and all the way in.
                        QueryFilter3D filter;
                        filter.mask = own_mask;
                        filter.ignore = s.inner;
                        const Shape3D capsule = capsule_shape(d.radius, d.height);
                        const Quatf rotation = capsule_rotation(up);
                        const Vec3f middle = up * (0.5f * d.height);
                        const std::vector<OverlapHit3D> at_edge = world.overlap(capsule, over + middle, rotation, filter);
                        room = std::none_of(at_edge.begin(), at_edge.end(), [](const OverlapHit3D& o) { return o.penetration > 0.005f; });
                        if (room) {
                            const std::optional<ShapeHit3D> sweep =
                                world.shape_cast(capsule, over + middle, rotation, ledge.stand - over, filter);
                            room = !sweep || sweep->fraction >= 1.0f;
                        }
                    }
                    if (!room) {
                        blocked(MantleBlock3D::NoRoom);
                    } else {
                        const auto local = [&](Vec3f w) {
                            return world.rotation(ledge.body).conjugate().rotate(w - world.position(ledge.body));
                        };
                        t.mantle_body = ledge.body;
                        t.mantle_path = {local(feet), local(over), local(ledge.stand - up * 0.005f)};
                        t.mantle_time = 0.0f;
                        t.mode = TraversalMode3D::Mantling;
                        t.resource.take(rc, td.pull_up_cost);
                        add_traversal_event(CharacterEventKind3D::MantleStarted, 0, 0, ledge.body, ledge.stand, up, 0.0f);
                        return mantle_step(index, s, dt);
                    }
                }
            }
        }
    }

    // --- along the surface: the move command shimmies and climbs every grip together (all or none, so that the hands
    // do not tear apart), and a slippery surface slides it by itself
    bool moved = false;
    {
        Vec2f move = s.input.move;
        const f32 n = length(Vec3f{move.x, move.y, 0.0f});
        if (n > 1.0f) {
            move = {move.x / n, move.y / n};
        }
        bool any_fixed = false;
        for (u32 h = 0; h < limbs; ++h) {
            any_fixed = any_fixed || (holding(t.grips[h]) && t.grips[h].fixed);
        }
        if (n > 0.05f && !any_fixed) {
            std::array<GripAnchor3D, max_grip_limbs3d> next{};
            bool all = true;
            u32 attempted = 0;
            for (u32 h = 0; h < limbs && all; ++h) {
                GripData& g = t.grips[h];
                if (!holding(g)) {
                    continue;
                }
                Vec3f p{}, nrm{};
                if (!anchor_world(s, g, false, 0.0f, p, nrm)) {
                    all = false;
                    break;
                }
                const SurfaceFrame3D f = surface_frame(normalize(nrm), up, facing);
                const Vec3f delta = f.right * (move.x * td.shimmy_speed * dt) + f.up * (move.y * td.climb_speed * dt);
                next[h] = anchor_of(g);
                all = grip_provider(g.kind).move(world, next[h], delta, up, facing);
                ++attempted;
            }
            if (all && attempted > 0) {
                for (u32 h = 0; h < limbs; ++h) {
                    GripData& g = t.grips[h];
                    if (holding(g)) {
                        g.body = next[h].body; // a rope's hand goes on to the next segment's body
                        g.local_point = next[h].local_point;
                        g.local_normal = next[h].local_normal;
                        g.kind = next[h].kind;
                        g.ledge = next[h].ledge;
                    }
                }
                moved = true;
            }
        }
    }
    for (u32 h = 0; h < limbs; ++h) {
        GripData& g = t.grips[h];
        if (!holding(g)) {
            continue;
        }
        const f32 slide = g.slip * td.slip_speed;
        Vec3f p{}, nrm{};
        if (!(slide > 1.0e-4f) || g.fixed || !anchor_world(s, g, false, 0.0f, p, nrm)) {
            if (g.slipping && !(slide > 1.0e-4f)) {
                g.slipping = false;
                g.phase = GripPhase3D::Held;
            }
            continue;
        }
        const Vec3f n = normalize(nrm);
        // Down the surface: -up with its part into the surface taken off (a ceiling or a floor has no down).
        Vec3f down = -up - n * dot(-up, n);
        if (length(down) < 0.2f) {
            continue;
        }
        down = normalize(down);
        GripAnchor3D a = anchor_of(g);
        if (!grip_provider(g.kind).move(world, a, down * (slide * dt), up, facing)) {
            // Off the end of the surface: nothing to hold.
            release(h, GripRelease3D::Slipped);
            continue;
        }
        g.body = a.body;
        g.local_point = a.local_point;
        g.local_normal = a.local_normal;
        g.slip_time += dt;
        if (!g.slipping) {
            g.slipping = true;
            g.phase = GripPhase3D::Slipping;
            add_traversal_event(CharacterEventKind3D::Slipped, h, 0, g.body, p, n, slide);
        }
        if (g.slip_time >= td.slip_release_seconds) {
            release(h, GripRelease3D::Slipped);
        }
    }

    held = held_count();
    if (held == 0) {
        leave_hang(s, true, released_any ? last_reason : GripRelease3D::Lost);
        walking_tick();
        return false;
    }

    // --- the resource: one hand takes the whole load, two share it; surfaces make it more or less
    {
        f32 mean_drain = 0.0f;
        for (u32 h = 0; h < limbs; ++h) {
            if (holding(t.grips[h])) {
                mean_drain += t.grips[h].drain;
            }
        }
        mean_drain /= static_cast<f32>(held);
        f32 rate = (held == 1 ? td.drain_one_hand : td.drain_two_hands) + (moved ? td.drain_moving : 0.0f);
        rate *= mean_drain;
        if (ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround) {
            rate *= td.ground_drain_factor;
        }
        const f32 before = t.resource.value;
        if (t.resource.step(rc, dt, rate)) {
            add_traversal_event(CharacterEventKind3D::Exhausted, 0, 0, {}, feet, up, before);
        }
    }

    // --- exhausted: the grips fail one hand at a time, the weakest first
    if (t.resource.exhausted) {
        std::array<u32, max_grip_limbs3d> order{};
        u32 count = 0;
        for (u32 h = 0; h < limbs; ++h) {
            if (holding(t.grips[h])) {
                order[count++] = h;
            }
        }
        std::stable_sort(order.begin(), order.begin() + count, [&](u32 a, u32 b) {
            return t.grips[a].quality < t.grips[b].quality;
        });
        for (u32 r = 0; r < count; ++r) {
            GripData& g = t.grips[order[r]];
            if (g.exhaust_t == 0.0f) {
                g.exhaust_delay = static_cast<f32>(r) * td.exhaust_interval;
            }
        }
        for (u32 r = 0; r < count; ++r) {
            const u32 h = order[r];
            GripData& g = t.grips[h];
            g.exhaust_t += dt;
            const f32 over = g.exhaust_t - g.exhaust_delay;
            g.fade = over <= 0.0f ? 1.0f : std::clamp(1.0f - over / std::max(td.exhaust_fade, 1.0e-4f), 0.0f, 1.0f);
            if (g.fade <= 0.0f) {
                release(h, GripRelease3D::Exhausted);
            }
        }
        held = held_count();
        if (held == 0) {
            leave_hang(s, true, GripRelease3D::Exhausted);
            return false;
        }
    } else {
        for (u32 h = 0; h < limbs; ++h) {
            t.grips[h].exhaust_t = 0.0f;
            t.grips[h].fade = 1.0f;
        }
    }

    // --- the hang place: the mean of the anchors (where they will be after this step), the hang's drop below it, a
    // capsule's width out from the surface
    Vec3f centre{}, centre_now{}, normal_sum{};
    u32 anchors = 0;
    for (u32 h = 0; h < limbs; ++h) {
        Vec3f p{}, n{}, now{}, ignored{};
        if (holding(t.grips[h]) && anchor_world(s, t.grips[h], true, dt, p, n) &&
            anchor_world(s, t.grips[h], false, 0.0f, now, ignored)) {
            centre += p;
            centre_now += now;
            normal_sum += n;
            ++anchors;
        }
    }
    if (anchors == 0) {
        leave_hang(s, true, GripRelease3D::Lost);
        walking_tick();
        return false;
    }
    centre = centre / static_cast<f32>(anchors);
    centre_now = centre_now / static_cast<f32>(anchors);
    const Vec3f normal = normalize(normal_sum);
    Vec3f out = flat(normal, up);
    const f32 out_length = length(out);
    if (out_length >= 0.2f) {
        out = out / out_length;
        t.facing = -out;
    } else {
        out = {};
        if (t.facing == Vec3f{}) {
            t.facing = facing;
        }
    }
    const f32 drop = td.hang_drop > 0.0f ? td.hang_drop : td.limbs[0].shoulder.y + 0.25f;
    // The shoulders of the hands that hold go under them: a body hangs from one hand with that shoulder under it, so that
    // the other hand is where it can reach.
    f32 lateral = 0.0f;
    for (u32 h = 0; h < limbs; ++h) {
        lateral += holding(t.grips[h]) ? td.limbs[h].shoulder.x : 0.0f;
    }
    lateral /= static_cast<f32>(anchors);
    const Vec3f side = normalize(cross(t.facing != Vec3f{} ? t.facing : facing, up));
    // A rope is a thin thing hung by its middle: the capsule hangs its own radius further out, and the arms are stiff (the
    // rope itself is what swings, with the character's mass on it).
    bool rope_hold = false;
    f32 rope_radius = 0.0f;
    for (u32 h = 0; h < limbs; ++h) {
        if (holding(t.grips[h]) && t.grips[h].kind == GripKind3D::Rope) {
            rope_hold = true;
            if (const auto ref = world.rope_segment_of(t.grips[h].body)) {
                rope_radius = std::max(rope_radius, world.rope_radius(ref->rope));
            }
        }
    }
    const Vec3f below = -up * drop + out * (d.radius + td.wall_gap + rope_radius) - side * lateral;
    // The place as the anchors are now (what the swing is measured from) and where they will be by the end of this step (what
    // it is driven to): a platform that moves is carried with no lag, the swing is only what the character does about it.
    const Vec3f place_now = centre_now + below;
    const Vec3f place = centre + below;

    // --- the swing: a damped spring about the place (from where it is now, so that a hand let go of or a platform that
    // moved is a continuous motion), its velocity read back from what the move achieved (no wind-up against the floor)
    f32 omega = two_pi * std::max(td.swing_frequency, 0.0f);
    f32 omega_up = two_pi * std::max(td.swing_vertical_frequency, 0.0f);
    f32 damping_side = td.swing_damping, damping_up = td.swing_vertical_damping;
    if (rope_hold) {
        // Quick and critically damped both ways: the hands arrive at the rope, then it carries them.
        omega = omega_up = two_pi * 4.0f;
        damping_side = damping_up = 1.0f;
    }
    Vec3f offset = feet - place_now;
    const f32 offset_length = length(offset);
    if (offset_length > td.max_swing && offset_length > 0.0f) {
        offset = offset * (td.max_swing / offset_length);
    }
    {
        // A pendulum across `up`, stiff arms along it.
        const Vec3f offset_up = up * dot(offset, up), velocity_up = up * dot(t.swing_velocity, up);
        const Vec3f offset_side = offset - offset_up, velocity_side = t.swing_velocity - velocity_up;
        const Vec3f acceleration = -offset_side * (omega * omega) - velocity_side * (2.0f * damping_side * omega) -
                                   offset_up * (omega_up * omega_up) - velocity_up * (2.0f * damping_up * omega_up);
        t.swing_velocity += acceleration * dt;
    }
    const Vec3f wanted = place + offset + t.swing_velocity * dt;

    const MaskFilter layer_filter(own_mask);
    const OwnBodyFilter body_filter(ch.GetInnerBodyID(), _env);
    const Vec3f arrived = move_to(ch, *_env.system, d.layer, layer_filter, body_filter, *_env.temp, wanted, dt);
    // Read back: the offset's own rate as the move achieved it (no wind-up against a floor that is in the way).
    t.swing_velocity = ((arrived - place) - offset) / dt;
    t.swing = arrived - place;
    t.hang_place = place;
    t.has_hang_place = true;
    t.hang_time += dt;

    // What the character did this step, for the walking controller that may take over.
    s.velocity = (arrived - feet) / dt;
    s.control = s.velocity;
    s.vertical = dot(s.velocity, up);
    s.relative = {};
    s.air_time = 0.0f;
    s.jumped = false;
    s.prev_jump = s.input.jump;
    s.ground = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround ? CharacterGround3D::OnGround
                                                                                   : CharacterGround3D::InAir;

    // --- the load: its weight at the anchors of the dynamic bodies it holds, shared between the hands
    {
        const f32 weight = d.mass * length(_env.gravity()) * d.gravity_scale;
        for (u32 h = 0; h < limbs; ++h) {
            const GripData& g = t.grips[h];
            Vec3f p{}, n{};
            if (holding(g) && anchor_world(s, g, false, 0.0f, p, n) && world.motion(g.body) == Body3DMotion::Dynamic) {
                if (g.kind == GripKind3D::Rope) {
                    // A rope is made of light segments: the climber's mass is added to the two segments nearest the hand
                    // (by distance along them), so that the rope carries and swings with it, and a grab in the air gives
                    // the rope its share of the momentum.
                    const auto ref = world.rope_segment_of(g.body);
                    if (!ref) {
                        continue;
                    }
                    const f32 share = d.mass / static_cast<f32>(held);
                    const f32 centre = static_cast<f32>(ref->segment) + g.local_point.y; // segment centres are whole numbers
                    const i32 lower = std::clamp<i32>(static_cast<i32>(std::floor(centre)), 0, static_cast<i32>(ref->segments) - 1);
                    const i32 upper = std::min<i32>(lower + 1, static_cast<i32>(ref->segments) - 1);
                    const f32 w = upper == lower ? 0.0f : std::clamp(centre - static_cast<f32>(lower), 0.0f, 1.0f);
                    for (const auto& [index, amount] : {std::pair<i32, f32>{lower, share * (1.0f - w)}, std::pair<i32, f32>{upper, share * w}}) {
                        const Body3D body = world.rope_segment_body(ref->rope, static_cast<u32>(index));
                        if (body.valid() && amount > 0.0f) {
                            world.set_added_mass(body, world.added_mass(body) + amount);
                            // A rope with someone on it does not go to sleep (the weight is on it though it is still).
                            world.activate(body);
                        }
                    }
                    if (just_hung) {
                        // The momentum it had (the speed of its feet) shared with the rope where it took hold.
                        world.add_impulse_at(g.body, (velocity_in - world.linear_velocity(g.body)) * share, p);
                    }
                } else {
                    world.add_force_at(g.body, -up * (weight / static_cast<f32>(held)), p);
                }
            }
        }
    }
    return true;
}

bool CharacterSet::mantle_step(u32 index, Slot& s, f32 dt) {
    (void)index;
    TravData& t = s.trav;
    const TraversalDesc3D& td = s.desc.traversal;
    const CharacterDesc3D& d = s.desc;
    JPH::CharacterVirtual& ch = *s.ch;
    World3D& world = *_env.world;
    const Vec3f up = s.up;
    const Vec3f feet = from_jolt(ch.GetPosition());
    const u32 limbs = std::clamp<u32>(td.limb_count, 1u, max_grip_limbs3d);

    // Nothing is spent while it climbs (and nothing comes back: the delay is running).
    t.resource.step(td.resource, dt, 0.0f);

    if (!world.contains(t.mantle_body)) {
        // The ledge went (a body destroyed): it falls.
        for (u32 h = 0; h < limbs; ++h) {
            release_grip(s, h, GripRelease3D::Lost);
        }
        leave_hang(s, true, GripRelease3D::Lost);
        return false;
    }
    t.mantle_time += dt;
    const f32 u = std::min(1.0f, t.mantle_time / std::max(td.mantle_seconds, dt));
    constexpr f32 split = 0.55f; // up the wall for the first part, over the edge for the rest
    const Vec3f local = u < split ? lerp(t.mantle_path[0], t.mantle_path[1], smooth(u / split))
                                  : lerp(t.mantle_path[1], t.mantle_path[2], smooth((u - split) / (1.0f - split)));
    // The body's place with its own motion over this step (a platform under the ledge carries the path).
    const Vec3f body_position = world.position(t.mantle_body);
    const Quatf body_rotation = world.rotation(t.mantle_body);
    Vec3f target = body_position + body_rotation.rotate(local);
    const Vec3f arm = target - world.centre_of_mass_position(t.mantle_body);
    target += (world.linear_velocity(t.mantle_body) + cross(world.angular_velocity(t.mantle_body), arm)) * dt;

    const MaskFilter layer_filter(mask_of(s));
    const OwnBodyFilter body_filter(ch.GetInnerBodyID(), _env);
    const Vec3f arrived = move_to(ch, *_env.system, d.layer, layer_filter, body_filter, *_env.temp, target, dt);
    s.velocity = (arrived - feet) / dt;
    s.control = s.velocity;
    s.vertical = dot(s.velocity, up);
    s.relative = {};
    s.air_time = 0.0f;
    s.jumped = false;
    s.prev_jump = s.input.jump;
    t.hang_time += dt;
    t.swing = {};
    t.swing_velocity = {};

    if (u >= 1.0f) {
        // Over: the hands are free, it walks. What it stands on is found now (no landing event for the step it ended).
        add_traversal_event(CharacterEventKind3D::MantleFinished, 0, 0, t.mantle_body, arrived, up, 0.0f);
        for (u32 h = 0; h < limbs; ++h) {
            release_grip(s, h, GripRelease3D::Mantle);
        }
        const f32 vertical_keep = 0.0f;
        leave_hang(s, false, GripRelease3D::Mantle);
        s.control = {};
        s.vertical = vertical_keep;
        const MaskFilter filter(mask_of(s));
        const OwnBodyFilter body_filter(ch.GetInnerBodyID(), _env);
        ch.RefreshContacts(_env.system->GetDefaultBroadPhaseLayerFilter(d.layer), filter, body_filter, JPH::ShapeFilter(),
                           *_env.temp);
        // It stands: the stand was found with a floor under it, a few millimetres down (no landing event for those).
        s.ground = CharacterGround3D::OnGround;
        if (ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround) {
            // Standing: the next step's relative speed is zero on whatever it stands on.
            s.vertical = dot(from_jolt(ch.GetGroundVelocity()), up);
        }
    }
    return true;
}

} // namespace physics3d_detail
} // namespace kin
