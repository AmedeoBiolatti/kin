#pragma once

// The character controllers of one World3D (kin/physics3d/character3d.hpp), over Jolt's CharacterVirtual. Internal: the
// world's Impl owns one and calls it from step(), the hash and the saved state.

#include "jolt_runtime.hpp"

#include <kin/physics3d/character3d.hpp>
#include <kin/physics3d/world3d.hpp>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorder.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace kin::physics3d_detail {

// What the set needs of the world around it.
struct CharacterEnv {
    JPH::PhysicsSystem* system = nullptr;
    JPH::TempAllocator* temp = nullptr;
    const std::array<u32, 32>* table = nullptr;
    // The world's gravity vector as it is now.
    std::function<Vec3f()> gravity;
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
    };

    Slot* find(Character3D c) { return c.index < _slots.size() && _slots[c.index].alive && _slots[c.index].generation == c.generation ? &_slots[c.index] : nullptr; }
    const Slot* find(Character3D c) const { return const_cast<CharacterSet*>(this)->find(c); }

    void update_one(u32 index, Slot& s, f32 dt);
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
};

} // namespace kin::physics3d_detail
