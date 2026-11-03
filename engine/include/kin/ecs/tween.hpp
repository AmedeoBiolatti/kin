#pragma once

// A field of an entity's component moved from what it is to a value over
// time: a panel sliding in, a card turning over, a coin fading out, a door
// swinging open. Any field scenes describe that holds a number, a pair or a
// colour (as animations name them: "Transform2D.pos", "SpriteRenderer.tint",
// "RectRenderer.size"); others (whole numbers, true/false, text) change at
// its end.
//
//     kin::tween(world, card, {.property = "Transform2D.pos", .to = kin::Vec2f{320, 200},
//                              .seconds = 0.3f, .easing = kin::Easing::BackOut});
//     // each frame
//     kin::update_tweens(world, dt);
//
// It starts from the field's value when it starts (after its delay), so
// tweens one after another follow on. A new tween of a field stops the one it
// replaces. They are the entity's (a Tweens component): gone with it.

#include <kin/anim/value.hpp>
#include <kin/core/types.hpp>
#include <kin/ecs/world.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace kin {

struct TweenDef {
    std::string property;          // "Component.field"
    AnimValue to = 0.0f;           // of the field's kind: a number (f32), a Vec2f, a Color...
    f32 seconds = 0.25f;
    Easing easing = Easing::EaseOut;
    f32 delay = 0.0f;              // seconds before it starts
    i32 repeats = 0;               // times it goes again once done; -1: on and on
    bool yoyo = false;             // each time again goes back the other way
    bool relative = false;         // `to` is added to where it starts (a number, a pair)
};

// An entity's tweens, as they run.
struct Tweens {
    struct Running {
        u32 id = 0;
        TweenDef def;
        AnimValue from = 0.0f;
        AnimValue goal = 0.0f; // where this time goes (relative and yoyo taken in)
        f32 time = 0.0f;       // into this time (below 0: still waiting)
        i32 rounds = 0;        // times done
        bool started = false;
    };
    std::vector<Running> running;
    std::vector<u32> ended; // those that got to their end in the last update_tweens
    u32 next_id = 1;
};

// Starts a tween of `entity`'s field; its id (for stop_tween), or 0 when there
// is no such field (none of the entity's components, or one not described)
// or `to` is not of its kind, with why in `error`.
u32 tween(EcsWorld& world, EcsEntity entity, TweenDef def, std::string* error = nullptr);
// Stops a tween where it is (it does not end: it is not in `ended`).
bool stop_tween(EcsEntity entity, u32 id);
// Stops the entity's tweens (of `property` only, when given); how many.
u32 stop_tweens(EcsEntity entity, std::string_view property = {});
// Whether the entity has tweens running (of `property`, when given).
bool tweening(EcsEntity entity, std::string_view property = {});

// Moves every tween on by `dt`; each that gets to its end sets its field to
// where it goes and is in its entity's Tweens::ended until the next update.
void update_tweens(EcsWorld& world, f32 dt);

} // namespace kin
