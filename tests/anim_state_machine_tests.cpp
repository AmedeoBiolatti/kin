#include <kin/anim/anim_format.hpp>
#include <kin/anim/binding.hpp>
#include <kin/anim/player.hpp>
#include <kin/ecs/render.hpp>
#include <kin/anim/state_machine.hpp>

#include <cassert>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace {

kin::Animation animation(std::string name, kin::f32 duration = 1.0f) {
    return kin::Animation{
        .name = std::move(name),
        .root = kin::clip_node(kin::Clip{.duration = duration}),
    };
}

kin::AnimationRegistry registry_with_basic_animations() {
    kin::AnimationRegistry registry;
    registry.add(animation("idle"));
    registry.add(animation("walk"));
    registry.add(animation("hurt", 0.2f));
    return registry;
}

std::shared_ptr<kin::AnimationStateMachine> basic_machine() {
    auto machine = std::make_shared<kin::AnimationStateMachine>();
    machine->initial = "idle";
    machine->states["idle"] = "idle";
    machine->states["walk"] = "walk";
    machine->states["hurt"] = "hurt";
    return machine;
}

kin::AnimationPlayer player_for(kin::AnimationRegistry& registry,
                                std::shared_ptr<const kin::AnimationStateMachine> machine) {
    return kin::AnimationPlayer{
        .registry = &registry,
        .machine = std::move(machine),
    };
}

void test_bool_condition_replaces_base() {
    kin::AnimationRegistry registry = registry_with_basic_animations();
    auto machine = basic_machine();
    machine->transitions.push_back({
        .from = "idle",
        .to = "walk",
        .when = {{.param = "moving", .op = kin::CondOp::IsTrue}},
    });
    machine->transitions.push_back({
        .from = "walk",
        .to = "idle",
        .when = {{.param = "moving", .op = kin::CondOp::IsFalse}},
    });

    kin::EcsWorld world;
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    kin::EcsEntity entity = world.entity("actor").set(player_for(registry, machine));

    auto* player = entity.get_mut<kin::AnimationPlayer>();
    player->params.set_bool("moving", true);
    kin::update_animation_state_machines(world);
    player = entity.get_mut<kin::AnimationPlayer>();
    assert(player->state == "walk");
    assert(player->layers.size() == 1);
    assert(player->layers[0].animation->name == "walk");

    player->params.set_bool("moving", false);
    kin::update_animation_state_machines(world);
    player = entity.get_mut<kin::AnimationPlayer>();
    assert(player->state == "idle");
    assert(player->layers[0].animation->name == "idle");
}

void test_float_conditions() {
    kin::AnimationStateMachine machine;
    machine.transitions = {
        {.from = "idle", .to = "gt", .when = {{.param = "speed", .op = kin::CondOp::Greater, .threshold = 3.0f}}},
        {.from = "idle", .to = "lt", .when = {{.param = "speed", .op = kin::CondOp::Less, .threshold = 0.0f}}},
        {.from = "idle", .to = "eq", .when = {{.param = "speed", .op = kin::CondOp::Equal, .threshold = 1.5f}}},
        {.from = "idle", .to = "ne", .when = {{.param = "speed", .op = kin::CondOp::NotEqual, .threshold = 2.0f}}},
    };

    kin::AnimationParams params;
    params.set_float("speed", 4.0f);
    assert(kin::evaluate(machine, "idle", params)->to == "gt");

    params.set_float("speed", -1.0f);
    assert(kin::evaluate(machine, "idle", params)->to == "lt");

    params.set_float("speed", 1.5f);
    assert(kin::evaluate(machine, "idle", params)->to == "eq");

    params.set_float("speed", 2.1f);
    assert(kin::evaluate(machine, "idle", params)->to == "ne");
}

void test_trigger_consumption_only_when_taken() {
    kin::AnimationStateMachine machine;
    machine.transitions.push_back({
        .from = "idle",
        .to = "hit",
        .when = {
            {.param = "hit", .op = kin::CondOp::TriggerSet},
            {.param = "alive", .op = kin::CondOp::IsTrue},
        },
    });

    kin::AnimationParams blocked;
    blocked.set_trigger("hit");
    assert(kin::evaluate(machine, "idle", blocked) == nullptr);
    assert(blocked.triggers.contains("hit"));

    blocked.set_bool("alive", true);
    assert(kin::evaluate(machine, "idle", blocked)->to == "hit");
    assert(!blocked.triggers.contains("hit"));
}

