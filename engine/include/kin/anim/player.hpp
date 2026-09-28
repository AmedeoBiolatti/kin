#pragma once

#include <kin/anim/animation.hpp>
#include <kin/anim/binding.hpp>
#include <kin/anim/registry.hpp>
#include <kin/anim/state_machine.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

// Enables heterogeneous (string_view) lookup on string-keyed maps so hot-path
// lookups don't allocate a temporary std::string per call.
struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

struct NodeCursor {
    NodeCursor() = default;
    NodeCursor(const NodeCursor& other)
        : time(other.time),
          active_child(other.active_child),
          remaining(other.remaining),
          children(other.children),
          child(other.child ? std::make_unique<NodeCursor>(*other.child) : nullptr),
          finished(other.finished),
          cached_duration(other.cached_duration),
          ref_name(other.ref_name),
          ref_animation(other.ref_animation),
          relative_base(other.relative_base),
          event_cursor(other.event_cursor) {
    }
    NodeCursor& operator=(const NodeCursor& other) {
        if (this == &other) {
            return *this;
        }
        time = other.time;
        active_child = other.active_child;
        remaining = other.remaining;
        children = other.children;
        child = other.child ? std::make_unique<NodeCursor>(*other.child) : nullptr;
        finished = other.finished;
        cached_duration = other.cached_duration;
        ref_name = other.ref_name;
        ref_animation = other.ref_animation;
        relative_base = other.relative_base;
        event_cursor = other.event_cursor;
        return *this;
    }
    NodeCursor(NodeCursor&&) noexcept = default;
    NodeCursor& operator=(NodeCursor&&) noexcept = default;

    f32 time = 0.0f;
    i32 active_child = 0;
    i32 remaining = 0;
    std::vector<NodeCursor> children;
    std::unique_ptr<NodeCursor> child;
    bool finished = false;
    // Memoized duration of the def node this cursor tracks (or, for a Repeat
    // cursor, of its child). -1 = not computed yet. Valid for the cursor's whole
    // lifetime because a cursor is bound to one immutable def node + fixed
    // bindings; it dies with the layer's animation, so there is no stale-pointer
    // hazard.
    f32 cached_duration = -1.0f;
    std::string ref_name;
    std::shared_ptr<const Animation> ref_animation;
    std::unordered_map<std::string, AnimValue> relative_base;
    std::vector<std::size_t> event_cursor;
};

enum class LayerKind : u8 {
    Base,
    Override,
};

struct PropertyMask {
    std::vector<std::string> properties;
    bool empty() const { return properties.empty(); }
    bool allows(std::string_view key) const;
};

struct PlayerLayer {
    LayerKind kind = LayerKind::Base;
    std::shared_ptr<const Animation> animation;
    NodeCursor cursor;
    f32 speed = 1.0f;
    f32 weight = 1.0f;
    f32 target_weight = 1.0f;
    f32 blend_in = 0.0f;
    f32 age = 0.0f;
    PropertyMask mask;
    bool done = false;
};

struct PushOptions {
    f32 speed = 1.0f;
    f32 weight = 1.0f;
    f32 blend_in = 0.0f;
    PropertyMask mask{};
};

struct AnimationPlayer {
    AnimationRegistry* registry = nullptr;
    const PropertyRegistry* properties = nullptr;
    const SpriteCatalog* sprite_catalog = nullptr;
    Bindings bindings;
    std::shared_ptr<const AnimationStateMachine> machine;
    AnimationParams params;
    std::string state;
    std::vector<PlayerLayer> layers;
    std::unordered_map<std::string, EcsId, TransparentStringHash, std::equal_to<>> target_cache;
    bool playing = true;
};

struct AnimationLayerSnapshot {
    LayerKind kind = LayerKind::Base;
    std::string animation;
    f32 time = 0.0f;
    f32 speed = 1.0f;
    f32 weight = 1.0f;
    bool done = false;
};

struct AnimationPlayerSnapshot {
    std::string state;
    std::vector<AnimationLayerSnapshot> layers;
    Bindings bindings;
    std::vector<std::string> triggers;
    bool playing = true;
};

struct AnimationPlayerConfig {
    AnimationRegistry* registry = nullptr;
    const PropertyRegistry* properties = nullptr;
    const SpriteCatalog* sprite_catalog = nullptr;
    std::shared_ptr<const AnimationStateMachine> machine;
    Bindings bindings;
    std::string base_animation;
};

AnimationPlayer make_animation_player(AnimationPlayerConfig config);
AnimationPlayerSnapshot snapshot(const AnimationPlayer& player);

void set_base(AnimationPlayer& player, std::string_view animation_name);
void push(AnimationPlayer& player, std::string_view animation_name, PushOptions options = {});
void push(AnimationPlayer& player, std::shared_ptr<const Animation> animation, PushOptions options = {});
void clear_overrides(AnimationPlayer& player);

// Steps every player's state machine once (evaluates one transition, consuming
// matched triggers). NOTE: `advance_animation_players` already calls this as its
// first step, so the normal per-frame pipeline is just advance -> sample ->
// dispatch. Call this directly ONLY to step a machine in isolation (e.g. unit
// tests, or driving a player without advancing time); calling it in the same
// frame as `advance_animation_players` double-evaluates transitions.
void update_animation_state_machines(EcsWorld& world);
void advance_animation_players(EcsWorld& world, f32 dt);
void sample_animation_players(EcsWorld& world);
EcsEntity resolve_animation_target(EcsEntity root, AnimationPlayer& player, std::string_view target);

} // namespace kin
