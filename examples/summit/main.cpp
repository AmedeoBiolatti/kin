// Summit: a one-player Peak-like on kin's experimental 3D renderer and physics (docs/3d_plan.md, P4). The simulation is
// summit_sim (level, game, input mapper, camera, bot); this file is its view: the window, the scene, the debug line and the
// scripted run with its pictures. See examples/README.md for the controls and the flags.

#include "camera.hpp"
#include "game.hpp"
#include "input_map.hpp"
#include "level.hpp"
#include "script.hpp"
#include "view.hpp"

#include <kin/platform/gamepad.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/widgets.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace summit {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr int exit_skipped = 77; // CTest's SKIP_RETURN_CODE

struct Options {
    u64 seed = 1;
    int width = 1280, height = 720;
    std::string look = "afternoon";
    std::string script;      // climb | fall | rope
    std::string screenshot;  // a file (a picture after --frames) or, for a script, the prefix of the key pictures
    std::string record;      // write the scripted run's command log here
    std::string replay;      // play a command log (CPU)
    std::string expect_hash; // fail unless the final state hash is this (hex)
    i64 expect_steps = -1;   // fail unless the run took this many steps
    int max_steps = 60000;
    int samples = 4;
    bool first_person = false;
    bool hud = true;
    bool timing = false;
    bool trace = false;
    bool skip_if_unavailable = false;
    bool help = false;
    int frames = 0;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 == argc) throw std::invalid_argument(arg + " needs a value");
            return argv[++i];
        };
        const auto number = [&](const std::string& text) -> double {
            try {
                return std::stod(text);
            } catch (const std::exception&) {
                throw std::invalid_argument(arg + " needs a number");
            }
        };
        if (arg == "--seed") o.seed = static_cast<u64>(number(value()));
        else if (arg == "--width") o.width = std::clamp(static_cast<int>(number(value())), 160, 4096);
        else if (arg == "--height") o.height = std::clamp(static_cast<int>(number(value())), 90, 4096);
        else if (arg == "--look" || arg == "--preset") o.look = value();
        else if (arg == "--script") o.script = value();
        else if (arg == "--screenshot") o.screenshot = value();
        else if (arg.starts_with("--screenshot=")) o.screenshot = arg.substr(13);
        else if (arg == "--record") o.record = value();
        else if (arg == "--replay") o.replay = value();
        else if (arg == "--expect-hash") o.expect_hash = value();
        else if (arg == "--expect-steps") o.expect_steps = static_cast<i64>(number(value()));
        else if (arg == "--max-steps") o.max_steps = std::clamp(static_cast<int>(number(value())), 1, 5'000'000);
        else if (arg == "--samples") o.samples = std::clamp(static_cast<int>(number(value())), 1, 8);
        else if (arg == "--first-person") o.first_person = true;
        else if (arg == "--no-hud") o.hud = false;
        else if (arg == "--timing") o.timing = true;
        else if (arg == "--trace") o.trace = true;
        else if (arg == "--skip-if-unavailable") o.skip_if_unavailable = true;
        else if (arg == "--help") o.help = true;
    }
    if (!find_look(o.look)) throw std::invalid_argument("no look named " + o.look + " (--help lists them)");
    return o;
}

void print_help() {
    std::cout << "summit: climb a procedural mountain on kin's experimental 3D renderer and physics.\n"
                 "  --seed N              the mountain (default 1)\n"
                 "  --look NAME           the sky and air:";
    for (const Look& l : looks()) std::cout << ' ' << l.name;
    std::cout << "\n  --width N --height N  window or picture size (1280x720)    --samples N   MSAA (4)    --first-person   --no-hud\n"
                 "  --script NAME         a scripted run by the route bot: climb (to the summit), fall (lets go once: a knock-out and a\n"
                 "                        get-up on the way), rope (stops over the rope). Without --screenshot it is a CPU run with no\n"
                 "                        window or GPU; it prints the steps, the state hash and the wall time\n"
                 "  --screenshot PATH     with --script: PATH_<moment>.png at key moments (needs KIN_RENDER_BACKEND=gpu; lavapipe is\n"
                 "                        enough). Otherwise a picture after --frames (headless) or after 30 frames (window)\n"
                 "  --record FILE         write the scripted run's command log        --replay FILE   play a log (CPU) and print its hash\n"
                 "  --expect-hash HEX --expect-steps N   exit 1 unless the scripted run ends so (same machine determinism)\n"
                 "  --max-steps N         the scripted run's bound (60000)            --timing   per climb leg: steps and lowest stamina\n"
                 "  --trace               a CPU script prints the bot's state every 20 steps\n"
                 "  --headless --frames N no window: N fixed steps (from the bot with --script)\n"
                 "  --skip-if-unavailable exit 77 (CTest: skipped) without a 3D renderer\n"
                 "Keys: WASD move, mouse look (click to capture, Esc to release), Shift sprint, Space jump / jump off, left and right mouse\n"
                 "      (or Q and E) the hands, F pull up, X let go, R rest at a camp, V first person, F2 next look, F3 hud, F12 or P picture.\n"
                 "Gamepad: left stick move, right stick look, A jump, LT and RT the hands, X pull up, B let go, Y rest, L3 sprint.\n";
}

