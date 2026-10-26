#include <kin/ecs/animator.hpp>

#include <kin/anim/binding.hpp>
#include <kin/anim/player.hpp>
#include <kin/ecs/render.hpp>

#include <algorithm>
#include <limits>
#include <variant>
#include <vector>

namespace kin {
namespace {

// How long a node plays (infinity: it repeats forever).
f32 node_length(const AnimationNode& node) {
    constexpr f32 forever = std::numeric_limits<f32>::infinity();
    if (const auto* clip = std::get_if<Clip>(&node.value)) {
        return clip->duration;
    }
    if (const auto* sequence = std::get_if<Sequence>(&node.value)) {
        f32 total = 0.0f;
        for (const AnimationNode& child : sequence->children) {
            total += node_length(child);
        }
        return total;
    }
    if (const auto* parallel = std::get_if<Parallel>(&node.value)) {
        f32 longest = 0.0f;
        for (const AnimationNode& child : parallel->children) {
            longest = std::max(longest, node_length(child));
        }
        return longest;
    }
    if (const auto* repeat = std::get_if<Repeat>(&node.value)) {
        return repeat->count <= 0 || !repeat->child ? forever : node_length(*repeat->child) * static_cast<f32>(repeat->count);
    }
    return 0.0f; // a reference: as long as what it names, not known here
}

// The entity a track names: the animated one, or a child of it by name (a
// path of names, "arm/hand").
flecs::entity target_of(flecs::entity root, std::string_view target) {
    flecs::entity at = root;
    while (!target.empty() && at) {
        const std::size_t slash = target.find('/');
        at = at.lookup(std::string(target.substr(0, slash)).c_str());
        target = slash == std::string_view::npos ? std::string_view{} : target.substr(slash + 1);
    }
    return at;
}

// A component the entity inherits from its prefab, copied down so writing it
// changes this entity only.
void own(flecs::entity entity, flecs::id_t component) {
    if (!entity || !entity.has(component) || entity.owns(component)) {
        return;
    }
    const ecs_type_info_t* info = ecs_get_type_info(entity.world(), component);
    const void* inherited = entity.get(component);
    if (info && inherited) {
        entity.set_ptr(component, static_cast<std::size_t>(info->size), inherited);
    }
}

// What the animation's tracks write, owned by the entities they write it on.
void own_targets(flecs::entity root, const AnimationNode& node, const PropertyRegistry& properties, flecs::id_t sprites) {
    if (const auto* clip = std::get_if<Clip>(&node.value)) {
        for (const PropertyTrack& track : clip->properties) {
            if (const PropertyAccessor* accessor = properties.find(track.property)) {
                own(target_of(root, track.target), accessor->component);
            }
        }
        for (const SpriteTrack& track : clip->sprites) {
            own(target_of(root, track.target), sprites);
        }
    } else if (const auto* sequence = std::get_if<Sequence>(&node.value)) {
        for (const AnimationNode& child : sequence->children) {
            own_targets(root, child, properties, sprites);
        }
    } else if (const auto* parallel = std::get_if<Parallel>(&node.value)) {
        for (const AnimationNode& child : parallel->children) {
            own_targets(root, child, properties, sprites);
        }
    } else if (const auto* repeat = std::get_if<Repeat>(&node.value); repeat && repeat->child) {
        own_targets(root, *repeat->child, properties, sprites);
    }
}

} // namespace

void update_animators(EcsWorld& world, f32 dt) {
    flecs::world& raw = world.raw();
    std::vector<flecs::entity> animated;
    raw.each([&](flecs::entity entity, const Animator&) { animated.push_back(entity); });
    for (const flecs::entity found : animated) {
        EcsEntity entity{found};
        Animator& animator = *entity.get_mut<Animator>(); // its own, when it is a prefab's
        if (!animator.library || !animator.properties) {
            continue; // its file not bound yet (Assets2D)
        }
        const bool fresh = animator.animation != animator.current || animator.bound != animator.library.get() ||
                           !found.has<AnimationPlayer>();
        if (fresh) {
            // Another animation, or the file read again: from its start.
            animator.current = animator.animation;
            animator.bound = animator.library.get();
            animator.time = 0.0f;
            animator.finished = false;
            AnimationPlayer player = make_animation_player({.registry = &animator.library->registry,
                                                            .properties = animator.properties,
                                                            .sprite_catalog = sprite_renderer_catalog(entity)});
            if (!animator.current.empty()) {
                set_base(player, animator.current);
            }
            if (!player.layers.empty() && player.layers.front().animation) {
                own_targets(found, player.layers.front().animation->root, *animator.properties,
                            raw.component<SpriteRenderer>().id());
            }
            found.set<AnimationPlayer>(std::move(player));
        }
        AnimationPlayer* player = found.get_mut<AnimationPlayer>();
        if (!player) {
            continue;
        }
        player->playing = animator.playing && !animator.finished;
        player->sprite_catalog = sprite_renderer_catalog(entity);
        if (!player->layers.empty()) {
            player->layers.front().speed = animator.speed;
        }
    }

    advance_animation_players(world, dt);
    sample_animation_players(world);

    for (const flecs::entity found : animated) {
        Animator* animator = found.get_mut<Animator>();
        const AnimationPlayer* player = found.get<AnimationPlayer>();
        if (!animator || !player || player->layers.empty() || !player->layers.front().animation) {
            continue;
        }
        const PlayerLayer& base = player->layers.front();
        const f32 length = node_length(base.animation->root);
        animator->time = std::min(base.cursor.time, length);
        animator->finished = base.cursor.time >= length;
    }
}

} // namespace kin
