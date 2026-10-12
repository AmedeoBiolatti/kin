#include "level.hpp"

#include <kin/runtime/state_hash.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace summit {

namespace {

// splitmix64: integer arithmetic only, so a seed gives the same numbers on every machine.
struct Rand {
    u64 s;
    // The seed is mixed first: the generator's stream steps by a constant, so seeds a constant's multiple apart would otherwise
    // be the same stream shifted.
    explicit Rand(u64 seed) : s(mix64(seed + 0x1234567ull)) {}
    static u64 mix64(u64 z) {
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    u64 next() {
        u64 z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    f32 uni() { return f32(next() >> 40) * (1.0f / 16777216.0f); }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * uni(); }
};

// A heading and a place: boxes are given in it as forward, right and up from the origin.
struct Frame {
    Vec3f origin{};
    f32 yaw = 0.0f; // degrees about +Y
    Quatf q{};

    Frame() = default;
    Frame(Vec3f o, f32 yaw_degrees) : origin(o), yaw(yaw_degrees), q(Quatf::axis_angle({0.0f, 1.0f, 0.0f}, yaw_degrees)) {}
    Vec3f dir() const { return q.rotate({0.0f, 0.0f, -1.0f}); }
    Vec3f right() const { return q.rotate({1.0f, 0.0f, 0.0f}); }
    // `u` is height above the origin's.
    Vec3f at(f32 f, f32 r, f32 u) const { return origin + dir() * f + right() * r + Vec3f{0.0f, u, 0.0f}; }
};

struct Builder {
    SummitLevel& level;
    // A box from f0 to f1 forward, r0 to r1 right and u0 to u1 up (u from the frame origin's height), turned with the frame.
    Piece& add(PieceKind kind, const Frame& frame, f32 f0, f32 f1, f32 r0, f32 r1, f32 u0, f32 u1) {
        Piece p;
        p.kind = kind;
        p.centre = frame.at(0.5f * (f0 + f1), 0.5f * (r0 + r1), 0.5f * (u0 + u1));
        p.half = {0.5f * (r1 - r0), 0.5f * (u1 - u0), 0.5f * (f1 - f0)};
        p.rotation = frame.q;
        level.pieces.push_back(p);
        return level.pieces.back();
    }
    // A tilted slab: its top surface runs from (f0, u0) to (f1, u1) in the frame (u relative to the origin), `thick` deep.
    Piece& ramp(const Frame& frame, f32 f0, f32 f1, f32 u0, f32 u1, f32 r0, f32 r1, f32 thick) {
        const f32 df = f1 - f0, du = u1 - u0;
        const f32 length = std::sqrt(df * df + du * du);
        const f32 angle = std::atan2(du, df) * 180.0f / pi;
        const Quatf tilt = Quatf::axis_angle({1.0f, 0.0f, 0.0f}, angle);
        const Quatf q = frame.q * tilt;
        const Vec3f normal = q.rotate({0.0f, 1.0f, 0.0f});
        Piece p;
        p.kind = PieceKind::Ramp;
        const Vec3f mid_top = frame.at(0.5f * (f0 + f1), 0.5f * (r0 + r1), 0.5f * (u0 + u1));
        p.centre = mid_top - normal * (0.5f * thick);
        p.half = {0.5f * (r1 - r0), 0.5f * thick, 0.5f * length};
        p.rotation = q;
        p.friction = 0.9f;
        level.pieces.push_back(p);
        return level.pieces.back();
    }
};

} // namespace

// ---- bytes and hash -------------------------------------------------------------------------------------------------------

namespace {
struct Writer {
    std::vector<u8> out;
    void raw(const void* p, std::size_t n) {
        const u8* b = static_cast<const u8*>(p);
        out.insert(out.end(), b, b + n);
    }
    void u(u64 v) { raw(&v, sizeof v); }
    void f(f32 v) { raw(&v, sizeof v); }
    void v3(Vec3f v) {
        f(v.x);
        f(v.y);
        f(v.z);
    }
    void q(Quatf v) {
        f(v.x);
        f(v.y);
        f(v.z);
        f(v.w);
    }
};
} // namespace

std::vector<u8> SummitLevel::bytes() const {
    Writer w;
    w.u(seed);
    w.u(pieces.size());
    for (const Piece& p : pieces) {
        w.u(static_cast<u64>(p.kind));
        w.v3(p.centre);
        w.v3(p.half);
        w.q(p.rotation);
        w.u(p.grippable ? 1 : 0);
        w.f(p.grip);
        w.f(p.slip);
        w.f(p.drain);
        w.u(p.tag);
        w.u(p.hold ? 1 : 0);
        w.f(p.hold_quality);
        w.f(p.friction);
    }
    w.u(camps.size());
    for (const Camp& c : camps) {
        w.u(u64(c.index));
        w.v3(c.spawn);
        w.f(c.radius);
        w.f(c.altitude);
        w.v3(c.marker);
    }
    w.u(ropes.size());
    for (const RopeSpec& r : ropes) {
        w.v3(r.anchor);
        w.f(r.length);
        w.v3(r.wall_normal);
    }
    w.u(route.size());
    for (const Leg& l : route) {
        w.u(static_cast<u64>(l.kind));
        w.v3(l.from);
        w.v3(l.to);
        w.v3(l.normal);
        w.f(l.height);
        w.f(l.roof_height);
        w.f(l.shimmy);
        w.u(u64(i64(l.rope)));
        w.u(u64(i64(l.camp)));
    }
    w.v3(start);
    w.v3(summit);
    w.f(summit_radius);
    w.f(kill_y);
    return w.out;
}

u64 SummitLevel::hash() const {
    const std::vector<u8> b = bytes();
    return hash_bytes(b.data(), b.size());
}

// ---- the generator ------------------------------------------------------------------------------------------------------

SummitLevel generate_level(u64 seed) {
    SummitLevel level;
    level.seed = seed;
    Rand rng(seed);
    Builder build{level};

    // Stage 0: base camp, a field with boulders round its edge.
    Frame frame({0.0f, 0.0f, 0.0f}, rng.range(-15.0f, 15.0f));
    build.add(PieceKind::Ground, frame, -20.0f, 16.0f, -24.0f, 24.0f, -14.0f, 0.0f);
    {
        const i32 n = 7 + i32(rng.uni() * 4.0f);
        for (i32 i = 0; i < n; ++i) {
            const f32 side = (i % 2 == 0) ? 1.0f : -1.0f;
            const f32 r = side * rng.range(7.0f, 19.0f);
            const f32 f = rng.range(-16.0f, 11.0f);
            const f32 s = rng.range(0.5f, 1.5f);
            Piece& p = build.add(PieceKind::Boulder, frame, f - s, f + s, r - s * 1.2f, r + s * 1.2f, 0.0f, s * 1.1f);
            p.rotation = frame.q * Quatf::axis_angle({0.0f, 1.0f, 0.0f}, rng.range(0.0f, 90.0f));
            p.centre.y -= 0.1f;
        }
    }
    const Vec3f base = frame.at(-3.0f, 0.0f, 0.0f);
    level.start = base;
    level.camps.push_back({0, base, 4.0f, 0.0f, frame.at(-2.0f, 6.5f, 0.0f)});
    Vec3f cursor = frame.at(0.0f, 0.0f, 0.0f);
    f32 yaw = frame.yaw;
    f32 u = 0.0f; // world height of the surface the cursor stands on

    const auto leg = [&](LegKind kind, Vec3f from, Vec3f to, const char* label) -> Leg& {
        Leg l;
        l.kind = kind;
        l.from = from;
        l.to = to;
        l.label = label;
        level.route.push_back(l);
        return level.route.back();
    };
    Vec3f position = base; // where the route has got to (the end of the last leg)

    // An approach and a bench in front of the face: new heading, landing, ramp, bench. Returns the frame at the foot of the
    // face (origin there, forward into the rock) and the route's lateral place.
    const auto approach = [&](f32 turn, f32 rise, f32 bench, f32 angle_degrees, const char* label) {
        yaw += turn;
        Frame f(cursor, yaw);
        f.origin.y = u;
        // The landing, thick so that it reads as rock: where the last stage's bench is, in the new heading.
        build.add(PieceKind::Ground, f, -1.0f, 10.0f, -8.0f, 8.0f, -16.0f, -0.03f);
        const f32 length = rise / std::tan(angle_degrees * pi / 180.0f);
        const f32 f0 = 10.0f - 0.5f;
        build.ramp(f, f0, f0 + length, -0.03f, rise, -9.0f, 9.0f, rise + 5.0f);
        build.add(PieceKind::Ground, f, f0 + length - 0.2f, f0 + length + bench, -13.0f, 13.0f, rise - 18.0f, rise - 0.03f);
        // The walk: from where the route is to the foot of the ramp's top.
        const Vec3f top_of_ramp = f.at(f0 + length + 1.0f, 0.0f, rise);
        leg(LegKind::Walk, position, top_of_ramp, label);
        position = top_of_ramp;
        Frame face_frame = f;
        face_frame.origin = f.at(f0 + length + bench, 0.0f, rise); // the foot of the face
        return face_frame;
    };

    // The camp at the top of an obstacle (`rise` above the frame's origin): 3.5 m back from the lip.
    const auto camp_on = [&](const Frame& at_face, f32 rise, f32 lateral) {
        const Vec3f spawn = at_face.at(3.5f, lateral, rise);
        level.camps.push_back({i32(level.camps.size()), spawn, 4.0f, spawn.y, at_face.at(3.0f, lateral + 5.5f, rise)});
        const i32 index = i32(level.camps.size()) - 1;
        Leg& walk = leg(LegKind::Walk, position, spawn, "to the camp");
        walk.camp = index;
        position = spawn;
        leg(LegKind::Rest, spawn, spawn, "rest at the camp").height = 1.0f;
        cursor = spawn;
        u = spawn.y;
    };

    // The face: a block of rock from the foot of the face (`ub`: the height there) up `h`, deep behind it.
    const auto cliff = [&](const Frame& f, f32 ub, f32 h, f32 width, f32 depth, PieceKind kind = PieceKind::Cliff) -> Piece& {
        const f32 rel = ub - f.origin.y;
        return build.add(kind, f, 0.0f, depth, -width, width, rel - 14.0f, rel + h);
    };

    // The face's route line: the foot point a standing player takes (0.95 m off the rock, hands then reach it) and the top.
    const auto climb_leg = [&](const Frame& f, f32 lateral, f32 h, const char* label) -> Leg& {
        const Vec3f foot = f.at(-0.95f, lateral, 0.0f);
        const Vec3f top = f.at(1.2f, lateral, h);
        leg(LegKind::Walk, position, foot, "to the foot");
        Leg& c = leg(LegKind::Climb, foot, top, label);
        c.normal = -f.dir();
        c.height = h;
        position = top;
        return c;
    };
    // Rock has some tilt: a lean of a degree or two in the face reads as rock and still climbs.

    // Stage 1: the scramble. A 24 to 28 degree approach and a wall of 4 to 5.5 m.
    {
        Frame f = approach(rng.range(-8.0f, 8.0f), rng.range(19.0f, 25.0f), 5.0f, rng.range(24.0f, 28.0f), "scramble up the approach");
        const f32 h = rng.range(4.0f, 5.5f);
        const f32 lateral = rng.range(-3.0f, 3.0f);
        const f32 ub = f.origin.y;
        cliff(f, ub, h, 14.0f, 34.0f);
        climb_leg(f, lateral, h, "the first wall");
        camp_on(f, h, lateral);
    }

    // Stage 2: the face. Two pitches with a bench between, and on the second a smooth slab with holds beside the cracks.
    {
        Frame f = approach(rng.range(-14.0f, 14.0f), rng.range(14.0f, 18.0f), 5.0f, rng.range(24.0f, 28.0f), "approach the face");
        const f32 h1 = rng.range(5.0f, 6.5f);
        const f32 ub = f.origin.y;
        const f32 lateral = rng.range(-3.0f, 3.0f);
        cliff(f, ub, h1, 14.0f, 36.0f);
        climb_leg(f, lateral, h1, "first pitch");
        // A bench 4.5 m deep, then the second pitch (set back).
        const f32 bench = 4.5f;
        Frame g = f;
        g.origin = f.at(bench, 0.0f, h1);
        const f32 h2 = rng.range(6.0f, 7.5f);
        cliff(g, g.origin.y, h2, 14.0f, 30.0f);
        const f32 lateral2 = std::clamp(lateral + rng.range(-3.0f, 3.0f), -5.0f, 4.0f);
        // The holds line: a smooth slab 0.1 m proud of the face, holds on it every 0.45 m up, alternating, a lip on top.
        {
            const f32 r0 = 6.0f, r1 = 10.5f;
            Piece& slab = build.add(PieceKind::Smooth, g, -0.12f, 0.0f, r0, r1, 0.0f, h2);
            slab.grippable = false;
            Piece& lip = build.add(PieceKind::Lip, g, -0.35f, 0.4f, r0, r1, h2 - 0.2f, h2 + 0.02f);
            (void)lip;
            u32 i = 0;
            for (f32 y = 1.7f; y <= h2 - 0.5f; y += 0.45f, ++i) {
                const f32 r = (r0 + r1) * 0.5f + ((i % 2) ? 0.18f : -0.18f);
                Piece& hold = build.add(PieceKind::Hold, g, -0.27f, -0.12f, r - 0.15f, r + 0.15f, y - 0.06f, y + 0.06f);
                hold.hold = true;
                hold.hold_quality = 0.8f;
            }
        }
        climb_leg(g, lateral2, h2, "second pitch");
        camp_on(g, h2, lateral2);
    }

    // Stage 3: the chimney. Two walls 1.5 m apart, wedged: cheaper to climb (drain x0.6). The front wall is a separate block
    // you walk round to get in.
    {
        Frame f = approach(rng.range(-14.0f, 14.0f), rng.range(15.0f, 19.0f), 6.5f, rng.range(24.0f, 28.0f), "approach the chimney");
        const f32 h = rng.range(7.0f, 8.5f);
        const f32 ub = f.origin.y;
        const f32 lateral = rng.range(-1.5f, 1.5f);
        Piece& back = cliff(f, ub, h, 14.0f, 30.0f);
        back.drain = 0.6f;
        // The front wall: its back face 1.5 m from the main one, 5 m wide, as high.
        const f32 rel = ub - f.origin.y;
        Piece& front = build.add(PieceKind::Cliff, f, -4.5f, -1.5f, lateral - 3.5f, lateral + 3.5f, rel - 14.0f, rel + h);
        front.drain = 0.6f;
        // The chimney's floor is the bench: you step in from the sides. The foot is in the gap.
        const Vec3f foot = f.at(-0.75f, lateral, 0.0f);
        const Vec3f top = f.at(1.2f, lateral, h);
        // Walk round the front wall: out to its side, forward, in.
        const Vec3f side = f.at(-6.0f, lateral + 5.0f, 0.0f);
        leg(LegKind::Walk, position, side, "round the front wall");
        const Vec3f beside = f.at(-0.75f, lateral + 5.0f, 0.0f);
        leg(LegKind::Walk, side, beside, "along the chimney");
        leg(LegKind::Walk, beside, foot, "into the chimney");
        Leg& c = leg(LegKind::Climb, foot, top, "the chimney");
        c.normal = -f.dir();
        c.height = h;
        position = top;
        camp_on(f, h, lateral);
    }

    // Stage 4: the overhang. A wall with a roof slab 3.6 m up that reaches 1.6 m out, over the first 8 m of it from the left: the
    // climb goes up to the roof, along under it to its end, and on up. It takes most of a bar.
    {
        Frame f = approach(rng.range(-14.0f, 14.0f), rng.range(14.0f, 18.0f), 6.0f, rng.range(24.0f, 28.0f), "approach the overhang");
        const f32 h = rng.range(5.2f, 5.8f);
        const f32 ub = f.origin.y;
        cliff(f, ub, h, 14.0f, 30.0f);
        const f32 rel = ub - f.origin.y;
        const f32 roof_y = rng.range(3.5f, 3.8f);
        const f32 r_end = rng.range(1.0f, 3.0f);
        const f32 shimmy = rng.range(4.0f, 5.0f);
        Piece& roof = build.add(PieceKind::Roof, f, -1.6f, 0.0f, r_end - 8.0f, r_end, rel + roof_y, rel + roof_y + 0.4f);
        roof.drain = 1.0f;
        const f32 start_r = r_end - shimmy;           // under the roof here
        const f32 end_r = r_end + 0.9f;               // and on up clear of its end
        const Vec3f foot = f.at(-0.95f, start_r, 0.0f);
        const Vec3f top = f.at(1.2f, end_r, h);
        leg(LegKind::Walk, position, foot, "to the foot under the roof");
        Leg& c = leg(LegKind::Overhang, foot, top, "the overhang");
        c.normal = -f.dir();
        c.height = h;
        c.roof_height = roof_y;
        c.shimmy = end_r - start_r;
        position = top;
        camp_on(f, h, end_r);
    }

    // Stage 5: the rope. A smooth wall of 6 m with a lip, a rope hanging 0.6 m out from it from a beam on a pole.
    {
        Frame f = approach(rng.range(-14.0f, 14.0f), rng.range(16.0f, 21.0f), 6.0f, rng.range(24.0f, 28.0f), "approach the rope");
        const f32 h = rng.range(5.8f, 6.4f);
        const f32 ub = f.origin.y;
        const f32 lateral = rng.range(-2.0f, 2.0f);
        Piece& mass = cliff(f, ub, h, 14.0f, 30.0f);
        mass.grippable = false;
        mass.kind = PieceKind::Smooth;
        const f32 rel = ub - f.origin.y;
        // The lip stands 0.2 m out of the wall along the top (a grippable thing flush behind a smooth wall is not seen by a
        // cast that stops at the first thing it meets).
        build.add(PieceKind::Lip, f, -0.2f, 1.5f, lateral - 4.0f, lateral + 4.0f, rel + h - 0.25f, rel + h + 0.02f);
        // A pole behind the lip with a beam out over it: the rope hangs from the beam's underside.
        build.add(PieceKind::Post, f, 0.3f, 0.8f, lateral + 0.9f, lateral + 1.3f, rel + h, rel + h + 2.5f);
        build.add(PieceKind::Post, f, -0.9f, 0.8f, lateral - 0.15f, lateral + 1.3f, rel + h + 2.2f, rel + h + 2.5f);
        RopeSpec rope;
        rope.anchor = f.at(-0.6f, lateral, h + 2.15f);
        rope.length = h + 2.15f - 0.3f;
        rope.wall_normal = -f.dir();
        level.ropes.push_back(rope);
        const Vec3f foot = f.at(-1.0f, lateral, 0.0f);
        const Vec3f top = f.at(1.2f, lateral, h);
        leg(LegKind::Walk, position, foot, "to the rope");
        Leg& r = leg(LegKind::Rope, foot, top, "the rope");
        r.normal = -f.dir();
        r.height = h;
        r.rope = 0;
        position = top;
        camp_on(f, h, lateral);
    }

    // Stage 6: the plateau (camp 6), then a last ramp and the summit flat with boulders on it.
    {
        Frame f = approach(rng.range(-14.0f, 14.0f), rng.range(14.0f, 18.0f), 8.0f, rng.range(24.0f, 27.0f), "up to the plateau");
        // The plateau is wide.
        build.add(PieceKind::Ground, f, 0.0f, 24.0f, -18.0f, 18.0f, -14.0f, -0.03f);
        camp_on(f, 0.0f, 0.0f);
        const f32 rise2 = rng.range(10.0f, 13.0f);
        const f32 len2 = rise2 / std::tan(26.0f * pi / 180.0f);
        build.ramp(f, 14.0f, 14.0f + len2, -0.03f, rise2, -7.0f, 7.0f, rise2 + 4.0f);
        build.add(PieceKind::Ground, f, 14.0f + len2 - 0.2f, 14.0f + len2 + 10.0f, -9.0f, 9.0f, rise2 - 14.0f, rise2 - 0.03f);
        const Vec3f top = f.at(14.0f + len2 + 6.0f, 0.0f, rise2);
        leg(LegKind::Walk, position, f.at(14.0f, 0.0f, 0.0f), "the plateau");
        leg(LegKind::Walk, f.at(14.0f, 0.0f, 0.0f), top, "the last ramp");
        // Boulders on the summit's flat, not on the route.
        for (i32 i = 0; i < 3; ++i) {
            const f32 s = rng.range(0.6f, 1.2f);
            const f32 rr = (i % 2 ? 1.0f : -1.0f) * rng.range(3.5f, 6.0f);
            const f32 ff = 14.0f + len2 + rng.range(1.0f, 8.0f);
            Piece& p = build.add(PieceKind::Boulder, f, ff - s, ff + s, rr - s, rr + s, rise2 - 0.03f, rise2 + s * 1.3f);
            p.rotation = f.q * Quatf::axis_angle({0.0f, 1.0f, 0.0f}, rng.range(0.0f, 90.0f));
        }
        level.summit = top;
        leg(LegKind::Summit, top, top, "the summit");
        position = top;
    }

    // Bounds and the kill plane.
    Vec3f lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const Piece& p : level.pieces) {
        // The box's extent in the world: the sum of its axes' absolute components.
        const Vec3f ax = p.rotation.rotate({p.half.x, 0.0f, 0.0f}), ay = p.rotation.rotate({0.0f, p.half.y, 0.0f}),
                    az = p.rotation.rotate({0.0f, 0.0f, p.half.z});
        const Vec3f e{std::abs(ax.x) + std::abs(ay.x) + std::abs(az.x), std::abs(ax.y) + std::abs(ay.y) + std::abs(az.y),
                      std::abs(ax.z) + std::abs(ay.z) + std::abs(az.z)};
        lo = {std::min(lo.x, p.centre.x - e.x), std::min(lo.y, p.centre.y - e.y), std::min(lo.z, p.centre.z - e.z)};
        hi = {std::max(hi.x, p.centre.x + e.x), std::max(hi.y, p.centre.y + e.y), std::max(hi.z, p.centre.z + e.z)};
    }
    level.bounds = {lo, hi};
    level.kill_y = lo.y - 6.0f;
    return level;
}