void test_any_transition_and_push_override() {
    kin::AnimationRegistry registry = registry_with_basic_animations();
    auto machine = basic_machine();
    machine->transitions.push_back({
        .from = "",
        .to = "hurt",
        .when = {{.param = "hit", .op = kin::CondOp::TriggerSet}},
        .mode = kin::TransitionMode::PushOverride,
    });

    kin::EcsWorld world;
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    kin::EcsEntity entity = world.entity("actor").set(player_for(registry, machine));

    kin::update_animation_state_machines(world);
    auto* player = entity.get_mut<kin::AnimationPlayer>();
    assert(player->state == "idle");
    assert(player->layers.size() == 1);

    player->params.set_trigger("hit");
    kin::update_animation_state_machines(world);
    player = entity.get_mut<kin::AnimationPlayer>();
    assert(player->state == "idle");
    assert(player->layers.size() == 2);
    assert(player->layers.back().animation->name == "hurt");
    assert(!player->params.triggers.contains("hit"));
}

void test_missing_targets_do_not_corrupt_player() {
    kin::AnimationRegistry registry;
    registry.add(animation("idle"));

    auto machine = std::make_shared<kin::AnimationStateMachine>();
    machine->initial = "idle";
    machine->states["idle"] = "idle";
    machine->states["missing-state"] = "missing-animation";
    machine->transitions.push_back({
        .from = "idle",
        .to = "missing-state",
        .when = {{.param = "go", .op = kin::CondOp::IsTrue}},
    });

    kin::EcsWorld world;
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    kin::EcsEntity entity = world.entity("actor").set(player_for(registry, machine));

    kin::update_animation_state_machines(world);
    auto* player = entity.get_mut<kin::AnimationPlayer>();
    assert(player->state == "idle");
    assert(player->layers.size() == 1);
    assert(player->layers[0].animation->name == "idle");

    player->params.set_bool("go", true);
    kin::update_animation_state_machines(world);
    player = entity.get_mut<kin::AnimationPlayer>();
    assert(player->state == "idle");
    assert(player->layers.size() == 1);
    assert(player->layers[0].animation->name == "idle");
}

void test_missing_initial_does_not_corrupt_player() {
    kin::AnimationRegistry registry;
    auto machine = std::make_shared<kin::AnimationStateMachine>();
    machine->initial = "missing";
    machine->states["missing"] = "missing-animation";

    kin::EcsWorld world;
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    kin::EcsEntity entity = world.entity("actor").set(player_for(registry, machine));

    kin::update_animation_state_machines(world);
    const auto* player = entity.get<kin::AnimationPlayer>();
    assert(player->state.empty());
    assert(player->layers.empty());
}

void test_shared_templated_machine_drives_bound_players() {
    kin::AnimationRegistry registry;
    registry.add_template(animation("{X}.idle"));
    registry.add_template(animation("{X}.walk"));
    registry.add_template(animation("{X}.hurt", 0.2f));

    auto machine = std::make_shared<kin::AnimationStateMachine>();
    machine->initial = "idle";
    machine->states["idle"] = "{X}.idle";
    machine->states["walk"] = "{X}.walk";
    machine->states["hurt"] = "{X}.hurt";
    machine->transitions.push_back({
        .from = "idle",
        .to = "walk",
        .when = {{.param = "moving", .op = kin::CondOp::IsTrue}},
    });
    machine->transitions.push_back({
        .from = "",
        .to = "hurt",
        .when = {{.param = "hit", .op = kin::CondOp::TriggerSet}},
        .mode = kin::TransitionMode::PushOverride,
    });

    kin::EcsWorld world;
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    kin::AnimationPlayer goblin_player = player_for(registry, machine);
    goblin_player.bindings = {{"X", "goblin"}};
    goblin_player.params.set_bool("moving", true);
    kin::EcsEntity goblin = world.entity("goblin").set(std::move(goblin_player));

    kin::AnimationPlayer orc_player = player_for(registry, machine);
    orc_player.bindings = {{"X", "orc"}};
    kin::EcsEntity orc = world.entity("orc").set(std::move(orc_player));

    kin::update_animation_state_machines(world);

    auto* goblin_stored = goblin.get_mut<kin::AnimationPlayer>();
    assert(goblin_stored->state == "walk");
    assert(goblin_stored->layers.size() == 1);
    assert(goblin_stored->layers[0].animation->name == "goblin.walk");

    auto* orc_stored = orc.get_mut<kin::AnimationPlayer>();
    assert(orc_stored->state == "idle");
    assert(orc_stored->layers.size() == 1);
    assert(orc_stored->layers[0].animation->name == "orc.idle");

    orc_stored->params.set_trigger("hit");
    kin::update_animation_state_machines(world);
    orc_stored = orc.get_mut<kin::AnimationPlayer>();
    assert(orc_stored->state == "idle");
    assert(orc_stored->layers.size() == 2);
    assert(orc_stored->layers.back().animation->name == "orc.hurt");
    assert(!orc_stored->params.triggers.contains("hit"));
}

