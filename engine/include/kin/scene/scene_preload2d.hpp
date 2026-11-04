#pragma once
#include <kin/assets/content.hpp>
#include <kin/core/jobs.hpp>
#include <kin/ecs/ecs_scene.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scene/scene_asset_runtime.hpp>

namespace kin {
enum class SceneLoadState2D { Loading, Uploading, Ready, Failed, Cancelled, Taken };
struct ScenePreloadOptions2D {
    std::size_t max_files = 1024;
    u64 max_bytes = 256ull * 1024 * 1024;
    std::vector<std::string> additional_assets; // dynamic script/game dependencies
    std::function<void(EcsWorld&)> register_components;
    std::vector<std::pair<std::string, Assets2D::Binder>> asset_binders; // renderer-thread preparation
    std::function<void(EcsWorld&, SceneContext&)> update;
};
class PreloadedScene2D : public EcsScene {
public:
    std::string_view name() const override { return _definition.name; }
    void update(SceneContext& ctx) override {
        if (_update)
            _update(ecs(), ctx);
        else
            ecs().run_frame(ctx.dt);
    }
    const SceneInstance& instance() const { return _runtime ? _runtime->instance() : _instance; }
    std::unique_ptr<DevelopmentChange> prepare_reload(const DevelopmentRevision& revision, std::string& error);
    const std::filesystem::path& content_root() const { return _content->root(); }

private:
    friend class ScenePreload2D;
    std::shared_ptr<const MemoryContent> _content;
    SceneAsset _definition;
    SceneInstance _instance;
    std::unique_ptr<SceneAssetRuntime> _runtime;
    std::function<void(EcsWorld&, SceneContext&)> _update;
};

// A renderer-thread request handle. File reads and image decoding run on jobs;
// reflection, entity construction and bounded uploads happen in pump(). Taking
// the ready scene is the explicit commit; failures leave the current scene alone.
class ScenePreload2D {
public:
    ScenePreload2D(Renderer2D& renderer, JobSystem& jobs, std::filesystem::path content_root, std::string scene,
                   ScenePreloadOptions2D options = {});
    ~ScenePreload2D();
    ScenePreload2D(const ScenePreload2D&) = delete;
    ScenePreload2D& operator=(const ScenePreload2D&) = delete;
    void pump(std::size_t texture_budget = 2);
    void cancel();
    SceneLoadState2D state() const;
    f32 progress() const;
    const std::string& error() const;
    std::unique_ptr<PreloadedScene2D> take();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace kin