std::string hex(u64 v) {
    char b[32];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

// ---- the CPU run -----------------------------------------------------------------------------------------------------------

struct CpuResult {
    RunReport report;
    double seconds = 0.0;
    bool ok = false;
};

int finish_report(const Options& o, const CpuResult& r, const Game& game) {
    const double game_seconds = double(r.report.steps) / 60.0;
    std::printf("summit: seed %llu, script %s: %s after %llu steps (%.1f s of game time), legs done %d\n", (unsigned long long)o.seed,
                o.script.c_str(), r.report.summit ? "the summit" : "stopped", (unsigned long long)r.report.steps, game_seconds, r.report.legs_done);
    std::printf("summit: falls %d, knock-outs %d, deaths %d, highest %.1f m, lowest stamina %.1f, camp %d\n", r.report.falls, r.report.stuns,
                r.report.deaths, double(r.report.highest), double(r.report.min_stamina), game.player(0).camp);
    std::printf("summit: final state hash %s, step %llu, wall time %.3f s (%.0f steps a second)\n", hex(r.report.final_hash).c_str(),
                (unsigned long long)game.step_count(), r.seconds, r.seconds > 0 ? double(r.report.steps) / r.seconds : 0.0);
    bool ok = r.ok && !r.report.non_finite;
    if (r.report.non_finite) std::fprintf(stderr, "summit: a non-finite number was found\n");
    if (!o.expect_hash.empty() && o.expect_hash != hex(r.report.final_hash)) {
        std::fprintf(stderr, "summit: the hash is %s, not the expected %s\n", hex(r.report.final_hash).c_str(), o.expect_hash.c_str());
        ok = false;
    }
    if (o.expect_steps >= 0 && i64(r.report.steps) != o.expect_steps) {
        std::fprintf(stderr, "summit: %llu steps, not the expected %lld\n", (unsigned long long)r.report.steps, (long long)o.expect_steps);
        ok = false;
    }
    return ok ? 0 : 1;
}

int run_cpu_script(const Options& o) {
    Game game(o.seed);
    BotConfig config;
    if (!script_config(o.script, game.level(), config)) {
        std::fprintf(stderr, "summit: no script named %s (climb, fall, rope)\n", o.script.c_str());
        return 2;
    }
    Bot bot(game, 0, config);
    CpuResult r;
    const auto start = Clock::now();
    // The bot's own end (the rope script) or the summit.
    std::vector<PlayerCommand> log;
    RunReport rep;
    i32 leg_now = bot.leg();
    u64 leg_start = 0;
    f32 leg_low = 1e9f;
    for (u64 s = 0; s < u64(o.max_steps) && !bot.finished() && !game.player(0).summit; ++s) {
        const PlayerCommand c = bot.next();
        log.push_back(c);
        game.step({&c, 1});
        if (o.trace && s % 20 == 0) {
            const Vec3f f = game.feet(0);
            const TraversalState3D ts = game.traversal(0);
            std::printf("summit: %6llu %-52s feet (%.1f %.1f %.1f) stamina %.0f held %u\n", (unsigned long long)s, bot.status().c_str(), double(f.x),
                        double(f.y), double(f.z), double(ts.resource.value), ts.held);
            if (bot.status().find("handover") != std::string::npos) {
                // What the left hand could take hold of from where it looks.
                GripProbeDesc3D pd;
                pd.origin = f + Vec3f{0.0f, 1.65f, 0.0f};
                pd.direction = game.look_direction(0);
                pd.reach = 0.95f;
                pd.clearance_height = 1.95f;
                pd.clearance_radius = 0.3f;
                const std::vector<GripCandidate3D> c = game.world().probe_grips(pd);
                std::printf("summit:        look yaw %.0f pitch %.0f: %zu candidates", double(game.player(0).yaw), double(game.player(0).pitch), c.size());
                for (const GripCandidate3D& k : c) std::printf(" [kind %d y %.2f ledge %d clear %d]", int(k.kind), double(k.point.y), int(k.ledge), int(k.clear));
                std::printf("\n");
            }
        }
        if (o.timing) {
            leg_low = std::min(leg_low, game.stamina(0));
            if (bot.leg() != leg_now) {
                const Leg& l = game.level().route[std::size_t(leg_now)];
                if (l.kind != LegKind::Walk && l.kind != LegKind::Rest) {
                    std::printf("summit:   leg %2d %-22s %5llu steps, lowest stamina %5.1f\n", leg_now, l.label.c_str(),
                                (unsigned long long)(s - leg_start), double(leg_low));
                }
                leg_now = bot.leg();
                leg_start = s;
                leg_low = 1e9f;
            }
        }
        const Vec3f f = game.feet(0);
        if (!std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.z)) rep.non_finite = true;
        rep.min_stamina = std::min(rep.min_stamina, game.stamina(0));
        rep.highest = std::max(rep.highest, f.y - game.level().start.y);
    }
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    rep.steps = log.size();
    rep.summit = game.player(0).summit;
    rep.summit_step = game.player(0).summit_step;
    rep.falls = game.player(0).falls;
    rep.stuns = game.player(0).stuns;
    rep.deaths = game.player(0).deaths;
    rep.final_hash = game.state_hash();
    rep.legs_done = bot.leg();
    r.report = std::move(rep);
    r.ok = r.report.summit || bot.finished();
    if (!o.record.empty() && !write_command_log(o.record, o.seed, log)) {
        std::fprintf(stderr, "summit: could not write %s\n", o.record.c_str());
        return 1;
    }
    return finish_report(o, r, game);
}

