#include <kin/anim/animation.hpp>

#include <algorithm>
#include <utility>
#include <variant>

namespace kin {
namespace {

bool sorted_property_keys(const PropertyTrack& track) {
    return std::is_sorted(track.keys.begin(), track.keys.end(), [](const Keyframe& a, const Keyframe& b) {
        return a.time <= b.time;
    });
}

bool sorted_sprite_keys(const SpriteTrack& track) {
    return std::is_sorted(track.keys.begin(), track.keys.end(), [](const SpriteKey& a, const SpriteKey& b) {
        return a.time <= b.time;
    });
}

bool sorted_event_keys(const EventTrack& track) {
    return std::is_sorted(track.keys.begin(), track.keys.end(), [](const EventKey& a, const EventKey& b) {
        return a.time <= b.time;
    });
}

bool validate_node(const AnimationNode& node, std::string& error);

bool validate_clip(const Clip& clip, std::string& error) {
    if (clip.duration < 0.0f) {
        error = "clip duration must be non-negative";
        return false;
    }

    for (const PropertyTrack& track : clip.properties) {
        if (track.property.empty()) {
            error = "property track has empty property";
            return false;
        }
        if (track.keys.empty()) {
            error = "property track has no keys";
            return false;
        }
        if (!sorted_property_keys(track)) {
            error = "property track keys are not sorted";
            return false;
        }
        if (clip.duration < track_duration(track)) {
            error = "clip duration is shorter than property track";
            return false;
        }
    }

    for (const SpriteTrack& track : clip.sprites) {
        if (track.keys.empty()) {
            error = "sprite track has no keys";
            return false;
        }
        if (!sorted_sprite_keys(track)) {
            error = "sprite track keys are not sorted";
            return false;
        }
        if (clip.duration < track_duration(track)) {
            error = "clip duration is shorter than sprite track";
            return false;
        }
    }

    for (const EventTrack& track : clip.events) {
        if (!sorted_event_keys(track)) {
            error = "event track keys are not sorted";
            return false;
        }
        if (!track.keys.empty() && clip.duration < track.keys.back().time) {
            error = "clip duration is shorter than event track";
            return false;
        }
    }

    return true;
}

bool validate_node(const AnimationNode& node, std::string& error) {
    if (const auto* clip = std::get_if<Clip>(&node.value)) {
        return validate_clip(*clip, error);
    }
    if (const auto* ref = std::get_if<Ref>(&node.value)) {
        if (ref->name.empty()) {
            error = "ref has empty name";
            return false;
        }
        return true;
    }
    if (const auto* sequence = std::get_if<Sequence>(&node.value)) {
        for (const AnimationNode& child : sequence->children) {
            if (!validate_node(child, error)) {
                return false;
            }
        }
        return true;
    }
    if (const auto* parallel = std::get_if<Parallel>(&node.value)) {
        // `primary` is only meaningful for ParallelEnd::Primary; for All/Any it is
        // ignored, so an out-of-range default must not fail validation (and an empty
        // Parallel with the default end is legal).
        if (parallel->end == ParallelEnd::Primary
            && (parallel->primary < 0 || parallel->primary >= static_cast<i32>(parallel->children.size()))) {
            error = "parallel primary index is out of range";
            return false;
        }
        for (const AnimationNode& child : parallel->children) {
            if (!validate_node(child, error)) {
                return false;
            }
        }
        return true;
    }

    const Repeat& repeat = std::get<Repeat>(node.value);
    if (repeat.count < 0) {
        error = "repeat count must be non-negative";
        return false;
    }
    if (!repeat.child) {
        error = "repeat has no child";
        return false;
    }
    return validate_node(*repeat.child, error);
}

bool terminates_repeat(const Repeat& repeat) {
    return repeat.count > 0 && repeat.child && terminates(*repeat.child);
}

} // namespace

AnimationNode clip_node(Clip clip) {
    return AnimationNode{.value = std::move(clip)};
}

AnimationNode ref_node(std::string name) {
    return AnimationNode{.value = Ref{.name = std::move(name)}};
}

AnimationNode sequence_node(std::vector<AnimationNode> children) {
    return AnimationNode{.value = Sequence{.children = std::move(children)}};
}

AnimationNode parallel_node(std::vector<AnimationNode> children, ParallelEnd end) {
    return AnimationNode{.value = Parallel{.children = std::move(children), .end = end}};
}

AnimationNode repeat_node(AnimationNode child, i32 count) {
    return AnimationNode{.value = Repeat{.child = std::make_unique<AnimationNode>(std::move(child)), .count = count}};
}

AnimationNode clone(const AnimationNode& node) {
    if (const auto* clip = std::get_if<Clip>(&node.value)) {
        return clip_node(*clip);
    }
    if (const auto* ref = std::get_if<Ref>(&node.value)) {
        return ref_node(ref->name);
    }
    if (const auto* sequence = std::get_if<Sequence>(&node.value)) {
        std::vector<AnimationNode> children;
        children.reserve(sequence->children.size());
        for (const AnimationNode& child : sequence->children) {
            children.push_back(clone(child));
        }
        return sequence_node(std::move(children));
    }
    if (const auto* parallel = std::get_if<Parallel>(&node.value)) {
        std::vector<AnimationNode> children;
        children.reserve(parallel->children.size());
        for (const AnimationNode& child : parallel->children) {
            children.push_back(clone(child));
        }
        AnimationNode out = parallel_node(std::move(children), parallel->end);
        std::get<Parallel>(out.value).primary = parallel->primary;
        return out;
    }

    const Repeat& repeat = std::get<Repeat>(node.value);
    Repeat out;
    out.count = repeat.count;
    if (repeat.child) {
        out.child = std::make_unique<AnimationNode>(clone(*repeat.child));
    }
    return AnimationNode{.value = std::move(out)};
}

Animation clone(const Animation& animation) {
    return Animation{
        .name = animation.name,
        .root = clone(animation.root),
        .is_template = animation.is_template,
    };
}

bool validate(const Animation& animation, std::string& error) {
    error.clear();
    if (animation.name.empty()) {
        error = "animation has empty name";
        return false;
    }
    return validate_node(animation.root, error);
}

bool terminates(const AnimationNode& node) {
    if (std::holds_alternative<Clip>(node.value) || std::holds_alternative<Ref>(node.value)) {
        return true;
    }
    if (const auto* sequence = std::get_if<Sequence>(&node.value)) {
        return std::all_of(sequence->children.begin(), sequence->children.end(), [](const AnimationNode& child) {
            return terminates(child);
        });
    }
    if (const auto* parallel = std::get_if<Parallel>(&node.value)) {
        if (parallel->children.empty()) {
            return true;
        }
        switch (parallel->end) {
        case ParallelEnd::All:
            return std::all_of(parallel->children.begin(), parallel->children.end(), [](const AnimationNode& child) {
                return terminates(child);
            });
        case ParallelEnd::Any:
            return std::any_of(parallel->children.begin(), parallel->children.end(), [](const AnimationNode& child) {
                return terminates(child);
            });
        case ParallelEnd::Primary:
            return parallel->primary >= 0
                && parallel->primary < static_cast<i32>(parallel->children.size())
                && terminates(parallel->children[parallel->primary]);
        }
    }
    return terminates_repeat(std::get<Repeat>(node.value));
}

} // namespace kin
