#include "arena_art.hpp"
#include "example_common.hpp"
#include "siege_audio.hpp"
#include "workloads.hpp"

#include <kin/anim/track.hpp>
#include <kin/core/json.hpp>
#include <kin/save/save_store.hpp>
#include <kin/scene/transitions.hpp>
#include <kin/ui2/widgets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace examples {
namespace {

constexpr Color ink = Color::rgb(226, 236, 242);
constexpr Color muted = Color::rgb(146, 170, 186);
constexpr Color teal = Color::rgb(103, 226, 206);
constexpr Color gold = Color::rgb(236, 214, 160);

// What the scenes of one app run share: options, sound, and the best run.
struct Siege {
    Siege(Options o, bool tool_run) : options(std::move(o)), audio(options.mute || options.benchmark || tool_run) {
        if (!tool_run) best.emplace();
    }
    Options options;
    SiegeAudio audio;
    std::optional<BestRunStore> best; // none in tool runs
};

std::string clock_text(float seconds) {
    const int s = std::max(0, static_cast<int>(seconds));
    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d", s / 60, s % 60);
    return text;
}

// Native-pixel UI over the whole window, in density-independent units.
struct Screen {
    Renderer2D& renderer;
    ui2::Context& ui;
    float dpi = 1, w = 0, h = 0;

