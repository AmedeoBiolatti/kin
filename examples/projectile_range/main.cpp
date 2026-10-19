#include "gameplay.hpp"

#include <iostream>
#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/ui2/text.hpp>

namespace {
using namespace kin;
class Range final : public Scene {
    std::filesystem::path _root;
    Renderer3D* _renderer = nullptr;
    std::unique_ptr<Assets3D> _assets;
    std::unique_ptr<SceneSession3D> _session;
    const projectile_range::Stats& stats() const { return *_session->world()->raw().get<projectile_range::Stats>(); }
    int _steps = 0;
    std::string errors() const {
        std::string result;
        for (const auto& error : _session->diagnostics())
            result += error + "\n";
        return result;
    }

public:
    explicit Range(std::filesystem::path root) : _root(std::move(root)) {}
    std::string_view name() const override { return "Projectile range"; }
    void on_enter(SceneContext& context) override {
        _renderer = context.renderer3d;
        if (!_renderer)
            throw std::runtime_error("runtime 3D renderer was not enabled");
        _assets = std::make_unique<Assets3D>(*_renderer, _root);
        SceneSession3DOptions options;
        options.viewport = {960, 540};
        options.view.samples = 1;
        options.physics = World3DConfig{.max_bodies = 32,
                                        .max_body_pairs = 128,
                                        .max_contact_constraints = 128,
                                        .temp_memory_bytes = 1024 * 1024,
                                        .single_threaded = true,
                                        .max_events = 128};
        options.max_runtime_entities = 24;
        options.max_runtime_prefabs = 24;
        options.max_spawn_entities = 2;
        options.max_pending_commands = 24;
        options.initialize = [](auto& scene) { projectile_range::install(scene); };
        _session = std::make_unique<SceneSession3D>(*_renderer, *_assets, options);
        if (!_session->load("scenes/projectile-range.kinscene"))
            throw std::runtime_error(errors());
        if (!projectile_range::prepare(*_session))
            throw std::runtime_error(errors());
    }
    void on_exit(SceneContext&) override {
        _session.reset();
        _assets.reset();
        _renderer = nullptr;
    }
    kin::EcsWorld* world() override { return _session ? _session->world() : nullptr; }
    void update(SceneContext& context) override {
        if (context.input.pressed(Key::Escape)) {
            context.app.quit();
            return;
        }
        if (context.input.pressed(Key::R) && (!_session->reload() || !projectile_range::prepare(*_session)))
            throw std::runtime_error(errors());
        if (!_session->step(context.dt)) {
            if (context.report)
                context.report->fail(errors());
            throw std::runtime_error(errors());
        }
        ++_steps;
    }
    void render(SceneContext& context) override {
        if (!_session->render())
            throw std::runtime_error(errors());
        context.renderer.clear();
        context.renderer.draw_texture(_session->view()->texture(), {0, 0, 960, 540});
        context.renderer.fill_rect({0, 0, 960, 85}, Color::rgba(15, 20, 30, 230));
        ui2::draw_text(context.renderer, "Projectile range   R restart   ESC exit", {14, 10}, 2, colors::white);
        const auto label = "Hits " + std::to_string(stats().hits) + "   Targets destroyed " +
                           std::to_string(stats().deaths) + "   Respawns " + std::to_string(stats().respawns);
        ui2::draw_text(context.renderer, label, {14, 44}, 2, Color::rgb(150, 235, 180));
    }
    void write_report(JsonWriter& json) const override {
        json.field("steps", _steps)
            .field("shots", stats().shots)
            .field("hits", stats().hits)
            .field("post_physics_steps", stats().post_physics_steps)
            .field("deaths", stats().deaths)
            .field("respawns", stats().respawns)
            .field("expired", stats().expired)
            .field("runtime_prefabs", u64(_session->runtime_prefabs()))
            .field("bodies", _session->physics()->body_count());
        const auto* resources = _session->world()->raw().get<projectile_range::Resources>();
        const auto pool = _session->pool_stats(resources->pool);
        json.field("pool_capacity", u64(pool.capacity))
            .field("pool_active", u64(pool.active))
            .field("pool_reused", pool.reused)
            .field("pool_exhausted", pool.exhausted)
            .field("reserved_entities", u64(_session->reserved_entities()));
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        const auto headless = kin::parse_headless_options(argc, argv);
        kin::SceneManager scenes;
        scenes.push(std::make_unique<Range>(kin::find_content_root(KIN_GAME_CONTENT, argc, argv)));
        return kin::run_scene_app({.window = {.title = "Projectile range",
                                              .width = 960,
                                              .height = 540,
                                              .logical_width = 960,
                                              .logical_height = 540,
                                              .max_fps = 60},
                                   .headless = headless,
                                   .render_headless = true,
                                   .rendering_3d = true},
                                  scenes);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
