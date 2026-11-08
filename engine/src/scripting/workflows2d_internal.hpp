#pragma once

// Native state shared by workflow stepping and the Lua adapters.
#include <kin/platform/input_router.hpp>
#include <kin/prefab/prefab_pool2d.hpp>
#include <kin/scripting/workflows2d.hpp>

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__GNUC__) && !defined(_WIN32)
#define KIN_WORKFLOW_PRIVATE __attribute__((visibility("hidden")))
#else
#define KIN_WORKFLOW_PRIVATE
#endif

// Shared state outlives an individual Lua handle and is settled by native steps.
namespace kin::w {

// A tween started from Lua (kin::tween), followed until it ends. The services
// settle it after each update_tweens, so its state outlives Tweens::ended.
struct KIN_WORKFLOW_PRIVATE LuaTweenState {
    enum class Phase : u8 { Running, Completed, Cancelled };
    EcsEntity entity;
    u64 lifetime = 0;
    u32 id = 0;
    Phase phase = Phase::Running;
    std::string reason; // why it was cancelled
};

KIN_WORKFLOW_PRIVATE void settle(LuaTweenState& tween);

} // namespace kin::w

#undef KIN_WORKFLOW_PRIVATE

namespace kin {

class ScenePreload2D;
using w::LuaTweenState;

struct ScriptWorkflows2D::Impl {
    EcsWorld& world;
    std::filesystem::path root;
    std::shared_ptr<const MemoryContent> content_owner;
    Renderer2D* renderer = nullptr;
    Gamepads* gamepads = nullptr;
    Assets2D* assets = nullptr;
    JobSystem* jobs = nullptr;
    std::unique_ptr<Assets2D> own_assets;
    std::optional<std::pair<u64, u64>> budgets;
    std::unique_ptr<JobSystem> own_jobs;
    const Input* input = nullptr;
    InputRouter router;
    InputRebinder rebinder;
    std::vector<std::shared_ptr<LuaTweenState>> tweens; // followed until they end
    std::vector<const void*> stepped;                   // the hosts that called step() since it last ran
    std::map<std::string, std::shared_ptr<PrefabPool2D>> pools;
    std::map<std::string, std::string> pool_sources;
    std::vector<std::pair<std::weak_ptr<PrefabPool2D>, PrefabLease2D>> released_leases;
    std::vector<std::shared_ptr<ScenePreload2D>> requests;
    std::deque<std::unique_ptr<PreloadedScene2D>> ready;
    Impl(EcsWorld& w, std::filesystem::path content);
    std::filesystem::path content_path(const std::string& path) const;
    Assets2D& asset_service();
    JobSystem& job_service();
    std::shared_ptr<LuaTweenState> follow(EcsEntity entity, u32 id);
    const InputRouter::Layer& layer(const std::string& name) const;
};

} // namespace kin