    ui2::TextStyle style(float points, Color color) const {
        const float size = std::min(32.0f, points * dpi);
        return {.font = ui2::system_ui_font(size), .scale = points * dpi / size, .color = color};
    }
    Rectf px(Rectf r) const { return {r.x * dpi, r.y * dpi, r.w * dpi, r.h * dpi}; }
    float width(std::string_view value, float points) const {
        const ui2::TextStyle s = style(points, ink);
        return ui2::measure_text(s.font, value, s.scale).x / dpi;
    }
    void text(std::string_view value, float x, float y, float points, Color color) const {
        ui.text(value, {x * dpi, y * dpi}, style(points, color));
    }
    void centered(std::string_view value, float y, float points, Color color) const {
        text(value, (w - width(value, points)) * .5f, y, points, color);
    }
};

Color with_alpha(Color c, float alpha) {
    c.a = static_cast<u8>(std::clamp(static_cast<float>(c.a) * alpha, 0.0f, 255.0f));
    return c;
}

// An entrance for titles: rises into place while fading in (kin property tracks).
float rise(float t) {
    static const PropertyTrack track{.property = "rise", .keys = {{.time = 0, .value = 1.0f}, {.time = .7f, .value = 0.0f, .easing = Easing::EaseOut}}};
    return std::get<f32>(sample(track, t));
}

std::unique_ptr<Scene> make_title(std::shared_ptr<Siege> siege);
std::unique_ptr<Scene> make_results(std::shared_ptr<Siege> siege, RunResult result);

class ArenaScene final : public Scene {
public:
    ArenaScene(std::shared_ptr<Siege> siege, bool autoplay, bool from_title)
        : _siege(std::move(siege)), _arena(_siege->options.enemies), _autoplay(autoplay), _from_title(from_title) {}
    std::string_view name() const override { return "Signal Siege"; }
    EcsWorld* world() override { return &_arena.world; }
    void on_enter(SceneContext& ctx) override {
        _seed = rng_detail::to_u64(ctx.rng); _arena.reset(_seed);
        // Tool runs play themselves; a run chosen on the title plays as chosen.
        if (!_from_title) _autoplay = _autoplay || _siege->options.benchmark || ctx.app.headless();
        _power_grid = _siege->options.power_grid;
        _siege->audio.set_drone(true);
    }
    void update(SceneContext& ctx) override {
        _siege->audio.update(ctx.dt);
        if (ctx.input.pressed("quit")) {
            if (_power_grid) { _power_grid = false; return; }
            if (_from_title) { ctx.scenes.push(transition_to(make_title(_siege), TransitionKind::Crossfade, .6f)); return; }
            ctx.app.quit();
        }
        if (ctx.input.pressed("power_grid")) { _power_grid = !_power_grid; return; }
        if (ctx.input.pressed("pause")) _paused = !_paused;
        if (ctx.input.pressed("autoplay")) _autoplay = !_autoplay;
        if (ctx.input.pressed("reset")) {
            _arena.reset(_seed); _power_grid = false; _paused = false; _power_state = {}; _hud.reset(); _ending = -1;
        }
        if (_paused || _power_grid) return;
        const Vec2f mouse = ctx.renderer.window_to_logical(ctx.input.mouse_pos());
        const Vec2f aim = _camera.screen_to_world(mouse);
        ArenaInput input{.move = {
            float(ctx.input.held("right")) - float(ctx.input.held("left")),
            float(ctx.input.held("down")) - float(ctx.input.held("up"))},
            .aim = {aim.x - _arena.player.x, aim.y - _arena.player.y},
            .fire = ctx.input.held("fire"), .dash = ctx.input.pressed("dash")};
        _arena.step(ctx.dt, _autoplay ? _arena.autopilot() : input, _autoplay);
        _siege->audio.play_events(_arena.events, _arena.player);
        _painter.update(_arena, ctx.dt);
        if (_hud.wave_started()) _siege->audio.play("wave");

        // A run that ends shows its result for a moment, then the results screen.
        const bool over = _arena.health <= 0 || (_arena.won() && !_autoplay);
        if (over && _ending < 0 && _from_title) {
            _ending = 0;
            _siege->audio.play(_arena.health > 0 ? "win" : "lose");
        }
        if (_ending >= 0 && (_ending += ctx.dt) > 1.8f) {
            _ending = -100; // hand over once
            int upgrades = 0;
            for (int id = 1; id < int(power_ups.size()); ++id) upgrades += _arena.has_power(id) ? 1 : 0;
            ctx.scenes.push(transition_to(make_results(_siege, {.won = _arena.health > 0, .time = std::min(_arena.elapsed, 90.0f),
                .kills = _arena.kills, .cores = _arena.collected, .upgrades = upgrades}), TransitionKind::Dissolve, .8f));
        }
    }
    void render(SceneContext& ctx) override {
        _display_scale = update_ui_scale(ctx.window, _siege->options, _minimum_size);
        auto& renderer = ctx.renderer;
        if (_grid_coordinates != _power_grid) {
            renderer.set_logical_size(_power_grid ? 0 : 1280, _power_grid ? 0 : 800);
            _grid_coordinates = _power_grid;
        }
        renderer.clear(Color::rgb(12, 20, 28));
        if (_power_grid) {
            _painter.disable_post_process(renderer); // keep the upgrade screen's text crisp
            if (render_power_grid(_arena,ctx.input,renderer,_power_state,_display_scale)) _power_grid=false;
            capture(ctx, _siege->options, ++_frames, _captured);
            return;
        }
        if (!_painter.ready()) _painter.init(renderer, _arena);
        _painter.enable_post_process(renderer);
        _camera.viewport = {1280, 800};
        _camera.offset = {std::clamp(_arena.player.x - 640, 0.0f, 1792.0f), std::clamp(_arena.player.y - 400, 0.0f, 1248.0f)};
        _painter.draw(renderer, _arena, _camera);
        const auto mouse = renderer.window_to_logical(ctx.input.mouse_pos());
        if (!_autoplay) {
            const auto p = _camera.world_to_screen(_arena.player);
            renderer.draw_line(p, mouse, Color::rgba(88, 171, 169, 35));
        }
        _hud.render(_arena, renderer, ctx.input, {.paused = _paused, .autoplay = _autoplay, .commands = int(_painter.commands()),
            .display_scale = _display_scale, .reticle = !_autoplay, .aim = mouse}, ctx.dt);
        capture(ctx, _siege->options, ++_frames, _captured);
    }
    void write_report(JsonWriter& json) const override {
        json.field("ticks", _arena.ticks).field("enemies", _arena.enemy_count()).field("kills", _arena.kills)
            .field("cores", _arena.collected).field("health", _arena.health).field("shots", u64(_arena.shots.size()))
            .field("commands", u64(_painter.commands())).field("checksum", _arena.checksum()).field("autoplay", _autoplay);
        json.field("lights", u64(_painter.lights())).field("particles", u64(_painter.particles())).field("lit", _painter.lit());
        json.field("sounds", _siege->audio.stats().played_requests);
        json.field("ui_scale", _display_scale);
        json.field("power_grid", _power_grid).field("available_cores", _arena.available_cores());
    }
private:
    std::shared_ptr<Siege> _siege;
    Arena _arena;
    Camera2D _camera;
    ArenaPainter _painter;
    ArenaHud _hud;
    Vec2i _minimum_size{};
    float _display_scale = 1;
    float _ending = -1; // seconds since the run ended; -1 while it runs
    u64 _seed = 7;
    int _frames = 0;
    PowerGridState _power_state;
    bool _autoplay = false, _from_title = false;
    bool _power_grid = false;
    bool _grid_coordinates = false;
    bool _paused = false, _captured = false;
};

// The title: an autoplaying arena behind a dimmed overlay, the logo rising in,
// and a menu driven by keyboard or mouse.
class TitleScene final : public Scene {
public:
    explicit TitleScene(std::shared_ptr<Siege> siege) : _siege(std::move(siege)), _attract(140, 3) {
        _menu.items = {{.id = "play", .label = "PLAY"}, {.id = "demo", .label = "WATCH AUTOPLAY"}, {.id = "quit", .label = "QUIT"}};
    }
    std::string_view name() const override { return "Signal Siege Title"; }
    void on_enter(SceneContext&) override { _siege->audio.set_drone(true); }
    void update(SceneContext& ctx) override {
        _siege->audio.update(ctx.dt);
        _time += ctx.dt;
        _attract.step(ctx.dt, _attract.autopilot(), true);
        if (_attract.elapsed > 80) _attract.reset(u64(_time * 1000) + 3);
        _painter.update(_attract, ctx.dt);
    }
    void render(SceneContext& ctx) override {
        _display_scale = update_ui_scale(ctx.window, _siege->options, _minimum_size);
        auto& renderer = ctx.renderer;
        renderer.clear(Color::rgb(12, 20, 28));
        if (!_painter.ready()) _painter.init(renderer, _attract);
        _painter.enable_post_process(renderer);
        _camera.viewport = {1280, 800};
        _camera.offset = {std::clamp(_attract.player.x - 640, 0.0f, 1792.0f), std::clamp(_attract.player.y - 400, 0.0f, 1248.0f)};
        _painter.draw(renderer, _attract, _camera);
        renderer.fill_rect({0, 0, 1280, 800}, Color::rgba(4, 8, 14, 150));

        const auto native = renderer.scoped_native_coordinates();
        const auto output = renderer.output_size();
        const float dpi = _display_scale > 0 ? _display_scale : 1;
        _ui.begin(ctx.input, renderer, ctx.dt);
        const Screen screen{renderer, _ui, dpi, output.x / dpi, output.y / dpi};
        const float y0 = screen.h * .22f + 30 * rise(_time);
        const float alpha = std::clamp(_time / .5f, 0.0f, 1.0f);
        screen.centered("SIGNAL SIEGE", y0, 64, with_alpha(teal, alpha));
        screen.centered("Hold the sector for ninety seconds.", y0 + 84, 16, with_alpha(ink, alpha));

        const float menu_w = 280;
        _menu.bounds = screen.px({(screen.w - menu_w) * .5f, y0 + 140, menu_w, 3 * 44.0f});
        _menu.row_height = 40 * dpi;
        _menu.row_spacing = 4 * dpi;
        _menu.text_style = screen.style(18, ink);
        const int before = _menu.selected;
        ui2::run(_ui, _menu);
        if (_menu.selected != before) _siege->audio.play("ui_move");
        if (_menu.activated >= 0) activate(ctx, _menu.items[std::size_t(_menu.activated)].id);
        if (_menu.cancelled) ctx.app.quit();

        if (_siege->best && _siege->best->best().time > 0) {
            const RunResult& record = _siege->best->best();
            const std::string best = std::string{record.won ? "BEST  SECTOR SECURED" : "BEST  " + clock_text(record.time)} +
                "  /  " + std::to_string(record.kills) + " KILLS";
            screen.centered(best, y0 + 300, 13, gold);
        }
        screen.centered("WASD move   mouse aim   hold LMB fire   SPACE dash   U power grid", screen.h - 48, 13, muted);
        _ui.end();
        capture(ctx, _siege->options, ++_frames, _captured);
    }
    void write_report(JsonWriter& json) const override {
        json.field("screen", "title").field("selected", _menu.selected).field("sounds", _siege->audio.stats().played_requests);
    }
private:
    void activate(SceneContext& ctx, std::string_view id) {
        _siege->audio.play("ui_select");
        if (id == "quit") { ctx.app.quit(); return; }
        ctx.scenes.push(transition_to(std::make_unique<ArenaScene>(_siege, id == "demo", true), TransitionKind::Iris, .7f));
    }
    std::shared_ptr<Siege> _siege;
    Arena _attract;
    ArenaPainter _painter;
    Camera2D _camera;
    ui2::Context _ui;
    ui2::MenuList _menu{.id = ui2::make_id("title-menu")};
    Vec2i _minimum_size{};
    float _display_scale = 1, _time = 0;
    int _frames = 0;
    bool _captured = false;
};

// The run's outcome, with counters rolling up; Enter retries, Escape returns.
class ResultsScene final : public Scene {
public:
    ResultsScene(std::shared_ptr<Siege> siege, RunResult result) : _siege(std::move(siege)), _result(result) {}
    std::string_view name() const override { return "Signal Siege Results"; }
    void on_enter(SceneContext& ctx) override {
        _new_best = _siege->best && _siege->best->record(_result);
        _siege->audio.set_drone(true);
        ctx.renderer.clear_post_process();
        _rows = {{{.value = _result.time, .speed = 60}, {.value = float(_result.kills), .speed = 160},
                  {.value = float(_result.cores), .speed = 60}, {.value = float(_result.upgrades), .speed = 12}}};
    }
    void update(SceneContext& ctx) override {
        _siege->audio.update(ctx.dt);
        _time += ctx.dt;
        if (_leaving) return;
        if (ctx.input.pressed("accept") || ctx.input.pressed("reset")) retry(ctx);
        else if (ctx.input.pressed("quit")) title(ctx);
    }
    void render(SceneContext& ctx) override {
        const float scale = update_ui_scale(ctx.window, _siege->options, _minimum_size);
        auto& renderer = ctx.renderer;
        const auto native = renderer.scoped_native_coordinates();
        const auto output = renderer.output_size();
        renderer.clear(Color::rgb(9, 14, 21));
        _ui.begin(ctx.input, renderer, ctx.dt);
        const float dpi = scale > 0 ? scale : 1;
        const Screen screen{renderer, _ui, dpi, output.x / dpi, output.y / dpi};
        for (float x = 0; x < screen.w; x += 48) renderer.fill_rect(screen.px({x, 0, 1, screen.h}), Color::rgb(16, 24, 33));
        for (float y = 0; y < screen.h; y += 48) renderer.fill_rect(screen.px({0, y, screen.w, 1}), Color::rgb(16, 24, 33));

        const float y0 = screen.h * .2f + 24 * rise(_time);
        const float alpha = std::clamp(_time / .4f, 0.0f, 1.0f);
        screen.centered(_result.won ? "SECTOR SECURED" : "SIGNAL LOST", y0, 48,
                        with_alpha(_result.won ? teal : Color::rgb(240, 120, 110), alpha));
        if (_new_best) screen.centered("NEW BEST RUN", y0 + 66, 14, with_alpha(gold, .6f + .4f * std::sin(_time * 4)));

        constexpr std::array<std::string_view, 4> labels{"TIME SURVIVED", "KILLS", "CORES COLLECTED", "UPGRADES INSTALLED"};
        const float left = screen.w * .5f - 200;
        for (std::size_t i = 0; i < _rows.size(); ++i) {
            ui2::AnimatedValue& row = _rows[i];
            if (_time > .5f + .25f * float(i)) ui2::step_animated_value(row, ctx.dt);
            const float y = y0 + 110 + 44 * float(i);
            screen.text(labels[i], left, y + 6, 14, muted);
            const std::string value = i == 0 ? clock_text(row.display) : std::to_string(int(std::lround(row.display)));
            screen.text(value, left + 400 - screen.width(value, 24), y, 24, ink);
        }

        const float button_y = y0 + 310;
        const auto button = [&](std::string_view id, std::string label, float x) {
            ui2::Button b{.id = ui2::make_id(id), .bounds = screen.px({x, button_y, 180, 44}), .label = std::move(label),
                          .text_style = screen.style(15, ink)};
            ui2::run(_ui, b);
            return b.clicked;
        };
        const bool retry_clicked = button("results-retry", "RETRY  [ENTER]", screen.w * .5f - 190);
        const bool title_clicked = button("results-title", "TITLE  [ESC]", screen.w * .5f + 10);
        _ui.end();
        if (!_leaving && retry_clicked) retry(ctx);
        if (!_leaving && title_clicked) title(ctx);
        capture(ctx, _siege->options, ++_frames, _captured);
    }
    void write_report(JsonWriter& json) const override {
        json.field("screen", "results").field("won", _result.won).field("kills", _result.kills).field("new_best", _new_best);
    }
private:
    void retry(SceneContext& ctx) {
        _leaving = true;
        _siege->audio.play("ui_select");
        ctx.scenes.push(transition_to(std::make_unique<ArenaScene>(_siege, false, true), TransitionKind::Wipe, .6f));
    }
    void title(SceneContext& ctx) {
        _leaving = true;
        _siege->audio.play("ui_select");
        ctx.scenes.push(transition_to(make_title(_siege), TransitionKind::Crossfade, .6f));
    }
    std::shared_ptr<Siege> _siege;
    RunResult _result;
    ui2::Context _ui;
    std::array<ui2::AnimatedValue, 4> _rows{};
    Vec2i _minimum_size{};
    float _time = 0;
    int _frames = 0;
    bool _new_best = false, _leaving = false, _captured = false;
};

std::unique_ptr<Scene> make_title(std::shared_ptr<Siege> siege) { return std::make_unique<TitleScene>(std::move(siege)); }
std::unique_ptr<Scene> make_results(std::shared_ptr<Siege> siege, RunResult result) {
    return std::make_unique<ResultsScene>(std::move(siege), result);
}

} // namespace

