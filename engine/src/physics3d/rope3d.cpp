// Ropes (kin/physics3d/rope3d.hpp): chains of capsules with ball joints, their length, tension and sampling, and the grip
// provider that lets a hand take hold of one at any point.

#include "rope3d_impl.hpp"

#include "bytes3d.hpp"
#include "traversal3d_impl.hpp"

#include <algorithm>
#include <cmath>

namespace kin {
namespace physics3d_detail {

namespace {

bool finite3(Vec3f v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

Quatf between(Vec3f from, Vec3f to) {
    const f32 d = dot(from, to);
    if (d < -0.999999f) {
        const Vec3f axis = normalize(cross(from, std::fabs(from.x) < 0.9f ? Vec3f{1.0f, 0.0f, 0.0f} : Vec3f{0.0f, 0.0f, 1.0f}));
        return {axis.x, axis.y, axis.z, 0.0f};
    }
    const Vec3f a = cross(from, to);
    return Quatf{a.x, a.y, a.z, 1.0f + d}.normalized();
}

constexpr u32 group_base = 0x20000000u;
constexpr u32 default_velocity_steps = 128;
constexpr u32 default_position_steps = 32;
constexpr u32 max_segments = 512;

// The point at arclength `s` along a polyline (clamped to its ends; the last direction continues past it).
Vec3f sample(const std::vector<Vec3f>& pts, const std::vector<f32>& cumulative, f32 s) {
    if (s <= 0.0f) {
        return pts.front();
    }
    if (s >= cumulative.back()) {
        const Vec3f d = pts.back() - pts[pts.size() - 2];
        const f32 n = length(d);
        return pts.back() + (n > 1.0e-6f ? d / n : Vec3f{0.0f, -1.0f, 0.0f}) * (s - cumulative.back());
    }
    const auto it = std::upper_bound(cumulative.begin(), cumulative.end(), s);
    const std::size_t k = static_cast<std::size_t>(it - cumulative.begin());
    const f32 span = cumulative[k] - cumulative[k - 1];
    const f32 t = span > 1.0e-9f ? (s - cumulative[k - 1]) / span : 0.0f;
    return lerp(pts[k - 1], pts[k], t);
}

} // namespace

// --- the set ---------------------------------------------------------------------------------------------------------

RopeHandle3D RopeSet::create(const RopeDesc3D& d) {
    const World3D& w = *_world;
    if (!finite3(d.start.point) || !finite3(d.end.point) || !finite3(d.sag_direction) || !finite3(d.hang_direction) ||
        !std::isfinite(d.length) || d.length < 0.0f || !(d.radius > 0.0f) || !std::isfinite(d.radius) ||
        !(d.mass_per_metre > 0.0f) || !std::isfinite(d.mass_per_metre) || !(d.segment_length > 0.0f) ||
        d.layer >= layers3d::Count || d.layer == layers3d::Static || !std::isfinite(d.break_tension)) {
        return {};
    }
    const auto resolve = [&](const RopeAnchor3D& a, Vec3f& out) {
        switch (a.kind) {
        case RopeAnchor3D::Kind::Free:
            out = a.point;
            return true;
        case RopeAnchor3D::Kind::World:
            out = a.point;
            return true;
        case RopeAnchor3D::Kind::Body:
            if (!w.contains(a.body)) {
                return false;
            }
            out = w.position(a.body) + w.rotation(a.body).rotate(a.point);
            return true;
        }
        return false;
    };
    Vec3f p0{}, p1{};
    if (!resolve(d.start, p0) || !resolve(d.end, p1)) {
        return {};
    }
    const bool start_fixed = d.start.kind != RopeAnchor3D::Kind::Free;
    const bool end_fixed = d.end.kind != RopeAnchor3D::Kind::Free;
    Vec3f gravity = w.gravity();
    const Vec3f down = length(gravity) > 1.0e-6f ? normalize(gravity) : Vec3f{0.0f, -1.0f, 0.0f};
    const Vec3f hang = length(d.hang_direction) > 1.0e-6f ? normalize(d.hang_direction) : down;
    const Vec3f sag_dir = length(d.sag_direction) > 1.0e-6f ? normalize(d.sag_direction) : down;
    f32 L = d.length;
    bool from_end = false;
    f32 sag = 0.0f;
    if (start_fixed && end_fixed) {
        const f32 D = length(p1 - p0);
        L = d.length > 0.0f ? std::max(d.length, D) : D;
        if (!(L > 1.0e-4f)) {
            return {};
        }
        if (L > D * 1.0001f) {
            // The parabola of the sag whose length is L, by bisection.
            const auto arc = [&](f32 amount) {
                f32 total = 0.0f;
                Vec3f prev = p0;
                for (int i = 1; i <= 64; ++i) {
                    const f32 u = static_cast<f32>(i) / 64.0f;
                    const Vec3f p = lerp(p0, p1, u) + sag_dir * (amount * 4.0f * u * (1.0f - u));
                    total += length(p - prev);
                    prev = p;
                }
                return total;
            };
            f32 lo = 0.0f, hi = std::max(L, 0.1f);
            for (int i = 0; i < 40; ++i) {
                const f32 mid = 0.5f * (lo + hi);
                (arc(mid) < L ? lo : hi) = mid;
            }
            sag = 0.5f * (lo + hi);
        }
    } else {
        if (!(L > 1.0e-4f)) {
            return {};
        }
        if (start_fixed || !end_fixed) {
            p1 = p0 + hang * L;
        } else {
            p0 = p1 + hang * L;
            from_end = true;
        }
    }
    u32 n = d.segments > 0 ? d.segments : static_cast<u32>(std::lround(L / d.segment_length));
    n = std::clamp<u32>(n, 1u, max_segments);
    if (static_cast<i32>(n) + 8 > static_cast<i32>(w.config().max_bodies) - w.body_count()) {
        return {};
    }

    // The joints: equal arclength along the initial curve.
    std::vector<Vec3f> curve;
    std::vector<f32> cumulative;
    const u32 samples = sag > 0.0f ? 256u : 1u;
    for (u32 i = 0; i <= samples; ++i) {
        const f32 u = static_cast<f32>(i) / static_cast<f32>(samples);
        curve.push_back(lerp(p0, p1, u) + sag_dir * (sag * 4.0f * u * (1.0f - u)));
    }
    cumulative.push_back(0.0f);
    for (std::size_t i = 1; i < curve.size(); ++i) {
        cumulative.push_back(cumulative.back() + length(curve[i] - curve[i - 1]));
    }
    const f32 seg = L / static_cast<f32>(n);
    std::vector<Vec3f> joints(n + 1);
    for (u32 k = 0; k <= n; ++k) {
        // The curve's own length is L (bisection) or the straight distance: the last joint lands on its end.
        joints[k] = sample(curve, cumulative, static_cast<f32>(k) * seg * (cumulative.back() / L));
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
    s.desc = d;
    s.n = n;
    s.rest = s.target = L;
    s.segment_length = seg;
    s.from_end = from_end;
    s.segs.assign(n, Body3D{});
    s.joints.assign(n + 1, Constraint3D{});
    const u32 group = group_base | (index + 1u);
    const f32 half = std::max(0.5f * seg - d.radius, 0.002f);
    const Shape3D shape = Shape3D::capsule(half, d.radius);
    const auto fail = [&]() {
        for (const Body3D b : s.segs) {
            if (b.valid()) {
                _world->owner_destroy_body(b);
            }
        }
        s.alive = false;
        s.segs.clear();
        s.joints.clear();
        _free.push_back(index);
        return RopeHandle3D{};
    };
    for (u32 k = 0; k < n; ++k) {
        Vec3f axis = joints[k + 1] - joints[k];
        axis = length(axis) > 1.0e-6f ? normalize(axis) : Vec3f{0.0f, -1.0f, 0.0f};
        Body3DDesc bd;
        bd.shape = shape;
        bd.motion = Body3DMotion::Dynamic;
        bd.position = (joints[k] + joints[k + 1]) * 0.5f;
        bd.rotation = between({0.0f, 1.0f, 0.0f}, axis);
        bd.mass = std::max(d.mass_per_metre * seg, 1.0e-3f);
        bd.friction = d.friction;
        bd.linear_damping = d.linear_damping;
        bd.angular_damping = d.angular_damping;
        bd.layer = d.layer;
        bd.continuous = d.continuous;
        bd.group = group;
        bd.group_self_collision = d.self_collision;
        bd.ignore_static = !d.collide_world;
        bd.velocity_steps = static_cast<u8>(std::min<u32>(d.velocity_steps > 0 ? d.velocity_steps : default_velocity_steps, 255));
        bd.position_steps = static_cast<u8>(std::min<u32>(d.position_steps > 0 ? d.position_steps : default_position_steps, 255));
        bd.user_id = d.user_id;
        s.segs[k] = _world->create_body(bd);
        if (!s.segs[k].valid()) {
            return fail();
        }
        // The rope's own: the game cannot destroy or reshape a segment under it (rope_points would read a dead body).
        _world->set_owner(s.segs[k], BodyOwner3D::Rope);
        // A hand never takes hold of a segment as a surface: the rope's own provider finds the point along it.
        SurfaceMaterial3D material;
        material.grippable = false;
        _world->set_surface_material(s.segs[k], material);
    }
    for (u32 k = 0; k <= n; ++k) {
        Constraint3DDesc c;
        c.kind = Constraint3DKind::Point;
        c.point = joints[k];
        c.collide_connected = false;
        c.break_force = d.break_tension;
        c.user_id = d.user_id;
        c.velocity_steps = d.velocity_steps > 0 ? d.velocity_steps : default_velocity_steps;
        c.position_steps = d.position_steps > 0 ? d.position_steps : default_position_steps;
        if (k == 0) {
            if (!start_fixed) {
                continue;
            }
            c.a = d.start.kind == RopeAnchor3D::Kind::Body ? d.start.body : Body3D{};
            c.b = s.segs[0];
        } else if (k == n) {
            if (!end_fixed) {
                continue;
            }
            c.a = d.end.kind == RopeAnchor3D::Kind::Body ? d.end.body : Body3D{};
            c.b = s.segs[n - 1];
        } else {
            c.a = s.segs[k - 1];
            c.b = s.segs[k];
        }
        s.joints[k] = _world->create_constraint(c);
        if (!s.joints[k].valid()) {
            return fail();
        }
    }
    // The shapes and anchors as fit() makes them, so that a rope made and a rope refitted by a restore are the same to the bit
    // (the anchors Jolt works out from world points differ from the exact ones in the last places).
    fit(s, L, false);
    for (u32 k = 0; k < n; ++k) {
        if (_by_body.size() <= s.segs[k].index) {
            _by_body.resize(static_cast<std::size_t>(s.segs[k].index) + 1);
        }
        _by_body[s.segs[k].index] = {index, k, s.segs[k].generation};
    }
    s.alive = true;
    ++_live;
    return {index, s.generation};
}

void RopeSet::destroy(RopeHandle3D h) {
    Slot* s = find(h);
    if (s == nullptr) {
        return;
    }
    for (std::size_t k = s->segs.size(); k-- > 0;) {
        const Body3D b = s->segs[k];
        if (b.index < _by_body.size() && _by_body[b.index].rope == h.index && _by_body[b.index].generation == b.generation) {
            _by_body[b.index] = {};
        }
        if (_world->contains(b)) {
            _world->owner_destroy_body(b);
        }
    }
    s->alive = false;
    ++s->generation;
    s->segs.clear();
    s->joints.clear();
    _free.push_back(h.index);
    --_live;
}

std::vector<RopeHandle3D> RopeSet::list() const {
    std::vector<RopeHandle3D> out;
    for (u32 i = 0; i < _slots.size(); ++i) {
        if (_slots[i].alive) {
            out.push_back({i, _slots[i].generation});
        }
    }
    return out;
}

void RopeSet::joint_points(const Slot& s, std::vector<Vec3f>& out) const {
    out.resize(s.n + 1);
    const f32 half = .5f * s.segment_length;
    Vec3f previous_end{};
    for (u32 k = 0; k < s.n; ++k) {
        const auto p = _world->position(s.segs[k]);
        const auto r = _world->rotation(s.segs[k]);
        const auto start = p + r.rotate({0, -half, 0});
        out[k] = k ? (previous_end + start) * .5f : start;
        previous_end = p + r.rotate({0, half, 0});
    }
    out[s.n] = previous_end;
}

std::vector<RopePoint3D> RopeSet::points(RopeHandle3D h) const {
    std::vector<RopePoint3D> out;
    points(h, out);
    return out;
}
void RopeSet::points(RopeHandle3D h, std::vector<RopePoint3D>& out) const {
    const auto* s = find(h);
    if (!s) {
        out.clear();
        return;
    }
    out.resize(s->n + 1);
    const f32 half = .5f * s->segment_length;
    Vec3f previous_end{}, previous_axis{};
    const auto tangent = [](Vec3f t) { return length(t) > 1e-6f ? normalize(t) : Vec3f{0, -1, 0}; };
    for (u32 k = 0; k < s->n; ++k) {
        const auto p = _world->position(s->segs[k]);
        const auto r = _world->rotation(s->segs[k]);
        const auto start = p + r.rotate({0, -half, 0}), axis = r.rotate({0, 1, 0});
        out[k] = {k ? (previous_end + start) * .5f : start, tangent(k ? previous_axis + axis : axis)};
        previous_end = p + r.rotate({0, half, 0});
        previous_axis = axis;
    }
    out[s->n] = {previous_end, tangent(previous_axis)};
}

std::vector<RopeSegment3D> RopeSet::segments(RopeHandle3D h) const {
    std::vector<RopeSegment3D> out;
    const Slot* s = find(h);
    if (s == nullptr) {
        return out;
    }
    const f32 half = 0.5f * s->segment_length;
    out.resize(s->n);
    for (u32 k = 0; k < s->n; ++k) {
        const Vec3f p = _world->position(s->segs[k]);
        const Quatf r = _world->rotation(s->segs[k]);
        out[k].a = p + r.rotate({0.0f, -half, 0.0f});
        out[k].b = p + r.rotate({0.0f, half, 0.0f});
        out[k].rotation = r;
        out[k].body = s->segs[k];
    }
    return out;
}

Body3D RopeSet::segment_body(RopeHandle3D h, u32 segment) const {
    const Slot* s = find(h);
    return s != nullptr && segment < s->n ? s->segs[segment] : Body3D{};
}

f32 RopeSet::radius(RopeHandle3D h) const {
    const Slot* s = find(h);
    return s != nullptr ? s->desc.radius : 0.0f;
}

RopeState3D RopeSet::state(RopeHandle3D h) const {
    RopeState3D out;
    const Slot* s = find(h);
    if (s == nullptr) {
        return out;
    }
    out.segments = s->n;
    out.rest_length = s->rest;
    out.target_length = s->target;
    Vec3f previous_end{}, previous_joint{};
    const f32 half = .5f * s->segment_length;
    for (u32 k = 0; k < s->n; ++k) {
        const auto p = _world->position(s->segs[k]);
        const auto r = _world->rotation(s->segs[k]);
        const auto start = p + r.rotate({0, -half, 0});
        const auto joint = k ? (previous_end + start) * .5f : start;
        if (k)
            out.current_length += length(joint - previous_joint);
        previous_joint = joint;
        previous_end = p + r.rotate({0, half, 0});
    }
    out.current_length += length(previous_end - previous_joint);
    for (const Constraint3D c : s->joints) {
        if (c.valid() && _world->contains(c) && _world->constraint_broken(c)) {
            out.broken = true;
        }
    }
    return out;
}

std::optional<RopeClosest3D> RopeSet::closest(RopeHandle3D h, Vec3f p) const {
    const Slot* s = find(h);
    if (s == nullptr || !finite3(p)) {
        return std::nullopt;
    }
    const std::vector<RopeSegment3D> segs = segments(h);
    RopeClosest3D best;
    f32 best_d2 = 3.0e38f;
    for (u32 k = 0; k < segs.size(); ++k) {
        const Vec3f ab = segs[k].b - segs[k].a;
        const f32 len2 = dot(ab, ab);
        const f32 t = len2 > 1.0e-12f ? std::clamp(dot(p - segs[k].a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        const Vec3f q = segs[k].a + ab * t;
        const f32 d2 = length_squared(p - q);
        if (d2 < best_d2) {
            best_d2 = d2;
            best.segment = k;
            best.t = t;
            best.point = q;
            best.tangent = len2 > 1.0e-12f ? ab / std::sqrt(len2) : Vec3f{0.0f, -1.0f, 0.0f};
        }
    }
    best.distance = std::sqrt(best_d2);
    best.along = (static_cast<f32>(best.segment) + best.t) / static_cast<f32>(s->n);
    return best;
}

bool RopeSet::point_at(RopeHandle3D h, u32 segment, f32 fraction, Vec3f& point, Vec3f& tangent) const {
    const Slot* s = find(h);
    if (s == nullptr || segment >= s->n || !_world->contains(s->segs[segment])) {
        return false;
    }
    const Quatf r = _world->rotation(s->segs[segment]);
    tangent = r.rotate({0.0f, 1.0f, 0.0f});
    point = _world->position(s->segs[segment]) + tangent * (fraction * s->segment_length);
    return true;
}

std::optional<RopeSegmentRef3D> RopeSet::segment_of(Body3D body) const {
    if (body.index >= _by_body.size()) {
        return std::nullopt;
    }
    const Ref& r = _by_body[body.index];
    if (r.rope == UINT32_MAX || r.generation != body.generation || r.rope >= _slots.size() || !_slots[r.rope].alive) {
        return std::nullopt;
    }
    const Slot& s = _slots[r.rope];
    if (r.segment >= s.n || !(s.segs[r.segment] == body)) {
        return std::nullopt;
    }
    RopeSegmentRef3D out;
    out.rope = {r.rope, s.generation};
    out.segment = r.segment;
    out.segments = s.n;
    out.segment_length = s.segment_length;
    return out;
}

Vec3f RopeSet::anchor_force(RopeHandle3D h, RopeEnd3D end) const {
    const Slot* s = find(h);
    if (s == nullptr) {
        return {};
    }
    const Constraint3D c = s->joints[end == RopeEnd3D::Start ? 0 : s->n];
    if (!c.valid() || !_world->contains(c)) {
        return {};
    }
    return s->force[end == RopeEnd3D::Start ? 0 : 1];
}

f32 RopeSet::tension(RopeHandle3D h, RopeEnd3D end) const {
    const Slot* s = find(h);
    if (s == nullptr) {
        return 0.0f;
    }
    const Vec3f f = anchor_force(h, end);
    // Toward the rope from the anchor: along it at the start, against it at the end.
    const Vec3f axis = _world->rotation(s->segs[end == RopeEnd3D::Start ? 0 : s->n - 1]).rotate({0.0f, 1.0f, 0.0f});
    return dot(f, end == RopeEnd3D::Start ? axis : -axis);
}

// Makes the segments and anchors for `length`; with `relay` also puts the chain along its own polyline at the new spacing.
void RopeSet::fit(Slot& s, f32 new_length, bool relay) {
    const u32 n = s.n;
    const f32 seg = new_length / static_cast<f32>(n);
    std::vector<Vec3f> next;
    std::vector<Quatf> rotations;
    if (relay) {
        std::vector<Vec3f> j;
        joint_points(s, j);
        if (s.from_end) {
            std::reverse(j.begin(), j.end());
        }
        std::vector<f32> cumulative(j.size(), 0.0f);
        for (std::size_t k = 1; k < j.size(); ++k) {
            cumulative[k] = cumulative[k - 1] + length(j[k] - j[k - 1]);
        }
        next.resize(n + 1);
        for (u32 k = 0; k <= n; ++k) {
            next[k] = sample(j, cumulative, static_cast<f32>(k) * seg);
        }
        if (s.from_end) {
            std::reverse(next.begin(), next.end());
        }
        rotations.resize(n);
        for (u32 k = 0; k < n; ++k) {
            const Vec3f dir = next[k + 1] - next[k];
            const f32 len = length(dir);
            const Quatf old = _world->rotation(s.segs[k]);
            if (len > 1.0e-6f) {
                const Vec3f old_axis = old.rotate({0.0f, 1.0f, 0.0f});
                rotations[k] = (between(old_axis, dir / len) * old).normalized();
            } else {
                rotations[k] = old;
            }
        }
    }
    const f32 half = std::max(0.5f * seg - s.desc.radius, 0.002f);
    const Shape3D shape = Shape3D::capsule(half, s.desc.radius);
    for (u32 k = 0; k < n; ++k) {
        _world->owner_set_shape(s.segs[k], shape);
        if (relay) {
            _world->set_position(s.segs[k], (next[k] + next[k + 1]) * 0.5f);
            _world->set_rotation(s.segs[k], rotations[k]);
        }
    }
    const f32 e = 0.5f * seg;
    for (u32 k = 0; k <= n; ++k) {
        const Constraint3D c = s.joints[k];
        if (!c.valid() || !_world->contains(c)) {
            continue;
        }
        if (k == 0) {
            (void)_world->set_constraint_anchor(c, 1, {0.0f, -e, 0.0f});
        } else if (k == n) {
            (void)_world->set_constraint_anchor(c, 1, {0.0f, e, 0.0f});
        } else {
            (void)_world->set_constraint_anchor(c, 0, {0.0f, e, 0.0f});
            (void)_world->set_constraint_anchor(c, 1, {0.0f, -e, 0.0f});
        }
    }
    // The anchored ends' own sides: a point of the world, or the body's offset (they do not change with the length).
    for (const u32 joint : {0u, n}) {
        const RopeAnchor3D& a = joint == 0 ? s.desc.start : s.desc.end;
        const Constraint3D c = s.joints[joint];
        if (c.valid() && _world->contains(c) && a.kind != RopeAnchor3D::Kind::Free) {
            (void)_world->set_constraint_anchor(c, 0, a.point);
        }
    }
    s.segment_length = seg;
    s.rest = new_length;
}

void RopeSet::set_length(RopeHandle3D h, f32 length) {
    Slot* s = find(h);
    if (s == nullptr || !std::isfinite(length)) {
        return;
    }
    length = std::max(length, static_cast<f32>(s->n) * std::max(2.0f * s->desc.radius, 0.01f));
    s->target = length;
    s->speed = 0.0f;
    if (length != s->rest) {
        fit(*s, length, true);
    }
}

void RopeSet::reel(RopeHandle3D h, f32 target, f32 speed) {
    Slot* s = find(h);
    if (s == nullptr || !std::isfinite(target) || !std::isfinite(speed)) {
        return;
    }
    if (!(speed > 0.0f)) {
        set_length(h, target);
        return;
    }
    s->target = std::max(target, static_cast<f32>(s->n) * std::max(2.0f * s->desc.radius, 0.01f));
    s->speed = speed;
}

void RopeSet::pre_step(f32 dt) {
    for (Slot& s : _slots) {
        if (!s.alive) {
            continue;
        }
        const bool at_end = !s.from_end; // the end that moves is the far one from where the chain is kept
        const RopeAnchor3D& anchor = at_end ? s.desc.end : s.desc.start;
        const bool load = anchor.kind == RopeAnchor3D::Kind::Body && _world->contains(anchor.body);
        const bool reeling = s.rest != s.target && s.speed > 0.0f;
        if (s.hoisting && (!reeling || s.target > s.rest || !load)) {
            // The reel stopped, or turned to pay out, or the load went: the body is its own again.
            s.hoisting = false;
            if (load && _world->motion(anchor.body) == Body3DMotion::Kinematic) {
                (void)_world->set_motion(anchor.body, Body3DMotion::Dynamic);
                _world->set_linear_velocity(anchor.body, {});
                _world->set_angular_velocity(anchor.body, {});
            }
        }
        if (!reeling) {
            continue;
        }
        const f32 step = s.speed * dt;
        const bool arriving = std::fabs(s.target - s.rest) <= step;
        const f32 next = arriving ? s.target : s.rest + (s.target > s.rest ? step : -step);
        const bool shorten = s.target < s.rest;
        fit(s, next, true);
        if (!shorten || !load || _world->motion(anchor.body) == Body3DMotion::Static) {
            continue;
        }
        // Reeling in a body: a chain of light segments pulls a heavy load only by position correction, which moves the light
        // ones and not it (measured: 40 kg rose a third of what it was reeled in over 5 s). The winch carries it instead: the
        // body is kinematic, put where the chain's end is, for as long as the rope is being reeled in; then dynamic again.
        (void)arriving;
        const std::size_t moving = at_end ? s.n - 1 : 0;
        const Vec3f tip = _world->position(s.segs[moving]) +
                          _world->rotation(s.segs[moving]).rotate({0.0f, (at_end ? 0.5f : -0.5f) * s.segment_length, 0.0f});
        const Quatf turn = _world->rotation(anchor.body);
        const Vec3f where = tip - turn.rotate(anchor.point);
        if (!s.hoisting) {
            // Someone else's kinematic body, or a ragdoll's bone or another rope's segment (set_motion refuses those: theirs to
            // switch): left alone.
            if (_world->motion(anchor.body) != Body3DMotion::Dynamic || _world->owner_of(anchor.body) != BodyOwner3D::None) {
                continue;
            }
            s.hoisting = true;
            (void)_world->set_motion(anchor.body, Body3DMotion::Kinematic);
        }
        // Put there, not driven there: the chain's end was put there too, and a velocity the rigid chain cannot have would be
        // answered by impulses of thousands of newtons.
        _world->set_position(anchor.body, where);
        _world->set_linear_velocity(anchor.body, {});
        _world->set_angular_velocity(anchor.body, {});
    }
}

void RopeSet::post_step(f32 sub_dt, std::span<const ConstraintEvent3D> broke) {
    _sub_dt = sub_dt;
    // The anchors' forces, smoothed (time constant 0.25 s of step time). The impulse is the one on the rope's segment; the
    // anchor feels its opposite.
    for (Slot& s : _slots) {
        if (!s.alive) {
            continue;
        }
        for (int e = 0; e < 2; ++e) {
            const Constraint3D c = s.joints[e == 0 ? 0 : s.n];
            Vec3f raw{};
            if (c.valid() && _world->contains(c)) {
                raw = -_world->constraint_impulse(c) / std::max(sub_dt, 1.0e-6f);
            }
            const f32 k = s.force_valid ? sub_dt / (sub_dt + 0.25f) : 1.0f;
            s.force[static_cast<std::size_t>(e)] += (raw - s.force[static_cast<std::size_t>(e)]) * k;
        }
        s.force_valid = true;
    }
    if (broke.empty()) {
        return;
    }
    std::vector<RopeEvent3D> batch;
    for (const ConstraintEvent3D& e : broke) {
        for (u32 i = 0; i < _slots.size(); ++i) {
            const Slot& s = _slots[i];
            if (!s.alive) {
                continue;
            }
            for (u32 k = 0; k <= s.n; ++k) {
                if (s.joints[k] == e.constraint) {
                    RopeEvent3D r;
                    r.rope = {i, s.generation};
                    r.user_id = s.desc.user_id;
                    r.kind = RopeEventKind3D::Broke;
                    r.joint = k;
                    r.force = e.force;
                    batch.push_back(r);
                }
            }
        }
    }
    std::sort(batch.begin(), batch.end(), [](const RopeEvent3D& a, const RopeEvent3D& b) {
        if (a.rope.index != b.rope.index) return a.rope.index < b.rope.index;
        return a.joint < b.joint;
    });
    _events.insert(_events.end(), batch.begin(), batch.end());
}

void RopeSet::save_header(std::vector<u8>& out) const {
    ByteOut w{out};
    w.u32v(static_cast<u32>(_slots.size()));
    for (const Slot& s : _slots) {
        w.u32v(s.generation);
        w.b(s.alive);
        if (!s.alive) {
            continue;
        }
        w.f32v(s.rest);
        w.f32v(s.target);
        w.f32v(s.speed);
        w.b(s.hoisting);
    }
}

std::unique_ptr<RopeSet::Parsed> RopeSet::parse_header(const u8*& cursor, const u8* end) const {
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
        if (o.alive) {
            o.rest = in.f32v();
            o.target = in.f32v();
            o.speed = in.f32v();
            o.hoisting = in.b();
        }
        if (in.failed) {
            return nullptr;
        }
    }
    cursor = in.p;
    return parsed;
}

void RopeSet::apply(const Parsed& parsed) {
    for (u32 i = 0; i < parsed.slots.size() && i < _slots.size(); ++i) {
        const Parsed::One& o = parsed.slots[i];
        Slot& s = _slots[i];
        if (!s.alive) {
            continue;
        }
        s.target = o.target;
        s.speed = o.speed;
        s.force_valid = false;
        if (o.hoisting != s.hoisting) {
            // The anchored body's motion type is not in Jolt's state: put it as it was.
            const RopeAnchor3D& anchor = s.from_end ? s.desc.start : s.desc.end;
            if (anchor.kind == RopeAnchor3D::Kind::Body && _world->contains(anchor.body)) {
                (void)_world->set_motion(anchor.body, o.hoisting ? Body3DMotion::Kinematic : Body3DMotion::Dynamic);
            }
            s.hoisting = o.hoisting;
        }
        if (o.rest != s.rest) {
            fit(s, o.rest, false);
        }
    }
}

// --- the grip provider -----------------------------------------------------------------------------------------------------

namespace {

// The closest points of two segments (Ericson): parameters in [0, 1] along each.
void closest_segments(Vec3f p1, Vec3f q1, Vec3f p2, Vec3f q2, f32& s, f32& t) {
    const Vec3f d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const f32 a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    constexpr f32 eps = 1.0e-9f;
    if (a <= eps && e <= eps) {
        s = t = 0.0f;
        return;
    }
    if (a <= eps) {
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
        return;
    }
    const f32 c = dot(d1, r);
    if (e <= eps) {
        t = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
        return;
    }
    const f32 b = dot(d1, d2);
    const f32 denom = a * e - b * b;
    s = denom > eps ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
    t = (b * s + f) / e;
    if (t < 0.0f) {
        t = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
    } else if (t > 1.0f) {
        t = 1.0f;
        s = std::clamp((b - c) / a, 0.0f, 1.0f);
    }
}

class RopeProvider final : public GripProvider3D {
public:
    GripKind3D kind() const override { return GripKind3D::Rope; }

    void probe(const World3D& world, const GripProbeDesc3D& p, std::span<RawGripHit3D>,
               std::vector<GripCandidate3D>& out) const override {
        if (world.rope_count() == 0) {
            return;
        }
        const Vec3f dir = normalize(p.direction);
        const Vec3f reach_end = p.origin + dir * p.reach;
        for (const RopeHandle3D rope : world.ropes()) {
            const std::vector<RopeSegment3D> segs = world.rope_segments(rope);
            if (segs.empty() || (p.mask & layer_bit(world.layer(segs[0].body))) == 0) {
                continue;
            }
            const f32 rope_radius = world.rope_radius(rope);
            f32 best = 3.0e38f;
            u32 best_seg = 0;
            f32 best_t = 0.0f, best_along = 0.0f;
            for (u32 k = 0; k < segs.size(); ++k) {
                f32 s = 0.0f, t = 0.0f;
                closest_segments(p.origin, reach_end, segs[k].a, segs[k].b, s, t);
                const Vec3f on_reach = lerp(p.origin, reach_end, s);
                const Vec3f on_rope = lerp(segs[k].a, segs[k].b, t);
                const f32 dist = length(on_reach - on_rope);
                // The fan: the reach's tolerance grows with the distance along it.
                const f32 allowed = rope_radius + p.radius + p.spread * s * p.reach;
                if (dist <= allowed && dist < best) {
                    best = dist;
                    best_seg = k;
                    best_t = t;
                }
            }
            if (!(best < 3.0e38f)) {
                continue;
            }
            const f32 seg_len = length(segs[0].b - segs[0].a);
            const u32 n = static_cast<u32>(segs.size());
            const f32 here = static_cast<f32>(best_seg) + best_t; // in segments
            // Candidates at the closest point, one hand's width up the rope, and one down it: a second hand takes the next
            // when the first has the nearest.
            const Vec3f t0 = normalize(segs[best_seg].b - segs[best_seg].a);
            const f32 up_sign = dot(t0, p.up) >= 0.0f ? 1.0f : -1.0f;
            const f32 step = seg_len > 1.0e-6f ? 0.3f / seg_len : 1.0f;
            const f32 offsets[3] = {0.0f, up_sign * step, -up_sign * step};
            for (const f32 off : offsets) {
                const f32 u = here + off;
                if (u < 0.0f || u > static_cast<f32>(n)) {
                    continue;
                }
                u32 k = std::min(static_cast<u32>(u), n - 1);
                const f32 frac = u - static_cast<f32>(k);
                GripCandidate3D c;
                c.body = segs[k].body;
                c.user_id = world.user_id(segs[k].body);
                const Vec3f tangent = normalize(segs[k].b - segs[k].a);
                c.point = lerp(segs[k].a, segs[k].b, frac);
                // The side the hand is on: against the way the character faces (its hands come from in front of it), taken
                // off the rope's own direction; the reach's direction if that is along the rope, and anything across it at last.
                Vec3f toward = -p.forward;
                toward -= tangent * dot(toward, tangent);
                if (length(toward) < 1.0e-3f) {
                    toward = -dir;
                    toward -= tangent * dot(toward, tangent);
                }
                if (length(toward) < 1.0e-3f) {
                    toward = cross(tangent, p.up);
                    if (length(toward) < 1.0e-3f) {
                        toward = cross(tangent, Vec3f{1.0f, 0.0f, 0.0f});
                    }
                }
                c.normal = normalize(toward);
                c.kind = GripKind3D::Rope;
                c.quality = 1.0f;
                out.push_back(c);
            }
        }
    }

    GripAnchor3D attach(const World3D& world, const GripCandidate3D& c) const override {
        GripAnchor3D a;
        a.body = c.body;
        a.kind = GripKind3D::Rope;
        // A share of the segment's length from its middle (-0.5 to 0.5): a rope that changes length keeps the hand at the
        // same place along it.
        f32 fraction = 0.0f;
        if (const auto ref = world.rope_segment_of(c.body)) {
            const Vec3f axis = world.rotation(c.body).rotate({0.0f, 1.0f, 0.0f});
            fraction = std::clamp(dot(c.point - world.position(c.body), axis) / std::max(ref->segment_length, 1.0e-6f), -0.5f, 0.5f);
        }
        a.local_point = {0.0f, fraction, 0.0f};
        a.local_normal = world.rotation(c.body).conjugate().rotate(c.normal);
        a.quality = c.quality;
        a.surface_tag = c.surface_tag;
        a.drain = c.drain;
        a.slip = c.slip;
        a.fixed = false;
        a.ledge = false;
        return a;
    }

    // `delta` along the rope moves the hand along it (the sideways part is ignored: a rope has no surface to slide over). False
    // at either end.
    bool move(const World3D& world, GripAnchor3D& a, Vec3f delta, Vec3f, Vec3f) const override {
        const auto ref = world.rope_segment_of(a.body);
        if (!ref) {
            return false;
        }
        Vec3f point{}, tangent{};
        if (!world.rope_point_at(ref->rope, ref->segment, a.local_point.y, point, tangent)) {
            return false;
        }
        const f32 seg = std::max(ref->segment_length, 1.0e-6f);
        f32 u = static_cast<f32>(ref->segment) + a.local_point.y + 0.5f + dot(delta, tangent) / seg;
        if (u < 0.0f || u > static_cast<f32>(ref->segments)) {
            return false;
        }
        const u32 k = std::min(static_cast<u32>(u), ref->segments - 1);
        const Body3D body = world.rope_segment_body(ref->rope, k);
        if (!body.valid()) {
            return false;
        }
        const Vec3f normal_world = world.rotation(a.body).rotate(a.local_normal);
        a.body = body;
        a.local_point = {0.0f, u - static_cast<f32>(k) - 0.5f, 0.0f};
        a.local_normal = world.rotation(body).conjugate().rotate(normal_world);
        return true;
    }
};

} // namespace

const GripProvider3D& rope_grip_provider() {
    static const RopeProvider provider;
    return provider;
}

} // namespace physics3d_detail
} // namespace kin
