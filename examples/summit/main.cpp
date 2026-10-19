// Summit: a one-player Peak-like on kin's experimental 3D renderer and physics (docs/3d_plan.md, P4). The simulation is
// summit_sim (level, game, input mapper, camera, bot); this file is its view: the window, the scene, the debug line and the
// scripted run with its pictures. See examples/README.md for the controls and the flags.

#include "audio.hpp"
#include "benchmark.hpp"
#include "camera.hpp"
#include "game.hpp"
#include "hud.hpp"
#include "hud_draw.hpp"
#include "input_map.hpp"
#include "level.hpp"
#include "script.hpp"
#include "settings.hpp"
#include "view.hpp"

#include <kin/assets/content.hpp>
#include <kin/platform/gamepad.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/widgets.hpp>

#include <algorithm>
#include <array>
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
    bool headless = false;
    bool look_given = false;
    bool mute = false;
    bool no_settings = false;
    std::string settings_dir;  // load and save the settings here (the platform's directory when empty and settings are used)
    bool storm = false;
    bool freeze_time = false;
    f32 time_speed = -1.0f;     // game hours a minute (the settings' when negative)
    f32 hours = -1.0f;          // the look's time of day, overridden
    std::string fx_off;         // comma separated effect names to switch off
    std::string fx_on;          // ... or on (the storm, which is off by default)
    int camp = -1;              // a picture (no script): start at this camp
    f32 drop = 0.0f;            // ... and this many metres up (a landing, for a picture)
    Vec3f orbit{0.0f, 0.0f, 0.0f}; // yaw, pitch, distance of a free orbit round the player (distance 0: off)
    std::string on_decor;         // a picture: stand on the top of a decor chunk, "boulder:N", "buttress:N" or "scree:N" (N-th of its kind)
    bool debug_colliders = false; // draw the colliders (wireframes) near the camera
    bool timing = false;
    bool gpu_timing = false;
    bool trace = false;
    bool skip_if_unavailable = false;
    bool help = false;
    bool classic = false;
    bool procedural_assets = false;
    bool expedition = false;
    bool dense_scenery = true;
    std::string benchmark;
    f64 benchmark_warmup = 2.0;
    Clock::time_point started = Clock::now();
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
        else if (arg == "--look" || arg == "--preset") o.look = value(), o.look_given = true;
        else if (arg == "--script") o.script = value();
        else if (arg == "--benchmark") o.benchmark = value();
        else if (arg == "--benchmark-warmup") o.benchmark_warmup = std::clamp(number(value()),0.0,30.0);
        else if (arg == "--scenery") {
            const auto detail = value();
            if (detail != "compact" && detail != "dense") throw std::invalid_argument("--scenery needs compact or dense");
            o.dense_scenery = detail == "dense";
        }
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
        else if (arg == "--mute") o.mute = true;
        else if (arg == "--no-settings") o.no_settings = true;
        else if (arg == "--settings") o.settings_dir = value();
        else if (arg == "--storm") o.storm = true;
        else if (arg == "--freeze-time") o.freeze_time = true;
        else if (arg == "--time-speed") o.time_speed = static_cast<f32>(number(value()));
        else if (arg == "--hours") o.hours = static_cast<f32>(number(value()));
        else if (arg == "--no-fx") o.fx_off = value();
        else if (arg == "--fx") o.fx_on = value();
        else if (arg == "--camp") o.camp = static_cast<int>(number(value()));
        else if (arg == "--drop") o.drop = static_cast<f32>(number(value()));
        else if (arg == "--orbit") {
            const std::string v = value();
            float a = 0, b = 0, c = 0;
            if (std::sscanf(v.c_str(), "%f,%f,%f", &a, &b, &c) != 3) throw std::invalid_argument("--orbit needs yaw,pitch,distance");
            o.orbit = {a, b, c};
        }
        else if (arg == "--debug-colliders") o.debug_colliders = true;
        else if (arg == "--on-decor") o.on_decor = value();
        else if (arg == "--timing") o.timing = true;
        else if (arg == "--gpu-timing") o.gpu_timing = true;
        else if (arg == "--trace") o.trace = true;
        else if (arg == "--skip-if-unavailable") o.skip_if_unavailable = true;
        else if (arg == "--help") o.help = true;
        else if (arg == "--classic") o.classic = true;
        else if (arg == "--procedural-assets") o.procedural_assets = true;
        else if (arg == "--expedition") o.expedition = true;
    }
    if (!o.benchmark.empty()) {
        if (o.script.empty()) o.script = "shelter";
        o.expedition = true; o.no_settings = true; o.mute = true;
        if (!o.screenshot.empty()) throw std::invalid_argument("benchmark captures every frame; take screenshots in a separate run");
    }
    if (!find_look(o.look)) throw std::invalid_argument("no look named " + o.look + " (--help lists them)");
    if (o.classic && o.expedition) throw std::invalid_argument("choose --classic or --expedition");
    return o;
}

