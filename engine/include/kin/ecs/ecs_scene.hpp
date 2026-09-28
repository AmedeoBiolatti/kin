#pragma once

#include <kin/ecs/render.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/render_profile.hpp>
#include <kin/scene/scene.hpp>

#include <functional>
#include <memory>
#include <string_view>

namespace kin {

struct EcsSceneRenderConfig {
    bool enabled = false;
    Color clear_color = Color::rgb(0, 0, 0);
    RenderProfile profile = top_down_2d_profile();
    f32 cull_padding = 32.0f;
    bool culling_enabled = true;
    bool static_cache_enabled = true;
    std::function<Camera2D(EcsWorld&, SceneContext&)> camera_callback;
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

    void render(SceneContext& ctx) override;

    EcsWorld* world() override {
        return &ecs();
    }

private:
    WorldRenderState& render_state();

    EcsWorld _owned_world;
    EcsWorld* _shared_world = nullptr;
    std::unique_ptr<WorldRenderState> _render_state;
    EcsSceneRenderConfig _render_config;
    StaticRenderCache _static_render_cache;
    RenderQueue _dynamic_render_queue{RenderSortMode::LayerThenY};
};

} // namespace kin
