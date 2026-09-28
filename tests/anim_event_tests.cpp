#include <kin/anim/events.hpp>
#include <kin/anim/player.hpp>

#include <cassert>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void register_components(kin::EcsWorld& world) {
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    world.component<kin::AnimationEventQueue>("AnimationEventQueue");
}

kin::EventKey event_key(kin::f32 time, std::string channel, std::string name, std::string value = {}) {
    return kin::EventKey{
        .time = time,
        .event = {
            .channel = std::move(channel),
            .name = std::move(name),
            .value = std::move(value),
        },
    };
}

kin::Clip event_clip(kin::f32 duration, std::vector<kin::EventKey> keys) {
    return kin::Clip{
        .duration = duration,
        .events = {kin::EventTrack{.keys = std::move(keys)}},
    };
}

kin::Clip targeted_event_clip(kin::f32 duration, std::string target, std::vector<kin::EventKey> keys) {
    return kin::Clip{
        .duration = duration,
        .events = {kin::EventTrack{.target = std::move(target), .keys = std::move(keys)}},
    };
}

void add_animation(kin::AnimationRegistry& registry, std::string name, kin::AnimationNode root) {
    registry.add(kin::Animation{
        .name = std::move(name),
        .root = std::move(root),
    });
}

kin::AnimationPlayer make_player(kin::AnimationRegistry& registry, std::string_view animation) {
    kin::AnimationPlayer player{.registry = &registry};
    kin::set_base(player, animation);
    return player;
}

std::vector<std::string> queued_names(const kin::AnimationEventQueue* queue) {
    std::vector<std::string> names;
    if (!queue) {
        return names;
    }
    for (const kin::AnimationEvent& event : queue->pending) {
        names.push_back(event.name);
    }
    return names;
}

void test_clip_event_fires_once_when_crossed() {
    kin::EcsWorld world;
    register_components(world);

    kin::AnimationRegistry registry;
    add_animation(registry, "clip", kin::clip_node(event_clip(1.0f, {
                                    event_key(0.25f, "sound", "step"),
                                })));

    kin::EcsEntity entity = world.entity("clip").set(make_player(registry, "clip"));

    kin::advance_animation_players(world, 0.1f);
    assert(entity.get<kin::AnimationEventQueue>() == nullptr);

    kin::advance_animation_players(world, 0.15f);
    assert((queued_names(entity.get<kin::AnimationEventQueue>()) == std::vector<std::string>{"step"}));

    kin::advance_animation_players(world, 0.15f);
    assert((queued_names(entity.get<kin::AnimationEventQueue>()) == std::vector<std::string>{"step"}));
}

void test_zero_time_event_fires_once_on_activation() {
    kin::EcsWorld world;
    register_components(world);

    kin::AnimationRegistry registry;
    add_animation(registry, "zero", kin::clip_node(event_clip(1.0f, {
                                    event_key(0.0f, "sound", "ready"),
                                })));

    kin::EcsEntity entity = world.entity("zero").set(make_player(registry, "zero"));

    kin::advance_animation_players(world, 0.0f);
    assert((queued_names(entity.get<kin::AnimationEventQueue>()) == std::vector<std::string>{"ready"}));

    kin::advance_animation_players(world, 0.1f);
    assert((queued_names(entity.get<kin::AnimationEventQueue>()) == std::vector<std::string>{"ready"}));
}

void test_sequence_events_emit_in_carryover_order() {
    kin::EcsWorld world;
    register_components(world);

    std::vector<kin::AnimationNode> children;
    children.push_back(kin::clip_node(event_clip(0.1f, {
        event_key(0.0f, "trace", "a0"),
        event_key(0.1f, "trace", "a1"),
    })));
    children.push_back(kin::clip_node(event_clip(0.2f, {
        event_key(0.0f, "trace", "b0"),
        event_key(0.05f, "trace", "b1"),
    })));

    kin::AnimationRegistry registry;
    add_animation(registry, "sequence", kin::sequence_node(std::move(children)));

    kin::EcsEntity entity = world.entity("sequence").set(make_player(registry, "sequence"));

    kin::advance_animation_players(world, 0.15f);
    assert((queued_names(entity.get<kin::AnimationEventQueue>())
            == std::vector<std::string>{"a0", "a1", "b0", "b1"}));
}

void test_repeat_wrap_emits_each_loop() {
    kin::EcsWorld world;
    register_components(world);

    kin::AnimationRegistry registry;
    add_animation(registry, "repeat", kin::repeat_node(
                                    kin::clip_node(event_clip(0.1f, {
                                        event_key(0.0f, "trace", "zero"),
                                        event_key(0.05f, "trace", "mid"),
                                    })),
                                    0));

    kin::EcsEntity entity = world.entity("repeat").set(make_player(registry, "repeat"));

    kin::advance_animation_players(world, 0.25f);
    assert((queued_names(entity.get<kin::AnimationEventQueue>())
            == std::vector<std::string>{"zero", "mid", "zero", "mid", "zero", "mid"}));
}

void test_ref_and_override_events_emit_in_layer_order() {
    kin::EcsWorld world;
    register_components(world);

    kin::AnimationRegistry registry;
    add_animation(registry, "target", kin::clip_node(event_clip(1.0f, {
                                    event_key(0.1f, "trace", "base"),
                                })));
    add_animation(registry, "ref", kin::ref_node("target"));
    add_animation(registry, "override", kin::clip_node(event_clip(1.0f, {
                                       event_key(0.1f, "trace", "override"),
                                   })));

    kin::AnimationPlayer player = make_player(registry, "ref");
    kin::push(player, "override");
    kin::EcsEntity entity = world.entity("layers").set(std::move(player));

    kin::advance_animation_players(world, 0.1f);
    assert((queued_names(entity.get<kin::AnimationEventQueue>())
            == std::vector<std::string>{"base", "override"}));
}

