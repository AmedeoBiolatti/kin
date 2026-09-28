#include <kin/anim/state_machine.hpp>

#include <cmath>
#include <vector>

namespace kin {
namespace {

constexpr f32 float_epsilon = 0.0001f;

bool from_matches(const AnimTransition& transition, std::string_view current) {
    return transition.from.empty() || transition.from == current;
}

bool condition_matches(const Condition& condition, const AnimationParams& params) {
    switch (condition.op) {
    case CondOp::IsTrue:
        return params.get_bool(condition.param);
    case CondOp::IsFalse:
        return !params.get_bool(condition.param);
    case CondOp::Greater:
        return params.get_float(condition.param) > condition.threshold;
    case CondOp::Less:
        return params.get_float(condition.param) < condition.threshold;
    case CondOp::Equal:
        return std::fabs(params.get_float(condition.param) - condition.threshold) <= float_epsilon;
    case CondOp::NotEqual:
        return std::fabs(params.get_float(condition.param) - condition.threshold) > float_epsilon;
    case CondOp::TriggerSet:
        return params.triggers.contains(condition.param);
    }
    return false;
}

bool transition_matches(const AnimTransition& transition, std::string_view current, const AnimationParams& params) {
    if (!from_matches(transition, current)) {
        return false;
    }
    for (const Condition& condition : transition.when) {
        if (!condition_matches(condition, params)) {
            return false;
        }
    }
    return true;
}

} // namespace

void AnimationParams::set_bool(std::string_view key, bool value) {
    bools[std::string{key}] = value;
}

void AnimationParams::set_float(std::string_view key, f32 value) {
    floats[std::string{key}] = value;
}

void AnimationParams::set_trigger(std::string_view key) {
    triggers.insert(std::string{key});
}

bool AnimationParams::get_bool(std::string_view key) const {
    const auto found = bools.find(std::string{key});
    return found != bools.end() ? found->second : false;
}

f32 AnimationParams::get_float(std::string_view key) const {
    const auto found = floats.find(std::string{key});
    return found != floats.end() ? found->second : 0.0f;
}

const AnimTransition* evaluate(const AnimationStateMachine& machine,
                               std::string_view current,
                               AnimationParams& params) {
    for (const AnimTransition& transition : machine.transitions) {
        if (!transition_matches(transition, current, params)) {
            continue;
        }
        for (const Condition& condition : transition.when) {
            if (condition.op == CondOp::TriggerSet) {
                params.triggers.erase(condition.param);
            }
        }
        return &transition;
    }
    return nullptr;
}

} // namespace kin
