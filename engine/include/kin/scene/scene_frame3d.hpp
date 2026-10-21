#pragma once

#include <kin/ecs/scene3d.hpp>
#include <kin/ecs/system.hpp>

namespace kin {

class AudioEngine;
class AudioCatalog;
class Renderer3D;
class WorldPhysicsState3D;

struct SceneFrameOptions3D {
    ParticleLimits3D particles;
    WorldPhysicsState3D* bodies = nullptr;
    Renderer3D* renderer = nullptr; // optional; used only to mesh ropes during collection
    AudioEngine* audio = nullptr;
    const AudioCatalog* audio_catalog = nullptr;
    // For an existing native simulation (e.g. Summit), instead of bodies->step;
    // specifying both is rejected so physics cannot accidentally advance twice.
    std::function<void(f32)> physics_step;
};

// Non-owning frame driver. The world, render state and option pointers outlive
// it. Owns no world, physics, assets, scene, view or GPU resources. Owner thread
// only; external edits happen between calls. Register/bind components first.
class SceneFrame3D {
public:
    SceneFrame3D(EcsWorld& world, WorldRenderState3D& rendering, SceneFrameOptions3D options = {});
    SceneFrame3D(const SceneFrame3D&) = delete;
    SceneFrame3D& operator=(const SceneFrame3D&) = delete;

    // ECS systems then native physics, animation/clocks/particles and audio,
    // at the same phase boundaries as SceneSession3D. hooks.after runs AFTER
    // systems/command buffers but BEFORE the native stage (e.g. lifecycle work).
    // Failure stops later phases; completed gameplay work is not rolled back.
    Status3D step(f32 dt, const SystemPhaseHooks& hooks = {});
    // For hosts with their own run_frame hooks. Call once after each phase;
    // do not also call step(). Editor validates and propagates late changes.
    Status3D after_phase(SystemPhase phase, f32 dt);
    // Clears draws/settings, applies authored settings and collects the world.
    // Caller owns the destination and view options; add custom draws afterwards
    // and submit with its own Renderer3D/View3D. No view is allocated here.
    // An authored SceneEnvironment3D sets the picture in `view` (samples,
    // background, hdr, exposure, bloom); `view->pick` is the caller's and
    // survives it (scene3d_view_options), the same as SceneSession3DOptions::view.pick.
    Status3D collect(Scene3D& scene, ViewOptions3D* view = nullptr, f32 alpha = 1);
    const std::vector<std::string>& diagnostics() const { return _errors; }

private:
    bool step_operation(f32 dt, const SystemPhaseHooks& hooks = {});
    bool after_phase_operation(SystemPhase phase, f32 dt);
    bool collect_operation(Scene3D& scene, ViewOptions3D* view = nullptr, f32 alpha = 1);
    bool validate();
    EcsWorld& _world;
    WorldRenderState3D& _rendering;
    SceneFrameOptions3D _options;
    std::vector<std::string> _errors;
    bool _running = false;
};

} // namespace kin
