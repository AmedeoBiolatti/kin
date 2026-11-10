#pragma once

#include <kin/assets/asset_scope.hpp>
#include <kin/assets/content.hpp>
#include <kin/core/jobs.hpp>
#include <kin/ecs/ecs_scene.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scene/scene_asset_runtime.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kin {

enum class SceneLoadState2D { Loading, Uploading, Ready, Failed, Cancelled, Taken };

struct ScenePreloadOptions2D {
    // Binders for the game's own asset components, run on the renderer thread.
    std::vector<std::pair<std::string, Assets2D::Binder>> asset_binders;
};

// A scene ScenePreload2D prepared: its own world, assets and content snapshot.
class PreloadedScene2D : public EcsScene {
public:
    std::string_view name() const override { return _definition.name; }
    void update(SceneContext& ctx) override { ecs().run_frame(ctx.dt); }
    const SceneInstance& instance() const { return _runtime->instance(); }
    // A development reload target (kin/runtime/development.hpp) for this
    // scene: its authored entities and assets from `revision`.
    std::unique_ptr<DevelopmentChange> prepare_reload(const DevelopmentRevision& revision, std::string& error);
    const std::filesystem::path& content_root() const { return _content->root(); }
    // Keeps the images the preload uploaded in its assets' manager, so a memory
    // budget does not evict them while the scene lives. A development reload,
    // which gives the scene new assets, empties it.
    const AssetScope& preloaded_assets() const { return _preloaded; }

private:
    friend class ScenePreload2D;

    std::shared_ptr<const MemoryContent> _content;
    SceneAsset _definition;
    std::unique_ptr<SceneAssetRuntime> _runtime;
    AssetScope _preloaded;
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