// ---- the planner --------------------------------------------------------------------------------------------------------

RoutePlan plan_route(const SummitLevel& level, const ClimbCosts& c) {
    RoutePlan plan;
    plan.least_margin = c.max_stamina;
    Vec3f at = level.start;
    const f32 hang = c.drain_two_hands + c.drain_moving;
    for (const Leg& l : level.route) {
        LegPlan p;
        p.kind = l.kind;
        p.start_stamina = c.max_stamina;
        switch (l.kind) {
        case LegKind::Walk: {
            const Vec3f d = l.to - at;
            p.seconds = length(d) / c.walk_speed;
            p.lowest = c.max_stamina;
            at = l.to;
            break;
        }
        case LegKind::Climb: {
            // The hands take hold about 2 m up (the head's height, the reach) with the feet on the ground, which costs nothing
            // (standing is rest): the climb that costs is from there to the lip, then the settling and the pull-up.
            const f32 t = std::max(0.0f, l.height - c.first_hold_height) / c.climb_speed + c.settle_seconds;
            p.seconds = t + 0.7f;
            p.cost = 2.0f * c.grab_cost + hang * t + c.pull_up_cost;
            at = l.to;
            break;
        }
        case LegKind::Overhang: {
            const f32 t = std::max(0.0f, l.height - c.first_hold_height) / c.climb_speed + std::abs(l.shimmy) / c.shimmy_speed + c.settle_seconds;
            p.seconds = t + 0.7f;
            p.cost = 2.0f * c.grab_cost + hang * t + c.pull_up_cost;
            at = l.to;
            break;
        }
        case LegKind::Rope: {
            const f32 t = std::max(0.0f, l.height + 0.35f - c.first_hold_height) / c.rope_speed + c.settle_seconds + 1.5f;
            p.seconds = t + 2.0f;
            // Two grabs on the rope, then a hand at a time to the lip (two more), the pull-up.
            p.cost = 4.0f * c.grab_cost + hang * t + c.pull_up_cost;
            at = l.to;
            break;
        }
        case LegKind::Rest:
            p.seconds = c.regen_delay + (c.max_stamina * 0.8f) / c.regen_rate;
            break;
        case LegKind::Summit:
            break;
        }
        if (l.kind != LegKind::Walk && l.kind != LegKind::Rest && l.kind != LegKind::Summit) {
            p.lowest = c.max_stamina - p.cost;
            p.feasible = p.cost <= c.max_stamina - 4.0f;
            plan.worst_cost = std::max(plan.worst_cost, p.cost);
            plan.least_margin = std::min(plan.least_margin, c.max_stamina - p.cost);
            plan.climbable = plan.climbable && p.feasible;
        } else {
            p.lowest = c.max_stamina;
        }
        plan.total_seconds += p.seconds;
        plan.legs.push_back(p);
    }
    return plan;
}

