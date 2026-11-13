#pragma once

#include <kin/core/rng.hpp>
#include <kin/core/types.hpp>
#include <filesystem>
#include <string>
#include <string_view>

namespace kin {

class JsonValue;
class App;
class Input;
class InputActionContext;
class Renderer2D;
class Window;

namespace ui2 { class Context; }

class SceneManager;
class JsonWriter;
class EcsWorld;
class AnimationAssetLibrary;
class Renderer3D;
struct RuntimeDebugOptions;
struct RunReport;
struct Camera2D;
struct Camera3D;
class PlatformServices;
class DevelopmentSession;

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
    // stepped state. Update contexts use 0.0; fixed-frame renders use 1.0,
    // including headless App::run_for hosts.
    f32 alpha = 1.0f;
    bool is_top = true;
    RuntimeDebugOptions* debug_options = nullptr;
    // Root RNG key for the run, seeded from --seed (default 0). Scenes split it
    // for deterministic randomness instead of seeding their own generators.
    RngKey rng{};
    // Pass/fail sink for headless runs. Null in contexts that do not produce a
    // run report. Use report->fail("reason") to abort with a non-zero exit code.
    RunReport* report = nullptr;
    // Optional shared 3D renderer, owned by the runtime. Null unless requested
    // by SceneAppConfig::rendering_3d (and always null in a 2D-only build).
    Renderer3D* renderer3d = nullptr;
    // The store's or platform's services (achievements, overlay, ...), from
    // SceneAppConfig::services; null where the host started none.
    PlatformServices* services = nullptr;
    bool development = false;
    // Fixed updates the run has made before this one (update contexts: this
    // update's index from 0), or in all (render contexts). fold_in(rng, step)
    // gives each update its own key, the same in every run with the same seed.
    u64 step = 0;
};

class Scene {
public:
    virtual ~Scene() = default;

    virtual std::string_view name() const { return "Scene"; }

    virtual void on_enter(SceneContext&) {}
    virtual void on_exit(SceneContext&) {}
    virtual void on_suspend(SceneContext&) {}
    virtual void on_resume(SceneContext&) {}

    // The app itself leaving the screen and returning (phones: the home button,
    // another app, the screen turned off), given to every scene in the stack.
    // The system may end an app in the background without another word, so
    // save the player's progress in on_app_background. (on_suspend/on_resume
    // are about another scene covering this one, not about the app.)
    virtual void on_app_background(SceneContext&) {}
    virtual void on_app_foreground(SceneContext&) {}
    // The system is short of memory: release what can be loaded again.
    virtual void on_low_memory(SceneContext&) {}

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
    // The main game UI, for semantic inspection and queued actions. Scenes
    // with a ui2 Context return it here; the server never drives a debug overlay.
    virtual ui2::Context* ui_context() { return nullptr; }
    virtual DevelopmentSession* development_session() { return nullptr; }

    // Optional animation asset library hook for tooling/server diagnostics.
    virtual const AnimationAssetLibrary* animation_library() const { return nullptr; }

    // A tool's view of the game: Kin Studio's scene view runs a copy of the
    // game as a scene server (no window), has its scene show the scene file
    // being edited (view.scene) and draws it through the view's camera
    // (view.frame), so the view shows what the game's own code draws.
    //
    // Shows `file` (a scene file; what it names read from `content`, empty:
    // the game's content) in place of what the scene shows, as it reads its
    // own: a scene whose entities come from a file reads this one instead.
    // False, with why in `error`, when it can't (the default: the scene does
    // not say how).
    virtual bool view_scene(const std::filesystem::path& file, const std::filesystem::path& content, std::string& error) {
        (void)file;
        (void)content;
        error = "the scene does not show other scene files (Scene::view_scene)";
        return false;
    }
    // Changes to the scene shown, made in place (view.edit): what the tool
    // changed since it sent the file, without reading it all again (a handle
    // dragged). `entities` is an array of {"id": an entity's id in the file,
    // "components": {name: {field: value, ...}, ...}}, written as the file
    // writes them; the fields named take those values, the others keep
    // theirs. False, with why in `error`, when it can't (the default): the
    // tool then sends the whole file again (view.scene).
    virtual bool view_edit(const JsonValue& entities, std::string& error) {
        (void)entities;
        error = "the scene takes no edits in place (Scene::view_edit)";
        return false;
    }
    // The world drawn through `camera` instead of the scene's own cameras;
    // null: its own again. False when the scene has no world to draw so (the
    // default; EcsScene has).
    virtual bool set_view_camera(const Camera2D* camera) {
        (void)camera;
        return false;
    }
    // The same for a 3D world (view.frame's camera3d): drawn through `camera`
    // (where it is, how it is turned, its field of view) instead of the
    // scene's own cameras; null: its own again. False when the scene draws no
    // 3D world so (the default).
    virtual bool set_view_camera3d(const Camera3D* camera) {
        (void)camera;
        return false;
    }

    virtual bool is_overlay() const { return false; }
    virtual bool updates_below() const { return false; }
};

} // namespace kin
