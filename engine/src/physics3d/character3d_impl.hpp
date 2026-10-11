#pragma once

// The character controllers of one World3D (kin/physics3d/character3d.hpp), over Jolt's CharacterVirtual. Internal: the
// world's Impl owns one and calls it from step(), the hash and the saved state.

#include "jolt_runtime.hpp"
#include "traversal3d_impl.hpp"

#include <kin/physics3d/character3d.hpp>
#include <kin/physics3d/world3d.hpp>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorder.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace kin::physics3d_detail {

// Which layers a character's own movement meets.
class MaskFilter final : public JPH::ObjectLayerFilter {
public:
    explicit MaskFilter(Layer3DMask mask) : _mask(mask) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer < 32 && (_mask >> layer & 1u) != 0; }

private:
    Layer3DMask _mask;
};

// What a character's own movement never meets: its own body and every body of its group (a ragdoll bound to it).
class OwnBodyFilter final : public JPH::BodyFilter {
public:
    template <typename Env>
    OwnBodyFilter(JPH::BodyID self, const Env& env) : _self(self), _group(env.group_of ? env.group_of(self) : 0u), _group_of(&env.group_of) {}
    bool ShouldCollide(const JPH::BodyID& id) const override {
        if (id == _self) {
            return false;
        }
        return _group == 0 || !*_group_of || (*_group_of)(id) != _group;
    }
    bool ShouldCollideLocked(const JPH::Body&) const override { return true; }

private:
    JPH::BodyID _self;
    u32 _group;
    const std::function<u32(const JPH::BodyID&)>* _group_of;
};

// What the set needs of the world around it.
struct CharacterEnv {
    // The world the set is in: grab and climb asks it the public questions (rays, sweeps, a body's place and velocity) and
    // loads what is held.
    World3D* world = nullptr;
    JPH::PhysicsSystem* system = nullptr;
    JPH::TempAllocator* temp = nullptr;
    const std::array<u32, 32>* table = nullptr;
    // The world's gravity vector as it is now.
    std::function<Vec3f()> gravity;
    // The collision group of a Jolt body (0: none): a character skips the bodies of its own group (its ragdoll's).
    std::function<u32(const JPH::BodyID&)> group_of;
    // The kin body a Jolt body is (invalid for none), and its user id.
    std::function<Body3D(const JPH::BodyID&)> body_of;
    std::function<u64(Body3D)> user_id_of;
    // The live body in a slot (invalid when it is gone).
    std::function<Body3D(u32)> body_of_slot;
    // The inner body of a character becomes a body slot of the world (registered) and goes again (unregistered: the
    // Jolt body is the character's to destroy).
    std::function<Body3D(const JPH::BodyID&, u64 user_id)> register_inner;
    std::function<void(Body3D)> unregister_inner;
};

class CharacterSet {
public:
    explicit CharacterSet(CharacterEnv env)
        : _env(std::move(env)) {}

    Character3D create(const CharacterDesc3D& desc);
    void destroy(Character3D c);
    bool contains(Character3D c) const { return find(c) != nullptr; }
    i32 count() const { return _live; }
    std::vector<Character3D> list() const;

    void set_input(Character3D c, const CharacterInput3D& input);
    CharacterInput3D input(Character3D c) const;
    CharacterState3D state(Character3D c) const;
    std::vector<CharacterContact3D> contacts(Character3D c) const;
    void teleport(Character3D c, Vec3f feet);
    void set_up(Character3D c, Vec3f up, bool instant);
    bool set_crouch(Character3D c, bool crouch);
    void set_desc(Character3D c, const CharacterDesc3D& desc);
    void set_traversal_input(Character3D c, const TraversalInput3D& input);
    TraversalInput3D traversal_input(Character3D c) const;
    TraversalState3D traversal(Character3D c) const;
    void set_traversal_resource(Character3D c, f32 value);
    // A disabled character is skipped by update() and its body collides with nothing (a ragdoll has taken its place).
    void set_enabled(Character3D c, bool enabled);
    bool enabled(Character3D c) const;

    // Advances every character by dt, in ascending number, before the world's step.
    void update(f32 dt);

    std::span<const CharacterEvent3D> events() const { return _events; }
    std::vector<CharacterEvent3D> drain_events() { return std::move(_events); }
    void clear_events() { _events.clear(); }
    // Keeps the newest `limit` (events that nobody drains must not grow without end).
    void cap_events(std::size_t limit) {
        if (_events.size() > limit) {
            _events.erase(_events.begin(), _events.end() - static_cast<std::ptrdiff_t>(limit));
        }
    }