void test_failed_bound_state_name_does_not_consume_trigger() {
    kin::AnimationRegistry registry;
    registry.add(animation("idle"));

    auto machine = std::make_shared<kin::AnimationStateMachine>();
    machine->initial = "idle";
    machine->states["idle"] = "idle";
    machine->states["bad"] = "{X}.bad";
    machine->transitions.push_back({
        .from = "idle",
        .to = "bad",
        .when = {{.param = "go", .op = kin::CondOp::TriggerSet}},
    });

    kin::EcsWorld world;
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    kin::EcsEntity entity = world.entity("actor").set(player_for(registry, machine));

    kin::update_animation_state_machines(world);
    auto* player = entity.get_mut<kin::AnimationPlayer>();
    assert(player->state == "idle");
    assert(player->layers.size() == 1);

    player->params.set_trigger("go");
    kin::update_animation_state_machines(world);
    player = entity.get_mut<kin::AnimationPlayer>();
    assert(player->state == "idle");
    assert(player->layers.size() == 1);
    assert(player->layers[0].animation->name == "idle");
    assert(player->params.triggers.contains("go"));
}

// A state left when its animation has played through ("finished"); an
// any-state transition into the state it is in is not taken again and again.
void test_finished_and_any_into_itself() {
    kin::AnimationRegistry registry = registry_with_basic_animations();
    auto machine = basic_machine();
    machine->initial = "hurt"; // 0.2 s long
    machine->transitions.push_back({.from = "hurt", .to = "idle", .when = {{.op = kin::CondOp::Finished}}});
    machine->transitions.push_back({.from = "", .to = "idle", .when = {{.param = "calm", .op = kin::CondOp::IsTrue}}});

    kin::EcsWorld world;
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    kin::EcsEntity entity = world.entity("actor").set(player_for(registry, machine));
    kin::advance_animation_players(world, 0.1f);
    assert(entity.get<kin::AnimationPlayer>()->state == "hurt");
    kin::advance_animation_players(world, 0.15f); // played through
    kin::advance_animation_players(world, 0.0f);
    assert(entity.get<kin::AnimationPlayer>()->state == "idle");

    // Calm: idle already, so idle is not started again (its time goes on).
    entity.get_mut<kin::AnimationPlayer>()->params.set_bool("calm", true);
    kin::advance_animation_players(world, 0.3f);
    kin::advance_animation_players(world, 0.3f);
    assert(entity.get<kin::AnimationPlayer>()->layers.front().cursor.time > 0.5f);

    // Written: "finished", and where an editor puts the states.
    kin::AnimationRegistryFragment fragment;
    kin::AnimationStateMachine written = *machine;
    written.places = {{"idle", {10.0f, 20.0f}}, {"any", {-40.0f, 0.0f}}};
    fragment.state_machines["moves"] = written;
    const std::string text = kin::serialize_animation_registry_fragment(fragment);
    assert(text.find("transition hurt -> idle when finished mode replace") != std::string::npos);
    assert(text.find("  place any -40 0\n  place idle 10 20\n") != std::string::npos);
    kin::AnimationRegistryFragment read;
    std::string error;
    assert(kin::parse_animation_registry_fragment(text, read, error));
    const kin::AnimationStateMachine& back = read.state_machines.at("moves");
    assert(back.transitions[0].when[0].op == kin::CondOp::Finished && back.places.at("idle").y == 20.0f);

    // A key that holds, then jumps: written and read so.
    kin::Clip held{.duration = 1.0f};
    held.properties.push_back({.property = "Transform2D.rotation",
                               .keys = {{.time = 0.0f, .value = 0.0f, .easing = kin::Easing::Step}, {.time = 1.0f, .value = 90.0f}}});
    kin::AnimationRegistryFragment steps;
    steps.animations.push_back({.name = "tick", .root = kin::clip_node(std::move(held))});
    const std::string stepped = kin::serialize_animation_registry_fragment(steps);
    assert(stepped.find("key 0 float 0 step") != std::string::npos);
    assert(kin::parse_animation_registry_fragment(stepped, read, error));
    assert(std::get<kin::Clip>(read.animations[0].root.value).properties[0].keys[0].easing == kin::Easing::Step);
}

// A clip setting Transform2D.pos from `from` to `to` over `seconds`, played over and over.
kin::Animation moving(std::string name, kin::f32 seconds, kin::Vec2f from, kin::Vec2f to, bool repeat = true) {
    kin::Clip clip{.duration = seconds};
    clip.properties.push_back({.property = "Transform2D.pos", .keys = {{.time = 0.0f, .value = from}, {.time = seconds, .value = to}}});
    kin::AnimationNode node = kin::clip_node(std::move(clip));
    return {.name = std::move(name), .root = repeat ? kin::repeat_node(std::move(node), 0) : std::move(node)};
}

bool close(kin::Vec2f a, kin::Vec2f b) {
    return std::abs(a.x - b.x) < 1e-2f && std::abs(a.y - b.y) < 1e-2f;
}