BestRunStore::BestRunStore(std::filesystem::path root)
    : _saves(std::make_unique<SaveStore>(SaveStoreConfig{.game_id = "signal_siege", .game_version = "0.2", .root_override = std::move(root)})) {
    const SaveLoadResult loaded = _saves->read_settings();
    if (loaded.result.ok) {
        _best.time = static_cast<float>(loaded.payload.number_at("best_time", 0));
        _best.kills = static_cast<int>(loaded.payload.int_at("best_kills", 0));
        _best.won = loaded.payload.bool_at("best_won", false);
    }
}

bool BestRunStore::record(const RunResult& run) {
    const bool better = (run.won && !_best.won) || (run.won == _best.won &&
        (run.time > _best.time + .01f || (std::abs(run.time - _best.time) <= .01f && run.kills > _best.kills)));
    if (!better) return false;
    _best = run;
    _saves->write_settings([&](JsonWriter& json) {
        json.begin_object()
            .field("best_time", static_cast<f64>(_best.time)).field("best_kills", _best.kills).field("best_won", _best.won)
            .end_object();
    });
    return true;
}

std::unique_ptr<Scene> make_signal_siege(const Options& options, bool start_in_arena, bool tool_run) {
    auto siege = std::make_shared<Siege>(options, tool_run);
    if (start_in_arena) return std::make_unique<ArenaScene>(std::move(siege), false, false);
    return make_title(std::move(siege));
}

} // namespace examples
