#include "arena_art.hpp"
#include "example_common.hpp"
#include "siege_audio.hpp"
#include "workloads.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <kin/platform/app.hpp>
#include <iostream>
#include <SDL3/SDL.h>

// Optional desktop-backed check (kept out of headless CTest). The SDL X11
// scaling hint can emulate a monitor content scale without a physical 4K panel.
static void check_native_display(const char* screenshot, bool game = false) {
    using namespace examples;
    kin::App app;
    auto& window = app.create_window({.width = 1280, .height = 800, .hidden = true, .high_pixel_density = true});
    Renderer2D renderer{make_render_backend(window, false, true)};
    const float scale = window.display_scale();
    const auto units = window.size(), pixels = window.pixel_size();
    window.set_size({int(std::ceil(1280 * scale * units.x / pixels.x)),
                     int(std::ceil(800 * scale * units.y / pixels.y))});
    // Desktop resize requests are asynchronous; settle before checking pixels.
    SDL_SyncWindow(SDL_GetWindowFromID(window.id()));
    if (game) {
        renderer.set_logical_size(1280, 800);
        Arena arena(180, 7);
        Camera2D camera; camera.viewport = {1280, 800}; camera.offset = {arena.player.x - 640, arena.player.y - 400};
        RenderQueue queue;
        renderer.clear(Color::rgb(12, 20, 28));
        const RenderView view{.camera = &camera, .culling_enabled = true};
        arena.collect(queue, view); queue.flush(renderer, view);
        const auto aim_window = renderer.logical_to_window({960, 320});
        render_arena_hud(arena, renderer, true, false, int(queue.size()), scale);
        const auto aim = renderer.window_to_logical(aim_window);
        assert(std::abs(aim.x - 960) < .01f && std::abs(aim.y - 320) < .01f);
        if (screenshot) assert(renderer.save_png(screenshot));
        renderer.present();
        std::cout << "Native game: backend=" << renderer.backend_name() << " scale=" << scale << " aim mapping restored\n";
        return;
    }
    Tracker model(10000, 7);
    TrackerDashboard dashboard;
    auto& input = app.input();
    renderer.clear();
    const auto output = renderer.output_size();
    const auto draw = [&] {
        dashboard.render(model, input, renderer, {float(output.x), float(output.y)}, 1.0f / 120, window.display_scale());
    };
    const Vec2f button{output.x - 130 * scale, 35 * scale};
    input.set_mouse_pos(renderer.logical_to_window(button)); draw();
    input.begin_frame(); input.set_mouse_pressed(MouseButton::Left); draw();
    input.begin_frame(); input.set_mouse_held(MouseButton::Left, false); draw();
    assert(!model.streaming && model.ticks == 0);
    assert(dashboard.drawn_rows() > 0 && dashboard.drawn_rows() < 20);
    input.begin_frame(); draw();
    if (screenshot) assert(renderer.save_png(screenshot));
    renderer.present();
    std::cout << "Native display: backend=" << renderer.backend_name() << " scale=" << scale
              << " pixels=" << output.x << 'x' << output.y << " rows=" << dashboard.drawn_rows() << '\n';
}

// The pieces of Signal Siege's flow: the best run survives a restart (and only a
// better run replaces it), muted sound still plays and finishes its effects, and
// the HUD announces the first wave once, again after a reset.
static void check_siege_flow() {
    using namespace examples;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-siege-best-run";
    std::filesystem::remove_all(dir);
    {
        BestRunStore store(dir);
        assert(store.best().time == 0);
        assert(store.record({.won = false, .time = 30, .kills = 12}));
        assert(!store.record({.won = false, .time = 20, .kills = 99}));
        assert(store.record({.won = false, .time = 30, .kills = 13}));
    }
    {
        BestRunStore reopened(dir);
        assert(reopened.best().time == 30 && reopened.best().kills == 13 && !reopened.best().won);
        assert(reopened.record({.won = true, .time = 90, .kills = 1}));
    }
    assert(BestRunStore(dir).best().won);
    std::filesystem::remove_all(dir);

    SiegeAudio audio(true);
    Arena arena(120, 5);
    for (int i = 0; i < 240; ++i) {
        arena.step(1.0f / 120, arena.autopilot(), true);
        audio.play_events(arena.events, arena.player);
        arena.events.clear();
        audio.update(1.0f / 120);
    }
    const int played = audio.stats().played_requests;
    assert(played > 20); // shots at least; kills and hits too
    audio.play("wave");
    for (int i = 0; i < 240; ++i) audio.update(1.0f / 120);
    assert(audio.stats().played_requests == played + 1);
    assert(audio.stats().active_voices == 0); // silent one-shots still end

    kin::App app{{.mode = kin::AppMode::Headless}};
    auto& window = app.create_window({.width = 1280, .height = 800, .hidden = true});
    Renderer2D renderer{window};
    renderer.set_logical_size(1280, 800);
    Input input;
    ArenaHud hud;
    hud.render(arena, renderer, input, {}, 1.0f / 60);
    assert(hud.wave_started());
    hud.render(arena, renderer, input, {}, 1.0f / 60);
    assert(!hud.wave_started());
    hud.reset();
    hud.render(arena, renderer, input, {.reticle = true, .aim = {640, 400}}, 1.0f / 60);
    assert(hud.wave_started());
    const auto mapped = renderer.window_to_logical(renderer.logical_to_window({960, 320}));
    assert(std::abs(mapped.x - 960) < .01f && std::abs(mapped.y - 320) < .01f);
    renderer.present();
}