// Blend spaces: the two around the parameter's value mixed by nearness, kept
// in step; a transition with a blend fades one state into the next.
void test_blend_spaces_and_cross_fades() {
    kin::AnimationBlendSpace space{.parameter = "speed", .entries = {{"walk", 0.0f}, {"run", 2.0f}, {"sprint", 4.0f}}};
    auto weights = kin::blend_weights(space, 1.0f);
    assert(weights[0] == 0.5f && weights[1] == 0.5f && weights[2] == 0.0f);
    weights = kin::blend_weights(space, 9.0f);
    assert(weights[2] == 1.0f && weights[0] == 0.0f);
    assert(kin::blend_weights(space, -1.0f)[0] == 1.0f);

    kin::AnimationRegistry registry;
    registry.add(moving("walk", 1.0f, {0, 0}, {100, 0}));
    registry.add(moving("run", 0.5f, {0, 0}, {0, 100}));
    registry.add_blend_space("loco", {.parameter = "speed", .entries = {{"walk", 0.0f}, {"run", 2.0f}}});
    kin::EcsWorld world;
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);
    kin::AnimationPlayer player{.registry = &registry, .properties = &properties};
    player.params.set_float("speed", 1.0f);
    kin::set_base(player, "loco");
    assert(player.layers.size() == 1 && player.layers.front().blend);
    kin::EcsEntity actor = world.entity("actor").set(kin::Transform2D{}).set(std::move(player));
    // Half and half: a turn takes 0.75 s; at 0.375 s both are halfway through theirs.
    kin::advance_animation_players(world, 0.375f);
    kin::sample_animation_players(world);
    assert(close(actor.get<kin::Transform2D>()->pos, {25.0f, 25.0f}));
    assert(kin::snapshot(*actor.get<kin::AnimationPlayer>()).layers.front().animation == "loco");

    // A state machine fading from one state to the next over a second.
    registry.add(moving("rest", 1.0f, {0, 0}, {0, 0}));
    registry.add(moving("away", 1.0f, {100, 0}, {100, 0}));
    auto machine = std::make_shared<kin::AnimationStateMachine>();
    machine->initial = "rest";
    machine->states = {{"rest", "rest"}, {"away", "away"}};
    machine->transitions.push_back({.from = "rest", .to = "away", .when = {{.param = "go", .op = kin::CondOp::IsTrue}}, .blend = 1.0f});
    kin::EcsEntity mover = world.entity("mover").set(kin::Transform2D{})
                               .set(kin::AnimationPlayer{.registry = &registry, .properties = &properties, .machine = machine});
    kin::advance_animation_players(world, 0.0f);
    mover.get_mut<kin::AnimationPlayer>()->params.set_bool("go", true);
    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);
    assert(mover.get<kin::AnimationPlayer>()->state == "away" && mover.get<kin::AnimationPlayer>()->fading.size() == 1);
    assert(close(mover.get<kin::Transform2D>()->pos, {50.0f, 0.0f}));
    kin::advance_animation_players(world, 0.6f);
    kin::sample_animation_players(world);
    assert(mover.get<kin::AnimationPlayer>()->fading.empty() && close(mover.get<kin::Transform2D>()->pos, {100.0f, 0.0f}));

    // Written and read: "blendspace" blocks, a transition's "blend".
    kin::AnimationRegistryFragment fragment;
    fragment.blend_spaces["loco"] = {.parameter = "speed", .entries = {{"walk", 0.0f}, {"run", 2.5f}}};
    fragment.state_machines["moves"] = *machine;
    const std::string text = kin::serialize_animation_registry_fragment(fragment);
    assert(text.find("blendspace loco speed\n  at 0 walk\n  at 2.5 run\n") != std::string::npos);
    assert(text.find("when bool:go == true mode replace blend 1\n") != std::string::npos);
    kin::AnimationRegistryFragment read;
    std::string error;
    assert(kin::parse_animation_registry_fragment(text, read, error));
    assert(read.blend_spaces.at("loco") == fragment.blend_spaces.at("loco"));
    assert(read.state_machines.at("moves").transitions[0].blend == 1.0f);
    assert(!kin::parse_animation_registry_fragment("blendspace loco\n", read, error));
}

} // namespace

int main() {
    test_blend_spaces_and_cross_fades();
    test_finished_and_any_into_itself();
    test_bool_condition_replaces_base();
    test_float_conditions();
    test_trigger_consumption_only_when_taken();
    test_any_transition_and_push_override();
    test_missing_targets_do_not_corrupt_player();
    test_missing_initial_does_not_corrupt_player();
    test_shared_templated_machine_drives_bound_players();
    test_failed_bound_state_name_does_not_consume_trigger();
    return 0;
}
