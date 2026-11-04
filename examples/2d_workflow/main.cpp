#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/ecs/assets2d.hpp>
#include <kin/ecs/lighting.hpp>
#include <kin/ecs/normal_sprite2d.hpp>
#include <kin/ecs/skeleton2d.hpp>
#include <kin/pathfinding/navigation2d.hpp>
#include <kin/platform/app.hpp>
#include <kin/platform/input_router.hpp>
#include <kin/platform/user_data.hpp>
#include <kin/runtime/development.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene_preload2d.hpp>
#include <kin/scripting/script_engine.hpp>
#include <kin/scripting/script_reload.hpp>
#include <kin/scripting/workflows2d.hpp>
#include <kin/ui2/accessibility.hpp>
#include <kin/ui2/context.hpp>
#include <sol/sol.hpp>
#include <stdexcept>

namespace {
using namespace kin;
void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
class Workshop {
public:
    Workshop(App& app, Window& window, Renderer2D& renderer, std::filesystem::path root)
        : app(app), renderer(renderer), root(root), assets(std::make_unique<Assets2D>(renderer, root)),
          access(window, renderer, "Kin 2D Workshop"), services(std::make_shared<ScriptWorkflows2D>(world, root)) {
        register_builtin_editor_components(world);
        assets->set_residency_diagnostics(true);
        services->configure(&renderer, app.gamepads(), assets.get(), &jobs);
        grid = std::make_shared<NavGridSnapshot>();
        grid->cols = 44;
        grid->rows = 36;
        grid->tile_size = 16;
        grid->version = 1;
        grid->blocked.assign(44 * 36, 0);
        for (int y = 8; y < 32; ++y)
            if (y < 18 || y >= 21)
                grid->blocked[y * 44 + 22] = 1;
        navigation = std::make_unique<Navigation2D>(world, grid, NavigationOptions2D{.jobs = &jobs});
        player = world.entities().create({.name = "Player", .authored_id = "player"});
        player.set(Transform2D{.pos = {136, 304}});
        player.set(SkeletonRenderer2D{.asset = {"hero.kinskel"}});
        player.set(SkeletonAnimator2D{
            .animation = "idle", .fade_seconds = .15f, .blend_space = "movement", .blend_cycle_seconds = .6f});
        player.set(SkeletonLayer2D{.animation = "fire", .bone = "arm", .weight = 0, .loop = false, .additive = true});
        auto hand = world.entities().create({.parent = player});
        hand.set(Transform2D{});
        hand.set(SkeletonAttachment2D{.attachment = "muzzle"});
        hand.set(RectRenderer{.size = {4, 4}, .color = {255, 225, 112, 255}});
        for (int i = 0; i < 3; ++i) {
            auto enemy = world.entities().create({.name = "Drone" + std::to_string(i)});
            enemy.set(Transform2D{.pos = {560.0f + i * 26, 144.0f + i * 80}});
            enemy.set(SpriteRenderer{
                .sprite = {nullptr, "drone"}, .asset = {"sprites/workshop.kinsprites"}, .size = {22, 22}});
            enemy.set(NavigationAgent2D{
                .target = {136, 304}, .speed = 66, .radius = 5, .arrival_distance = 18, .neighbour_distance = 30});
            enemies.push_back(enemy);
        }
        assets->bind(world);
        script = std::make_unique<ScriptEngine>([this](sol::state& lua) {
            auto game = lua.create_named_table("game");
            game["position"] = [this] {
                const auto p = player.get<Transform2D>()->pos;
                return std::make_tuple(p.x, p.y);
            };
            game["move"] = [this](f32 x, f32 y, f32 dt) { move(x, y, dt); };
            game["command"] = [this] { return std::exchange(command, std::string{}); };
            game["pause"] = [this](bool value) { paused = value; };
            game["status"] = [this](const std::string& text) {
                status = text;
                access.announce(text);
            };
            game["progress"] = [this](f32 value) { progress = value; };
            game["fired"] = [this] {
                fire_age = 0;
                auto* layer = player.get_mut<SkeletonLayer2D>();
                layer->time = 0;
                layer->current.clear();
            };
            game["hit"] = [this](f32 x, f32 y) { return hit({x, y}); };
            game["room_budget"] = [this](u64 bytes) {
                if (room && room->assets()) {
                    room->assets()->manager().set_memory_budget(bytes);
                    room->assets()->set_texture_budget(bytes);
                }
            };
            game["counters"] = [this](int a, int b, int c, int d) {
                shots = a;
                recycled = b;
                active = c;
                rooms = d;
            };
        });
        require(script->load_file(root, "game.lua"), script->last_error());
        require(script->call_on_load({.world = &world, .workflows = services}), script->last_error());
        sync_input_map();
        ui.set_controller_navigation({});
    }
    void enable_development(std::unique_ptr<DevelopmentSession> session) {
        development = std::move(session);
        services->use_content(development->active().content);
        development->add_target("actor assets", [this](const DevelopmentRevision& revision, std::string& error) {
            struct State {
                std::unique_ptr<Assets2D> next, old;
            };
            auto state = std::make_shared<State>();
            state->next = assets->for_snapshot(revision.content);
            state->next->set_residency_diagnostics(true);
            state->next->manager().set_memory_budget(assets->manager().memory_budget());
            state->next->set_texture_budget(assets->texture_budget());
            kin::EcsWorld preview;
            world.components().copy_schema_to(preview.components());
            if (!instantiate_scene(preview, scene_document_from_world(world), error))
                return std::unique_ptr<DevelopmentChange>{};
            state->next->bind(preview);
            if (!state->next->problems().empty()) {
                error = state->next->problems().begin()->second;
                return std::unique_ptr<DevelopmentChange>{};
            }
            return std::make_unique<DevelopmentChange>(
                DevelopmentChange{[this, state](std::string&) {
                                      state->old = std::move(assets);
                                      assets = std::move(state->next);
                                      assets->bind(world);
                                      services->configure(&renderer, app.gamepads(), assets.get(), &jobs);
                                      return true;
                                  },
                                  [this, state] {
                                      if (state->old) {
                                          state->next = std::move(assets);
                                          assets = std::move(state->old);
                                          assets->bind(world);
                                          services->configure(&renderer, app.gamepads(), assets.get(), &jobs);
                                      }
                                  }});
        });
        development->add_target("authored room", [this](const DevelopmentRevision& revision, std::string& error) {
            if (room)
                return room->prepare_reload(revision, error);
            return std::make_unique<DevelopmentChange>(DevelopmentChange{[](std::string&) { return true; }, [] {}});
        });
        development->add_target("Lua", [this](const DevelopmentRevision& revision, std::string& error) {
            return prepare_script_reload(script, revision, "game.lua", error);
        });
    }
    void update(f32 dt) {
        if (!script->call_update({.dt = dt, .input = &app.input(), .app = &app})) {
            if (development) {
                status = script->last_error();
                return;
            }
            require(false, script->last_error());
        }
        sync_input_map();
        while (auto next = services->take_scene()) {
            room = std::move(next);
            ++commits;
            access.announce("Room ready");
        }
        const auto p = player.get<Transform2D>()->pos;
        for (auto enemy : enemies)
            enemy.get_mut<NavigationAgent2D>()->target = p;
        navigation->update(paused ? 0 : dt);
        for (const auto& event : navigation->events())
            replans += event.kind == NavigationEventKind2D::Replanned;
        for (auto enemy : enemies)
            detour |= enemy.get<Transform2D>()->pos.x < 344;
        fire_age += dt;
        player.get_mut<SkeletonLayer2D>()->weight = fire_age < .3f ? 1.0f : 0.0f;
        update_skeletons2d(world, paused ? 0 : dt);
        assets->bind(world);
        if (++frames % 60 == 0 && !development)
            assets->reload_changed();
    }
    void render(bool present = true) {
        renderer.clear({13, 21, 32, 255});
        if (room)
            render_world(room->ecs(), renderer);
        render_world(world, renderer);
        renderer.fill_rect({704, 0, 320, 640}, {19, 30, 44, 255});
        ui.begin(app.input(), renderer, 1.0f / 60);
        const auto panel = ui2::make_id("control-room");
        ui.describe(panel, "group", "Control room", {704, 0, 320, 640});
        ui.semantic_key(panel, "control_room");
        label("KIN 2D WORKSHOP", {24, 20}, 2.5f);
        label("Move: WASD / left stick   Fire: Space / A", {24, 590}, 1.7f);
        label("Drones find the gap. Pulse them to score.", {24, 614}, 1.5f);
        label("CONTROL ROOM", {724, 24}, 2);
        label("Score " + std::to_string(score) + "   Pulses " + std::to_string(active) + " / 32", {724, 58}, 1.5f);
        if (button("Fire pulse", {724, 90, 276, 32}))
            command = "fire";
        if (button(paused ? "Resume" : "Pause", {724, 130, 276, 32}))
            command = "pause";
        if (button("Rebind Fire", {724, 170, 276, 32}))
            command = "rebind";
        if (button("Next room", {724, 210, 176, 32}))
            command = "next";
        if (button("Cancel", {908, 210, 92, 32}))
            command = "cancel";
        ui2::ProgressBar bar{.bounds = {724, 252, 276, 6}, .value = progress};
        ui2::run(ui, bar);
        if (button("Apply 1 KiB budgets", {724, 272, 276, 32}))
            command = "trim";
        ui2::Toggle motion{.id = ui2::make_id("reduced-motion"),
                           .bounds = {724, 312, 276, 28},
                           .label = "Reduced motion",
                           .value = ui.reduced_motion()};
        ui2::run(ui, motion);
        ui.semantic_key(motion.id, "reduced_motion");
        ui.semantic_parent(motion.id, panel);
        if (motion.changed)
            ui.set_reduced_motion(motion.value);
        label("ACTOR ASSET CACHE", {724, 360}, 1.7f);
        const auto cpu = assets->manager().memory_report(), gpu = assets->texture_memory_report();
        label("CPU " + std::to_string(cpu.resident_bytes) + " B / " + std::to_string(cpu.budget_bytes) + " B",
              {724, 390}, 1.4f);
        label("GPU " + std::to_string(gpu.resident_bytes) + " B / " + std::to_string(gpu.budget_bytes) + " B",
              {724, 412}, 1.4f);
        const auto room_memory = room && room->assets() ? room->assets()->texture_memory_report() : AssetMemoryReport{};
        label("Room GPU: " + std::to_string(room_memory.resident_bytes) + " B (pinned)", {724, 434}, 1.4f);
        label(gpu.over_budget    ? "Budget exceeded: live handles retained"
              : gpu.budget_bytes ? "Within texture budget"
                                 : "Budget 0: unlimited",
              {724, 456}, 1.2f);
        f32 y = 480;
        for (const auto& row : assets->texture_residency()) {
            label(row.path + "  " + std::to_string(row.bytes) + " B", {724, y}, 1.1f);
            label(row.retained_by.empty() ? "unused; eligible for eviction" : row.retained_by.front(), {724, y + 14},
                  1.0f);
            y += 34;
            if (y > 550)
                break;
        }
        label(status, {724, 590}, 1.1f);
        label("Room " + std::to_string(rooms) + "  " +
                  (renderer.capabilities().data_buffers ? "GPU lighting" : "CPU lighting"),
              {724, 615}, 1.2f);
        ui.end();
        access.update(ui);
        if (!screenshot.empty()) {
            std::vector<u8> pixels;
            Vec2i size;
            require(renderer.read_rgba({0, 0, 1024, 640}, pixels, size), "screenshot readback failed");
            require(save_image(Image{size, std::move(pixels)}, screenshot), "screenshot write failed");
            screenshot.clear();
        }
        if (present)
            renderer.present();
    }
    void action(const std::string& selector, ui2::SemanticAction action = ui2::SemanticAction::Click) {
        std::string error;
        require(ui.queue_action(selector, action, {}, error), selector + ": " + error);
        ++semantic_actions;
    }
    void verify() {
        require(commits >= 2 && rooms >= 2, "room preload/commit was not exercised");
        require(shots > 0 && recycled == shots && active == 0, "pulse pool/tween lifecycle failed");
        require(replans > 0 && detour, "navigation did not pass through the wall gap");
        require(semantic_actions >= 5 && ui.reduced_motion(), "semantic UI actions did not reach their targets");
        require(assets->texture_memory_report().over_budget, "live texture budget pressure was not reported");
        require(!script->last_error().size(), script->last_error());
        if (renderer.capabilities().data_buffers) {
            bool used = false;
            room->ecs().each(
                [&](const NormalSpriteRenderer2D& sprite) { used |= sprite.cache && sprite.cache->gpu.used; });
            require(used, "GPU normal lighting did not execute");
        }
        std::printf("workshop verified: shots=%d recycled=%d rooms=%d replans=%d semantic_actions=%d\n", shots,
                    recycled, rooms, replans, semantic_actions);
    }
    void capture(std::filesystem::path path) { screenshot = std::move(path); }
    kin::EcsWorld& ecs() { return world; }
    DevelopmentSession* dev_session() { return development.get(); }
    ui2::Context& interface() { return ui; }
    void write_report(JsonWriter& json) const {
        json.field("room_name", room ? room->name() : std::string_view{});
        const auto position = player.get<Transform2D>()->pos;
        json.field("shots", shots)
            .field("recycled", recycled)
            .field("active", active)
            .field("score", score)
            .field("rooms", rooms)
            .field("commits", commits)
            .field("replans", replans)
            .field("paused", paused)
            .field("progress", progress)
            .field("reduced_motion", ui.reduced_motion())
            .field("script_error", script->last_error());
        json.key("player").begin_object().field("x", position.x).field("y", position.y).end_object();
        const auto stats = jobs.stats();
        json.key("jobs")
            .begin_object()
            .field("queued", static_cast<u64>(stats.queued))
            .field("running", static_cast<u64>(stats.running))
            .field("awaiting_pump", static_cast<u64>(stats.awaiting_pump))
            .end_object();
    }

private:
    void sync_input_map() {
        if (const auto* play = services->input_router().layer("play")) {
            auto map = play->map;
            map.bind("next_room", Key::N);
            app.input().set_map(std::move(map));
        }
    }
    void move(f32 x, f32 y, f32 dt) {
        auto* t = player.get_mut<Transform2D>();
        const Vec2f next{std::clamp(t->pos.x + x * 140 * dt, 40.0f, 668.0f),
                         std::clamp(t->pos.y + y * 140 * dt, 88.0f, 544.0f)};
        if (!grid->is_blocked(grid->world_to_tile(next)))
            t->pos = next;
        player.get_mut<SkeletonAnimator2D>()->blend_position = {x, y};
    }
    bool hit(Vec2f p) {
        for (auto enemy : enemies) {
            auto* t = enemy.get_mut<Transform2D>();
            if (std::hypot(t->pos.x - p.x, t->pos.y - p.y) < 22) {
                t->pos = {600, 400};
                ++score;
                return true;
            }
        }
        return false;
    }
    bool button(std::string text, Rectf bounds) {
        const auto key = text == "Pause" || text == "Resume" ? "pause"
                         : text == "Fire pulse"              ? "fire"
                         : text == "Rebind Fire"             ? "rebind"
                         : text == "Next room"               ? "next_room"
                         : text == "Cancel"                  ? "cancel_load"
                                                             : "memory_budget";
        ui2::Button button{.id = ui2::make_id(key), .bounds = bounds, .label = std::move(text)};
        ui.semantic_key(button.id, key);
        ui2::run(ui, button);
        ui.semantic_parent(button.id, ui2::make_id("control-room"));
        return button.clicked;
    }
    void label(std::string text, Vec2f pos, f32 scale) {
        const auto width = (pos.x < 704 ? 680.0f : 1000.0f) - pos.x;
        const auto font = ui2::bitmap_font();
        if (ui2::measure_text(font, text, scale).x > width) {
            while (!text.empty() && ui2::measure_text(font, text + "...", scale).x > width)
                text.pop_back();
            text += "...";
        }
        ui2::Label label{
            .bounds = {pos.x, pos.y, width, 20}, .text = std::move(text), .text_style = {.font = font, .scale = scale}};
        ui2::run(ui, label);
    }
    App& app;
    Renderer2D& renderer;
    std::filesystem::path root;
    kin::EcsWorld world;
    std::unique_ptr<Assets2D> assets;
    JobSystem jobs{{.single_threaded = true}};
    ui2::Context ui;
    ui2::Accessibility access;
    std::shared_ptr<ScriptWorkflows2D> services;
    std::unique_ptr<ScriptEngine> script;
    std::unique_ptr<DevelopmentSession> development;
    std::unique_ptr<PreloadedScene2D> room;
    std::shared_ptr<NavGridSnapshot> grid;
    std::unique_ptr<Navigation2D> navigation;
    kin::EcsEntity player;
    std::vector<kin::EcsEntity> enemies;
    std::string command, status = "Ready. Controller Start joins a player.";
    std::filesystem::path screenshot;
    f32 fire_age = 10, progress = 0;
    bool paused = false, detour = false;
    int shots = 0, recycled = 0, active = 0, score = 0, rooms = 0, commits = 0, replans = 0, semantic_actions = 0,
        frames = 0;
};
class WorkshopScene final : public Scene {
public:
    explicit WorkshopScene(std::filesystem::path root) : root(std::move(root)) {}
    std::string_view name() const override { return "Workshop"; }
    void on_enter(SceneContext& ctx) override {
        if (ctx.development) {
            auto session = std::make_unique<DevelopmentSession>(root);
            game = std::make_unique<Workshop>(ctx.app, ctx.window, ctx.renderer, session->active().content->root());
            game->enable_development(std::move(session));
        } else
            game = std::make_unique<Workshop>(ctx.app, ctx.window, ctx.renderer, root);
    }
    void update(SceneContext& ctx) override {
        try {
            game->update(ctx.dt);
        } catch (const std::exception& error) {
            if (ctx.report)
                ctx.report->fail(error.what());
            else
                throw;
        }
    }
    DevelopmentSession* development_session() override { return game ? game->dev_session() : nullptr; }
    void render(SceneContext&) override { game->render(false); }
    kin::EcsWorld* world() override { return game ? &game->ecs() : nullptr; }
    ui2::Context* ui_context() override { return game ? &game->interface() : nullptr; }
    void write_report(JsonWriter& json) const override {
        if (game)
            game->write_report(json);
    }
    void collect_actions(InputActionContext& actions) const override {
        actions.add("fire", "Fire pulse");
        actions.add("pause", "Pause/resume");
        actions.add("next_room", "Load next room");
    }

private:
    std::filesystem::path root;
    std::unique_ptr<Workshop> game;
};
} // namespace
int main(int argc, char** argv) {
    try {
        bool test = false;
        std::filesystem::path content, pack, screenshot;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--self-test")
                test = true;
            else if (arg == "--content" && i + 1 < argc)
                content = argv[++i];
            else if (arg == "--pack" && i + 1 < argc)
                pack = argv[++i];
            else if (arg == "--screenshot" && i + 1 < argc)
                screenshot = argv[++i];
        }
        auto root = content.empty() ? kin::find_content_root(KIN_GAME_CONTENT, argc, argv) : content;
        if (!pack.empty()) {
            std::string error;
            auto archive = kin::ContentPack::open(pack, &error);
            require(archive != nullptr, error);
            kin::mount_content_pack(root, archive);
        }
        auto headless = kin::parse_headless_options(argc, argv);
        if (headless.server || headless.inspect || headless.development) {
            const bool server = headless.server;
            kin::SceneManager scenes;
            auto factory = [root](kin::SceneManager& target) { target.push(std::make_unique<WorkshopScene>(root)); };
            kin::GameInfo info;
            info.id = "kin.workshop";
            info.title = "Kin 2D Workshop";
            info.version = KIN_GAME_VERSION;
            factory(scenes);
            if (server)
                kin::set_user_data_dir_override(std::filesystem::current_path() / "user-data");
            const auto result =
                kin::run_scene_app({.window = {.title = "Kin 2D Workshop", .width = 1024, .height = 640},
                                    .headless = std::move(headless),
                                    .game = &info,
                                    .render_headless = true,
                                    .reset_scenes = factory},
                                   scenes);
            if (server)
                kin::clear_user_data_dir_override();
            return result;
        }
        auto profile = std::filesystem::temp_directory_path() /
                       ("kin-workshop-test-profile-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (test) {
            while (!std::filesystem::create_directory(profile))
                profile += "-next";
            kin::set_user_data_dir_override(profile);
        }
        kin::App app{{.mode = test ? kin::AppMode::Headless : kin::AppMode::Windowed}};
        app.open_gamepads();
        auto& window = app.create_window({.title = "Kin 2D Workshop", .width = 1024, .height = 640, .hidden = test});
        kin::Renderer2D renderer{kin::make_render_backend(window, false, true)};
        Workshop game{app, window, renderer, root};
        if (test) {
            app.run_for(
                480,
                [&](kin::f32 dt, int frame) {
                    auto& input = app.input();
                    if (frame == 3) {
                        input.set_key_pressed(kin::Key::D);
                        input.set_key_pressed(kin::Key::Space);
                    }
                    if (frame == 4)
                        input.set_key_released(kin::Key::Space);
                    if (frame == 24)
                        input.set_key_released(kin::Key::D);
                    if (frame == 40)
                        game.action("Pause");
                    if (frame == 45)
                        game.action("Resume");
                    if (frame == 60)
                        game.action("Rebind Fire");
                    if (frame == 63)
                        input.set_key_pressed(kin::Key::Enter);
                    if (frame == 64)
                        input.set_key_released(kin::Key::Enter);
                    if (frame == 70)
                        input.set_key_pressed(kin::Key::Enter);
                    if (frame == 71)
                        input.set_key_released(kin::Key::Enter);
                    if (frame == 85)
                        game.action("Next room");
                    if (frame == 100)
                        game.action("Apply 1 KiB budgets");
                    if (frame == 120)
                        game.action("Reduced motion");
                    if (frame == 479 && !screenshot.empty())
                        game.capture(screenshot);
                    game.update(dt);
                },
                [&](kin::f32) { game.render(); });
            game.verify();
            std::filesystem::remove_all(profile);
            kin::clear_user_data_dir_override();
        } else
            app.run([&](kin::f32 dt) { game.update(dt); }, [&](kin::f32) { game.render(); });
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