int run_cpu_replay(const Options& o) {
    u64 seed = 0;
    std::vector<PlayerCommand> log;
    if (!read_command_log(o.replay, seed, log)) {
        std::fprintf(stderr, "summit: %s is not a command log\n", o.replay.c_str());
        return 1;
    }
    Game game(seed);
    const auto start = Clock::now();
    CpuResult r;
    r.report.final_hash = replay(game, log);
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.report.steps = log.size();
    r.report.summit = game.player(0).summit;
    r.report.summit_step = game.player(0).summit_step;
    r.report.falls = game.player(0).falls;
    r.report.stuns = game.player(0).stuns;
    r.report.deaths = game.player(0).deaths;
    r.ok = true;
    Options shown = o;
    shown.seed = seed;
    shown.script = "replay";
    return finish_report(shown, r, game);
}

// ---- the scene -----------------------------------------------------------------------------------------------------------------

// A picture of a moment of a scripted run: when the bot's status has `status` in it and `phase` is its sub-state, `delay` steps
// later.
struct ShotPlan {
    std::string name;
    std::string status;
    std::string phase;
    int delay = 60;
    int countdown = -1;
    bool done = false;
};

class SummitScene final : public Scene {
public:
    explicit SummitScene(Options options) : _options(std::move(options)) {
        _game = std::make_unique<Game>(_options.seed);
        _bindings = InputBindings::defaults();
        _rig.settings.first_person = _options.first_person;
        if (!_options.script.empty()) {
            BotConfig config;
            if (!script_config(_options.script, _game->level(), config)) throw std::invalid_argument("no script named " + _options.script);
            _bot = std::make_unique<Bot>(*_game, 0, config);
            _shots = {{"base", "", "", 45},
                      {"first_wall", "the first wall", "climb", 90},
                      {"second_pitch", "second pitch", "climb", 120},
                      {"chimney", "the chimney", "climb", 100},
                      {"overhang", "the overhang", "shimmy", 100},
                      {"rope", "the rope", "rope", 200},
                      {"summit", "the summit", "", 40}};
            if (_options.script == "fall") _shots.push_back({"ragdoll", "", "ragdoll", 40});
        }
        _rig.observe(*_game, 0);
    }
    ~SummitScene() override { release_mouse(); }

    std::string_view name() const override { return "Summit"; }

