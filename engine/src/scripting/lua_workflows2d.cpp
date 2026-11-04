#include "lua_paths.hpp"
#include "lua_reload_internal.hpp"
#include "lua_scene_context.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <kin/anim/binding.hpp>
#include <kin/anim/tween.hpp>
#include <kin/ecs/assets2d.hpp>
#include <kin/ecs/lifecycle.hpp>
#include <kin/platform/input_router.hpp>
#include <kin/platform/user_data.hpp>
#include <kin/prefab/prefab_pool2d.hpp>
#include <kin/scene/scene_preload2d.hpp>
#include <kin/scripting/workflows2d.hpp>
#include <sol/sol.hpp>
#include <stdexcept>

namespace kin {
namespace {
void mutable_call() {
    if (scripting_detail::reload_staging)
        throw std::runtime_error("workflow mutations are unavailable while preparing reload");
    if (scripting_detail::current_script_commands())
        throw std::runtime_error("workflow mutations run in update(), outside a registered ECS system");
}
bool same_lifetime(EcsEntity e, u64 generation) {
    if (!e.alive())
        return false;
    const auto* life = e.get<EntityLifetime>();
    return generation == (life ? life->generation : 0);
}
AnimValue value_from_lua(AnimValueKind kind, const sol::object& value) {
    switch (kind) {
    case AnimValueKind::Float: {
        const auto v = value.as<f32>();
        if (!std::isfinite(v))
            throw sol::error("non-finite tween value");
        return v;
    }
    case AnimValueKind::Int:
        return value.as<i32>();
    case AnimValueKind::Bool:
        return value.as<bool>();
    case AnimValueKind::String:
        return value.as<std::string>();
    case AnimValueKind::Vec2: {
        const auto t = value.as<sol::table>();
        const Vec2f v{t["x"], t["y"]};
        if (!std::isfinite(v.x) || !std::isfinite(v.y))
            throw sol::error("non-finite tween vector");
        return v;
    }
    case AnimValueKind::Color: {
        const auto t = value.as<sol::table>();
        const auto channel = [&](const char* key, int fallback) {
            return static_cast<u8>(std::clamp(t[key].get_or(fallback), 0, 255));
        };
        return Color{channel("r", 255), channel("g", 255), channel("b", 255), channel("a", 255)};
    }
    }
    throw sol::error("unsupported tween value");
}
struct LuaTween2D {
    std::shared_ptr<Tween> tween;
    EcsEntity entity;
    u64 generation = 0;
    std::shared_ptr<PropertyRegistry> properties;
    std::shared_ptr<std::vector<ComponentId>> required = std::make_shared<std::vector<ComponentId>>();
    std::function<void(const std::shared_ptr<Tween>&)> activate;
    LuaTween2D& to(const std::string& key, const sol::object& destination, f32 seconds,
                   sol::optional<std::string> easing) {
        mutable_call();
        if (!same_lifetime(entity, generation))
            throw sol::error("stale tween entity");
        const auto* found = properties->find(key);
        if (!found || !entity.raw().has(found->component))
            throw sol::error("unknown or missing tween property: " + key);
        Easing curve = Easing::Linear;
        if (easing && !parse_easing(*easing, curve))
            throw sol::error("unknown tween easing");
        const auto access = *found;
        const auto e = entity;
        const auto life = generation;
        const auto guards = required;
        guards->push_back(access.component);
        auto live = [e, life, guards] {
            return same_lifetime(e, life) &&
                   std::ranges::all_of(*guards, [&](auto component) { return e.raw().has(component); });
        };
        tween->while_alive(live).property([e, access] { return access.get(e.raw().get(access.component)); },
                                          [e, access, live](const AnimValue& value) mutable {
                                              if (!e.raw().owns(access.component))
                                                  e.raw().set_ptr(access.component, e.raw().get(access.component));
                                              if (!live())
                                                  return;
                                              if (auto* data = e.raw().get_mut(access.component)) {
                                                  access.set(data, value);
                                                  e.raw().modified(access.component);
                                              }
                                          },
                                          value_from_lua(access.kind, destination), seconds, curve);
        return *this;
    }
};
struct DeferredLeaseRelease2D {
    std::function<void()> enqueue;
    ~DeferredLeaseRelease2D() {
        if (enqueue)
            enqueue();
    }
};
struct LuaLease2D {
    std::weak_ptr<PrefabPool2D> pool;
    PrefabLease2D lease;
    std::shared_ptr<DeferredLeaseRelease2D> finalizer;
    bool alive() const {
        const auto p = pool.lock();
        return p && p->resolve(lease);
    }
    bool release() {
        mutable_call();
        const auto p = pool.lock();
        if (finalizer)
            finalizer->enqueue = {};
        return p && p->release(lease);
    }
    sol::object entity(sol::this_state state) const {
        if (scripting_detail::reload_staging)
            throw std::runtime_error("entity access is unavailable while preparing reload");
        sol::state_view lua{state};
        const auto p = pool.lock();
        const auto* instance = p ? p->resolve(lease) : nullptr;
        if (!instance)
            return sol::lua_nil;
        sol::protected_function wrap = lua.registry()["kin.entity"];
        if (!wrap.valid())
            throw sol::error("this Lua host has no entity wrapper");
        const auto weak = pool;
        const auto captured = lease;
        auto result = wrap(instance->root, std::function<bool()>{[weak, captured] {
                               const auto current = weak.lock();
                               return current && current->resolve(captured);
                           }});
        if (!result.valid())
            throw sol::error(result);
        return result.get<sol::object>();
    }
};
struct LuaPreload2D {
    std::shared_ptr<ScenePreload2D> request;
    std::function<bool()> commit;
};
std::string_view load_state(SceneLoadState2D state) {
    switch (state) {
    case SceneLoadState2D::Loading:
        return "loading";
    case SceneLoadState2D::Uploading:
        return "uploading";
    case SceneLoadState2D::Ready:
        return "ready";
    case SceneLoadState2D::Failed:
        return "failed";
    case SceneLoadState2D::Cancelled:
        return "cancelled";
    case SceneLoadState2D::Taken:
        return "taken";
    }
    return "failed";
}
sol::table memory_table(sol::state_view lua, const AssetMemoryReport& r) {
    return lua.create_table_with("resident_bytes", r.resident_bytes, "budget_bytes", r.budget_bytes, "pinned_bytes",
                                 r.pinned_bytes, "over_budget", r.over_budget, "evicted", r.evicted);
}
sol::table residency_table(sol::state_view lua, const std::vector<AssetResidency>& rows) {
    auto result = lua.create_table();
    std::size_t i = 0;
    for (const auto& row : rows) {
        auto chain = lua.create_table(), dependencies = lua.create_table();
        for (std::size_t j = 0; j < row.retained_by.size(); ++j)
            chain[j + 1] = row.retained_by[j];
        for (std::size_t j = 0; j < row.dependencies.size(); ++j)
            dependencies[j + 1] = row.dependencies[j];
        result[++i] = lua.create_table_with("key", row.key, "path", row.path, "bytes", row.bytes, "last_access",
                                            row.last_access, "references", row.external_references, "estimated",
                                            row.size_estimated, "retained_by", chain, "dependencies", dependencies);
    }
    return result;
}
} // namespace
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
    std::map<std::string, InputMap> defaults;
    std::vector<std::shared_ptr<Tween>> tweens;
    std::map<std::string, std::shared_ptr<PrefabPool2D>> pools;
    std::map<std::string, std::string> pool_sources;
    std::vector<std::pair<std::weak_ptr<PrefabPool2D>, PrefabLease2D>> released_leases;
    std::vector<std::shared_ptr<ScenePreload2D>> requests;
    std::deque<std::unique_ptr<PreloadedScene2D>> ready;
    Impl(EcsWorld& w, std::filesystem::path content) : world(w), root(std::move(content)) {}
    std::filesystem::path content_path(const std::string& path) const {
        const auto full = scripting_detail::normalized_path(root / path);
        if (!scripting_detail::path_within_root(full, root))
            throw sol::error("asset path is outside the content root");
        return full;
    }
    Assets2D& asset_service() {
        if (assets)
            return *assets;
        if (!renderer)
            throw sol::error("asset memory requires a renderer; configure ScriptWorkflows2D first");
        if (!own_assets)
            own_assets = std::make_unique<Assets2D>(*renderer, root);
        return *own_assets;
    }
    JobSystem& job_service() {
        if (jobs)
            return *jobs;
        if (!own_jobs)
            own_jobs = std::make_unique<JobSystem>(JobSystemConfig{.workers = 1});
        return *own_jobs;
    }
    InputRouter::Layer& layer(const std::string& name) {
        auto* result = router.layer(name);
        if (!result)
            throw sol::error("unknown input context: " + name);
        return *result;
    }
};
void ScriptWorkflows2D::use_content(std::shared_ptr<const MemoryContent> content) {
    _impl->content_owner = std::move(content);
    _impl->root = _impl->content_owner->root();
}
ScriptWorkflows2D::ScriptWorkflows2D(EcsWorld& w, std::filesystem::path root)
    : _impl(std::make_unique<Impl>(w, scripting_detail::normalized_path(root))) {}
