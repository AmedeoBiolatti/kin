#include "workflows2d_internal.hpp"

#include "lua_paths.hpp"

#include <kin/ecs/assets2d.hpp>
#include <kin/ecs/lifecycle.hpp>
#include <kin/ecs/tween.hpp>
#include <kin/scene/scene_preload2d.hpp>

#include <algorithm>
#include <cmath>
#include <sol/error.hpp>
#include <stdexcept>

namespace kin {
namespace {

// The world's shared services (ScriptWorkflows2D::shared), held weakly: the
// Lua hosts using them own them.
struct WorldWorkflows2D {
    std::weak_ptr<ScriptWorkflows2D> services;
};

} // namespace

namespace w {

void settle(LuaTweenState& tween) {
    if (tween.phase != LuaTweenState::Phase::Running) {
        return;
    }
    if (!same_lifetime(tween.entity, tween.lifetime)) {
        tween.phase = LuaTweenState::Phase::Cancelled;
        tween.reason = "the tween's entity is gone";
        return;
    }
    const Tweens* tweens = tween.entity.get<Tweens>();
    if (tweens && std::ranges::find(tweens->ended, tween.id) != tweens->ended.end()) {
        tween.phase = LuaTweenState::Phase::Completed;
        return;
    }
    const auto running = [&](const Tweens::Running& item) { return item.id == tween.id; };
    if (!tweens || std::ranges::none_of(tweens->running, running)) {
        tween.phase = LuaTweenState::Phase::Cancelled; // stopped, or replaced by a tween of the same field
        tween.reason = "the tween was stopped";
    }
}

} // namespace w

ScriptWorkflows2D::Impl::Impl(EcsWorld& w, std::filesystem::path content) : world(w), root(std::move(content)) {}

std::filesystem::path ScriptWorkflows2D::Impl::content_path(const std::string& path) const {
    const auto full = scripting_detail::normalized_path(root / path);
    if (!scripting_detail::path_within_root(full, root))
        throw sol::error("asset path is outside the content root");
    return full;
}

Assets2D& ScriptWorkflows2D::Impl::asset_service() {
    if (assets)
        return *assets;
    if (!renderer)
        throw sol::error("asset memory requires a renderer; configure ScriptWorkflows2D first");
    if (!own_assets)
        own_assets = std::make_unique<Assets2D>(*renderer, root);
    return *own_assets;
}

JobSystem& ScriptWorkflows2D::Impl::job_service() {
    if (jobs)
        return *jobs;
    if (!own_jobs)
        own_jobs = std::make_unique<JobSystem>(JobSystemConfig{.workers = 1});
    return *own_jobs;
}

std::shared_ptr<LuaTweenState> ScriptWorkflows2D::Impl::follow(EcsEntity entity, u32 id) {
    auto state = std::make_shared<LuaTweenState>();
    state->entity = entity;
    state->lifetime = entity_lifetime(entity);
    state->id = id;
    tweens.push_back(state);
    return state;
}

const InputRouter::Layer& ScriptWorkflows2D::Impl::layer(const std::string& name) const {
    const auto* result = router.layer(name);
    if (!result)
        throw sol::error("unknown input context: " + name);
    return *result;
}

void ScriptWorkflows2D::use_content(std::shared_ptr<const MemoryContent> content) {
    _impl->content_owner = std::move(content);
    _impl->root = _impl->content_owner->root();
}
ScriptWorkflows2D::ScriptWorkflows2D(EcsWorld& w, std::filesystem::path root)
    : _impl(std::make_unique<Impl>(w, scripting_detail::normalized_path(root))) {}

ScriptWorkflows2D::~ScriptWorkflows2D() {
    for (const auto& request : _impl->requests) {
        request->cancel();
    }
}

std::shared_ptr<ScriptWorkflows2D> ScriptWorkflows2D::shared(EcsWorld& world, const std::filesystem::path& content,
                                                             std::shared_ptr<ScriptWorkflows2D> given) {
    if (given && &given->world() != &world) {
        throw std::invalid_argument("workflow services belong to another world");
    }
    world.raw().component<WorldWorkflows2D>("WorldWorkflows2D");
    const WorldWorkflows2D* slot = world.raw().get<WorldWorkflows2D>();
    if (std::shared_ptr<ScriptWorkflows2D> current = slot ? slot->services.lock() : nullptr) {
        if (given && given != current) {
            throw std::invalid_argument("the world already shares other 2D workflow services");
        }
        return current;
    }
    std::shared_ptr<ScriptWorkflows2D> services =
        given ? std::move(given) : std::make_shared<ScriptWorkflows2D>(world, content);
    world.raw().set<WorldWorkflows2D>({services});
    return services;
}
void ScriptWorkflows2D::configure(Renderer2D* renderer, Gamepads* gamepads, Assets2D* assets, JobSystem* jobs) {
    if (_impl->renderer && renderer && _impl->renderer != renderer)
        throw std::invalid_argument("workflow renderer cannot change");
    if (renderer)
        _impl->renderer = renderer;
    _impl->gamepads = gamepads;
    if (assets && assets != _impl->assets && _impl->budgets) {
        assets->manager().set_memory_budget(_impl->budgets->first);
        assets->set_texture_budget(_impl->budgets->second);
    }
    if (assets)
        _impl->assets = assets;
    if (jobs)
        _impl->jobs = jobs;
}
EcsWorld& ScriptWorkflows2D::world() {
    return _impl->world;
}
InputRouter& ScriptWorkflows2D::input_router() {
    return _impl->router;
}
void ScriptWorkflows2D::step(const void* host, const Input& input, f32 dt) {
    if (!std::isfinite(dt) || dt < 0)
        throw std::invalid_argument("invalid workflow dt");
    auto& p = *_impl;
    // Every host sharing the services calls this each step; the first call of
    // a step does the work (a host that calls again begins the next step).
    if (!p.stepped.empty() && std::ranges::find(p.stepped, host) == p.stepped.end()) {
        p.stepped.push_back(host);
        return;
    }
    p.stepped.assign(1, host);
    // Lua GC can run inside a system transaction. Defer dropped leases until
    // this main-thread boundary instead of mutating the ECS from a finalizer.
    const auto released = std::exchange(p.released_leases, {});
    for (const auto& [pool, lease] : released)
        if (auto current = pool.lock())
            current->release(lease);
    p.input = &input;
    p.router.update(input);
    p.rebinder.update(input, dt);
    update_tweens(p.world, dt);
    std::erase_if(p.tweens, [](const std::shared_ptr<LuaTweenState>& tween) {
        settle(*tween);
        return tween->phase != LuaTweenState::Phase::Running;
    });
    if (p.jobs)
        p.jobs->pump();
    if (p.own_jobs)
        p.own_jobs->pump();
    for (const auto& request : p.requests) {
        if (request.use_count() == 1)
            request->cancel();
        else
            request->pump(2);
    }
    std::erase_if(p.requests, [](const auto& request) {
        const auto state = request->state();
        return state == SceneLoadState2D::Taken || state == SceneLoadState2D::Cancelled ||
               state == SceneLoadState2D::Failed;
    });
    if (auto* assets = p.assets ? p.assets : p.own_assets.get()) {
        if (assets->manager().over_memory_budget()) {
            assets->manager().trim_memory();
        }
        if (assets->over_texture_budget()) {
            assets->trim_texture_memory();
        }
    }
}

void ScriptWorkflows2D::leave(const void* host) {
    std::erase(_impl->stepped, host);
}

std::unique_ptr<PreloadedScene2D> ScriptWorkflows2D::take_scene() {
    if (_impl->ready.empty())
        return {};
    auto result = std::move(_impl->ready.front());
    _impl->ready.pop_front();
    return result;
}
} // namespace kin