    void update(SceneContext& ctx) override {
        if (_quit || _failed) return;
        PlayerCommand c;
        if (_bot) {
            if (_bot->finished() || _game->player(0).summit) {
                // The run is over: a few more steps for a last picture, then the app ends.
                ++_after;
                if (_after > 120 && _pending_shot < 0) _quit = true;
            } else {
                c = _bot->next();
            }
        } else {
            const bool use = _captured || ctx.app.headless();
            if (use) {
                if (_pad && !ctx.app.headless()) _pad_state = _pad->poll();
                c = map_input(read_raw_input(ctx.input, &_pad_state), _bindings, fixed_step);
            }
        }
        _game->step({&c, 1});
        _rig.observe(*_game, 0);
        ++_updates;
        if (_bot && !_options.screenshot.empty()) plan_shots();
        if (_bot && _updates >= _options.max_steps) _quit = true;
    }

    void render(SceneContext& ctx) override {
        if (_quit) {
            ctx.app.quit();
            return;
        }
        if (_failed) return;
        const bool headless = ctx.app.headless();
        const bool scripted_shots = _bot && !_options.screenshot.empty();
        Renderer2D& renderer = ctx.renderer;
        if (!_r3d && !init(ctx)) return;
        handle_keys(ctx, headless);
        const auto now = Clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - _last).count(), 0.0001f, 0.1f);
        _last = now;
        const auto native = renderer.scoped_native_coordinates();
        const Vec2i out = renderer.output_size();
        if (!_view.valid()) {
            _view = _r3d->create_view({std::max(1, out.x), std::max(1, out.y)}, _summit_view->view_options(u32(_options.samples)));
            _view_size = out;
        } else if (out != _view_size) {
            _r3d->resize(_view, {std::max(1, out.x), std::max(1, out.y)});
            _view_size = out;
        }
        const f32 alpha = headless ? 1.0f : std::clamp(ctx.alpha, 0.0f, 1.0f);
        const CameraView cam = _rig.frame(_game->world(), alpha, dt);
        // The person's clips and IK advance with every frame (a scripted run's frames are steps), but a scripted run draws only
        // the frames a picture is wanted of.
        bool wanted = !scripted_shots || _pending_shot >= 0;
        for (const ShotPlan& sp : _shots) wanted = wanted || (!sp.done && sp.countdown >= 0);
        if (!wanted) return;
        _summit_view->fill(_scene, *_game, cam, headless ? fixed_step : dt, 0);
        if (scripted_shots && _pending_shot < 0) return;
        const auto t0 = Clock::now();
        _r3d->render(_view, _scene);
        _render_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        renderer.clear(Color::rgb(0, 0, 0));
        renderer.draw_texture(_view.texture(), {0.0f, 0.0f, float(out.x), float(out.y)});
        ++_frames;
        if (_options.hud) draw_hud(ctx, out, dt);
        take_pictures(ctx, out, headless);
    }

