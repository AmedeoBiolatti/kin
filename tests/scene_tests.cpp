#include <kin/ecs/ecs_scene.hpp>
#include <kin/ecs/render.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/debug_options.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/scene/transitions.hpp>

#include <cassert>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <memory>
#include <string_view>
#include <vector>
#include <string>

namespace {

struct Counts {
    int enters = 0;
    int exits = 0;
    int suspends = 0;
    int resumes = 0;
    int updates = 0;
    int renders = 0;
};

class FakeTextureBackend final : public kin::ITextureBackend {
public:
    explicit FakeTextureBackend(kin::Vec2i size)
        : _size(size) {
    }

    kin::Vec2i size() const override { return _size; }

private:
    kin::Vec2i _size{};
};

class FakeBackend final : public kin::IRenderer2DBackend {
public:
    std::string_view name() const override { return "fake"; }
    void clear(kin::Color color) override {
        ++clears;
        clear_colors.push_back(color);
    }
    void present() override {}
    void set_logical_size(kin::Vec2i) override {}
    void set_integer_logical_size(kin::Vec2i) override {}
    kin::Vec2i output_size() const override { return {320, 180}; }
    kin::Vec2f window_to_logical(kin::Vec2f value) const override { return value; }
    kin::Vec2f logical_to_window(kin::Vec2f value) const override { return value; }
    kin::Texture create_texture_from_rgba(const kin::u8*, kin::Vec2i size) override {
        return kin::Texture{std::make_shared<FakeTextureBackend>(size)};
    }
    void draw_texture(const kin::Texture&, kin::Rectf dest) override { textures.push_back(dest); }
    void draw_texture(const kin::Texture&, kin::Rectf, kin::Rectf dest) override { textures.push_back(dest); }
    void fill_rect(kin::Rectf rect, kin::Color color) override {
        rects.push_back(rect);
        colors.push_back(color);
    }
    void draw_rect(kin::Rectf rect, kin::Color color) override {
        outlines.push_back(rect);
        colors.push_back(color);
    }
    void draw_line(kin::Vec2f, kin::Vec2f, kin::Color) override {}
    void set_viewport(kin::Rectf) override {}
    void reset_viewport() override {}
    void push_viewport(kin::Rectf) override {}
    void pop_viewport() override {}

    int clears = 0;
    std::vector<kin::Color> clear_colors;
    std::vector<kin::Rectf> rects;
    std::vector<kin::Rectf> outlines;
    std::vector<kin::Rectf> textures;
    std::vector<kin::Color> colors;
};

class DefaultRenderedScene final : public kin::EcsScene {
public:
    DefaultRenderedScene() {
        ecs().component<kin::Transform2D>("Transform2D");
        ecs().component<kin::RectRenderer>("RectRenderer");
        ecs().component<kin::StaticRenderable>("StaticRenderable");

        ecs().entity("world-rect")
            .set(kin::Transform2D{{10.0f, 10.0f}})
            .set(kin::RectRenderer{.size = {8.0f, 8.0f}, .color = kin::Color::rgb(20, 30, 40), .static_renderable = true});
        ecs().entity("dynamic-rect")
            .set(kin::Transform2D{{20.0f, 10.0f}})
            .set(kin::RectRenderer{.size = {4.0f, 6.0f}, .color = kin::Color::rgb(80, 90, 100), .order = 1});

        render_config().enabled = true;
        render_config().clear_color = kin::Color::rgb(1, 2, 3);
        render_config().cull_padding = 0.0f;
        render_config().camera_callback = [](kin::EcsWorld&, kin::SceneContext&) {
            kin::Camera2D camera;
            camera.offset = {5.0f, 0.0f};
            camera.set_viewport({64.0f, 64.0f});
            return camera;
        };
    }
};

class CountingScene final : public kin::Scene {
public:
    CountingScene(std::string_view label, Counts& counts, bool overlay = false, bool update_below = false)
        : _label(label),
          _counts(counts),
          _overlay(overlay),
          _update_below(update_below) {
    }

    std::string_view name() const override { return _label; }
    bool is_overlay() const override { return _overlay; }
    bool updates_below() const override { return _update_below; }

    void on_enter(kin::SceneContext&) override { ++_counts.enters; }
    void on_exit(kin::SceneContext&) override { ++_counts.exits; }
    void on_suspend(kin::SceneContext&) override { ++_counts.suspends; }
    void on_resume(kin::SceneContext&) override { ++_counts.resumes; }
    void update(kin::SceneContext&) override { ++_counts.updates; }
    void render(kin::SceneContext&) override { ++_counts.renders; }

private:
    std::string_view _label;
    Counts& _counts;
    bool _overlay = false;
    bool _update_below = false;
};

class ActionScene final : public kin::Scene {
public:
    ActionScene(std::string_view label, std::string_view action, bool overlay = false, bool update_below = false)
        : _label(label),
          _action(action),
          _overlay(overlay),
          _update_below(update_below) {
    }