bool expedition_mode(const Options& o) {
    return !o.classic && (o.expedition || o.script == "expedition" || o.script == "shelter" ||
                         (o.script.empty() && o.camp < 0 && o.on_decor.empty()));
}

void print_help() {
    std::cout << "summit: a beacon expedition on kin's experimental 3D renderer and physics.\n"
                 "  Interactive default: begin at base, choose orange climb / blue cave, activate the beacon, request extraction.\n"
                 "  --classic             the original procedural 150 m climbing course\n"
                 "  --scenery dense|compact  forested expedition (default dense) or the earlier sparse layout\n"
                 "  --benchmark CSV       real-time bot traversal with streamed full-frame metrics (windowed only)\n"
                 "  --benchmark-warmup S   render at base for S seconds before starting the bot (2)\n"
                 "  --procedural-assets   use the original camp props and omit imported scenery\n"
                 "  --expedition          select the expedition explicitly (including --camp diagnostic pictures)\n"
                 "  --seed N              the mountain (default 1)\n"
                 "  --look NAME           the sky and air:";
    for (const Look& l : looks()) std::cout << ' ' << l.name;
    std::cout << "\n  --width N --height N  window or picture size (1280x720)    --samples N   MSAA (4)    --first-person   --no-hud\n"
                 "  --script NAME         a scripted run by the route bot: climb (to the summit), fall (lets go once: a knock-out and a\n"
                 "                        get-up on the way), rope (stops over the rope). Without --screenshot it is a CPU run with no\n"
                 "                        window or GPU; it prints the steps, the state hash and the wall time\n"
                 "                        expedition / shelter: complete the beacon mission via the direct / cave branch\n"
                 "  --screenshot PATH     with --script: PATH_<moment>.png at key moments (needs KIN_RENDER_BACKEND=gpu; lavapipe is\n"
                 "                        enough). Otherwise a picture after --frames (headless) or after 30 frames (window)\n"
                 "  --record FILE         write the scripted run's command log        --replay FILE   play a log (CPU) and print its hash\n"
                 "  --expect-hash HEX --expect-steps N   exit 1 unless the scripted run ends so (same machine determinism)\n"
                 "  --max-steps N         the scripted run's bound (60000)            --timing   per climb leg: steps and lowest stamina\n"
                 "  --gpu-timing          (a picture, windowed: --frames 400) the picture is taken after 90 frames and the GPU time of the\n"
                 "                        frame is printed, median over them (a device with GPU timing; the headless path has none)\n"
                 "  --trace               a CPU script prints the bot's state every 20 steps\n"
                 "  --headless --frames N no window: N fixed steps (from the bot with --script)\n"
                 "  --debug-colliders     draw the colliders near the camera as wireframes (boxes grey, buttress hulls orange, stones cyan)\n"
                 "  --on-decor KIND:N     (a single picture) stand on the N-th boulder, buttress or scree stone (--on-decor boulder:3)\n"
                 "  --no-hud --mute       hide the HUD (also in pictures); no sound (the null mixer: the same code runs, silently)\n"
                 "  --settings DIR        load and save the settings file in DIR (a windowed run uses the platform's directory; scripted and\n"
                 "                        headless runs use the defaults unless this is given); --no-settings never reads or writes it\n"
                 "  --no-fx LIST          switch effects off: altitude,time,snow,lights,fires,puffs,wind   --fx storm   the storm look\n"
                 "  --freeze-time         the clock stands still    --time-speed H   game hours a minute (0.25)    --hours H   start time\n"
                 "  --camp N [--drop H]   (a single picture) start at camp N (H metres up: a landing)     --orbit YAW,PITCH,DIST  the picture's camera round the player\n"
                 "  --skip-if-unavailable exit 77 (CTest: skipped) without a 3D renderer\n"
                 "Keys: WASD move, mouse look (click to capture), Shift sprint, Space jump / jump off, left and right mouse (or Q and E) the\n"
                 "      hands, F pull up, X let go, R rest at a camp, Esc the menu (resume, restart from camp, look, camera, volumes, controls,\n"
                 "      quit), V first person, F2 next look, F3 debug line, F4 hide the HUD, F12 or P picture.\n"
                 "Gamepad: left stick move, right stick look, A jump, LT and RT the hands, X pull up, B let go, Y rest, L3 sprint, Start the\n"
                 "      menu (D-pad or left stick to move in it, A accepts, B goes back).\n";
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
    Game game(o.seed, {.expedition = expedition_mode(o), .dense_scenery = o.dense_scenery});
    BotConfig config;
    if (!script_config(o.script, game.level(), config)) {
        std::fprintf(stderr, "summit: no script named %s for this map (climb, fall, rope, expedition, shelter)\n", o.script.c_str());
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
    if (!o.record.empty() && !write_command_log(o.record, o.seed, log, game.config().expedition, game.level().expedition.dense_scenery)) {
        std::fprintf(stderr, "summit: could not write %s\n", o.record.c_str());
        return 1;
    }
    return finish_report(o, r, game);
}

int run_cpu_replay(const Options& o) {
    u64 seed = 0;
    std::vector<PlayerCommand> log;
    bool expedition = false, dense = false;
    if (!read_command_log(o.replay, seed, log, &expedition, &dense)) {
        std::fprintf(stderr, "summit: %s is not a command log\n", o.replay.c_str());
        return 1;
    }
    Game game(seed, {.expedition = expedition, .dense_scenery = dense});
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
        _game = std::make_unique<Game>(_options.seed, GameConfig{.expedition = expedition_mode(_options), .dense_scenery = _options.dense_scenery});
        // Settings: a windowed run reads and keeps the player's file; a scripted or headless run uses the defaults (a picture must
        // not depend on a file of the machine) unless a directory is given.
        const bool use_settings = !_options.no_settings && (!_options.settings_dir.empty() || (_options.script.empty() && !_options.headless));
        if (use_settings) {
            _store = std::make_unique<SaveStore>(make_settings_store(_options.settings_dir));
            bool found = false;
            _settings = load_settings(*_store, &found);
            if (found) std::printf("summit: settings read from %s\n", _store->settings_path().string().c_str());
        }
        if (_options.look_given || !find_look(_settings.look)) _settings.look = _options.look;
        if (_options.first_person) _settings.first_person = true;
        if (!_options.hud) _settings.hud = false;
        if (_options.storm) _settings.effects.storm = true;
        if (_options.freeze_time) _settings.time_speed = 0.0f;
        if (_options.time_speed >= 0.0f) _settings.time_speed = _options.time_speed;
        apply_fx_list(_options.fx_off, false);
        apply_fx_list(_options.fx_on, true);
        _bindings = _settings.bindings;
        _menu.set_settings(&_settings);
        _rig.settings.first_person = _settings.first_person;
        _audio = std::make_unique<SummitAudio>(_options.mute || _options.headless);
        _audio->set_volumes({_settings.master_volume, _settings.sfx_volume, _settings.ambience_volume});
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
                      {"summit", "the summit", "", 40},
                      {"menu", "", "", 3},
                      {"controls", "", "", 3}};
            if (_options.script == "fall") _shots.push_back({"ragdoll", "", "ragdoll", 40});
            if (_game->config().expedition) {
                _shots = {{"briefing", "", "", 0}, {"base", "", "", 45}, {"fork", "", "", 0},
                          {"final_wall", "the final wall", "climb", 90}, {"beacon", "", "", 10},
                          {"extraction", "", "", 4}, {"complete", "", "", 20}};
                if (config.sheltered_route) _shots.push_back({"shelter", "", "", 15});
            }
        } else if (_options.camp >= 0 && std::size_t(_options.camp) < _game->level().camps.size()) {
            if (_game->config().expedition) {
                // A diagnostic camp start skips only the briefing; progression still requires the beacon and extraction.
                const PlayerCommand idle{}, begin{.interact = true};
                _game->step({&idle, 1});
                _game->step({&begin, 1});
            }
            _game->debug_teleport(0, _game->level().camps[std::size_t(_options.camp)].spawn + Vec3f{0.0f, _options.drop, 0.0f});
        }
        if (!_options.on_decor.empty() && _options.script.empty()) {
            // "kind:N": on the top of the N-th chunk of that kind (the first that a ray down from above finds the top of).
            const std::size_t colon = _options.on_decor.find(':');
            const std::string kind = _options.on_decor.substr(0, colon);
            int want = colon == std::string::npos ? 0 : std::atoi(_options.on_decor.c_str() + colon + 1);
            const DecorKind k = kind == "buttress" ? DecorKind::Buttress : kind == "scree" ? DecorKind::Scree : DecorKind::Boulder;
            for (std::size_t j = 0; j < _game->decor().size(); ++j) {
                const DecorChunk& c = _game->decor()[j];
                if (c.kind != k || want-- > 0) continue;
                const auto hit = _game->world().raycast(c.centre + Vec3f{0.0f, 12.0f, 0.0f}, c.centre - Vec3f{0.0f, 12.0f, 0.0f});
                if (hit && hit->body == _game->decor_body(j)) {
                    _game->debug_teleport(0, hit->point + Vec3f{0.0f, 0.05f, 0.0f});
                    break;
                }
                want = 0;
            }
        }
        _rig.observe(*_game, 0);
        if (!_options.benchmark.empty()) _benchmark = std::make_unique<ExpeditionBenchmark>(_options.benchmark,_options.started,_options.benchmark_warmup);
    }
    ~SummitScene() override {
        release_mouse();
        save();
    }

    std::string_view name() const override { return "Summit"; }

    void update(SceneContext& ctx) override {
        if (_quit || _failed || (_benchmark && !_benchmark->ready())) return;
        if (_retry_requested) {
            _retry_requested = false;
            _game = std::make_unique<Game>(_options.seed, GameConfig{.expedition = true, .dense_scenery = _options.dense_scenery});
            _notices.reset(); _cue_generator.reset(); _rig.reset(); _look_feed = {};
            _rig.observe(*_game, 0);
            _scene.clear();
            _view = {};
            _view_size = {};
            _summit_view.reset();
            _r3d = nullptr;
            release_mouse();
        }
        // The menu pauses the game: nothing steps, nothing is read but the menu's own keys (in render).
        if (_menu.open()) return;
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
            const bool use = _captured || ctx.app.headless() ||
                             (_game->config().expedition && _game->player(0).expedition.phase == ExpeditionPhase::Briefing);
            if (use) {
                c = map_input(read_raw_input(ctx.input, &_pad_state), _bindings, fixed_step);
            }
        }
        if (_begin_requested) { c.interact = true; _begin_requested = false; }
        // The look goes into the step in even shares (see LookFeed); what is left waits for the next steps and shows in the camera.
        if (!_bot) c.look = _look_feed.take(c.look);
        const ExpeditionPhase before = _game->player(0).expedition.phase;
        _game->step({&c, 1});
        if (_game->config().expedition && !_bot && !ctx.app.headless()) {
            const ExpeditionPhase after = _game->player(0).expedition.phase;
            if (before == ExpeditionPhase::Briefing && after == ExpeditionPhase::Climbing) capture_mouse(ctx.window);
            if (before != ExpeditionPhase::Complete && after == ExpeditionPhase::Complete) release_mouse();
        }
        _rig.observe(*_game, 0);
        _notices.observe(*_game, 0);
        // The sound and the dust of this step.
        _cues.clear();
        _cue_generator.observe(*_game, 0, _cues);
        for (const SoundCue& cue : _cues) {
            _audio->play(cue);
            _cue_counts[std::size_t(cue.kind)]++;
        }
        if (_summit_view) _summit_view->on_step(*_game, 0);
        ++_updates;
        if (_benchmark && _game->player(0).summit) _quit = true;
        if (_bot && !_options.screenshot.empty()) plan_shots();
        if (_bot && _updates >= _options.max_steps) _quit = true;
    }

    void render(SceneContext& ctx) override {
        if (_benchmark) _benchmark->begin_frame(ctx.app.frame_stats());
        if (_quit) {
            if (_benchmark) _benchmark->finish(*_game,_options.samples);
            if (_bot) print_sound_report();
            ctx.app.quit();
            return;
        }
        if (_failed) return;
        const bool headless = ctx.app.headless();
        const bool scripted_shots = _bot && !_options.screenshot.empty();
        Renderer2D& renderer = ctx.renderer;
        if (!_r3d && !init(ctx)) return;
        const auto now = Clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - _last).count(), 0.0001f, 0.1f);
        _last = now;
        const float frame_dt = headless ? fixed_step : dt;
        // The benchmark bot owns the run. Shared-desktop key/pad events must not
        // pause it, change the look or capture the user's mouse mid-measurement.
        if (!_benchmark) handle_keys(ctx, headless);
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
        // The look the hand has made that no step has taken yet (the camera is the hand's, not the step's), and the person at the
        // camera's instant.
        Vec2f ahead = _bot ? Vec2f{} : _look_feed.pending();
        if (!_bot && !_menu.open() && (_captured || headless)) {
            const Vec2f unread = look_of(read_raw_input(ctx.input, &_pad_state), _bindings, alpha * fixed_step);
            ahead = {unread.x + _look_feed.pending().x, unread.y + _look_feed.pending().y};
        }
        _rig.set_look_ahead(ahead);
        CameraView cam = _rig.frame(_game->world(), alpha, frame_dt);
        if (_game->player(0).state == PlayerState::Alive) _summit_view->set_person_pose(_rig.person_position(alpha), _rig.person_facing(alpha));
        else _summit_view->set_person_pose(_game->feet(0), _game->player(0).facing_yaw);
        if (_options.orbit.z > 0.0f) {
            // A free camera round the player for a picture: yaw, pitch, distance.
            const Vec3f target = _game->head(0);
            cam.rotation = Quatf::yaw_pitch(_options.orbit.x, _options.orbit.y);
            cam.position = target - cam.rotation.rotate({0.0f, 0.0f, -1.0f}) * _options.orbit.z;
        }
        // The sound follows the camera (a listener in the 3D world), the menu holds the effects.
        AudioListener3D listener;
        listener.position = cam.position;
        listener.forward = cam.rotation.rotate({0.0f, 0.0f, -1.0f});
        listener.up = cam.rotation.rotate({0.0f, 1.0f, 0.0f});
        _audio->update(*_game, 0, listener, frame_dt, _menu.open());
        // The person's clips and IK advance with every frame (a scripted run's frames are steps), but a scripted run draws only
        // the frames a picture is wanted of.
        bool wanted = !scripted_shots || _pending_shot >= 0;
        for (const ShotPlan& sp : _shots) wanted = wanted || (!sp.done && sp.countdown >= 0);
        if (!wanted) return;
        // The menu's pictures open it (the game is not stepped while it is up).
        if (_pending_shot >= 0) {
            const std::string& shot = _shots[std::size_t(_pending_shot)].name;
            if (shot == "menu") _menu.open_main();
            else if (shot == "controls") _menu.open_controls();
        }
        _summit_view->fill(_scene, *_game, cam, frame_dt, 0);
        if (scripted_shots && _pending_shot < 0) return;
        const auto t0 = Clock::now();
        const f64 scene_ms = std::chrono::duration<f64,std::milli>(t0-now).count();
        _r3d->render(_view, _scene);
        _render_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        renderer.clear(Color::rgb(0, 0, 0));
        renderer.draw_texture(_view.texture(), {0.0f, 0.0f, float(out.x), float(out.y)});
        ++_frames;
        _presentation_time += double(frame_dt);
        draw_ui(ctx, out);
        if (_benchmark) _benchmark->capture(*_game,_bot ? _bot->leg() : -1,*_summit_view,_r3d->debug_stats(),_view_size,scene_ms,_render_ms);
        if (_options.gpu_timing) {
            const RendererBackendStats st = ctx.renderer.backend_stats();
            if (st.gpu_frames_sampled != _gpu_sampled) {
                _gpu_sampled = st.gpu_frames_sampled;
                _gpu_samples.push_back(st.last_gpu_frame_ms);
            }
        }
        take_pictures(ctx, out, headless);
    }

