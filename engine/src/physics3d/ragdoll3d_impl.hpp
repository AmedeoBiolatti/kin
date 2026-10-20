#pragma once

// The ragdolls of one World3D (kin/physics3d/ragdoll3d.hpp). Internal: the world's Impl owns one and calls it from step(), the
// hash and the saved state. Written over the world's public body and constraint calls: a ragdoll is ordinary bodies, marked
// as its own (BodyOwner3D::Ragdoll) and destroyed and switched by the world's owner calls.

#include <kin/physics3d/world3d.hpp>

#include <algorithm>
#include <memory>
#include <span>
#include <vector>

namespace kin::physics3d_detail {

class RagdollSet {
public:
    explicit RagdollSet(World3D& world) : _world(&world) {}

    RagdollHandle3D create(const RagdollDesc& desc, std::span<const Mat4f> bone_world, f32 scale);
    void destroy(RagdollHandle3D h);
    bool contains(RagdollHandle3D h) const { return find(h) != nullptr; }
    i32 count() const { return _live; }
    std::vector<RagdollHandle3D> list() const;
    bool has_body(RagdollHandle3D h,Body3D body)const;
    std::vector<Body3D> bodies(RagdollHandle3D h) const;
    const RagdollDesc* desc(RagdollHandle3D h) const;

    void set_targets(RagdollHandle3D h, std::span<const Mat4f> bone_world);
    void start(RagdollHandle3D h, const RagdollStart3D& start);
    void blend(RagdollHandle3D h, f32 alpha, f32 seconds);
    void get_up(RagdollHandle3D h, f32 seconds);
    void animate(RagdollHandle3D h);
    void set_weights(RagdollHandle3D h, std::span<const f32> weights);
    void set_subtree(RagdollHandle3D h, i32 bone, f32 weight);
    RagdollState3D state(RagdollHandle3D h) const;
    void pose(RagdollHandle3D h, std::vector<RagdollBonePose3D>& out) const;
    RagdollPlace3D place(RagdollHandle3D h, Vec3f up, Layer3DMask mask, f32 max_distance, Body3D ignore) const;

    // Before the world's step: blends advance, bodies change between kinematic and dynamic, kinematic ones follow the
    // targets. After it: settling, events.
    void pre_step(f32 dt);
    void post_step(f32 dt);

    std::span<const RagdollEvent3D> events() const { return _events; }
    std::vector<RagdollEvent3D> drain_events() {
        _batch = 0;
        return std::move(_events);
    }
    void drain_events(std::vector<RagdollEvent3D>& output) {
        output.swap(_events);
        clear_events();
    }
    void clear_events() {
        _events.clear();
        _batch = 0;
    }
    // Keeps the first `limit` and says how many went: the newest are dropped, as the world's contact events are
    // (World3DConfig::max_events). What is kept is sorted already, so the next batch starts after it.
    std::size_t cap_events(std::size_t limit) {
        if (_events.size() <= limit) {
            return 0;
        }
        const std::size_t dropped = _events.size() - limit;
        _events.resize(limit);
        _batch = std::min(_batch, _events.size());
        return dropped;
    }

    // State: the slot table and each live ragdoll's own state (bytes for the hash and the saved state).
    void save_header(std::vector<u8>& out) const;
    struct Parsed {
        struct One {
            bool alive = false;
            u32 generation = 0;
            RagdollMode3D mode = RagdollMode3D::Animated;
            f32 alpha = 0.0f, from = 0.0f, to = 0.0f, t = 0.0f, duration = 0.0f, settle_timer = 0.0f;
            bool frozen = false, settled = false, has_targets = false, started = false;
            std::vector<f32> weights;
            std::vector<Vec3f> target_position;
            std::vector<Quatf> target_rotation;
            std::vector<Vec3f> snap_position;
            std::vector<Quatf> snap_rotation;
        };
        std::vector<One> slots;
    };
    // Parses what save_header wrote; null when the set of ragdolls is not the same.
    std::unique_ptr<Parsed> parse_header(const u8*& cursor, const u8* end) const;
    // Puts the saved state in and the bodies' motion types as they were, before the physics system restores.
    void apply(const Parsed& parsed);

private:
    struct Slot {
        bool alive = false;
        u32 generation = 0;
        RagdollDesc desc;
        f32 scale = 1.0f;
        std::vector<Body3D> bodies;
        std::vector<Constraint3D> joints;
        std::vector<f32> weight;
        std::vector<Vec3f> target_position;
        std::vector<Quatf> target_rotation;
        // The bodies' places when a get-up began: what it blends from (the bodies themselves follow the clip by then).
        std::vector<Vec3f> snap_position;
        std::vector<Quatf> snap_rotation;
        bool has_targets = false;
        RagdollMode3D mode = RagdollMode3D::Animated;
        f32 alpha = 0.0f, from = 0.0f, to = 0.0f, t = 0.0f, duration = 0.0f;
        bool frozen = false;
        bool settled = false;
        f32 settle_timer = 0.0f;
        bool started = false; // a start was made since the last recovery (Settled is for these)
        // Cached from the state: which bodies are dynamic now, and whether the bodies are parked.
        std::vector<u8> dynamic;
        bool parked = false;
    };

    Slot* find(RagdollHandle3D h) { return h.index < _slots.size() && _slots[h.index].alive && _slots[h.index].generation == h.generation ? &_slots[h.index] : nullptr; }
    const Slot* find(RagdollHandle3D h) const { return const_cast<RagdollSet*>(this)->find(h); }
    void apply_motion(Slot& s, bool keep_velocity);
    void push(const Slot& s, u32 index, RagdollEventKind3D kind);

    World3D* _world;
    std::vector<Slot> _slots;
    std::vector<u32> _free;
    i32 _live = 0;
    std::vector<RagdollEvent3D> _events;
    // Where the batch that is still to be sorted starts (events made since the last step).
    std::size_t _batch = 0;
};

} // namespace kin::physics3d_detail
