#include <kin/ecs/sprite_animator.hpp>

#include <kin/ecs/render.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <cmath>
#include <vector>

namespace kin {
namespace {

// Instances of a prefab inherit what it has (described components are
// inherited): each gets its own animator, and SpriteRenderer, to change.
void own_inherited_animators(flecs::world& world) {
    const bool animators = world.component<SpriteAnimator>().has(flecs::OnInstantiate, flecs::Inherit);
    const bool renderers = world.component<SpriteRenderer>().has(flecs::OnInstantiate, flecs::Inherit);
    if (!animators && !renderers) {
        return; // copied onto instances when they are made
    }
    std::vector<flecs::entity> inheriting;
    world.each([&](flecs::entity entity, const SpriteAnimator&) {
        if (!entity.owns<SpriteAnimator>() || (entity.has<SpriteRenderer>() && !entity.owns<SpriteRenderer>())) {
            inheriting.push_back(entity);
        }
    });
    for (const flecs::entity raw : inheriting) {
        EcsEntity entity{raw};
        entity.get_mut<SpriteAnimator>();
        if (entity.has<SpriteRenderer>()) {
            entity.get_mut<SpriteRenderer>();
        }
    }
}

} // namespace

void update_sprite_animators(EcsWorld& world, f32 dt) {
    flecs::world& raw = world.raw();
    own_inherited_animators(raw);
    raw.each([dt](SpriteAnimator& animator, SpriteRenderer& renderer) {
        if (animator.animation != animator.current) {
            animator.current = animator.animation;
            animator.time = 0.0f;
            animator.finished = false;
        }
        const SpriteAnimationDefinition* animation =
            renderer.sprite.catalog && !animator.current.empty() ? renderer.sprite.catalog->animation(animator.current) : nullptr;
        if (!animation || animation->frames.empty()) {
            animator.frame = -1;
            return; // its catalog not bound yet, or no such animation: the sprite stays
        }
        if (animator.playing) {
            animator.time += dt * animator.speed;
            // Kept within a turn (a Once one: at its end), so it keeps its
            // precision as it plays on.
            if (const f32 length = sprite_animation_length(*animation); length > 0.0f && std::abs(animator.time) >= length) {
                animator.time = animation->playback == SpritePlayback::Once ? std::copysign(length, animator.time)
                                                                             : std::fmod(animator.time, length);
            }
        }
        animator.frame = sprite_animation_frame(*animation, animator.time, &animator.finished);
        const std::string& sprite = animation->frames[static_cast<std::size_t>(animator.frame)].sprite;
        if (renderer.sprite.id != sprite) {
            renderer.sprite.id = sprite;
        }
    });
}

} // namespace kin
