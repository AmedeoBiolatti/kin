#pragma once

// The ropes of one World3D (kin/physics3d/rope3d.hpp). Internal: the world's Impl owns one and calls it from step(), the hash and
// the saved state. A rope is capsule bodies and point constraints made through the world's public calls.

#include <kin/physics3d/world3d.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace kin::physics3d_detail {

class RopeSet {
public:
    explicit RopeSet(World3D& world) : _world(&world) {}

    RopeHandle3D create(const RopeDesc3D& desc);
    void destroy(RopeHandle3D h);
    bool contains(RopeHandle3D h) const { return find(h) != nullptr; }
    i32 count() const { return _live; }
    std::vector<RopeHandle3D> list() const;

    std::vector<RopePoint3D> points(RopeHandle3D h) const;
    std::vector<RopeSegment3D> segments(RopeHandle3D h) const;
    RopeState3D state(RopeHandle3D h) const;
    std::optional<RopeClosest3D> closest(RopeHandle3D h, Vec3f p) const;
    bool point_at(RopeHandle3D h, u32 segment, f32 fraction, Vec3f& point, Vec3f& tangent) const;
    Body3D segment_body(RopeHandle3D h, u32 segment) const;
    std::optional<RopeSegmentRef3D> segment_of(Body3D body) const;
    f32 radius(RopeHandle3D h) const;
    Vec3f anchor_force(RopeHandle3D h, RopeEnd3D end) const;
    f32 tension(RopeHandle3D h, RopeEnd3D end) const;
    void set_length(RopeHandle3D h, f32 length);
    void reel(RopeHandle3D h, f32 target, f32 speed);

    // Before the world's step: the loads of the last step are taken off, the length moves toward its target. After: the
    // joints that broke (the constraint events of this step), the collision step's length.
    void pre_step(f32 dt);
    void post_step(f32 sub_dt, std::span<const ConstraintEvent3D> broke);

    std::vector<RopeEvent3D> drain_events() { return std::move(_events); }
    void clear_events() { _events.clear(); }

    void save_header(std::vector<u8>& out) const;
    struct Parsed {
        struct One {
            bool alive = false;
            u32 generation = 0;
            f32 rest = 0.0f, target = 0.0f, speed = 0.0f;
            bool hoisting = false;
        };
        std::vector<One> slots;
    };
    std::unique_ptr<Parsed> parse_header(const u8*& cursor, const u8* end) const;
    // Before the physics system restores: the segments and anchors made for the saved length.
    void apply(const Parsed& parsed);

private:
    struct Slot {
        bool alive = false;
        u32 generation = 0;
        RopeDesc3D desc;
        u32 n = 0;
        f32 rest = 0.0f, target = 0.0f, speed = 0.0f;
        f32 segment_length = 0.0f;
        std::vector<Body3D> segs;
        std::vector<Constraint3D> joints; // n + 1: [0] start anchor, [k] between k-1 and k, [n] end anchor (invalid when free)
        // The force on each anchor (N, world), smoothed over about a quarter of a second: the solver's own residual and a load's bounce make a single
        // step's impulse wander by a tenth or so. Not state: it starts again after a restore.
        std::array<Vec3f, 2> force{};
        bool force_valid = false;
        // From which end the chain keeps its place when the length changes.
        bool from_end = false;
        // A dynamic body anchored at the moving end is carried by the winch while the rope is reeled in (made kinematic).
        bool hoisting = false;
    };

    Slot* find(RopeHandle3D h) { return h.index < _slots.size() && _slots[h.index].alive && _slots[h.index].generation == h.generation ? &_slots[h.index] : nullptr; }
    const Slot* find(RopeHandle3D h) const { return const_cast<RopeSet*>(this)->find(h); }
    void fit(Slot& s, f32 length, bool relay);
    void joint_points(const Slot& s, std::vector<Vec3f>& out) const;

    World3D* _world;
    std::vector<Slot> _slots;
    std::vector<u32> _free;
    i32 _live = 0;
    // From a body number to the rope segment it is.
    struct Ref {
        u32 rope = UINT32_MAX;
        u32 segment = 0;
        u32 generation = 0;
    };
    std::vector<Ref> _by_body;
    std::vector<RopeEvent3D> _events;
    f32 _sub_dt = 1.0f / 60.0f;
};

} // namespace kin::physics3d_detail