    // State: for state_hash() (these bytes), save_state() and restore_state().
    // Slot table (generation, alive) and each live character's own state, then (after the physics system's) Jolt's.
    void save_header(std::vector<u8>& out) const;
    // Parses what save_header wrote; false when the set of characters is not the same. Fills `parsed` for apply().
    struct Parsed {
        struct One {
            bool alive = false;
            u32 generation = 0;
            u64 user_id = 0;
            CharacterInput3D input;
            bool prev_jump = false;
            f32 jump_buffer = 0.0f, coyote = 0.0f;
            Vec3f up, up_target;
            bool up_explicit = false, crouching = false, stand_blocked = false;
            Vec3f velocity;
            Vec3f control;
            f32 vertical = 0.0f;
            Vec3f relative;
            f32 air_time = 0.0f;
            bool jumped = false;
            u8 ground = 0;
            bool touching_ceiling = false;
            std::vector<std::pair<u32, bool>> touched;
            TravData trav;
            bool disabled = false;
        };
        std::vector<One> slots;
    };
    std::unique_ptr<Parsed> parse_header(const u8*& cursor, const u8* end) const;
    void save_jolt(JPH::StateRecorder& out) const;
    // Before the physics system restores: puts each character in the stance that was saved.
    void prepare_restore(const Parsed& parsed);
    // After the physics system has restored.
    bool restore(Parsed& parsed, JPH::StateRecorder& in);
    std::size_t slot_count() const { return _slots.size(); }

private:
    struct Slot {
        JPH::Ref<JPH::CharacterVirtual> ch;
        JPH::RefConst<JPH::Shape> stand, crouch;
        u32 generation = 0;
        bool alive = false;
        // Handed over to a ragdoll: not updated, collides with nothing (saved in the alive byte).
        bool disabled = false;
        CharacterDesc3D desc;
        CharacterInput3D input;
        Body3D inner{};
        // Controller state (saved, hashed).
        bool prev_jump = false;
        f32 jump_buffer = 0.0f;
        f32 coyote = 0.0f;
        Vec3f up{0.0f, 1.0f, 0.0f};
        Vec3f up_target{0.0f, 1.0f, 0.0f};
        bool up_explicit = false;
        bool crouching = false;
        bool stand_blocked = false;
        Vec3f velocity{};
        Vec3f control{};
        f32 vertical = 0.0f;
        Vec3f relative{};
        f32 air_time = 0.0f;
        bool jumped = false;
        CharacterGround3D ground = CharacterGround3D::InAir;
        bool touching_ceiling = false;
        // What it touched at the last step: (body number, sensor) sorted, to make Begin and End from.
        std::vector<std::pair<u32, bool>> touched;
        std::vector<CharacterContact3D> contacts;
        // Grab and climb (TraversalDesc3D::enabled): what the limbs hold, the resource, the mode.
        TravData trav;
    };

    Slot* find(Character3D c) { return c.index < _slots.size() && _slots[c.index].alive && _slots[c.index].generation == c.generation ? &_slots[c.index] : nullptr; }
    const Slot* find(Character3D c) const { return const_cast<CharacterSet*>(this)->find(c); }

    void update_one(u32 index, Slot& s, f32 dt);
    // Grab and climb. Called at the start of a step of a character with traversal on: takes the grips, the resource and
    // the hanging or mantling move. True when it moved the character itself (hanging, mantling); false leaves the step to
    // the walking controller (which it may have given a jump off or a fall to do). The events go in `traversal_events_`.
    bool update_traversal(u32 index, Slot& s, f32 dt);
    bool mantle_step(u32 index, Slot& s, f32 dt);
    // Where a grip's anchor is in the world (false when its body is gone); `predicted` adds the body's motion over `dt`
    // (a kinematic platform that is about to move: the hand and the character arrive together).
    bool anchor_world(const Slot& s, const GripData& g, bool predicted, f32 dt, Vec3f& point, Vec3f& normal) const;
    // One limb lets go: the event (when it held), the limb free, latched when the command is still given.
    void release_grip(Slot& s, u32 hand, GripRelease3D reason);
    void add_traversal_event(CharacterEventKind3D kind, u32 hand, u8 reason, Body3D body, Vec3f point, Vec3f normal, f32 speed);
    // Hanging or mantling ends: back to the walking controller with the speed it has; `fell` says so (an event).
    void leave_hang(Slot& s, bool fell, GripRelease3D why);
    void push_event(u32 index, const Slot& s, CharacterEvent3D e);
    // Contacts begun and ended since `before` (the sorted (body number, sensor) list of the step before).
    void emit_contact_changes(u32 index, Slot& s, const std::vector<std::pair<u32, bool>>& before);
    void flush_traversal_events(u32 index, const Slot& s);
    void collect_contacts(u32 index, Slot& s);
    void swap_shape(Slot& s, bool crouch);
    // Settings the descriptor gives a live character (slope, mass, strength, friction).
    static void apply_desc(Slot& s);
    Layer3DMask mask_of(const Slot& s) const;

    CharacterEnv _env;
    std::vector<Slot> _slots;
    std::vector<u32> _free;
    i32 _live = 0;
    std::vector<CharacterEvent3D> _events;
    // The grab and climb events of the character being updated, sorted and moved to `_events` at the end of its step.
    std::vector<CharacterEvent3D> _traversal_events;
};

} // namespace kin::physics3d_detail
