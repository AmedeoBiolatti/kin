#pragma once

#include <kin/anim/events.hpp>
#include <kin/anim/track.hpp>

#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace kin {

struct Clip {
    f32 duration = 0.0f;
    std::vector<PropertyTrack> properties;
    std::vector<SpriteTrack> sprites;
    std::vector<EventTrack> events;
};

struct AnimationNode;

struct Ref {
    std::string name;
};

struct Sequence {
    std::vector<AnimationNode> children;
};

enum class ParallelEnd : u8 {
    All,
    Any,
    Primary,
};

struct Parallel {
    std::vector<AnimationNode> children;
    ParallelEnd end = ParallelEnd::All;
    i32 primary = 0;
};

struct Repeat {
    std::unique_ptr<AnimationNode> child;
    i32 count = 0;
};

struct AnimationNode {
    std::variant<Clip, Ref, Sequence, Parallel, Repeat> value;
};

struct Animation {
    std::string name;
    AnimationNode root;
    bool is_template = false;
};

AnimationNode clip_node(Clip clip);
AnimationNode ref_node(std::string name);
AnimationNode sequence_node(std::vector<AnimationNode> children);
AnimationNode parallel_node(std::vector<AnimationNode> children, ParallelEnd end = ParallelEnd::All);
AnimationNode repeat_node(AnimationNode child, i32 count = 0);

AnimationNode clone(const AnimationNode& node);
Animation clone(const Animation& animation);

bool validate(const Animation& animation, std::string& error);
bool terminates(const AnimationNode& node);

} // namespace kin