    std::string_view name() const override { return _label; }
    bool is_overlay() const override { return _overlay; }
    bool updates_below() const override { return _update_below; }

    void collect_actions(kin::InputActionContext& context) const override {
        context.add(_action, _label);
    }

private:
    std::string_view _label;
    std::string_view _action;
    bool _overlay = false;
    bool _update_below = false;
};

struct Fixture {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window;
    kin::Renderer2D renderer;
    kin::SceneManager scenes;
    kin::SceneContext ctx;

    Fixture()
        : window(app.create_window({.title = "scene-test", .width = 64, .height = 64, .hidden = true})),
          renderer(window),
          ctx{app, window, renderer, app.input(), scenes, 0.1f, true} {
    }
};

void reset_fixture(Fixture& fixture) {
    fixture.scenes.clear();
    fixture.scenes.update(fixture.ctx);
}

void test_stack_lifecycle(Fixture& fixture) {
    reset_fixture(fixture);
    Counts a{};
    Counts b{};

    fixture.scenes.push(std::make_unique<CountingScene>("a", a));
    fixture.scenes.update(fixture.ctx);
    assert(fixture.scenes.depth() == 1);
    assert(a.enters == 1);
    assert(a.updates == 0);

    fixture.scenes.update(fixture.ctx);
    assert(a.updates == 1);

    fixture.scenes.push(std::make_unique<CountingScene>("b", b));
    fixture.scenes.update(fixture.ctx);
    assert(a.suspends == 1);
    assert(b.enters == 1);

    fixture.scenes.pop();
    fixture.scenes.update(fixture.ctx);
    assert(b.exits == 1);
    assert(a.resumes == 1);
    assert(fixture.scenes.depth() == 1);

    fixture.scenes.clear();
    fixture.scenes.update(fixture.ctx);
    assert(a.exits == 1);
    assert(fixture.scenes.empty());
}

void test_overlay_render_and_update_rules(Fixture& fixture) {
    reset_fixture(fixture);
    Counts base{};
    Counts overlay{};

    fixture.scenes.push(std::make_unique<CountingScene>("base", base));
    fixture.scenes.update(fixture.ctx);
    fixture.scenes.push(std::make_unique<CountingScene>("overlay", overlay, true, false));
    fixture.scenes.update(fixture.ctx);

    fixture.scenes.update(fixture.ctx);
    assert(base.updates == 1);
    assert(overlay.updates == 1);

    fixture.scenes.render(fixture.ctx);
    assert(base.renders == 1);
    assert(overlay.renders == 1);

    Counts ticking_overlay{};
    fixture.scenes.push(std::make_unique<CountingScene>("tick", ticking_overlay, true, true));
    fixture.scenes.update(fixture.ctx);
    fixture.scenes.update(fixture.ctx);
    assert(overlay.updates == 3);
    assert(ticking_overlay.updates == 1);
}

void test_available_actions_follow_active_scene_stack(Fixture& fixture) {
    reset_fixture(fixture);
    kin::InputMap map;
    map.bind("play", kin::Key::Enter);
    map.bind("pause", kin::Key::Escape);
    fixture.app.input().set_map(map);

    fixture.scenes.push(std::make_unique<ActionScene>("play", "play"));
    fixture.scenes.update(fixture.ctx);

    auto available = fixture.scenes.available_actions(fixture.app.input().map());
    assert(available.size() == 1);
    assert(available[0].name == "play");
    assert(available[0].bindings.size() == 1);

    fixture.scenes.push(std::make_unique<ActionScene>("pause", "pause", true, false));
    fixture.scenes.update(fixture.ctx);
    available = fixture.scenes.available_actions(fixture.app.input().map());
    assert(available.size() == 1);
    assert(available[0].name == "pause");

    fixture.scenes.pop();
    fixture.scenes.update(fixture.ctx);
    fixture.scenes.push(std::make_unique<ActionScene>("hud", "pause", true, true));
    fixture.scenes.update(fixture.ctx);
    available = fixture.scenes.available_actions(fixture.app.input().map());
    assert(available.size() == 2);
    assert(available[0].name == "play");
    assert(available[1].name == "pause");
}

void test_fade_overlay_pops_itself(Fixture& fixture) {
    reset_fixture(fixture);
    Counts base{};

    fixture.scenes.push(std::make_unique<CountingScene>("base", base));
    fixture.scenes.update(fixture.ctx);
    fixture.scenes.push(kin::fade_out(0.05f));
    fixture.scenes.update(fixture.ctx);
    assert(fixture.scenes.depth() == 2);

    fixture.scenes.update(fixture.ctx);
    assert(fixture.scenes.depth() == 1);
    assert(fixture.scenes.top() != nullptr);
    assert(fixture.scenes.top()->name() == "base");
}

void test_replace_with_fade_replaces_scene_below_overlay(Fixture& fixture) {
    reset_fixture(fixture);
    Counts old_scene{};
    Counts next_scene{};

    fixture.scenes.push(std::make_unique<CountingScene>("old", old_scene));
    fixture.scenes.update(fixture.ctx);
    fixture.scenes.push(kin::replace_with_fade(std::make_unique<CountingScene>("next", next_scene), 0.05f));
    fixture.scenes.update(fixture.ctx);
    assert(fixture.scenes.depth() == 2);

    fixture.scenes.update(fixture.ctx);
    assert(old_scene.exits == 1);
    assert(next_scene.enters == 1);
    assert(fixture.scenes.depth() == 2);
    assert(fixture.scenes.at(0)->name() == "next");
    assert(fixture.scenes.at(1)->name() == "FadeScene");
}

void test_transition_to_replaces_scene_below_overlay(Fixture& fixture) {
    reset_fixture(fixture);
    Counts old_scene{};
    Counts next_scene{};

    fixture.scenes.push(std::make_unique<CountingScene>("old", old_scene));
    fixture.scenes.update(fixture.ctx);
    // Dissolve degrades to the flat-color fade on this headless (materials_2d=false)
    // backend, but the orchestration (out -> swap -> in -> pop) is identical.
    fixture.scenes.push(kin::transition_to(std::make_unique<CountingScene>("next", next_scene),
                                           kin::TransitionKind::Dissolve, 0.05f));
    fixture.scenes.update(fixture.ctx);
    assert(fixture.scenes.depth() == 2); // old + TransitionOutScene

    // Advance past the (short) transition; render each step to exercise the degrade path.
    for (int i = 0; i < 6; ++i) {
        fixture.scenes.render(fixture.ctx);
        fixture.scenes.update(fixture.ctx);
    }

    assert(next_scene.enters == 1);
    assert(old_scene.exits == 1);
    assert(fixture.scenes.depth() == 1);
    assert(fixture.scenes.at(0)->name() == "next");
}

void test_ecs_scene_default_render_draws_world() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "ecs-render-scene-test", .width = 64, .height = 64, .hidden = true});
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    kin::Renderer2D renderer{std::move(backend)};
    kin::SceneManager scenes;
    kin::SceneContext ctx{app, window, renderer, app.input(), scenes, 0.1f, true};

    DefaultRenderedScene scene;
    scene.render(ctx);

    assert(raw->clears == 1);
    assert(raw->clear_colors[0] == kin::Color::rgb(1, 2, 3));
    assert(raw->rects.size() >= 2);
    assert((raw->rects[0] == kin::Rectf{5.0f, 10.0f, 8.0f, 8.0f}));
    assert((raw->rects[1] == kin::Rectf{15.0f, 10.0f, 4.0f, 6.0f}));
    assert(!scene.static_render_cache().dirty());

    raw->rects.clear();
    raw->colors.clear();
    scene.render(ctx);
    assert(raw->clears == 2);
    assert(raw->rects.size() >= 2);
    assert((raw->rects[0] == kin::Rectf{5.0f, 10.0f, 8.0f, 8.0f}));
    assert((raw->rects[1] == kin::Rectf{15.0f, 10.0f, 4.0f, 6.0f}));

    raw->rects.clear();
    raw->colors.clear();
    kin::RuntimeDebugOptions debug_options;
    debug_options.detailed_render_timings_enabled = true;
    std::vector<std::string> timing_names;
    debug_options.record_timing = [&](std::string_view name, kin::f64) {
        timing_names.push_back(std::string{name});
    };
    ctx.debug_options = &debug_options;
    scene.mark_render_cache_dirty("test reason");
    scene.render(ctx);
    assert(debug_options.render_stats.static_cache_used);
    assert(debug_options.render_stats.static_commands == 1);
    assert(debug_options.render_stats.dynamic_commands == 1);
    assert(debug_options.render_stats.frames == 1);
    assert(debug_options.render_stats.static_cache_rebuilds_other == 1);
    assert(debug_options.render_stats.static_cache_last_dirty_reason == "test reason");
    assert(std::ranges::find(timing_names, "render.scene.camera") != timing_names.end());
    assert(std::ranges::find(timing_names, "render.scene.collect_dynamic") != timing_names.end());
    assert(std::ranges::find(timing_names, "render.scene.flush_world") != timing_names.end());

    raw->rects.clear();
    raw->colors.clear();
    debug_options.pass_world_enabled = false;
    debug_options.pass_ui_enabled = false;
    scene.render(ctx);

    assert(raw->clears == 4);
    assert(raw->rects.empty());
}

} // namespace

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    Fixture fixture;
    test_stack_lifecycle(fixture);
    test_overlay_render_and_update_rules(fixture);
    test_available_actions_follow_active_scene_stack(fixture);
    test_fade_overlay_pops_itself(fixture);
    test_replace_with_fade_replaces_scene_below_overlay(fixture);
    test_transition_to_replaces_scene_below_overlay(fixture);
    test_ecs_scene_default_render_draws_world();
    return 0;
}
