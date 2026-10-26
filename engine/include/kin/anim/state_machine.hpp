#pragma once

#include <kin/core/types.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kin {

struct AnimationParams {
    std::unordered_map<std::string, bool> bools;
    std::unordered_map<std::string, f32> floats;
    std::unordered_set<std::string> triggers;

    void set_bool(std::string_view key, bool value);
    void set_float(std::string_view key, f32 value);
    void set_trigger(std::string_view key);
    bool get_bool(std::string_view key) const;
    f32 get_float(std::string_view key) const;
};

enum class CondOp : u8 {
    IsTrue,
    IsFalse,
    Greater,
    Less,
    Equal,
    NotEqual,
    TriggerSet,
    // The state's animation has played through (one that ends).
    Finished,
};

struct Condition {
    std::string param;
    CondOp op = CondOp::IsTrue;
    f32 threshold = 0.0f;
};

enum class TransitionMode : u8 {
    ReplaceBase,
    PushOverride,
};

struct AnimTransition {
    std::string from;
    std::string to;
    std::vector<Condition> when;
    TransitionMode mode = TransitionMode::ReplaceBase;
};

struct AnimationStateMachine {
    std::string initial;
    std::unordered_map<std::string, std::string> states;
    std::vector<AnimTransition> transitions;
    // Where an editor draws each state (and "any", the any-state), by name:
    // kept in the file, never read when it plays.
    std::unordered_map<std::string, Vec2f> places;
};

// The first transition (in order) from `current` whose conditions hold, its
// triggers consumed; `finished`: the state's animation has played through.
// A transition from any state into the one it is in, replacing it, is not
// taken (it would start the state again every step).
const AnimTransition* evaluate(const AnimationStateMachine& machine,
                               std::string_view current,
                               AnimationParams& params,
                               bool finished = false);

} // namespace kin