private:
    bool init(SceneContext& ctx) {
        _r3d = std::make_unique<Renderer3D>(ctx.renderer);
        if (!_r3d->available()) {
            std::fprintf(stderr, "summit: this renderer cannot draw 3D: it needs SDL_GPU (KIN_RENDER_BACKEND=gpu, a Vulkan/Metal/D3D12 device)\n");
            if (ctx.report) ctx.report->fail("no 3D renderer");
            _failed = true;
            _unavailable = true;
            ctx.app.quit();
            _r3d.reset();
            return false;
        }
        _summit_view = std::make_unique<SummitView>(*_r3d, _game->level());
        _summit_view->set_look(_options.look);
        if (!ctx.app.headless()) _pad = std::make_unique<Gamepads>();
        _last = Clock::now();
        return true;
    }

    void handle_keys(SceneContext& ctx, bool headless) {
        const Input& in = ctx.input;
        if (in.frame_pressed(Key::Escape)) {
            if (_captured) release_mouse();
            else if (!headless) _quit = true;
        }
        if (!headless && !_captured && in.mouse_frame_pressed(MouseButton::Left)) capture_mouse(ctx.window);
        if (in.frame_pressed(Key::V)) _rig.settings.first_person = !_rig.settings.first_person;
        if (in.frame_pressed(Key::F3)) _options.hud = !_options.hud;
        if (in.frame_pressed(Key::F2)) {
            const std::vector<Look>& all = looks();
            std::size_t i = 0;
            for (; i < all.size(); ++i) {
                if (&all[i] == &_summit_view->look()) break;
            }
            const Look& next = all[(i + 1) % all.size()];
            _summit_view->set_look(next.name);
            _view = {}; // the view's HDR options follow the look
        }
        if (in.frame_pressed(Key::F12) || in.frame_pressed(Key::P)) _picture_requested = true;
    }

    void capture_mouse(Window& window) {
        window.set_relative_mouse(true);
        _window = &window;
        _captured = true;
    }
    void release_mouse() {
        if (_window != nullptr && _captured) _window->set_relative_mouse(false);
        _captured = false;
    }

    void plan_shots() {
        for (ShotPlan& s : _shots) {
            if (s.done || _pending_shot >= 0) continue;
            if (s.countdown < 0) {
                bool hit = false;
                if (s.name == "base") {
                    hit = _updates >= 5;
                } else if (s.name == "summit") {
                    hit = _game->player(0).summit;
                } else if (s.phase == "ragdoll") {
                    hit = _game->player(0).state == PlayerState::Stunned;
                } else {
                    const std::string status = _bot->status();
                    hit = status.find(s.status) != std::string::npos && s.phase == _bot->phase();
                }
                if (hit) s.countdown = s.delay;
            } else if (--s.countdown <= 0) {
                _pending_shot = int(&s - _shots.data());
            }
        }
    }

    void take_pictures(SceneContext& ctx, Vec2i out, bool headless) {
        if (_picture_requested) {
            _picture_requested = false;
            for (int n = 1;; ++n) {
                char name[40];
                std::snprintf(name, sizeof name, "summit_%03d.png", n);
                if (!fs::exists(name)) {
                    std::printf("summit: %s %s\n", ctx.renderer.save_png(name) ? "saved" : "could not save", name);
                    break;
                }
            }
        }
        if (_pending_shot >= 0) {
            ShotPlan& s = _shots[std::size_t(_pending_shot)];
            _pending_shot = -1;
            s.done = true;
            const std::string path = _options.screenshot + "_" + s.name + ".png";
            const fs::path p{path};
            if (!p.parent_path().empty()) fs::create_directories(p.parent_path());
            std::vector<u8> pixels;
            Vec2i size{};
            ctx.renderer.read_rgba({0.0f, 0.0f, float(out.x), float(out.y)}, pixels, size);
            double lum = 0.0;
            std::size_t n = std::size_t(size.x) * std::size_t(size.y);
            for (std::size_t i = 0; i + 3 < pixels.size() && i / 4 < n; i += 4) lum += 0.2126 * pixels[i] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2];
            const bool ok = ctx.renderer.save_png(path);
            std::printf("summit: %s %s (%dx%d, mean luminance %.1f, step %d, 3D render %.1f ms)\n", ok ? "saved" : "could not save", path.c_str(), size.x,
                        size.y, n ? lum / double(n) : 0.0, _updates, _render_ms);
            if (!ok) {
                _failed = true;
                if (ctx.report) ctx.report->fail("could not save a picture");
            }
        } else if (!_bot && !_options.screenshot.empty() && !_single_done && _frames >= std::max(1, std::max(_options.frames, headless ? 1 : 30))) {
            _single_done = true;
            const fs::path p{_options.screenshot};
            if (!p.parent_path().empty()) fs::create_directories(p.parent_path());
            if (!ctx.renderer.save_png(_options.screenshot)) {
                _failed = true;
                if (ctx.report) ctx.report->fail("could not save the picture");
            } else {
                std::printf("summit: saved %s\n", _options.screenshot.c_str());
            }
            ctx.app.quit();
        }
    }

    // The debug line: altitude, stamina, state (a HUD is P4b's).
    void draw_hud(SceneContext& ctx, Vec2i out, float dt) {
        (void)dt;
        const Player& p = _game->player(0);
        const TraversalState3D ts = _game->traversal(0);
        const char* state = "walking";
        switch (p.state) {
        case PlayerState::Stunned: state = "knocked out"; break;
        case PlayerState::GettingUp: state = "getting up"; break;
        case PlayerState::Dead: state = "dead"; break;
        case PlayerState::Alive:
            if (ts.mode == TraversalMode3D::Hanging) state = ts.held >= 2 ? "hanging (2 hands)" : "hanging (1 hand)";
            else if (ts.mode == TraversalMode3D::Mantling) state = "pulling up";
            else if (_game->character_state(0).ground != CharacterGround3D::OnGround) state = "in the air";
            break;
        }
        char line[200];
        std::snprintf(line, sizeof line, "alt %5.1f m   stamina %3.0f / %3.0f%s   health %3.0f   %s   camp %d   step %llu%s", double(_game->altitude(0)),
                      double(ts.resource.value), double(ts.resource.max), ts.resource.exhausted ? " (spent)" : "", double(p.health), state, p.camp,
                      (unsigned long long)_game->step_count(), p.summit ? "   SUMMIT" : "");
        const float u = std::max(0.75f, std::min(2.0f, float(out.x) / 1280.0f));
        _ui.begin(ctx.input, ctx.renderer, 0.0f);
        const ui2::TextStyle st{.font = ui2::system_ui_font(13.0f * u), .scale = 1.0f, .color = Color::rgb(236, 240, 244)};
        const Vec2f size = ui2::measure_text(st.font, line, st.scale);
        ui2::Panel back{.bounds = {8 * u, 8 * u, size.x + 16 * u, size.y + 10 * u}, .color = Color::rgba(8, 11, 16, 190), .border = Color::rgb(60, 72, 88)};
        ui2::run(_ui, back);
        _ui.text(line, {16 * u, 13 * u}, st);
        _ui.end();
    }