// ---- render meshes --------------------------------------------------------------------------------------------------------

namespace {

u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// A value in 0..1 from three lattice coordinates and a seed.
f32 lattice(i32 x, i32 y, i32 z, u32 seed) {
    return f32(mix(u32(x) * 73856093u ^ u32(y) * 19349663u ^ u32(z) * 83492791u ^ seed) & 0xFFFFu) / 65535.0f;
}

// Smooth value noise at `p` (lattice cell `scale` metres).
f32 value_noise(Vec3f p, f32 scale, u32 seed) {
    const Vec3f q = p * (1.0f / scale);
    const i32 x0 = i32(std::floor(q.x)), y0 = i32(std::floor(q.y)), z0 = i32(std::floor(q.z));
    const f32 fx = q.x - f32(x0), fy = q.y - f32(y0), fz = q.z - f32(z0);
    const auto s = [](f32 t) { return t * t * (3.0f - 2.0f * t); };
    const f32 sx = s(fx), sy = s(fy), sz = s(fz);
    f32 c[2][2][2];
    for (i32 i = 0; i < 2; ++i)
        for (i32 j = 0; j < 2; ++j)
            for (i32 k = 0; k < 2; ++k) c[i][j][k] = lattice(x0 + i, y0 + j, z0 + k, seed);
    const auto l = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
    return l(l(l(c[0][0][0], c[1][0][0], sx), l(c[0][1][0], c[1][1][0], sx), sy), l(l(c[0][0][1], c[1][0][1], sx), l(c[0][1][1], c[1][1][1], sx), sy), sz);
}

Color shade(Color c, f32 k) {
    const auto f = [&](u8 v) { return u8(std::clamp(f32(v) * k, 0.0f, 255.0f)); };
    return Color::rgb(f(c.r), f(c.g), f(c.b));
}

// One box of the level as faces cut into cells of about `cell` metres, the colours of its vertices from noise at the vertex's
// place in the world: mottled, with strata (bands in y), so that a flat face is not a flat colour. The shape is the box's.
void append_box(MeshData& out, const Piece& p, Color tint, u32 seed, f32 cell) {
    const Mat4f m = Mat4f::trs(p.centre, p.rotation, {1.0f, 1.0f, 1.0f});
    const Vec3f h = p.half;
    // Each face: its normal axis (0 x, 1 y, 2 z), the sign, and the two axes it spans.
    for (i32 axis = 0; axis < 3; ++axis) {
        for (i32 sign = -1; sign <= 1; sign += 2) {
            const i32 ua = (axis + 1) % 3, va = (axis + 2) % 3;
            const f32 hs[3] = {h.x, h.y, h.z};
            const i32 nu = std::clamp(i32(std::ceil(2.0f * hs[ua] / cell)), 1, 40);
            const i32 nv = std::clamp(i32(std::ceil(2.0f * hs[va] / cell)), 1, 40);
            Vec3f normal{};
            (axis == 0 ? normal.x : axis == 1 ? normal.y : normal.z) = f32(sign);
            const Vec3f wn = p.rotation.rotate(normal);
            const u32 base = u32(out.vertices.size());
            for (i32 j = 0; j <= nv; ++j) {
                for (i32 i = 0; i <= nu; ++i) {
                    f32 c[3];
                    c[axis] = f32(sign) * hs[axis];
                    c[ua] = -hs[ua] + 2.0f * hs[ua] * f32(i) / f32(nu);
                    c[va] = -hs[va] + 2.0f * hs[va] * f32(j) / f32(nv);
                    const Vec3f local{c[0], c[1], c[2]};
                    const Vec3f world = m.apply(local);
                    // Mottle, strata, a darker foot and a little grain.
                    const f32 blotch = value_noise(world, 3.2f, seed) * 0.6f + value_noise(world, 1.1f, seed + 7u) * 0.4f;
                    const f32 strata = lattice(0, i32(std::floor(world.y * 1.1f)), 0, seed + 13u);
                    const f32 grain = lattice(i32(std::floor(world.x * 3.0f)), i32(std::floor(world.y * 3.0f)), i32(std::floor(world.z * 3.0f)), seed + 29u);
                    const f32 k = 0.72f + 0.38f * blotch + 0.14f * (strata - 0.5f) + 0.06f * (grain - 0.5f);
                    MeshVertex v;
                    v.position = local;
                    v.normal = normal;
                    v.uv = {f32(i) / f32(nu), f32(j) / f32(nv)};
                    v.color = shade(tint, k);
                    out.vertices.push_back(v);
                }
            }
            // Winding: counter-clockwise seen from outside, whichever way the axes' cross product points.
            const Vec3f eu = axis == 0 ? Vec3f{0, 1, 0} : axis == 1 ? Vec3f{0, 0, 1} : Vec3f{1, 0, 0};
            const Vec3f ev = axis == 0 ? Vec3f{0, 0, 1} : axis == 1 ? Vec3f{1, 0, 0} : Vec3f{0, 1, 0};
            const bool flip = dot(cross(eu, ev), normal) < 0.0f;
            for (i32 j = 0; j < nv; ++j) {
                for (i32 i = 0; i < nu; ++i) {
                    const u32 a = base + u32(j * (nu + 1) + i), b = a + 1, c2 = a + u32(nu + 1) + 1, d = a + u32(nu + 1);
                    if (!flip) out.indices.insert(out.indices.end(), {a, b, c2, a, c2, d});
                    else out.indices.insert(out.indices.end(), {a, c2, b, a, d, c2});
                }
            }
            (void)wn;
        }
    }
    // The vertices were made in the box's own space: move them to the world with the normals turned.
    (void)m;
}

} // namespace

