#pragma once

// Animations mixed by a number: a blend space puts each of its animations at
// a position along a parameter (idle at 0, walk at 1.5, run at 4 along
// "speed"). Played, the two either side of the parameter's value are mixed,
// each by how near the value is to it, and kept in step: each goes through
// its clip in the same share of a turn, so a walk's and a run's steps land
// together. A state of a state machine plays one by its name, as it plays an
// animation; so does set_base.
//
//     blendspace locomotion speed
//       at 0 idle
//       at 1.5 walk
//       at 4 run

#include <kin/core/types.hpp>

#include <string>
#include <vector>

namespace kin {

struct BlendSpaceEntry {
    std::string animation;
    f32 position = 0.0f;

    friend bool operator==(const BlendSpaceEntry&, const BlendSpaceEntry&) = default;
};

struct AnimationBlendSpace {
    std::string parameter; // a float parameter of the player
    std::vector<BlendSpaceEntry> entries;

    friend bool operator==(const AnimationBlendSpace&, const AnimationBlendSpace&) = default;
};

// How much each entry (in the space's order) weighs at `value`: the two
// around it share 1 by nearness; past either end, the end one alone.
std::vector<f32> blend_weights(const AnimationBlendSpace& space, f32 value);

} // namespace kin