ScriptWorkflows2D::~ScriptWorkflows2D() {
    for (const auto& request : _impl->requests)
        request->cancel();
    for (const auto& tween : _impl->tweens)
        tween->cancel();
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
void ScriptWorkflows2D::advance(const Input& input, f32 dt) {
    if (!std::isfinite(dt) || dt < 0)
        throw std::invalid_argument("invalid workflow dt");
    auto& p = *_impl;
    // Lua GC can run inside a system transaction. Defer dropped leases until
    // this main-thread boundary instead of mutating the ECS from a finalizer.
    const auto released = std::exchange(p.released_leases, {});
    for (const auto& [pool, lease] : released)
        if (auto current = pool.lock())
            current->release(lease);
    p.input = &input;
    p.router.update(input);
    p.rebinder.update(input, dt);
    const auto stepping = p.tweens; // property observers may schedule another tween
    for (const auto& tween : stepping)
        tween->advance(dt);
    std::erase_if(p.tweens, [](const auto& tween) { return tween->finished() || tween->cancelled(); });
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
        if (assets->manager().memory_budget())
            assets->manager().trim_memory();
        if (assets->texture_budget())
            assets->trim_texture_memory();
    }
}
std::unique_ptr<PreloadedScene2D> ScriptWorkflows2D::take_scene() {
    if (_impl->ready.empty())
        return {};
    auto result = std::move(_impl->ready.front());
    _impl->ready.pop_front();
    return result;
}
void bind_lua_workflows2d(sol::state& lua, std::function<std::shared_ptr<ScriptWorkflows2D>()> services) {
    auto get = [services = std::move(services)] {
        auto result = services();
        if (!result)
            throw sol::error("2D workflows require a loaded scene");
        return result;
    };
    lua.new_usertype<LuaTween2D>(
        "KinTween2D", sol::no_constructor, "to", sol::policies(&LuaTween2D::to, sol::returns_self()), "wait",
        sol::policies(
            [](LuaTween2D& t, f32 seconds) {
                mutable_call();
                t.tween->wait(seconds);
            },
            sol::returns_self()),
        "parallel",
        sol::policies(
            [](LuaTween2D& t) {
                mutable_call();
                t.tween->parallel();
            },
            sol::returns_self()),
        "pause",
        [](LuaTween2D& t) {
            mutable_call();
            t.tween->pause();
        },
        "resume",
        [](LuaTween2D& t) {
            mutable_call();
            t.tween->resume();
        },
        "cancel",
        [](LuaTween2D& t) {
            mutable_call();
            t.tween->cancel();
        },
        "restart",
        [](LuaTween2D& t) {
            mutable_call();
            t.activate(t.tween);
            t.tween->restart();
        },
        "paused", [](const LuaTween2D& t) { return t.tween->paused(); }, "finished",
        [](const LuaTween2D& t) { return t.tween->finished(); }, "cancelled",
        [](const LuaTween2D& t) { return t.tween->cancelled(); });
    lua.create_named_table("tweens")["to"] = [get](sol::object object, const std::string& key, sol::object destination,
                                                   f32 seconds, sol::optional<std::string> easing,
                                                   sol::this_state state) {
        mutable_call();
        auto service = get();
        auto& p = *service->_impl;
        EcsEntity entity;
        if (object.is<std::string>())
            entity = p.world.entities().find_by_name(object.as<std::string>());
        else {
            sol::state_view lua{state};
            if (object.get_type() == sol::type::table)
                object = object.as<sol::table>()["entity"].get<sol::object>();
            sol::protected_function unwrap = lua.registry()["kin.unwrap_entity"];
            if (!unwrap.valid())
                throw sol::error("this Lua host accepts entity names for tweens");
            auto result = unwrap(object);
            if (!result.valid())
                throw sol::error(result);
            entity = result.get<EcsEntity>();
        }
        if (!entity.alive() || entity.raw().world().c_ptr() != p.world.raw().c_ptr())
            throw sol::error("stale or foreign tween entity");
        auto properties = std::make_shared<PropertyRegistry>();
        register_builtin_properties(*properties, p.world);
        register_described_properties(*properties, p.world);
        const auto* life = entity.get<EntityLifetime>();
        LuaTween2D result{std::make_shared<Tween>(), entity, life ? life->generation : 0, std::move(properties)};
        result.activate = [weak = std::weak_ptr<ScriptWorkflows2D>(service)](const std::shared_ptr<Tween>& tween) {
            auto owner = weak.lock();
            if (!owner)
                throw sol::error("tween service has closed");
            auto& active = owner->_impl->tweens;
            if (std::ranges::find(active, tween) == active.end())
                active.push_back(tween);
        };
        result.to(key, destination, seconds, easing);
        p.tweens.push_back(result.tween);
        return result;
    };
    auto contexts = lua.create_named_table("input_contexts");
    contexts["push"] = [get](const std::string& name, const std::string& text, sol::optional<bool> modal,
                             sol::optional<bool> consume) {
        mutable_call();
        InputMap map;
        std::string error;
        if (name.empty() || !parse_input_map(text, map, error))
            throw sol::error("invalid input context: " + error);
        auto& p = *get()->_impl;
        p.defaults[name] = map;
        p.router.push({name, std::move(map), true, consume.value_or(true), modal.value_or(false)});
    };
    contexts["remove"] = [get](const std::string& name) {
        mutable_call();
        auto& p = *get()->_impl;
        p.defaults.erase(name);
        return p.router.remove(name);
    };
    contexts["enable"] = [get](const std::string& name, bool enabled) {
        mutable_call();
        get()->_impl->layer(name).enabled = enabled;
    };
    contexts["held"] = [get](const std::string& name, const std::string& action) {
        return get()->_impl->router.action(name, action).held;
    };
    contexts["pressed"] = [get](const std::string& name, const std::string& action) {
        return get()->_impl->router.action(name, action).pressed;
    };
    contexts["released"] = [get](const std::string& name, const std::string& action) {
        return get()->_impl->router.action(name, action).released;
    };
    contexts["axis"] = [get](const std::string& name, const std::string& axis) {
        return get()->_impl->router.axis(name, axis);
    };
    contexts["overrides"] = [get](const std::string& name) {
        auto& p = *get()->_impl;
        return serialize_input_overrides(p.defaults.at(name), p.layer(name).map);
    };
    contexts["apply_overrides"] = [get](const std::string& name, const std::string& text) {
        mutable_call();
        auto& p = *get()->_impl;
        InputMap map;
        std::string error;
        if (!apply_input_overrides(text, p.defaults.at(name), map, error))
            throw sol::error(error);
        p.layer(name).map = std::move(map);
    };
    auto save_path = [](const std::string& name) {
        if (name.empty() || name.size() > 80 ||
            !std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }))
            throw sol::error("invalid input profile name");
        return user_data_dir("kin") / "input" / (name + ".json");
    };
    contexts["save"] = [get, save_path](const std::string& name, const std::string& profile) {
        mutable_call();
        auto& p = *get()->_impl;
        return save_input_overrides(p.defaults.at(name), p.layer(name).map, save_path(profile));
    };
    contexts["load"] = [get, save_path](const std::string& name, const std::string& profile) {
        mutable_call();
        auto& p = *get()->_impl;
        std::string error;
        InputMap map;
        if (!load_input_overrides(p.defaults.at(name), map, save_path(profile), error))
            throw sol::error(error);
        p.layer(name).map = std::move(map);
    };
    auto rebind = lua.create_named_table("rebind");
    rebind["begin"] = [get](const std::string& action, sol::optional<f32> timeout) {
        mutable_call();
        auto& p = *get()->_impl;
        if (!p.input)
            throw sol::error("rebinding requires an input frame");
        p.rebinder.begin(*p.input, action, timeout.value_or(10));
    };
    rebind["cancel"] = [get] {
        mutable_call();
        get()->_impl->rebinder.cancel();
    };
    rebind["state"] = [get] {
        switch (get()->_impl->rebinder.state()) {
        case RebindState::Idle:
            return "idle";
        case RebindState::Listening:
            return "listening";
        case RebindState::Captured:
            return "captured";
        case RebindState::Cancelled:
            return "cancelled";
        case RebindState::TimedOut:
            return "timed_out";
        }
        return "idle";
    };
    rebind["candidate"] = [get] {
        const auto candidate = get()->_impl->rebinder.candidate();
        return candidate ? binding_text(*candidate) : std::string{};
    };
    rebind["conflicts"] = [get](const std::string& name) {
        auto& p = *get()->_impl;
        return sol::as_table(p.rebinder.conflicts(p.layer(name).map));
    };
    rebind["apply"] = [get](const std::string& name, sol::optional<std::string> policy) {
        mutable_call();
        auto& p = *get()->_impl;
        const auto text = policy.value_or("reject");
        if (text != "reject" && text != "replace" && text != "allow")
            throw sol::error("unknown conflict policy");
        std::string error;
        if (!p.rebinder.apply(p.layer(name).map,
                              text == "allow"     ? RebindConflict::Allow
                              : text == "replace" ? RebindConflict::Replace
                                                  : RebindConflict::Reject,
                              error))
            throw sol::error(error);
    };
    lua.new_usertype<LuaLease2D>("KinPrefabLease2D", sol::no_constructor, "alive", &LuaLease2D::alive, "release",
                                 &LuaLease2D::release, "entity", &LuaLease2D::entity);
    auto pools = lua.create_named_table("pools");
    pools["create"] = [get](const std::string& name, const std::string& path, std::size_t capacity) {
        mutable_call();
        auto& p = *get()->_impl;
        if (name.empty() || capacity == 0 || capacity > 4096)
            throw sol::error("invalid prefab pool");
        if (p.pools.contains(name)) {
            if (p.pool_sources.at(name) == path && p.pools.at(name)->capacity() == capacity)
                return;
            throw sol::error("destroy an existing pool before changing its recipe or capacity");
        }
        auto source = load_prefab_asset(p.content_path(path), p.world.components());
        if (!source.ok())
            throw sol::error(source.diagnostics.empty() ? "cannot load prefab" : source.diagnostics.front());
        auto pool = std::make_shared<PrefabPool2D>(p.world, std::move(*source.prefab),
                                                   PrefabPoolOptions2D{.capacity = capacity});
        if (!pool->error().empty())
            throw sol::error(pool->error());
        p.pool_sources[name] = path;
        p.pools.emplace(name, std::move(pool));
    };
    pools["acquire"] = [get](const std::string& name, sol::optional<f32> x,
                             sol::optional<f32> y) -> std::optional<LuaLease2D> {
        mutable_call();
        auto service = get();
        auto& p = *service->_impl;
        const auto found = p.pools.find(name);
        if (found == p.pools.end())
            throw sol::error("unknown prefab pool");
        const auto lease = found->second->acquire({.pos = {x.value_or(0), y.value_or(0)}});
        if (!lease)
            return std::nullopt;
        auto finalizer = std::make_shared<DeferredLeaseRelease2D>();
        finalizer->enqueue = [weak = std::weak_ptr<ScriptWorkflows2D>(service),
                              pool = std::weak_ptr<PrefabPool2D>(found->second), lease = *lease] {
            if (auto owner = weak.lock())
                owner->_impl->released_leases.emplace_back(pool, lease);
        };
        return LuaLease2D{found->second, *lease, std::move(finalizer)};
    };
    pools["active"] = [get](const std::string& name) { return get()->_impl->pools.at(name)->active(); };
    pools["destroy"] = [get](const std::string& name) {
        mutable_call();
        auto& p = *get()->_impl;
        p.pool_sources.erase(name);
        return p.pools.erase(name) != 0;
    };
    lua.new_usertype<LuaPreload2D>(
        "KinScenePreload2D", sol::no_constructor, "state",
        [](const LuaPreload2D& load) { return std::string(load_state(load.request->state())); }, "progress",
        [](const LuaPreload2D& load) { return load.request->progress(); }, "error",
        [](const LuaPreload2D& load) { return load.request->error(); }, "cancel",
        [](LuaPreload2D& load) {
            mutable_call();
            load.request->cancel();
        },
        "commit",
        [](LuaPreload2D& load) {
            mutable_call();
            return load.commit();
        });
    lua.create_named_table("preload")["scene"] = [get](const std::string& path) {
        mutable_call();
        auto service = get();
        auto& p = *service->_impl;
        p.content_path(path);
        if (!p.renderer)
            throw sol::error("scene preloading requires a configured renderer");
        auto request = std::make_shared<ScenePreload2D>(*p.renderer, p.job_service(), p.root, path);
        p.requests.push_back(request);
        const auto weak = std::weak_ptr<ScriptWorkflows2D>(service);
        return LuaPreload2D{request, [weak, request] {
                                auto owner = weak.lock();
                                if (!owner || request->state() != SceneLoadState2D::Ready)
                                    return false;
                                owner->_impl->ready.push_back(request->take());
                                return true;
                            }};
    };
    auto controllers = lua.create_named_table("controllers");
    controllers["devices"] = [get](sol::this_state state) {
        sol::state_view lua{state};
        auto result = lua.create_table();
        const auto* pads = get()->_impl->gamepads;
        if (pads) {
            std::size_t i = 0;
            for (const auto& d : pads->devices())
                result[++i] = lua.create_table_with("id", d.id, "name", d.name, "connected", d.state.connected);
        }
        return result;
    };
    controllers["join"] = [get](u32 device, std::size_t player) {
        mutable_call();
        auto* pads = get()->_impl->gamepads;
        return pads && player > 0 && pads->players().join(device, player - 1);
    };
    controllers["leave"] = [get](std::size_t player) {
        mutable_call();
        if (auto* pads = get()->_impl->gamepads; pads && player > 0)
            pads->players().leave(player - 1);
    };
    controllers["state"] = [get](std::size_t player, sol::this_state state) {
        sol::state_view lua{state};
        auto* pads = get()->_impl->gamepads;
        GamepadState value;
        if (pads && player > 0)
            value = pads->players().state(player - 1);
        return lua.create_table_with("connected", value.connected, "device", value.device, "left_x", value.left_x,
                                     "left_y", value.left_y, "right_x", value.right_x, "right_y", value.right_y,
                                     "left_trigger", value.left_trigger, "right_trigger", value.right_trigger,
                                     "buttons", value.buttons);
    };
    controllers["rumble"] = [get](std::size_t player, f32 low, f32 high, u32 milliseconds) {
        mutable_call();
        auto* pads = get()->_impl->gamepads;
        return pads && player > 0 && pads->rumble_player(player - 1, low, high, milliseconds);
    };
    controllers["label"] = [get](std::size_t player, u32 button) {
        auto* pads = get()->_impl->gamepads;
        if (button >= static_cast<u32>(GamepadButton::Count))
            throw sol::error("invalid gamepad button");
        const auto family = pads && player > 0 ? pads->players().state(player - 1).family : GamepadFamily::Generic;
        return std::string(gamepad_button_label(family, static_cast<GamepadButton>(button)));
    };
    auto memory = lua.create_named_table("asset_memory");
    memory["budget"] = [get](u64 cpu, u64 gpu) {
        mutable_call();
        auto& p = *get()->_impl;
        auto& assets = p.asset_service();
        p.budgets = std::pair{cpu, gpu};
        assets.manager().set_memory_budget(cpu);
        assets.set_texture_budget(gpu);
    };
    memory["report"] = [get](sol::this_state state) {
        sol::state_view lua{state};
        auto& assets = get()->_impl->asset_service();
        return lua.create_table_with("cpu", memory_table(lua, assets.manager().memory_report()), "gpu",
                                     memory_table(lua, assets.texture_memory_report()));
    };
    memory["residency"] = [get](sol::this_state state) {
        sol::state_view lua{state};
        auto& assets = get()->_impl->asset_service();
        return lua.create_table_with("cpu", residency_table(lua, assets.manager().residency()), "gpu",
                                     residency_table(lua, assets.texture_residency()));
    };
    memory["trim"] = [get](sol::this_state state) {
        mutable_call();
        sol::state_view lua{state};
        auto& assets = get()->_impl->asset_service();
        return lua.create_table_with("cpu", memory_table(lua, assets.manager().trim_memory()), "gpu",
                                     memory_table(lua, assets.trim_texture_memory()));
    };
}
sol::optional<sol::object> scripting_detail::clone_workflow_reload_value(const sol::object& value,
                                                                         sol::state_view lua) {
    if (value.is<LuaLease2D>())
        return sol::make_object(lua, value.as<LuaLease2D>());
    if (value.is<LuaPreload2D>())
        return sol::make_object(lua, value.as<LuaPreload2D>());
    if (value.is<LuaTween2D>())
        return sol::make_object(lua, value.as<LuaTween2D>());
    return sol::nullopt;
}
} // namespace kin