std::vector<RenderMeshData> build_render_meshes(const SummitLevel& level) {
    enum Class { Rock, Smooth, Roof, Hold, Lip, Ground, Count };
    std::vector<RenderMeshData> out(Count);
    const char* names[Count] = {"rock", "smooth", "roof", "hold", "lip", "ground"};
    const Color tints[Count] = {Color::rgb(176, 160, 142), Color::rgb(176, 178, 186), Color::rgb(138, 128, 124), Color::rgb(232, 136, 66),
                                Color::rgb(206, 186, 150), Color::rgb(152, 160, 124)};
    for (i32 i = 0; i < Count; ++i) {
        out[std::size_t(i)].name = names[i];
        out[std::size_t(i)].material = {.color = colors::white, .roughness = 0.94f};
    }
    for (std::size_t i = 0; i < level.pieces.size(); ++i) {
        const Piece& p = level.pieces[i];
        Class c = Rock;
        switch (p.kind) {
        case PieceKind::Ground: c = Ground; break;
        case PieceKind::Ramp: c = Ground; break;
        case PieceKind::Cliff: c = Rock; break;
        case PieceKind::Boulder: c = Rock; break;
        case PieceKind::Roof: c = Roof; break;
        case PieceKind::Smooth: c = Smooth; break;
        case PieceKind::Hold: c = Hold; break;
        case PieceKind::Lip: c = Lip; break;
        case PieceKind::Post: c = Roof; break;
        }
        MeshData box;
        append_box(box, p, tints[c], u32(level.seed) * 2654435761u + u32(i) * 40503u, c == Hold || c == Lip ? 0.25f : 1.6f);
        out[std::size_t(c)].mesh.append(box, Mat4f::trs(p.centre, p.rotation, {1.0f, 1.0f, 1.0f}));
    }
    std::vector<RenderMeshData> kept;
    for (RenderMeshData& m : out) {
        if (!m.mesh.vertices.empty()) kept.push_back(std::move(m));
    }
    return kept;
}

} // namespace summit