public:
    bool failed() const { return _failed; }
    bool unavailable() const { return _unavailable; }

private:
    Options _options;
    std::unique_ptr<Game> _game;
    std::unique_ptr<Bot> _bot;
    InputBindings _bindings;
    CameraRig _rig;
    std::unique_ptr<Gamepads> _pad;
    GamepadState _pad_state;
    std::unique_ptr<Renderer3D> _r3d;
    std::unique_ptr<SummitView> _summit_view;
    View3D _view;
    Vec2i _view_size{};
    Scene3D _scene;
    ui2::Context _ui;
    std::vector<ShotPlan> _shots;
    int _pending_shot = -1;
    int _updates = 0, _frames = 0, _after = 0;
    double _render_ms = 0.0;
    bool _captured = false, _quit = false, _failed = false, _unavailable = false, _picture_requested = false, _single_done = false;
    Window* _window = nullptr;
    Clock::time_point _last = Clock::now();
};

int run(int argc, char** argv) {
    try {
        Options options = parse(argc, argv);
        if (options.help) {
            print_help();
            return 0;
        }
        if (!options.replay.empty()) return run_cpu_replay(options);
        if (!options.script.empty() && options.screenshot.empty()) return run_cpu_script(options);

        // --screenshot is ours (the picture without the app's capture): the runtime must not see it.
        std::vector<char*> kept;
        for (int i = 0; i < argc; ++i) {
            const std::string_view a = argv[i];
            if (a == "--screenshot" || a == "--seed" || a == "--width" || a == "--height" || a == "--look" || a == "--preset" || a == "--script" ||
                a == "--record" || a == "--replay" || a == "--expect-hash" || a == "--expect-steps" || a == "--max-steps" || a == "--samples") {
                ++i;
                continue;
            }
            if (a.starts_with("--screenshot=")) continue;
            kept.push_back(argv[i]);
        }
        auto headless = parse_headless_options(int(kept.size()), kept.data());
        options.frames = headless.frames;
        if (!options.script.empty() && headless.frames == 0) {
            // A script is as long as it is: the scene ends the app itself.
            headless.frames = options.max_steps;
        }
        if (options.script.empty() && !options.screenshot.empty() && headless.enabled && headless.frames == 0) {
            options.frames = headless.frames = 30;
        }
        InputMap input;
        GameInfo game{.id = "summit", .title = "Summit", .version = "0.1",
                      .description = "A one-player climbing game on kin's experimental 3D renderer and physics",
                      .window = {.width = options.width, .height = options.height, .resizable = true},
                      .tags = {"example", "3d"}, .input_map = input};
        auto window = window_config(game);
        window.vsync = false;
        window.max_fps = 0.0f;
        window.high_pixel_density = !headless.enabled;
        SceneManager scenes;
        SummitScene* scene = nullptr;
        {
            auto s = std::make_unique<SummitScene>(options);
            scene = s.get();
            scenes.push(std::move(s));
        }
        const int code = run_scene_app({.window = window, .headless = headless, .game = &game, .render_headless = true}, scenes);
        if (scene != nullptr && scene->unavailable() && options.skip_if_unavailable) return exit_skipped;
        return code;
    } catch (const std::exception& e) {
        std::cerr << "summit: " << e.what() << '\n';
        return 1;
    }
}

} // namespace
} // namespace summit

int main(int argc, char** argv) { return summit::run(argc, argv); }
