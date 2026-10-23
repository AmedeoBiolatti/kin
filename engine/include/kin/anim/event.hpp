#pragma once

#include <kin/core/types.hpp>
#include <string>

namespace kin {

struct AnimationEvent {
    std::string channel;
    std::string name;
    std::string value;
    std::string target;
    Vec2f offset{};
};

} // namespace kin