private:
    bool init(SceneContext& ctx) {
        const auto setup_start = Clock::now();
        _r3d = ctx.renderer3d;
        if (!_r3d || !_r3d->available()) {
            std::fprintf(stderr, "summit: this renderer cannot draw 3D: it needs SDL_GPU (KIN_RENDER_BACKEND=gpu, a Vulkan/Metal/D3D12 device)\n");
            if (ctx.report) ctx.report->fail("no 3D renderer");
            _failed = true;
            _unavailable = true;
            ctx.app.quit();
            _r3d = nullptr;
            return false;
        }
        _summit_view = std::make_unique<SummitView>(*_r3d, _game->level(), !_options.procedural_assets);
        _summit_view->set_look(_settings.look);
        _summit_view->set_effects(_settings.effects, _settings.time_speed);
        if (_options.hours >= 0.0f) _summit_view->set_start_hours(_options.hours);
        _summit_view->set_show_colliders(_options.debug_colliders);
        if (!ctx.app.headless()) _pad = std::make_unique<Gamepads>();
        _last = Clock::now();
        if (_benchmark) _benchmark->set_asset_setup_ms(std::chrono::duration<f64,std::milli>(_last-setup_start).count());
        return true;
    }

    // "snow,fires" and so on.
    void apply_fx_list(const std::string& list, bool on) {
        std::size_t start = 0;
        while (start <= list.size() && !list.empty()) {
            std::size_t end = list.find(',', start);
            if (end == std::string::npos) end = list.size();
            const std::string name = list.substr(start, end - start);
            EffectSettings& f = _settings.effects;
            if (name == "altitude") f.altitude_look = on;
            else if (name == "time") f.time_of_day = on;
            else if (name == "snow") f.snow_line = on;
            else if (name == "lights") f.camp_lights = on;
            else if (name == "fires") f.fires = on;
            else if (name == "puffs") f.puffs = on;
            else if (name == "wind") f.wind_particles = on;
            else if (name == "storm") f.storm = on;
            else if (!name.empty()) throw std::invalid_argument("no effect named " + name);
            start = end + 1;
        }
    }

    // Only a run that changed them writes the file (a picture run, or one that only played, leaves it as it was).
    void save() {
        if (_store && !_options.no_settings && _settings_changed) save_settings(*_store, _settings);
        _settings_changed = false;
    }

    void apply_settings() {
        _bindings = _settings.bindings;
        _rig.settings.first_person = _settings.first_person;
        if (_summit_view) {
            if (_summit_view->look().name != _settings.look) {
                _summit_view->set_look(_settings.look);
                _view = {}; // the view's HDR options follow the look
            }
            _summit_view->set_effects(_settings.effects, _settings.time_speed);
        }
        _audio->set_volumes({_settings.master_volume, _settings.sfx_volume, _settings.ambience_volume});
        _settings_changed = true;
        save();
    }

    void open_menu(SceneContext& ctx) {
        _menu.toggle();
        _recapture = _captured;
        release_mouse();
        (void)ctx;
        _audio->play_ui("ui_select");
    }

    void close_menu(SceneContext& ctx) {
        _menu.close();
        if (_recapture && !ctx.app.headless()) capture_mouse(ctx.window);
        _recapture = false;
    }

    void handle_menu_action(SceneContext& ctx, MenuAction action) {
        switch (action) {
        case MenuAction::None: break;
        case MenuAction::Resume: close_menu(ctx); break;
        case MenuAction::RestartFromCamp:
            if (_game->config().expedition && _game->player(0).expedition.phase == ExpeditionPhase::Complete) {
                _retry_requested = true;
                close_menu(ctx);
                break;
            }
            if (restart_from_camp(*_game, 0)) {
                _rig.reset();
                _rig.observe(*_game, 0);
            }
            close_menu(ctx);
            break;
        case MenuAction::Quit:
            _quit = true;
            break;
        case MenuAction::Changed:
            apply_settings();
            break;
        }
    }

    void handle_keys(SceneContext& ctx, bool headless) {
        const Input& in = ctx.input;
        // The menu: Escape or the pad's Start; its own keys and pad edges while it is up.
        if (_pad && !headless) _pad_state = _pad->poll();
        RawInput raw = read_raw_input(in, headless ? nullptr : &_pad_state);
        const MenuEdges edges = menu_edges(_menu_prev, raw);
        _menu_prev = raw;
        const bool toggle = (in.frame_pressed(Key::Escape) || edges.toggle) && !headless;
        if (toggle) {
            if (!_menu.open()) {
                open_menu(ctx);
            } else {
                handle_menu_action(ctx, _menu.toggle());
            }
        } else if (_menu.open() && edges.input.any()) {
            const MenuAction a = _menu.navigate(edges.input);
            if (!edges.input.accept) _audio->play_ui("ui_move");
            else _audio->play_ui("ui_select");
            handle_menu_action(ctx, a);
        }
        const bool mission_overlay = _game->config().expedition &&
            (_game->player(0).expedition.phase == ExpeditionPhase::Briefing || _game->player(0).expedition.phase == ExpeditionPhase::Complete);
        if (!headless && !_captured && !_menu.open() && !_menu_was_open && !mission_overlay && in.mouse_frame_pressed(MouseButton::Left)) capture_mouse(ctx.window);
        if (mission_overlay && !_menu.open() && _game->player(0).expedition.phase == ExpeditionPhase::Complete && edges.input.accept) _retry_requested = true;
        _menu_was_open = _menu.open();
        if (_menu.open()) return;
        if (in.frame_pressed(Key::V)) {
            _settings.first_person = !_settings.first_person;
            apply_settings();
        }
        if (in.frame_pressed(Key::F3)) _debug_line = !_debug_line;
        if (in.frame_pressed(Key::F4)) _options.hud = !_options.hud;
        if (in.frame_pressed(Key::F2)) {
            const std::vector<Look>& all = looks();
            std::size_t i = 0;
            for (; i < all.size(); ++i) {
                if (&all[i] == &_summit_view->look()) break;
            }
            _settings.look = all[(i + 1) % all.size()].name;
            apply_settings();
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
                } else if (s.name == "briefing") {
                    hit = _game->player(0).expedition.phase == ExpeditionPhase::Briefing;
                } else if (s.name == "fork") {
                    hit = length(_game->feet(0) - _game->level().expedition.fork) < 3.0f;
                } else if (s.name == "shelter") {
                    hit = in_expedition_shelter(_game->level(), _game->feet(0));
                } else if (s.name == "beacon") {
                    hit = _game->interaction() == ExpeditionInteraction::Beacon;
                } else if (s.name == "extraction") {
                    hit = _game->player(0).expedition.phase == ExpeditionPhase::Extraction;
                } else if (s.name == "complete") {
                    hit = _game->player(0).expedition.phase == ExpeditionPhase::Complete;
                } else if (s.name == "summit") {
                    hit = _game->player(0).summit;
                } else if (s.name == "menu") {
                    hit = _updates >= 70;
                } else if (s.name == "controls") {
                    hit = _updates >= 75;
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

    void print_sound_report() {
        if (_reported) return;
        _reported = true;
        std::printf("summit: sound cues:");
        for (std::size_t k = 0; k < cue_kind_count; ++k) {
            if (_cue_counts[k] > 0) std::printf(" %s %d", cue_name(CueKind(k)), _cue_counts[k]);
        }
        const AudioEngineStats st = _audio->stats();
        std::printf("  (engine: %d requests, %d culled, %s)\n", st.played_requests, st.culled_requests, _audio->device_available() ? "device" : "no device: null mixer");
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
            if (s.name == "menu" || s.name == "controls") _menu.close();
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
        } else if (!_bot && !_options.screenshot.empty() && !_single_done && _frames >= (_options.gpu_timing ? 90 : std::max(1, std::max(_options.frames, headless ? 1 : 30)))) {
            _single_done = true;
            if (_options.gpu_timing && _gpu_samples.size() <= 12) std::printf("summit: GPU timing gave %zu samples (not enough: a device with timing and --frames 60 or more)\n", _gpu_samples.size());
            if (_options.gpu_timing && _gpu_samples.size() > 12) {
                std::vector<double> v(_gpu_samples.begin() + 8, _gpu_samples.end());
                std::sort(v.begin(), v.end());
                std::printf("summit: GPU frame time %.2f ms median, %.2f ms 90th percentile (%zu samples) at %dx%d, %d samples, %zu rock triangles, %u lights, %u particles\n",
                            v[v.size() / 2], v[v.size() * 9 / 10], v.size(), out.x, out.y, _options.samples, std::size_t(_summit_view->static_triangles()),
                            _summit_view->lights_added(), _summit_view->particles_alive());
            }
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

    // The HUD (unless hidden) and the menu, painted over the 3D picture; the menu's mouse is handled here, where the pointer is.
    void draw_ui(SceneContext& ctx, Vec2i out) {
        const bool show_hud = _options.hud && _settings.hud;
        const bool menu_open = _menu.open();
        _menu.set_expedition_complete(_game->config().expedition && _game->player(0).expedition.phase == ExpeditionPhase::Complete);
        const bool mission_overlay = _game->config().expedition &&
            (_game->player(0).expedition.phase == ExpeditionPhase::Briefing || _game->player(0).expedition.phase == ExpeditionPhase::Complete);
        if (!show_hud && !menu_open && !mission_overlay) return;
        _ui.begin(ctx.input, ctx.renderer, 0.0f);
        const Vec2f screen{float(out.x), float(out.y)};
        if (show_hud || mission_overlay) {
            const HudModel model = build_hud_model(*_game, 0, _notices);
            const HudLayout layout = hud_layout(screen);
            std::string debug;
            if (_debug_line) {
                char line[220];
                const EnvironmentState& e = _summit_view->environment();
                std::snprintf(line, sizeof line, "step %llu   stamina %.0f/%.0f   health %.0f   wind %.1f m/s   %.1f h   fog %.5f   snow %.2f   render %.1f ms",
                              (unsigned long long)_game->step_count(), double(model.stamina), double(model.stamina_cap), double(model.health),
                              double(_game->wind().speed), double(e.hours), double(e.fog_density), double(e.snow), _render_ms);
                debug = line;
            }
            draw_hud(_ui, model, layout, _bindings, _presentation_time, debug);
            if (model.expedition && !menu_open &&
                (model.phase == ExpeditionPhase::Briefing || model.phase == ExpeditionPhase::Complete)) {
                const Rectf p = layout.expedition_panel;
                ui2::Button button{.id = ui2::make_id("expedition-action"),
                    .bounds = {p.x + p.w * 0.25f, p.y + p.h * 0.80f, p.w * 0.5f, p.h * 0.13f},
                    .label = model.phase == ExpeditionPhase::Briefing ? "Begin expedition" : "Try again",
                    .text_style = {.font = ui2::system_ui_font_bold(18 * layout.u), .color = colors::white}};
                ui2::run(_ui, button);
                if (button.clicked && !_bot) {
                    if (model.phase == ExpeditionPhase::Briefing) _begin_requested = true;
                    else _retry_requested = true;
                }
            }
        }
        if (menu_open) {
            const i32 rows = _menu.screen() == MenuScreen::Controls ? i32(action_count) + 1 : i32(menu_row_count);
            const MenuLayout layout = menu_layout(screen, rows);
            const Vec2f pointer = _ui.pointer();
            draw_menu(_ui, _menu, layout, _bindings, screen, pointer);
            // A click on a row focuses and accepts it; on a slider's track it sets the value.
            if (ctx.input.mouse_frame_pressed(MouseButton::Left) && _menu.screen() == MenuScreen::Main) {
                const i32 row = menu_row_at(layout, pointer);
                if (row >= 0) {
                    _menu.set_focus(row);
                    const Rectf track = menu_row_track(layout.rows[std::size_t(row)]);
                    if (_menu.slider(MenuRow(row)) >= 0.0f && pointer.x >= track.x - 4 && pointer.x <= track.x + track.w + 4) {
                        _menu.set_slider(MenuRow(row), (pointer.x - track.x) / std::max(1.0f, track.w));
                        handle_menu_action(ctx, MenuAction::Changed);
                    } else {
                        MenuInput accept;
                        accept.accept = true;
                        handle_menu_action(ctx, _menu.navigate(accept));
                    }
                }
            } else if (ctx.input.mouse_frame_pressed(MouseButton::Left) && _menu.screen() == MenuScreen::Controls) {
                _menu.toggle();
            }
        }
        _ui.end();
    }

public:
    void write_report(JsonWriter& json) const override {
        const Player& p = _game->player(0);
        const HudModel hud = build_hud_model(*_game, 0, _notices);
        const char* phases[] = {"briefing", "climbing", "extraction", "complete"};
        json.field("mode", _game->config().expedition ? "expedition" : "classic")
            .field("phase", _game->config().expedition ? phases[u8(p.expedition.phase)] : p.summit ? "complete" : "climbing")
            .field("seed", _game->seed()).field("step", _game->step_count()).field("state_hash", hex(_game->state_hash()))
            .field("objective", hud.objective).field("interaction", hud.interaction).field("interaction_progress", f64(hud.interaction_progress))
            .field("run_seconds", f64(hud.run_seconds)).field("sheltered", hud.sheltered).field("shelter_visited", hud.shelter_visited)
            .field("health", f64(p.health)).field("stamina", f64(_game->stamina(0))).field("camp", p.camp).field("falls", p.falls)
            .field("paused", _menu.open());
        const Vec3f feet = _game->feet(0);
        json.key("feet").begin_array().value(f64(feet.x)).value(f64(feet.y)).value(f64(feet.z)).end_array();
        if (hud.expedition && (hud.phase == ExpeditionPhase::Briefing || hud.phase == ExpeditionPhase::Complete)) {
            const Rectf panel = hud_layout({f32(_view_size.x), f32(_view_size.y)}).expedition_panel;
            json.key("action_button").begin_object().field("x", f64(panel.x + panel.w * 0.25f)).field("y", f64(panel.y + panel.h * 0.80f))
                .field("w", f64(panel.w * 0.5f)).field("h", f64(panel.h * 0.13f)).end_object();
        }
    }
    bool failed() const { return _failed; }
    bool unavailable() const { return _unavailable; }

private:
    Options _options;
    SummitSettings _settings;
    std::unique_ptr<SaveStore> _store;
    std::unique_ptr<Game> _game;
    std::unique_ptr<Bot> _bot;
    std::unique_ptr<ExpeditionBenchmark> _benchmark;
    InputBindings _bindings;
    CameraRig _rig;
    std::unique_ptr<Gamepads> _pad;
    GamepadState _pad_state;
    LookFeed _look_feed;
    Renderer3D* _r3d = nullptr;
    std::unique_ptr<SummitView> _summit_view;
    std::unique_ptr<SummitAudio> _audio;
    CueGenerator _cue_generator;
    std::vector<SoundCue> _cues;
    std::array<int, cue_kind_count> _cue_counts{};
    HudNotices _notices;
    Menu _menu;
    RawInput _menu_prev;
    View3D _view;
    Vec2i _view_size{};
    Scene3D _scene;
    ui2::Context _ui;
    std::vector<ShotPlan> _shots;
    int _pending_shot = -1;
    int _updates = 0, _frames = 0, _after = 0;
    double _render_ms = 0.0, _presentation_time = 0.0;
    std::vector<double> _gpu_samples;
    u64 _gpu_sampled = 0;
    bool _captured = false, _quit = false, _failed = false, _unavailable = false, _picture_requested = false, _single_done = false;
    bool _settings_changed = false;
    bool _begin_requested = false, _retry_requested = false;
    bool _recapture = false, _menu_was_open = false, _debug_line = false, _reported = false;
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
        if (!options.script.empty() && options.screenshot.empty() && options.benchmark.empty()) return run_cpu_script(options);

        if (!options.procedural_assets && find_content_root(KIN_GAME_CONTENT, argc, argv).empty())
            std::fprintf(stderr, "summit: content unavailable; using procedural props\n");

        // The runtime times the GPU's frames only when someone reads them (KIN_LOG_FRAME_STATS asks it to).
        if (options.gpu_timing) ::setenv("KIN_LOG_FRAME_STATS", "1", 1);
        // --screenshot is ours (the picture without the app's capture): the runtime must not see it.
        std::vector<char*> kept;
        for (int i = 0; i < argc; ++i) {
            const std::string_view a = argv[i];
            if (a == "--screenshot" || a == "--seed" || a == "--width" || a == "--height" || a == "--look" || a == "--preset" || a == "--script" ||
                a == "--record" || a == "--replay" || a == "--expect-hash" || a == "--expect-steps" || a == "--max-steps" || a == "--samples" ||
                a == "--settings" || a == "--drop" || a == "--time-speed" || a == "--hours" || a == "--no-fx" || a == "--fx" || a == "--camp" || a == "--orbit" || a == "--scenery" || a == "--benchmark" || a == "--benchmark-warmup") {
                ++i;
                continue;
            }
            if (a.starts_with("--screenshot=")) continue;
            kept.push_back(argv[i]);
        }
        auto headless = parse_headless_options(int(kept.size()), kept.data());
        options.frames = headless.frames;
        options.headless = headless.enabled;
        if (!options.benchmark.empty() && headless.enabled) throw std::invalid_argument("benchmark needs the real windowed present loop; omit --headless");
        if (!options.script.empty() && headless.enabled && headless.frames == 0) {
            // A script is as long as it is: the scene ends the app itself.
            headless.frames = options.max_steps;
        }
        if (options.script.empty() && !options.screenshot.empty() && headless.enabled && headless.frames == 0) {
            options.frames = headless.frames = 30;
        }
        InputMap input;
        GameInfo game{.id = "summit", .title = "Summit", .version = "0.1",
                      .description = "A climbing expedition: choose a trail, restore the beacon and request extraction",
                      .window = {.width = options.width, .height = options.height, .resizable = true},
                      .tags = {"example", "3d"}, .input_map = input};
        auto window = window_config(game);
        window.vsync = false;
        window.max_fps = 0.0f;
        window.high_pixel_density = !headless.enabled && options.benchmark.empty();
        SceneManager scenes;
        SummitScene* scene = nullptr;
        {
            auto s = std::make_unique<SummitScene>(options);
            scene = s.get();
            scenes.push(std::move(s));
        }
        const int code = run_scene_app({.window = window, .headless = headless, .game = &game, .render_headless = true, .rendering_3d = true}, scenes);
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
