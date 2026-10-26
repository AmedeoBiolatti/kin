#pragma once

// An entity animated by an animation of an animation file of the game's
// content (`.kinanim`; Assets2D reads it): its property tracks set fields of
// the entity's components ("Transform2D.pos", any described field), and of its
// children's (a track's target names a child), its sprite tracks its
// SpriteRenderer's sprite, and its events go to its AnimationEventQueue.
// Scene files name the file and the animation; the game changes the
// animation, and the one named starts from its beginning:
//
//     door.set(kin::Animator{.asset = {"animations/door.kinanim"}, .animation = "open"});
//     // each frame
//     kin::update_animators(world, dt);
//
// An animation that repeats plays on; one that ends stays on its last frame
// (`finished`). Frames only (a walk cycle of a sprite sheet) are a
// SpriteAnimator's (kin/ecs/sprite_animator.hpp).

#include <kin/anim/anim_format.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>

#include <memory>
#include <string>

namespace kin {

class PropertyRegistry;

struct Animator {
    ComponentAssetRef asset; // an animation file (.kinanim)
    std::string animation;   // of the file; empty: none plays
    f32 speed = 1.0f;        // 2: twice as fast
    bool playing = true;     // false: it stays where it is

    // Where it is, kept by update_animators(): seconds into `current`, and
    // whether one that ends has played through.
    f32 time = 0.0f;
    bool finished = false;
    std::string current;

    // Set by Assets2D (bind_animators) from `asset`: the file's animations,
    // and the fields tracks can set.
    std::shared_ptr<AnimationAssetLibrary> library;
    const PropertyRegistry* properties = nullptr;
    const void* bound = nullptr; // the library `current` was started with
};

// Each Animator moved on by `dt` (times its speed, while it plays), and the
// fields its animation's tracks name set; then every other AnimationPlayer of
// the world too (call this instead of advance_ and sample_animation_players).
// One on an entity made from a prefab gets its own copy first, as do the
// components its tracks set.
void update_animators(EcsWorld& world, f32 dt);

} // namespace kin
