// Small authored 3D host: sample commands, load/step/render and draw UI.
// Player gameplay lives in gameplay.hpp; content uses the shared scene/prefab APIs.
#include "audio.hpp"
#include "gameplay.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/ecs/system.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene_session3d.hpp>
#include <kin/ui2/text.hpp>
#include <stdexcept>

namespace {
using namespace kin;
class Camp final : public Scene {
    std::filesystem::path _root;
    bool _script;
    Renderer3D* _renderer = nullptr;
    std::unique_ptr<Assets3D> _assets;
    std::unique_ptr<SceneSession3D> _session;
    camp_walk::InputMapper _mapper;
    std::unique_ptr<Gamepads> _pads;
    std::unique_ptr<camp_walk::FootstepAudio> _audio;
    bool _pad_connected = false;
    bool _edited = false, _edit_collision = false, _edit_particles = false;
    int _live_edits = 0;
    std::string _error;
    void diagnostic() {
        _error.clear();
        for (const auto& error : _session->diagnostics())
            _error += error + "\n";
        std::cerr << _error;
    }
    bool edit_camp() {
        auto barrel = _session->entity("brass-barrel"), smoke = _session->entity("campfire.low-smoke");
        const auto old_body = _session->bodies()->body_of(barrel.raw());
        const auto old_volume = _session->physics()->shape(old_body).volume();
        const auto alive = smoke.get<ParticleEmitter3D>()->system.count();
        const float scale = _edited ? 2.62f : 3.5f;
        // Typed configuration; per-frame gameplay state uses native APIs below.
        if (!_session->reconfigure<Transform3D>(_session->handle(barrel),
                                                [&](auto& t) { t.scale = {scale, scale, scale}; }) ||
            !_session->reconfigure<ModelMaterials3D>(
                _session->handle(barrel), [&](auto& material) { material.variant = _edited ? "brass" : "blue"; }) ||
            !_session->reconfigure<ParticleEmitter3D>(
                _session->handle(smoke), [&](auto& emitter) { emitter.system.desc.rate = _edited ? 6 : 12; })) {
            diagnostic();
            return false;
        }
        const auto new_body = _session->bodies()->body_of(barrel.raw());
        const auto new_volume = _session->physics()->shape(new_body).volume();
        _edit_collision = new_body != old_body && (_edited ? new_volume < old_volume : new_volume > old_volume);
        _edit_particles = smoke.get<ParticleEmitter3D>()->system.count() == alive;
        _edited = !_edited;
        ++_live_edits;
        _error.clear();
        return _edit_collision && _edit_particles;
    }

public:
    Camp(std::filesystem::path root, bool script) : _root(std::move(root)), _script(script) {}
    std::string_view name() const override { return "Camp walk"; }
    void on_enter(SceneContext& context) override {
        _renderer = context.renderer3d;
        if (!_renderer)
            throw std::runtime_error("runtime 3D renderer was not enabled");
        _assets = std::make_unique<Assets3D>(*_renderer, _root);
        SceneSession3DOptions options;
        options.viewport = {960, 540};
        options.view.samples = 1;
        options.physics = World3DConfig{.max_bodies = 128,
                                        .max_body_pairs = 512,
                                        .max_contact_constraints = 512,
                                        .temp_memory_bytes = 2u * 1024u * 1024u,
                                        .single_threaded = true,
                                        .max_events = 256};
        options.max_runtime_prefabs = 8;
        options.max_runtime_entities = 16;
        options.max_spawn_entities = 4;
        options.max_pending_commands = 8;
        options.register_components = camp_walk::register_components;
        options.initialize = camp_walk::install;
        options.compose = camp_walk::compose;
        _session = std::make_unique<SceneSession3D>(*_renderer, *_assets, options);
        if (!_session->load("scenes/camp-play.kinscene")) {
            diagnostic();
            throw std::runtime_error("camp could not load");
        }
        if (!camp_walk::prepare(*_session))
            throw std::runtime_error("camp gameplay could not start");
        _pads = std::make_unique<Gamepads>();
        _audio = std::make_unique<camp_walk::FootstepAudio>(context.app.headless());
    }
    void on_exit(SceneContext&) override {
        _audio.reset();
        _pads.reset();
        _session.reset();
        _assets.reset();
        _renderer = nullptr;
    }
    kin::EcsWorld* world() override { return _session ? _session->world() : nullptr; }
    void collect_actions(InputActionContext& actions) const override { camp_walk::collect_actions(actions); }
    void update(SceneContext& ctx) override {
        const auto& pad = _pads->poll();
        _pad_connected = pad.connected;
        const auto live = _mapper.sample(ctx.input, pad, ctx.dt);
        if (live.quit) {
            ctx.app.quit();
            return;
        }
        const auto commands =
            _script ? camp_walk::demo_commands(camp_walk::state(*_session->world()).progress.steps) : live;
        if (commands.restart || commands.refresh || camp_walk::state(*_session->world()).restart_requested) {
            if (!camp_walk::restart(*_session, commands.refresh)) {
                diagnostic();
                if (ctx.report) {
                    ctx.report->fail("camp restart failed");
                    ctx.app.quit();
                    return;
                }
                if (!camp_walk::state(*_session->world()).prepared) {
                    ctx.app.quit();
                    return;
                }
            } else {
                _edited = false;
                _error.clear();
            }
        }
        if (commands.edit && !edit_camp() && ctx.report)
            ctx.report->fail("live camp editing/collision/particle preservation check failed");
        camp_walk::submit(*_session->world(), commands.player);
        if (!_session->step(ctx.dt)) {
            diagnostic();
            if (ctx.report)
                ctx.report->fail(_error);
            ctx.app.quit();
            return;
        }
        const auto& game = camp_walk::state(*_session->world());
        const auto camera = _session->entity("camera");
        auto listener = camera.get<SceneCamera3D>()->camera;
        listener.position = camera.get<Transform3D>()->position;
        listener.rotation = camera.get<Transform3D>()->rotation;
        _audio->step({game.footstep_positions.data(), game.footstep_count}, listener, ctx.dt);
        if (_script && game.progress.steps == 230 &&
            (!camp_walk::demo_passed(game) || _session->runtime_prefabs() != 2)) {
            if (ctx.report)
                ctx.report->fail("camp gameplay movement/collection/pushing/reset/lifetime check failed");
            ctx.app.quit();
        }
    }
    void render(SceneContext& ctx) override {
        ctx.renderer.clear();
        camp_walk::present(*_session, ctx.app.headless() ? 1.f : ctx.alpha, ctx.app.frame_stats().raw_frame_time);
        if (!_session->render(ctx.app.headless() ? 1.f : ctx.alpha)) {
            diagnostic();
            if (ctx.report)
                ctx.report->fail(_error);
            return;
        }
        ctx.renderer.draw_texture(_session->view()->texture(), {0, 0, 960, 540});
        ctx.renderer.fill_rect({0, 0, 960, 88}, Color::rgba(15, 20, 30, 225));
        ui2::draw_text(ctx.renderer,
                       _pad_connected ? "LS move  A jump  LS click sprint  B crouch"
                                      : "WASD move  SPACE jump  SHIFT sprint  C crouch",
                       {14, 8}, 2, colors::white);
        ui2::draw_text(ctx.renderer,
                       _pad_connected ? "X collect  Y push crate  START restart"
                                      : "E collect   F push crate   R restart   ESC exit",
                       {14, 34}, 2, colors::white);
        ui2::draw_text(ctx.renderer, _pad_connected ? "RS orbit" : "RMB orbit", {650, 8}, 2, Color::rgb(150, 220, 190));
        const auto& game = camp_walk::state(*_session->world());
        const auto verb = game.nearby >= 0 ? camp_walk::Collect : game.push_focus.verb;
        const auto prompt = camp_walk::interaction_prompt(verb, _pad_connected);
        const auto hint = game.pickup.target ? std::string("   Picking up...")
                          : prompt.empty()   ? std::string{}
                                             : "   " + std::string(prompt);
        const auto objective = game.collected == 3
                                   ? std::string("Camp stocked! R to play again")
                                   : std::string("Find supplies: ") + std::to_string(game.collected) + " / 3" + hint;
        ui2::draw_text(ctx.renderer, objective, {14, 62}, 2, Color::rgb(150, 235, 180));
        const auto* controller = _session->entity("player").get<CharacterController3D>();
        ui2::draw_text(ctx.renderer, controller->state.ground == CharacterGround3D::OnGround ? "Grounded" : "Airborne",
                       {650, 34}, 2, Color::rgb(150, 220, 190));
        if (!_error.empty())
            ui2::draw_text(ctx.renderer, "Gameplay operation failed; see diagnostic.", {14, 96}, 2,
                           Color::rgb(255, 170, 100));
    }
    void write_report(JsonWriter& json) const override {
        const auto& game = camp_walk::state(*_session->world());
        const auto& p = game.progress;
        json.field("steps", p.steps)
            .field("grounded_steps", p.grounded)
            .field("airborne_steps", p.airborne)
            .field("jumped", p.jumped)
            .field("crouched", p.crouched)
            .field("moved", p.moved)
            .field("input_steps", p.input_steps)
            .field("movement_steps", p.movement_steps)
            .field("interaction_steps", p.interaction_steps);
        json.field("live_edits", _live_edits)
            .field("edit_collision_updated", _edit_collision)
            .field("edit_particles_preserved", _edit_particles);
        const auto lantern = game.lantern.root.resolve(*_session->world());
        const auto* attachment = lantern ? lantern.get<JointAttachment3D>() : nullptr;
        json.field("lantern_attached", attachment && attachment->status() == JointAttachmentStatus3D::Ready);
        json.field("pickups_started", p.pickups_started)
            .field("pickup_contacts", p.pickup_contacts)
            .field("pickups_cancelled", p.pickups_cancelled)
            .field("pickup_awards", p.pickup_awards);
        json.field("footsteps", p.footsteps)
            .field("animation_events_dropped", p.animation_events_dropped)
            .field("animation_events_unhandled", p.animation_events_unhandled)
            .field("footstep_audio_requests", _audio ? _audio->stats().played_requests : 0);
        json.field("camera_steps", p.camera_steps)
            .field("camera_following", bool(game.presentation))
            .field("camera_arm", game.presentation ? game.presentation->arm.length : 0.f);
        json.field("selected_supply", game.nearby).field("query_overflows", game.query_overflows);
        json.field("supplies_collected", game.collected)
            .field("supplies_collected_total", p.collected_total)
            .field("rounds_completed", p.rounds_completed)
            .field("level_resets", p.level_resets)
            .field("runtime_spawned_total", p.spawned_total)
            .field("crate_pushes", p.pushes)
            .field("crate_moved", p.crate_moved)
            .field("stale_handles_rejected", p.stale_handles_rejected);
        if (_session && _session->active()) {
            const auto* controller = _session->entity("player").get<CharacterController3D>();
            json.field("player_x", controller->state.position.x)
                .field("player_y", controller->state.position.y)
                .field("player_z", controller->state.position.z);
            u64 alive = 0, capacity = 0;
            u32 emitters = 0;
            _session->world()->raw().each([&](const ParticleEmitter3D& e) {
                ++emitters;
                alive += e.system.count();
                capacity += e.system.desc.max_particles;
            });
            json.field("foot_dust_alive", u64(_session->entity("foot-dust").get<ParticleEmitter3D>()->system.count()));
            json.field("particle_emitters", emitters)
                .field("particles_alive", alive)
                .field("particle_capacity", capacity);
            json.field("runtime_prefabs", u64(_session->runtime_prefabs()))
                .field("runtime_entities", u64(_session->runtime_entities()));
        }
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        bool script = false;
        std::vector<char*> args{argv[0]};
        for (int i = 1; i < argc; ++i)
            if (std::string_view(argv[i]) == "--demo")
                script = true;
            else
                args.push_back(argv[i]);
        auto headless = kin::parse_headless_options(int(args.size()), args.data());
        const auto root = kin::find_content_root(KIN_GAME_CONTENT, argc, argv);
        kin::SceneManager scenes;
        scenes.push(std::make_unique<Camp>(root, script));
        return kin::run_scene_app({.window = {.title = "Camp walk",
                                              .width = 960,
                                              .height = 540,
                                              .logical_width = 960,
                                              .logical_height = 540,
                                              .max_fps = 60,
                                              .input_map = camp_walk::input_map()},
                                   .headless = headless,
                                   .render_headless = true,
                                   .rendering_3d = true},
                                  scenes);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
