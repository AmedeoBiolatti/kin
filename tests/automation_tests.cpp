#include <cassert>
#include <chrono>
#include <fstream>
#include <kin/core/json.hpp>
#include <kin/ecs/render.hpp>
#include <kin/runtime/scene_server.hpp>
#include <kin/ui2/context.hpp>
#include <sstream>

namespace {
using namespace kin;
struct Health {
    i32 value = 12;
    u64 serial = ~u64{0};
};
class Probe : public Scene {
public:
    Probe() {
        data.components().native<Health>("Health").field("value", &Health::value).field("serial", &Health::serial);
        auto hero = data.entities().create({.name = "Hero", .authored_id = "hero"});
        hero.set(Health{});
    }
    kin::EcsWorld* world() override { return &data; }
    ui2::Context* ui_context() override { return &ui; }
    void update(SceneContext& ctx) override { draw(ctx); }
    void render(SceneContext& ctx) override { draw(ctx); }
    void draw(SceneContext& ctx) {
        ui.begin(ctx.input, ctx.renderer, ctx.dt);
        for (int i = 0; i < 2; ++i) {
            const auto group = ui2::make_id(i == 0 ? "left" : "right");
            ui.describe(group, "group", i == 0 ? "Left" : "Right", {float(i * 100), 0, 100, 100});
            ui.semantic_key(group, i == 0 ? "left" : "right");
            ui2::Button button{.id = ui2::make_id(i == 0 ? "fire-left" : "fire-right"),
                               .bounds = {float(i * 100), 0, 80, 20},
                               .label = "Fire",
                               .enabled = i == 0};
            ui.semantic_key(button.id, i == 0 ? "fire-left" : "fire-right");
            ui2::run(ui, button);
            ui.semantic_parent(button.id, group);
            clicks += button.clicked;
        }
        ui.end();
    }
    void write_report(JsonWriter& json) const override { json.field("clicks", clicks).field("wide", ~u64{0}); }
    kin::EcsWorld data;
    ui2::Context ui;
    int clicks = 0;
};
JsonValue params(std::string_view text) {
    auto parsed = parse_json(text);
    assert(parsed.ok());
    return std::move(*parsed.value);
}
JsonValue call(ServerContext& ctx, std::string_view method, std::string_view text = "{}") {
    const auto response = dispatch_server_command(ctx, nullptr, method, params(text));
    if (!response.ok)
        std::fprintf(stderr, "%s: %s\n", std::string(method).c_str(), response.error.c_str());
    assert(response.ok);
    return params(response.result_json);
}
void reject(ServerContext& ctx, std::string_view method, std::string_view text) {
    assert(!dispatch_server_command(ctx, nullptr, method, params(text)).ok);
}
} // namespace
int main() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    auto& window = app.create_window({.width = 240, .height = 120, .hidden = true});
    kin::Renderer2D renderer{window};
    kin::SceneManager scenes;
    scenes.push(std::make_unique<Probe>());
    kin::ServerContext ctx{app, window, renderer, scenes, {}, kin::make_key(7), 1.0f / 60, false};
    ctx.realize_scenes();
    ctx.set_reset_factory([](auto& manager) { manager.push(std::make_unique<Probe>()); });
    auto schema = call(ctx, "server.schema");
    assert(schema.string_at("schema") == "kin.automation/1" && schema.find("methods")->contains("ui.query"));
    auto caps = call(ctx, "server.capabilities");
    assert(caps.bool_at("ui") && caps.bool_at("world") && caps.bool_at("reset"));
    assert(caps.find("methods")->bool_at("ui.action") && !caps.find("methods")->bool_at("sim.step"));
    auto entities = call(ctx, "world.query", R"({"selector":{"authored_id":"hero","all":["Health"]}})");
    assert(entities.int_at("count") == 1 && !entities.bool_at("truncated"));
    const auto& hero = entities.find("entities")->items().front();
    assert(hero.find("id")->is_string());
    const auto& fields = hero.find("components")->items().front().find("fields")->items();
    bool precise = false;
    for (const auto& field : fields)
        if (field.string_at("name") == "serial")
            precise = field.string_at("value") == "18446744073709551615";
    assert(precise);
    assert(call(ctx, "world.query", R"({"selector":{"none":["Health"]}})").int_at("count") == 0);
    reject(ctx, "world.query", R"({"selector":{"all":["Missing"]}})");
    reject(ctx, "world.query", R"({"limit":1.5})");
    reject(ctx, "world.query", R"({"limit":1e99})");
    auto ui = call(ctx, "ui.query", R"({"selector":{"role":"button","label":"Fire"}})");
    assert(ui.int_at("count") == 2);
    auto scoped = call(ctx, "ui.query", R"({"selector":{"role":"button","within":{"key":"left"}}})");
    assert(scoped.int_at("count") == 1);
    reject(ctx, "ui.action", R"({"selector":{"role":"button","label":"Fire"}})");
    reject(ctx, "ui.action", R"({"selector":{"key":"fire-right"}})");
    reject(ctx, "ui.action", R"({"id":"Fire","selector":{"key":"fire-left"}})");
    reject(ctx, "ui.query", R"({"selector":{"key":"absent","within":{"typo":true}}})");
    reject(ctx, "ui.query", R"({"selector":{"enabled":"yes"}})");
    call(ctx, "ui.action", R"({"selector":{"key":"fire-left","within":{"key":"left"}}})");
    auto* probe = static_cast<Probe*>(scenes.top_mut());
    assert(probe->clicks == 0);
    ctx.step();
    assert(probe->clicks == 1);
    call(ctx, "ui.action", R"({"selector":{"key":"fire-left"},"action":"focus"})");
    ctx.step();
    assert(call(ctx, "ui.query", R"({"selector":{"focused":true}})").int_at("count") == 1);
    ctx.set_automation_mode("inspect");
    caps = call(ctx, "server.capabilities");
    assert(!caps.find("methods")->bool_at("ui.action") && caps.find("methods")->bool_at("sim.step"));
    ctx.set_automation_mode("driven");
    const auto path =
        std::filesystem::temp_directory_path() /
        ("kin-automation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    ctx.start_recording(path, nullptr, 7);
    call(ctx, "ui.action", R"({"selector":{"key":"fire-left"}})");
    call(ctx, "sim.tick", R"({"count":1})");
    call(ctx, "scene.current");
    ctx.stop_recording();
    std::ifstream file(path);
    std::string line;
    int commands = 0, frames = 0;
    bool header = false, wide = false;
    while (std::getline(file, line)) {
        auto row = params(line);
        if (row.string_at("kind") == "header")
            header = row.string_at("seed") == "7";
        if (row.string_at("kind") == "command")
            ++commands;
        if (row.string_at("kind") == "frame")
            ++frames;
        wide |= line.find("18446744073709551615") != std::string::npos;
    }
    assert(header && commands == 3 && frames == 1 && wide);
    file.close();
    std::filesystem::remove(path);
    call(ctx, "input.key", R"({"name":"Space","mode":"hold"})");
    ctx.step();
    assert(app.input().held(Key::Space));
    const InputMap old_map = app.input().map();
    reject(ctx, "sim.reset", R"({"seed":9007199254740992})");
    reject(ctx, "sim.reset", R"({"seed":"18446744073709551616"})");
    call(ctx, "sim.reset", R"({"seed":"18446744073709551615"})");
    assert(!app.input().held(Key::Space) && app.input().map() == old_map && ctx.frame() == 0);
    assert(ctx.rng() == make_key(~u64{0}));
    ctx.step();
    assert(!app.input().held(Key::Space));
    ctx.shutdown_scenes();
}
