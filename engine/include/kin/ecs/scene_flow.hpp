#pragma once

// Going from scene to scene, as data: doors (SceneDoor) the player (a
// SceneTraveller) goes through to another scene file, or to another place of
// this one, with a transition. A door names the scene and the entity of it
// the traveller arrives at (another door, a marker): a pair of doors, each
// naming the other, is a way there and back. The game reads the scene a
// travel names (or code, or a script's scene.go, asks for one), then puts its
// travellers where they arrive:
//
//     kin::SceneFlow flow;
//     // each step, after the travellers moved:
//     flow.update(world, ctx.input);
//     if (std::optional<kin::SceneTravel> go = flow.take()) {
//         // the scene read again from go->scene (unless empty: this one), then
//         flow.arrive(world, go->arrive);
//     }
//
// A traveller goes through a Touch door as it steps into the door's area, an
// Action door when it presses the door's action inside it. One standing in a
// door as it arrives (or as the flow starts) goes through only once it has
// stepped out and in again, so arriving at a door does not send it back.
// transition_within (kin/scene/transitions.hpp) plays the transition while
// the same scene reads the other file.

#include <kin/ecs/component.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/color.hpp>
#include <kin/scene/transitions.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class Input;

enum class DoorTrigger : u8 {
    Touch,  // stepping into it
    Action, // pressing its action inside it
};

struct SceneDoor {
    ComponentAssetRef scene{}; // a .kinscene of the content; none: a place of this scene
    std::string arrive;        // the entity there arrived at, by its name; empty: where the scene puts travellers
    DoorTrigger trigger = DoorTrigger::Touch;
    std::string action = "interact"; // an Action door's
    Vec2f size{32.0f, 32.0f};        // its area, centred on the entity (and scaled with it)
    Vec2f offset{};                  // the area's centre, from the entity
    Vec2f exit{};                    // where those arriving at this door stand, from it
    std::optional<TransitionKind> transition = TransitionKind::Fade; // none: at once
    f32 duration = 0.5f; // seconds, the whole transition
    Color color = colors::black;
    std::string prompt = "Enter"; // what a game may show by an Action door
    bool enabled = true;
};

// Who goes through doors (the player): its position is what is in a door.
struct SceneTraveller {
    bool enabled = true;
};

// Where to go, and how.
struct SceneTravel {
    std::string scene;  // a .kinscene of the content; empty: a place of this scene
    std::string arrive; // the entity there arrived at, by its name; empty: none
    std::optional<TransitionKind> transition = TransitionKind::Fade; // none: at once
    f32 duration = 0.5f;
    Color color = colors::black;
    std::optional<EcsId> door; // the door gone through; none: asked by code or a script
};

class SceneFlow {
public:
    // The doors gone through this step (the first a traveller goes through
    // is the travel asked for, unless one is already).
    void update(EcsWorld& world, const Input& input);

    // A travel asked for by code (a script's scene.go): it replaces one not yet taken.
    void go(SceneTravel travel);
    // The travel asked for, once; then none until another is.
    std::optional<SceneTravel> take();
    bool pending() const { return _pending.has_value(); }

    // The travellers put at the entity named `arrive` (at its door's exit,
    // when it is a door), their bodies with them at the next physics step;
    // the doors they stand in then wait for them to step out. False when
    // nothing has that name (they stay where they are). An empty name puts
    // no one anywhere, and is true.
    bool arrive(EcsWorld& world, std::string_view arrive);

    // The doors travellers stand in forgotten (the scene read again, say):
    // the next update notes them without going through.
    void reset();

    // The enabled Action door the first traveller stands in, if any (for
    // the game to show its prompt).
    std::optional<EcsEntity> action_door(EcsWorld& world) const;

private:
    struct Inside {
        EcsId traveller = 0;
        EcsId door = 0;
        bool operator==(const Inside&) const = default;
    };
    std::vector<Inside> standing(EcsWorld& world) const;

    std::vector<Inside> _inside;
    bool _noted = false; // _inside is of this scene
    std::optional<SceneTravel> _pending;
};

// A door's travel.
SceneTravel travel_of(const SceneDoor& door, std::optional<EcsId> id = std::nullopt);

// Is `point` (in the world) in the door's area? (`at` is the door entity's world transform.)
bool door_contains(const SceneDoor& door, const WorldTransform& at, Vec2f point);

} // namespace kin
