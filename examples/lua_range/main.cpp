#include <iostream>
#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene_session3d.hpp>
#include <kin/ui2/text.hpp>
#include <sol/sol.hpp>

namespace {
using namespace kin;
// The host owns services and presents the view. Gameplay lives in lua-range.lua.
class LuaRange final : public Scene {
    std::filesystem::path _root;
    Renderer3D* _renderer = nullptr;
    std::unique_ptr<Assets3D> _assets;
    std::unique_ptr<SceneSession3D> _session;
    void require(bool result) const {
        if (result)
            return;
        std::string errors;
        for (const auto& message : _session->diagnostics())
            errors += message + "\n";
        throw std::runtime_error(errors);
    }

public:
    explicit LuaRange(std::filesystem::path root) : _root(std::move(root)) {}
    std::string_view name() const override { return "Lua range"; }
    void on_enter(SceneContext& ctx) override {
        _renderer = ctx.renderer3d;
        if (!_renderer)
            throw std::runtime_error("Lua range requires runtime 3D opt-in");
        InputMap input;
        input.bind("launch", Key::E);
        input.bind("forward", Key::W);
        input.bind("back", Key::S);
        input.bind("left", Key::A);
        input.bind("right", Key::D);
        input.bind("jump", Key::Space);
        input.bind("sprint", Key::LeftShift);
        input.bind("crouch", Key::C);
        input.bind("action", Key::F);
        input.bind("quit", Key::Escape);
        input.bind("reload", Key::R);
        ctx.input.set_map(input);
        _assets = std::make_unique<Assets3D>(*_renderer, _root);
        SceneSession3DOptions options;
        options.viewport = {960, 540};
        options.view.samples = 1;
        World3DConfig physics;
        physics.single_threaded = true;
        physics.max_bodies = 16;
        physics.max_body_pairs = 64;
        physics.max_contact_constraints = 64;
        physics.temp_memory_bytes = 1024 * 1024;
        physics.contact_events = true;
        physics.max_events = 128;
        options.physics = physics;
        options.max_runtime_prefabs = 9; // one prepared reference plus eight pool slots
        options.max_runtime_entities = 9;
        options.max_pending_commands = 8;
        options.scripts.app = &ctx.app;
        options.scripts.input = &ctx.input;
        options.scripts.bind = [autoplay = ctx.app.headless()](sol::state& lua) { lua["range_autoplay"] = autoplay; };
        _session = std::make_unique<SceneSession3D>(*_renderer, *_assets, options);
        require(_session->load("scenes/lua-range.kinscene"));
    }
    void update(SceneContext& ctx) override {
        if (ctx.input.pressed("reload") && !_session->reload())
            for (const auto& error : _session->diagnostics())
                std::cerr << error << '\n';
        require(_session->step(ctx.dt));
    }
    void render(SceneContext& ctx) override {
        if (!_renderer->available())
            return;
        require(_session->render());
        ctx.renderer.clear();
        ctx.renderer.draw_texture(_session->view()->texture(), {0, 0, 960, 540});
        ctx.renderer.fill_rect({0, 0, 960, 60}, Color::rgba(15, 20, 30, 230));
        ui2::draw_text(ctx.renderer, "WASD move  SPACE jump  SHIFT sprint  C crouch", {12, 8}, 2, colors::white);
        ui2::draw_text(ctx.renderer, "E launch crate  F action  R reload  ESC exit", {12, 34}, 2, colors::white);
    }
    void collect_actions(InputActionContext& actions) const override {
        if (_session)
            require(_session->collect_actions(actions));
    }
    void write_report(JsonWriter& json) const override {
        if (!_session)
            return;
        json.field("runtime_prefabs", u64(_session->runtime_prefabs()));
        json.field("bodies", _session->physics()->body_count());
        require(_session->write_report(json));
    }
    kin::EcsWorld* world() override { return _session ? _session->world() : nullptr; }
    void on_exit(SceneContext&) override {
        _session.reset();
        _assets.reset();
        _renderer = nullptr;
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        SceneManager scenes;
        scenes.push(std::make_unique<LuaRange>(find_content_root(KIN_GAME_CONTENT, argc, argv)));
        return run_scene_app({.window = {.title = "Lua range",
                                         .width = 960,
                                         .height = 540,
                                         .logical_width = 960,
                                         .logical_height = 540,
                                         .max_fps = 60},
                              .headless = parse_headless_options(argc, argv),
                              .render_headless = true,
                              .rendering_3d = true},
                             scenes);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
