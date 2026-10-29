#pragma once

// An entity's sprite animated: one of its sprite catalog's animations (the
// catalog its SpriteRenderer names; a .kinsprites file's `animation` lines)
// played on it, its frames shown in turn. update_sprite_animators() moves
// every one on and sets its SpriteRenderer's sprite. Scene files name the
// animation (SpriteAnimator's `animation`); the game changes it, and the one
// named starts from its first frame:
//
//     hero.set(kin::SpriteRenderer{.asset = {"sprites/hero.kinsprites"}});
//     hero.set(kin::SpriteAnimator{.animation = "idle"});
//     // each frame
//     hero.get_mut<kin::SpriteAnimator>()->animation = moving ? "walk" : "idle";
//     kin::update_sprite_animators(world, dt);
//
// Frames only: blending, state machines, events on frames and properties
// animated are kin::AnimationPlayer's (kin/anim/player.hpp).

#include <kin/ecs/world.hpp>

#include <string>

namespace kin {

struct SpriteAnimator {
    std::string animation; // of its SpriteRenderer's catalog; empty: none plays
    f32 speed = 1.0f;      // 2: twice as fast
    bool playing = true;   // false: it stays on the frame it is on

    // Where it is, kept by update_sprite_animators(): seconds into `current`
    // (set 0 to play it again from its start), the frame shown (-1: none
    // yet, or its catalog has no such animation), and whether a Once one has
    // played through. Another animation named starts from 0.
    f32 time = 0.0f;
    i32 frame = -1;
    bool finished = false;
    std::string current;
};

// Each SpriteAnimator moved on by `dt` (times its speed, while it plays), and
// its entity's SpriteRenderer given the sprite of the frame it is on. One on
// an entity made from a prefab gets its own copy (and its SpriteRenderer)
// first. A SpriteRenderer whose catalog is not bound yet (Assets2D) waits.
void update_sprite_animators(EcsWorld& world, f32 dt);

} // namespace kin
