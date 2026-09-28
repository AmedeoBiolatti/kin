#include <kin/anim/player.hpp>
#include <kin/anim/state_machine.hpp>

#include <cassert>
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

} // namespace

int main() {
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