void test_dispatch_drains_known_and_preserves_unknown() {
    kin::EcsWorld world;
    register_components(world);

    kin::EcsEntity entity = world.entity("dispatch").set(kin::AnimationEventQueue{
        .pending = {
            {.channel = "sound", .name = "step"},
            {.channel = "missing", .name = "kept"},
        },
    });

    std::vector<std::string> handled;
    kin::AnimationEventDispatch dispatch;
    dispatch.on("sound", [&handled](kin::EcsEntity, const kin::AnimationEvent& event) {
        handled.push_back(event.name);
    });
    dispatch.run(world);

    assert((handled == std::vector<std::string>{"step"}));
    assert((queued_names(entity.get<kin::AnimationEventQueue>()) == std::vector<std::string>{"kept"}));
}

void test_anim_interpreter_can_push_self_override() {
    kin::EcsWorld world;
    register_components(world);

    kin::AnimationRegistry registry;
    add_animation(registry, "base", kin::clip_node(event_clip(1.0f, {
                                  event_key(0.1f, "anim", "push", "override"),
                              })));
    add_animation(registry, "override", kin::clip_node(event_clip(0.2f, {})));

    kin::EcsEntity entity = world.entity("anim").set(make_player(registry, "base"));

    kin::advance_animation_players(world, 0.1f);

    kin::AnimationEventDispatch dispatch;
    dispatch.on("anim", kin::make_anim_event_interpreter());
    dispatch.run(world);

    const auto* player = entity.get<kin::AnimationPlayer>();
    assert(player != nullptr);
    assert(player->layers.size() == 2);
    assert(entity.get<kin::AnimationEventQueue>()->pending.empty());
}

void test_anim_event_with_target_pushes_child_override() {
    kin::EcsWorld world;
    register_components(world);

    kin::AnimationRegistry registry;
    add_animation(registry, "base", kin::clip_node(targeted_event_clip(1.0f, "arm", {
                                  event_key(0.1f, "anim", "push", "override"),
                              })));
    add_animation(registry, "child.base", kin::clip_node(event_clip(1.0f, {})));
    add_animation(registry, "override", kin::clip_node(event_clip(0.2f, {})));

    kin::EcsEntity root = world.entity("targeted-anim").set(make_player(registry, "base"));
    kin::EcsEntity arm = world.entity("arm").set(make_player(registry, "child.base")).child_of(root);

    kin::advance_animation_players(world, 0.1f);

    kin::AnimationEventDispatch dispatch;
    dispatch.on("anim", kin::make_anim_event_interpreter());
    dispatch.run(world);

    assert(root.get<kin::AnimationPlayer>()->layers.size() == 1);
    assert(arm.get<kin::AnimationPlayer>()->layers.size() == 2);
    assert(root.get<kin::AnimationEventQueue>()->pending.empty());
}

void test_anim_interpreter_missing_player_or_animation_drains() {
    kin::EcsWorld world;
    register_components(world);

    kin::AnimationRegistry registry;
    add_animation(registry, "base", kin::clip_node(event_clip(1.0f, {})));

    kin::EcsEntity no_player = world.entity("no-player").set(kin::AnimationEventQueue{
        .pending = {{
            .channel = "anim",
            .value = "override",
        }},
    });
    kin::EcsEntity missing_animation = world.entity("missing-animation")
                                           .set(make_player(registry, "base"))
                                           .set(kin::AnimationEventQueue{
                                               .pending = {{
                                                   .channel = "anim",
                                                   .value = "missing",
                                               }},
                                           });

    kin::AnimationEventDispatch dispatch;
    dispatch.on("anim", kin::make_anim_event_interpreter());
    dispatch.run(world);

    assert(no_player.get<kin::AnimationEventQueue>()->pending.empty());
    assert(missing_animation.get<kin::AnimationEventQueue>()->pending.empty());
    assert(missing_animation.get<kin::AnimationPlayer>()->layers.size() == 1);
}

void test_known_event_with_missing_target_drains() {
    kin::EcsWorld world;
    register_components(world);

    kin::EcsEntity entity = world.entity("missing-target-dispatch")
                                .set(kin::AnimationPlayer{})
                                .set(kin::AnimationEventQueue{
                                    .pending = {
                                        {.channel = "sound", .name = "lost", .target = "missing"},
                                    },
                                });

    bool handled = false;
    kin::AnimationEventDispatch dispatch;
    dispatch.on("sound", [&handled](kin::EcsEntity, const kin::AnimationEvent&) {
        handled = true;
    });
    dispatch.run(world);

    assert(!handled);
    assert(entity.get<kin::AnimationEventQueue>()->pending.empty());
}

} // namespace

void run_event_consumer_tests();

int main() {
    test_clip_event_fires_once_when_crossed();
    test_zero_time_event_fires_once_on_activation();
    test_sequence_events_emit_in_carryover_order();
    test_repeat_wrap_emits_each_loop();
    test_ref_and_override_events_emit_in_layer_order();
    test_dispatch_drains_known_and_preserves_unknown();
    test_anim_interpreter_can_push_self_override();
    test_anim_event_with_target_pushes_child_override();
    test_anim_interpreter_missing_player_or_animation_drains();
    test_known_event_with_missing_target_drains();
    run_event_consumer_tests();
    return 0;
}
