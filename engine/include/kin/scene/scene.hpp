#pragma once

#include <kin/core/rng.hpp>
#include <kin/core/types.hpp>
#include <kin/platform/app.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/window.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <string_view>

namespace kin {

class SceneManager;
class JsonWriter;
class EcsWorld;
class AnimationAssetLibrary;
struct RuntimeDebugOptions;
struct RunReport;

struct SceneContext {
    App& app;
    Window& window;
    Renderer2D& renderer;
    Input& input;
    SceneManager& scenes;
    f32 dt = 0.0f;
    // Fixed-timestep interpolation factor for render contexts: fraction of the
    // next sim step elapsed at render time (accumulator / fixed_dt, in [0,1)).
    // Defaults to 1.0 so contexts that do not interpolate render the latest
    // stepped state. 0.0 in update contexts and headless fixed-frame renders.
    f32 alpha = 1.0f;
    bool is_top = true;
    RuntimeDebugOptions* debug_options = nullptr;
    // Root RNG key for the run, seeded from --seed (default 0). Scenes split it
    // for deterministic randomness instead of seeding their own generators.
    RngKey rng{};
    // Pass/fail sink for headless runs. Null in contexts that do not produce a
    // run report. Use report->fail("reason") to abort with a non-zero exit code.
    RunReport* report = nullptr;
};

class Scene {
public:
    virtual ~Scene() = default;

    virtual std::string_view name() const { return "Scene"; }

    virtual void on_enter(SceneContext&) {}
    virtual void on_exit(SceneContext&) {}
    virtual void on_suspend(SceneContext&) {}
    virtual void on_resume(SceneContext&) {}

    virtual void update(SceneContext&) {}
    virtual void render(SceneContext&) {}
    virtual void collect_actions(InputActionContext&) const {}

    // Contribute machine-readable state to the headless run report. Called once
    // after the run completes, with the writer positioned inside this scene's
    // "state" object. Override to expose assertable game state (score, board,
    // win/lose) to agents.
    virtual void write_report(JsonWriter&) const {}

    // Expose the scene's ECS world for server/inspection endpoints. Returns null
    // for scenes that do not use an ECS world. EcsScene overrides this.
    virtual EcsWorld* world() { return nullptr; }

    // Optional animation asset library hook for tooling/server diagnostics.
    virtual const AnimationAssetLibrary* animation_library() const { return nullptr; }

    virtual bool is_overlay() const { return false; }
    virtual bool updates_below() const { return false; }
};

} // namespace kin
