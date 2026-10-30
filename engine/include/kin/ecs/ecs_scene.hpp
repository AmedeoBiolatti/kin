#pragma once

#include <kin/ecs/assets2d.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/lighting.hpp>
#include <kin/renderer/render_profile.hpp>
#include <kin/scene/scene.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kin {

struct EcsSceneRenderConfig {
    bool enabled = false;
    Color clear_color = Color::rgb(0, 0, 0);
    RenderProfile profile = top_down_2d_profile();
    f32 cull_padding = 32.0f;
    bool culling_enabled = true;
    bool static_cache_enabled = true;
    std::function<Camera2D(EcsWorld&, SceneContext&)> camera_callback;
    // Without a camera_callback: the world drawn through its first current
    // CameraComponent (kin/ecs/camera.hpp), when it has one, its viewport
    // `view_size` (zero: the window's size, in points: the size a game that
    // sets its logical size to the window's draws at).
    bool scene_cameras = true;
    Vec2f view_size{};
    // The assets its components name (TextureRenderer.asset, and what binders
    // added with add_asset_binder() give out): read from `asset_root` (empty:
    // the game's content, content_root(), else the working folder) and bound
    // before each frame is drawn. Files changed are read again every
    // `asset_check_frames` drawn frames (0: never).
    bool assets = true;
    std::filesystem::path asset_root;
    i32 asset_check_frames = 30;
    // A SceneLighting in the world darkens the world to its ambient and lights
    // it with its LightComponents, after the world and before what is on the
    // UI layer and above (kin/ecs/lighting.hpp); a ColorGrade sets the
    // renderer's colour output while it is there.
    bool scene_lighting = true;
};

class EcsScene : public Scene {
public:
    explicit EcsScene(EcsWorld* shared_world = nullptr)
        : _shared_world(shared_world) {
    }

    EcsWorld& ecs() {
        return _shared_world ? *_shared_world : _owned_world;
    }

    const EcsWorld& ecs() const {
        return _shared_world ? *_shared_world : _owned_world;
    }

    bool uses_shared_world() const {
        return _shared_world != nullptr;
    }

    EcsSceneRenderConfig& render_config() { return _render_config; }
    const EcsSceneRenderConfig& render_config() const { return _render_config; }
    StaticRenderCache& static_render_cache() { return _static_render_cache; }
    const StaticRenderCache& static_render_cache() const { return _static_render_cache; }
    void mark_render_cache_dirty(std::string_view reason = "explicit") { _static_render_cache.mark_dirty(reason); }

    // The assets its world's components name, once it has drawn (it needs the
    // renderer); nullptr before.
    Assets2D* assets() { return _assets.get(); }
    // A binder for a component of the game's own that names an asset (see
    // Assets2D), kept for when the assets are made.
    void add_asset_binder(std::string name, Assets2D::Binder binder);

    void render(SceneContext& ctx) override;
    // Drawn through `camera` (a tool's view of the game, see Scene::view_scene)
    // instead of its camera_callback and scene cameras; null: those again.
    bool set_view_camera(const Camera2D* camera) override;
    // The camera a tool views the world through, null when none: what the
    // game draws in the world itself goes through it too.
    const Camera2D* view_camera() const { return _view_camera ? &*_view_camera : nullptr; }

    EcsWorld* world() override {
        return &ecs();
    }

private:
    WorldRenderState& render_state();
    void bind_assets(SceneContext& ctx);

    EcsWorld _owned_world;
    EcsWorld* _shared_world = nullptr;
    std::unique_ptr<WorldRenderState> _render_state;
    EcsSceneRenderConfig _render_config;
    StaticRenderCache _static_render_cache;
    RenderQueue _dynamic_render_queue{RenderSortMode::LayerThenY};
    std::unique_ptr<Assets2D> _assets;
    std::vector<std::pair<std::string, Assets2D::Binder>> _asset_binders;
    i32 _asset_frames = 0;
    LightLayer _lighting;
    std::optional<Camera2D> _view_camera; // a tool's (set_view_camera)
    bool _graded = false; // a ColorGrade set the colour output
};

} // namespace kin
