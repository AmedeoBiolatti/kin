#include "scene_components_internal.hpp"

#include <kin/assets/asset_manager.hpp>
#include <kin/ecs/animator.hpp>
#include <kin/ecs/dialogue.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/saving.hpp>
#include <kin/ecs/scene_flow.hpp>
#include <kin/ecs/ui_screens.hpp>
#if KIN_ENABLE_SCRIPTING
#include <kin/ecs/script.hpp>
#endif

#include <optional>
#include <string>

namespace kin::scene_detail {

void register_scene_gameplay_components(EcsWorld& world) {
#if KIN_ENABLE_SCRIPTING
    if (!world.components().find("ScriptComponent")) {
        // A Lua script of the game's content run for the entity (EntityScripts:
        // on_start, on_update, on_contact; kin/scripting/entity_scripts.hpp).
        world.components().native<ScriptComponent>("ScriptComponent")
            .field("asset", &ScriptComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::Script)})
            .description("A .lua file of the content.")
            .field("enabled", &ScriptComponent::enabled)
            .field("player", &ScriptComponent::player)
            .range(0.0, 8.0)
            .description("Whose input its script reads (input.held, input.axis...): 0 anyone's, 1 the first player's, 2 the second's.");
    }
#endif
    if (!world.components().find("Animator")) {
        // An animation of an animation file of the game's content played on
        // the entity: its tracks set its components' fields (update_animators).
        world.components().native<Animator>("Animator")
            .field("asset", &Animator::asset)
            .asset_type(std::string{asset_type_name(AssetType::Animation)})
            .field("animation", &Animator::animation)
            .entry_of("asset")
            .description("An animation of the file.")
            .field("machine", &Animator::machine)
            .entry_of("asset")
            .description("A state machine of the file: it plays in place of the animation.")
            .field("speed", &Animator::speed)
            .range(0.0, 100.0)
            .description("1: as the animation says; 2: twice as fast.")
            .field("playing", &Animator::playing)
            .property<f32>("time", [](const Animator& a) { return a.time; }, [](Animator&, const f32&) { return true; })
            .readonly()
            .units("seconds")
            .description("Into the animation now.")
            .property<bool>("finished", [](const Animator& a) { return a.finished; }, [](Animator&, const bool&) { return true; })
            .readonly()
            .description("One that ends has played through.")
            .property<std::string>("state", [](const Animator& a) { return a.state; }, [](Animator&, const std::string&) { return true; })
            .readonly()
            .description("The machine's state now.");
    }
    if (!world.components().find("TalkComponent")) {
        // A character the player talks to: its dialogue starts when the
        // Talker, near it, presses the talk action (update_talk).
        world.components().native<TalkComponent>("TalkComponent")
            .field("asset", &TalkComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::Dialogue)})
            .description("A .kindialogue of the content.")
            .field("start", &TalkComponent::start)
            .description("The node it starts at; empty: the file's start.")
            .field("radius", &TalkComponent::radius)
            .range(0.0, 100000.0)
            .description("How near the Talker must be.")
            .field("prompt", &TalkComponent::prompt)
            .field("enabled", &TalkComponent::enabled);
    }
    if (!world.components().find("Talker")) {
        // The one who talks (the player): TalkComponents are measured from it.
        world.components().native<Talker>("Talker").field("enabled", &Talker::enabled);
    }
    if (!world.components().find("SceneDoor")) {
        // A way to another scene, or another place of this one: a
        // SceneTraveller stepping into its area (or pressing its action in
        // it) goes there (SceneFlow, kin/ecs/scene_flow.hpp).
        world.components().native<SceneDoor>("SceneDoor")
            .field("scene", &SceneDoor::scene)
            .asset_type(std::string{asset_type_name(AssetType::Scene)})
            .description("The scene it leads to; none: a place of this one.")
            .field("arrive", &SceneDoor::arrive)
            .entry_of("scene")
            .description("The entity there travellers arrive at, by its name (another door, a marker).")
            .property<std::string>("trigger",
                                   [](const SceneDoor& d) { return std::string(d.trigger == DoorTrigger::Action ? "action" : "touch"); },
                                   [](SceneDoor& d, const std::string& value) {
                                       if (value == "touch") d.trigger = DoorTrigger::Touch;
                                       else if (value == "action") d.trigger = DoorTrigger::Action;
                                       else return false;
                                       return true;
                                   })
            .choices({"touch", "action"})
            .description("Touch: stepping into it; action: pressing its action in it.")
            .field("action", &SceneDoor::action)
            .description("An action door's (an action of input.kininput).")
            .field("size", &SceneDoor::size)
            .description("Its area, centred on the entity.")
            .field("offset", &SceneDoor::offset)
            .description("The area's centre, from the entity.")
            .field("exit", &SceneDoor::exit)
            .description("Where those arriving at this door stand, from it (out of its area).")
            .property<std::string>("transition",
                                   [](const SceneDoor& d) {
                                       return d.transition ? std::string(transition_name(*d.transition)) : std::string("none");
                                   },
                                   [](SceneDoor& d, const std::string& value) {
                                       if (value == "none") {
                                           d.transition.reset();
                                           return true;
                                       }
                                       const std::optional<TransitionKind> kind = transition_from_name(value);
                                       if (!kind) return false;
                                       d.transition = kind;
                                       return true;
                                   })
            .choices({"fade", "crossfade", "dissolve", "pixelate", "wipe", "iris", "none"})
            .field("duration", &SceneDoor::duration)
            .range(0.0, 10.0)
            .units("seconds")
            .field("color", &SceneDoor::color)
            .description("What the screen goes to between the scenes.")
            .field("prompt", &SceneDoor::prompt)
            .description("What the game shows by an action door.")
            .field("enabled", &SceneDoor::enabled);
    }
    if (!world.components().find("SceneTraveller")) {
        // Who goes through SceneDoors (the player): where it is, is what is in a door.
        world.components().native<SceneTraveller>("SceneTraveller").field("enabled", &SceneTraveller::enabled);
    }
    if (!world.components().find("Parallax")) {
        // A background (or foreground) layer moving less (or more) than the
        // world as the camera moves; its entities with it (kin/ecs/render.hpp).
        world.components().native<Parallax>("Parallax")
            .field("factor", &Parallax::factor)
            .description("How much it moves with the world: 1 like the rest, 0 fixed on the screen, 0.5 far away, above 1 nearer.")
            .field("anchor", &Parallax::anchor)
            .description("Where the camera looks when the layer is where the scene puts it.")
            .field("repeat", &Parallax::repeat)
            .units("points")
            .description("Its own sprite, texture, rectangle or shape drawn again every so many points along each axis, filling the view; 0: once.")
            .field("scroll", &Parallax::scroll)
            .units("points/s")
            .description("How fast it moves by itself (clouds).")
            .field("offset", &Parallax::offset)
            .readonly();
    }
    if (!world.components().find("Saved")) {
        // What the entity keeps while the player is elsewhere, and in saves
        // (SceneSaves, kin/ecs/saving.hpp); once destroyed, it stays destroyed.
        world.components().native<Saved>("Saved")
            .field("keep", &Saved::keep)
            .description("The components it keeps, by name, commas between (\"Transform2D, Health\"); none: only that it is gone.")
            .field("script", &Saved::script)
            .description("Its script's own values (those of its self) kept too.");
    }
    if (!world.components().find("SpawnedPrefab")) {
        // A prefab instance made while the game ran (a script's world.spawn):
        // what a save makes it again from.
        world.components().native<SpawnedPrefab>("SpawnedPrefab")
            .field("prefab", &SpawnedPrefab::prefab)
            .description("The prefab it was made from, as the content names it.")
            .readonly();
    }
    if (!world.components().find("UiScreenComponent")) {
        // A UI screen (.kinui) shown over the world while the entity is there
        // (SceneScreens draws it; kin/ecs/ui_screens.hpp).
        world.components().native<UiScreenComponent>("UiScreenComponent")
            .field("asset", &UiScreenComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::UiScreen)})
            .description("A .kinui screen of the content.")
            .field("visible", &UiScreenComponent::visible)
            .field("order", &UiScreenComponent::order)
            .description("Higher draws over lower.");
    }
}

} // namespace kin::scene_detail
