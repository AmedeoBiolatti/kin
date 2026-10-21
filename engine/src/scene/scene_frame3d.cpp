#include <cmath>
#include <kin/audio/audio.hpp>
#include <kin/ecs/audio.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/scene/scene_frame3d.hpp>
#include <stdexcept>
#include <utility>

namespace kin {

SceneFrame3D::SceneFrame3D(EcsWorld& world, WorldRenderState3D& rendering, SceneFrameOptions3D options)
    : _world(world), _rendering(rendering), _options(std::move(options)) {
    if (bool(_options.audio) != bool(_options.audio_catalog))
        throw std::invalid_argument("scene frame audio requires both an engine and a catalog");
    if (_options.bodies && _options.physics_step)
        throw std::invalid_argument("scene frame requires one physics step owner");
}

bool SceneFrame3D::validate() {
    _errors = validate_particles3d(_world, _options.particles).diagnostics;
    return _errors.empty();
}

Status3D SceneFrame3D::after_phase(SystemPhase phase, f32 dt) {
    const bool accepted = after_phase_operation(phase, dt);
    return status3d(accepted, _errors);
}

bool SceneFrame3D::after_phase_operation(SystemPhase phase, f32 dt) {
    _errors.clear();
    if (!std::isfinite(dt) || dt <= 0) {
        _errors = {"scene frame requires finite positive dt"};
        return false;
    }
    try {
        switch (phase) {
        case SystemPhase::Physics:
            if (!validate())
                return false;
            if (_options.physics_step)
                _options.physics_step(dt);
            else if (_options.bodies)
                _options.bodies->step(dt);
            _rendering.propagate_transforms();
            break;
        case SystemPhase::Animation:
            if (!validate())
                return false;
            _rendering.animate(dt);
            _rendering.advance_time_of_day(dt);
            _rendering.advance_weather(dt);
            _rendering.propagate_transforms();
            _rendering.simulate_particles(dt);
            break;
        case SystemPhase::Audio:
            if (_options.audio) {
                update_audio_listeners(_world, *_options.audio);
                update_audio_emitters(_world, *_options.audio, *_options.audio_catalog);
                consume_audio_one_shots(_world, *_options.audio, *_options.audio_catalog);
            }
            break;
        case SystemPhase::Editor:
            if (!validate())
                return false;
            _rendering.propagate_transforms();
            break;
        default:
            break;
        }
        return _errors.empty();
    } catch (const std::exception& error) {
        _errors = {"scene frame stage: " + std::string(error.what())};
        return false;
    }
}

Status3D SceneFrame3D::step(f32 dt, const SystemPhaseHooks& hooks) {
    const bool accepted = step_operation(dt, hooks);
    return status3d(accepted, _errors);
}

bool SceneFrame3D::step_operation(f32 dt, const SystemPhaseHooks& hooks) {
    if (_running) {
        _errors = {"recursive scene frame step"};
        return false;
    }
    _errors.clear();
    if (!std::isfinite(dt) || dt <= 0) {
        _errors = {"scene frame requires finite positive dt"};
        return false;
    }
    struct Guard {
        bool& running;
        ~Guard() { running = false; }
    } guard{_running};
    _running = true;
    SystemPhaseHooks stages;
    stages.before = hooks.before;
    stages.after = [&](SystemPhase phase, std::string& error) {
        if (hooks.after && !hooks.after(phase, error))
            return false;
        if (after_phase_operation(phase, dt))
            return true;
        for (const auto& message : _errors) {
            if (!error.empty())
                error += '\n';
            error += message;
        }
        return false;
    };
    if (_world.run_frame(dt, stages))
        return true;
    _errors = {_world.systems().last_error()};
    return false;
}

Status3D SceneFrame3D::collect(Scene3D& scene, ViewOptions3D* view, f32 alpha) {
    const bool accepted = collect_operation(scene, view, alpha);
    return status3d(accepted, _errors);
}

bool SceneFrame3D::collect_operation(Scene3D& scene, ViewOptions3D* view, f32 alpha) {
    if (_running) {
        _errors = {"recursive scene frame collection"};
        return false;
    }
    struct Guard {
        bool& running;
        ~Guard() { running = false; }
    } guard{_running};
    _running = true;
    _errors.clear();
    try {
        if (!std::isfinite(alpha) || alpha < 0 || alpha > 1) {
            _errors = {"render interpolation alpha must be finite and in [0,1]"};
            return false;
        }
        if (!validate())
            return false;
        scene.clear();
        scene.camera = {};
        scene.lights = {};
        scene.sky = {};
        scene.fog = {};
        scene.weather = {};
        scene.debug = {};
        scene.ambient_occlusion = {};
        scene.contact_shadows = {};
        scene.taa = {};
        scene.lod = {};
        scene.cull = true;
        _rendering.propagate_transforms();
        _errors = apply_scene3d_settings(_world, scene, view).diagnostics;
        if (!_errors.empty())
            return false;
        _rendering.apply_time_of_day(scene);
        const auto fog = scene.fog;
        _rendering.apply_weather(scene, fog);
        _rendering.collect(scene, _options.bodies && alpha < 1
                                      ? WorldRenderState3D::Presentation3D{[&](flecs::entity e) {
                                            return _options.bodies->render_transform(e, alpha);
                                        }}
                                      : WorldRenderState3D::Presentation3D{});
        if (_options.bodies && _options.renderer && _options.renderer->available())
            _options.bodies->collect_ropes(*_options.renderer, scene);
        _errors = scene.diagnostics();
        if (_errors.empty())
            if (const auto* overlay = scene.overlay_if_any())
                _errors = overlay->diagnostics();
        return _errors.empty();
    } catch (const std::exception& error) {
        _errors = {"scene frame collection: " + std::string(error.what())};
        return false;
    }
}

} // namespace kin