// The painter only reads the simulation: attaching art and effects to an arena
// leaves its checksum identical to a plain one, it consumes the arena's events,
// and on a real backend it lights the scene.
static void check_painter() {
    using namespace examples;
    kin::App app{{.mode = kin::AppMode::Headless}};
    auto& window = app.create_window({.width = 1280, .height = 800, .hidden = true});
    Renderer2D renderer{window};
    renderer.set_logical_size(1280, 800);
    Arena painted(180, 11), plain(180, 11);
    ArenaPainter painter;
    painter.init(renderer, painted);
    for (int i = 0; i < 600; ++i) {
        painted.step(1.0f / 120, painted.autopilot(), true);
        plain.step(1.0f / 120, plain.autopilot(), true);
        painter.update(painted, 1.0f / 120);
        assert(painted.events.empty());
    }
    assert(painted.checksum() == plain.checksum());
    assert(painted.kills > 0 && painter.particles() > 0);
    assert(!plain.events.empty() && plain.events.size() <= 1024);
    Camera2D camera;
    camera.viewport = {1280, 800};
    camera.offset = {painted.player.x - 640, painted.player.y - 400};
    renderer.clear();
    painter.draw(renderer, painted, camera);
    assert(painter.lit() && painter.lights() >= 2 && painter.commands() > 0);
    assert(painted.checksum() == plain.checksum());
    renderer.present();
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--native-game") {
        check_native_display(argc > 2 ? argv[2] : nullptr, true);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--native-display") {
        check_native_display(argc > 2 ? argv[2] : nullptr);
        return 0;
    }
    using namespace examples;
    Arena first(180, 42), second(180, 42), different(180, 43);
    assert(first.checksum() == second.checksum());
    assert(first.checksum() != different.checksum());
    for (int i = 0; i < 960; ++i) {
        first.step(1.0f / 120, first.autopilot(), true);
        second.step(1.0f / 120, second.autopilot(), true);
    }
    assert(first.checksum() == second.checksum());
    assert(first.kills > 0 && first.health == 100);
    assert(first.shots.size() <= 8192 && first.sparks.size() <= 8192);
    assert(std::isfinite(first.player.x) && first.player.x >= 20 && first.player.x <= 3052);
    RenderQueue queue;
    Camera2D camera; camera.viewport = {1280, 800}; camera.offset = {first.player.x - 640, first.player.y - 400};
    first.collect(queue, {.camera = &camera, .culling_enabled = true});
    assert(!queue.empty());
    const auto visual_checksum=first.checksum();
    const auto visual_commands=queue.size();
    first.collect(queue,{.camera=&camera,.culling_enabled=true});
    assert(first.checksum()==visual_checksum && queue.size()==visual_commands);
    assert(queue.size()<=4*first.enemy_count()+2*first.shots.size()+first.sparks.size()+3*first.pickups.size()+96*8+32);
    first.reset(42); second.reset(42);
    assert(first.ticks == 0 && first.kills == 0 && first.shots.empty() && first.checksum() == second.checksum());
    const auto start = first.player;
    first.step(1.0f / 120, {.move = {1, 0}, .fire = true, .dash = true});
    assert(first.player.x > start.x && first.dash_cooldown > 0 && !first.shots.empty());
    check_painter();
    check_siege_flow();

    Tracker tracker(10000, 7), replay(10000, 7);
    // Purchases are atomic, dependency-gated, and reset with the run.
    Arena powers(1,42), baseline(1,42);
    assert(powers.available_cores()==3 && powers.has_power(0));
    assert(!powers.buy_power(-1) && !powers.buy_power(int(power_ups.size())) && !powers.buy_power(0));
    assert(!powers.buy_power(2));
    assert(powers.buy_power(1) && powers.available_cores()==0);
    assert(!powers.buy_power(1) && !powers.buy_power(3));
    powers.step(.01f,{.move={1,0}},true); baseline.step(.01f,{.move={1,0}},true);
    assert(powers.player.x>baseline.player.x);
    powers.collected=30;
    assert(powers.buy_power(2));
    powers.step(.01f,{.dash=true},true);
    assert(std::abs(powers.dash_cooldown-.9f)<.001f);
    assert(powers.buy_power(3) && powers.buy_power(4));
    assert(!powers.buy_power(6) && powers.buy_power(5) && powers.buy_power(6));
    assert(powers.health==140 && powers.max_health()==140 && powers.available_cores()==9);
    const auto installed=powers.checksum();
    assert(!powers.buy_power(6) && powers.checksum()==installed);
    powers.reset(42); baseline.reset(42);
    assert(powers.checksum()==baseline.checksum() && powers.available_cores()==3 && powers.max_health()==100);
    powers.health=0; assert(!powers.buy_power(1));
    powers.health=100; powers.elapsed=90; assert(!powers.buy_power(1));
    // Exercise actual damage, cadence and attraction, not just ownership flags.
    Arena combat(1,7);
    assert(combat.buy_power(3));
    combat.world.raw().each([&](kin::Transform2D& t,Enemy& enemy) {
        t.pos=combat.player; enemy.kind=1; enemy.hp=3;
    });
    combat.shots.push_back({combat.player,{0,0},1,false});
    combat.step(0,{},true);
    combat.world.raw().each([](Enemy& enemy) {assert(enemy.hp==1);});
    combat.reset(7); combat.collected=5;
    assert(combat.buy_power(3) && combat.buy_power(4));
    combat.step(.01f,{.fire=true},true);
    combat.step(.095f,{.fire=true},true);
    assert(combat.shots.size()==2);
    baseline.reset(7); baseline.collected=5;
    baseline.step(.01f,{.fire=true},true); baseline.step(.095f,{.fire=true},true);
    assert(baseline.shots.size()==1);
    combat.reset(7); assert(combat.buy_power(5));
    combat.pickups.push_back({{combat.player.x+180,combat.player.y}});
    combat.step(.01f,{},true);
    assert(combat.pickups[0].pos.x<combat.player.x+180);
    // Every catalog entry is reachable, charged once, and contributes to stats.
    Arena complete(1,7); complete.collected=1000;
    int total_cost=0;
    for (int id=1;id<int(power_ups.size());++id) {
        assert(power_ups[id].parent>=0 && power_ups[id].parent<id);
        assert(power_ups[id].effect!=PowerEffect::None && power_ups[id].value!=0);
        assert(complete.buy_power(id) && complete.has_power(id));
        assert(!complete.buy_power(id)); total_cost+=power_ups[id].cost;
    }
    assert(complete.available_cores()==1003-total_cost);
    assert(complete.has_power(32) && complete.has_power(36) && !complete.has_power(64));
    const auto& stats=complete.power_stats();
    assert(stats.move==340 && stats.dash_speed==1150 && std::abs(stats.dash_cooldown-.7f)<.001f);
    assert(std::abs(stats.dash_duration-.24f)<.001f && stats.damage==3.5f);
    assert(std::abs(stats.fire-.75f*.85f*.85f)<.001f);
    assert(stats.magnet==360 && stats.pull==740 && stats.repair==6);
    assert(stats.hull==250 && stats.armor==3 && stats.regen==4);
    assert(stats.shot_speed==1200 && std::abs(stats.shot_life-2.2f)<.001f && stats.pellets==3);
    complete.health=100; complete.step(.5f,{},true); assert(complete.health==102);
    complete.step(.01f,{.fire=true},true); assert(complete.shots.size()==5);
    complete.shots.resize(8190,{{0,0},{0,0},10,true});
    complete.step(.1f,{.fire=true},true); assert(complete.shots.size()<=8192);
    complete.reset(7); assert(!complete.has_power(36) && complete.power_stats().regen==0);
    assert(complete.max_health()==100 && complete.available_cores()==3);
    assert(complete.buy_power(25));
    complete.world.raw().each([&](Transform2D& t,Enemy&) {t.pos=complete.player;});
    complete.step(0,{},false); assert(complete.health==93);
    for (int i = 0; i < 240; ++i) { tracker.step(1.0f / 120); replay.step(1.0f / 120); }
    assert(tracker.checksum() == replay.checksum() && tracker.revisions > 0);
    assert(tracker.logs.size() <= 128);
    tracker.filter("Llama", true);
    assert(!tracker.visible.empty() && tracker.visible.size() < tracker.runs.size());
    double previous = 0;
    for (int id : tracker.visible) { assert(tracker.runs[id].model == "Llama 8B"); assert(tracker.runs[id].loss >= previous); previous = tracker.runs[id].loss; }
    tracker.filter("RUNNING", false); tracker.selected = 0;
    assert(std::ranges::find(tracker.visible, 0) != tracker.visible.end());
    tracker.toggle_selected();
    assert(tracker.runs[0].status == "PAUSED" && std::ranges::find(tracker.visible, 0) == tracker.visible.end());
    const int step = tracker.runs[0].step;
    for (int i = 0; i < 120; ++i) tracker.step(1.0f / 120);
    assert(tracker.runs[0].step == step);
    tracker.filter("no-match", false); assert(tracker.visible.empty());
    tracker.filter("", false); assert(tracker.visible.size() == tracker.runs.size());
    tracker.streaming = false; const int revision = tracker.revisions;
    for (int i = 0; i < 120; ++i) tracker.step(1.0f / 120);
    assert(tracker.revisions == revision);

    // Real renderer + input path: UI controls act in render without a sim step,
    // and scrolling does not submit thousands of offscreen rows.
    kin::App app({.mode = kin::AppMode::Headless});
    auto& window = app.create_window({.width = 1280, .height = 800, .hidden = true});
    Renderer2D renderer(window);
    Tracker ui_model(10000, 7);
    TrackerDashboard dashboard;
    auto& input = app.input();
    // Grid uses matching native-pixel drawing and hit testing at fractional DPI.
    input.bind("grid_next",Key::Right);
    input.bind("grid_down",Key::Down);
    input.bind("grid_buy",Key::Enter);
    for (float scale : {1.0f,1.25f,2.0f}) {
        window.set_size({int(640*scale),int(480*scale)});
        Arena grid_model(1,7); PowerGridState grid;
        const auto grid_draw=[&] {return render_power_grid(grid_model,input,renderer,grid,scale);};
        input.begin_frame(); input.set_mouse_pos({117*scale,187*scale});
        input.set_mouse_pressed(MouseButton::Left); grid_draw();
        assert(grid.selected==1 && !grid_model.has_power(1));
        input.begin_frame(); input.set_mouse_pos({540*scale,450*scale});
        input.set_mouse_pressed(MouseButton::Left); grid_draw();
        assert(grid_model.has_power(1) && grid_model.available_cores()==0 && grid_model.ticks==0);
        input.begin_frame(); input.set_mouse_pos({300*scale,230*scale}); input.set_mouse_wheel_y(-100);
        grid_draw(); assert(grid.selected==3);
        input.begin_frame(); input.set_action_pressed("grid_buy"); input.advance_keyboard_edges(); grid_draw();
        assert(grid.feedback=="Need 3 more cores");
        input.begin_frame(); input.set_mouse_pos({524*scale,117*scale});
        input.set_mouse_pressed(MouseButton::Left); grid_draw(); assert(grid.selected==31);
        for (int n=0;n<5;++n) {
            input.begin_frame(); input.set_action_pressed("grid_next"); input.advance_keyboard_edges(); grid_draw();
        }
        assert(grid.selected==36);
        input.begin_frame(); input.set_action_pressed("grid_buy"); grid_draw();
        assert(grid.feedback=="Requires SALVAGER");
        input.begin_frame(); input.set_action_pressed("grid_next"); grid_draw(); assert(grid.selected==31);
        // Each Enter binding must survive the fixed-update edge consumption.
        for (Key key:{Key::Enter,Key::KeypadEnter}) {
            grid_model.reset(7); grid.selected=1;
            input.unbind("grid_buy"); input.bind("grid_buy",key);
            input.begin_frame(); input.set_action_pressed("grid_buy"); input.advance_keyboard_edges(); grid_draw();
            assert(grid_model.has_power(1) && grid.feedback=="Installed DRIVE");
            input.begin_frame(); grid_draw(); assert(grid_model.available_cores()==0);
        }
        grid.selected=1;
        for (int branch=0;branch<6;++branch) {
            for (int tier=0;tier<6;++tier) {
                assert(grid.selected==power_branches[branch][tier]);
                input.begin_frame(); input.set_action_pressed("grid_next"); grid_draw();
            }
            input.begin_frame(); input.set_action_pressed("grid_down"); grid_draw();
        }
        input.begin_frame(); input.set_mouse_pos({550*scale,38*scale});
        input.set_mouse_pressed(MouseButton::Left); assert(grid_draw());
        renderer.present();
    }
    input.begin_frame(); input.set_mouse_held(MouseButton::Left,false);
    window.set_size({1280,800});
    const auto draw = [&] { dashboard.render(ui_model, input, renderer, {1280, 800}, 1.0f / 120); };
    input.set_mouse_pos({1150, 35}); draw();
    input.begin_frame(); input.set_mouse_pressed(MouseButton::Left); draw();
    input.begin_frame(); input.set_mouse_held(MouseButton::Left, false); draw();
    assert(!ui_model.streaming);
    assert(ui_model.ticks == 0 && dashboard.drawn_rows() < 30);
    input.begin_frame(); input.set_mouse_pos({120, 320}); input.set_mouse_wheel_y(-100); draw();
    assert(dashboard.drawn_rows() > 0 && dashboard.drawn_rows() < 30);
    // Search field receives text immediately, without waiting for simulation.
    input.begin_frame(); input.set_mouse_pos({100, 200}); draw();
    input.begin_frame(); input.set_mouse_pressed(MouseButton::Left); draw();
    input.begin_frame(); input.set_mouse_held(MouseButton::Left, false); input.set_text_input("Llama"); draw();
    assert(ui_model.visible.size() < ui_model.runs.size() && !ui_model.visible.empty());
    for (int id : ui_model.visible) assert(ui_model.runs[id].model == "Llama 8B");

    // DPI changes alter raster sizes, not the number of density-independent
    // rows. Exercise fractional scales and change scale on the same dashboard.
    Tracker scaled_model(10000, 7);
    TrackerDashboard scaled_dashboard;
    int base_rows = 0;
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f, 1.0f}) {
        window.set_size({int(1280 * scale), int(800 * scale)});
        const auto draw_scaled = [&] {
            scaled_dashboard.render(scaled_model, input, renderer, {1280 * scale, 800 * scale}, 1.0f / 120, scale);
        };
        input.begin_frame(); input.set_mouse_pos({1150 * scale, 35 * scale}); draw_scaled();
        const bool before = scaled_model.streaming;
        input.begin_frame(); input.set_mouse_pressed(MouseButton::Left); draw_scaled();
        input.begin_frame(); input.set_mouse_held(MouseButton::Left, false); draw_scaled();
        assert(scaled_model.streaming != before && scaled_model.ticks == 0);
        if (!base_rows) base_rows = scaled_dashboard.drawn_rows();
        assert(scaled_dashboard.drawn_rows() == base_rows);
        // A row click and wheel event must use the same scaled bounds as drawing.
        input.begin_frame(); input.set_mouse_pos({100 * scale, 322 * scale}); draw_scaled();
        input.begin_frame(); input.set_mouse_pressed(MouseButton::Left); draw_scaled();
        input.begin_frame(); input.set_mouse_held(MouseButton::Left, false); draw_scaled();
        assert(scaled_model.selected == 1);
        input.begin_frame(); input.set_mouse_wheel_y(-10); draw_scaled();
        input.begin_frame(); input.set_mouse_wheel_y(10); draw_scaled();
        input.begin_frame(); input.set_mouse_pos({100 * scale, 282 * scale}); draw_scaled();
        input.begin_frame(); input.set_mouse_pressed(MouseButton::Left); draw_scaled();
        input.begin_frame(); input.set_mouse_held(MouseButton::Left, false); draw_scaled();
        assert(scaled_model.selected == 0);
        renderer.present();
    }

    // HUD scopes must restore the game's presentation and aim conversion,
    // including both wide and tall letterboxed windows and fractional DPI.
    Arena hud_model(180, 7);
    renderer.set_logical_size(1280, 800);
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (Vec2i size : {Vec2i{1280, 800}, Vec2i{1920, 1080}, Vec2i{640, 480}}) {
            window.set_size({int(size.x * scale), int(size.y * scale)});
            renderer.clear(Color::rgb(12, 20, 28));
            const auto aim_window = renderer.logical_to_window({960, 320});
            const auto before = hud_model.checksum();
            render_arena_hud(hud_model, renderer, true, false, 180, scale);
            const auto aim = renderer.window_to_logical(aim_window);
            assert(std::abs(aim.x - 960) < .01f && std::abs(aim.y - 320) < .01f);
            assert(hud_model.checksum() == before);
            renderer.present();
        }
    }
}
