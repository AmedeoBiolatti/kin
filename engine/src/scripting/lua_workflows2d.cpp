#include <kin/scripting/workflows2d.hpp>

#include "lua_api_surface.hpp"
#include "lua_paths.hpp"
#include "lua_reload_internal.hpp"
#include "lua_scene_context.hpp"
#include "lua_workflows2d_internal.hpp"
#include "workflows2d_internal.hpp"

#include <kin/ecs/assets2d.hpp>
#include <kin/ecs/lifecycle.hpp>
#include <kin/ecs/tween.hpp>
#include <kin/platform/input_router.hpp>
#include <kin/prefab/prefab_pool2d.hpp>
#include <kin/scene/scene_preload2d.hpp>
#include <kin/scripting/lua_values.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <sol/sol.hpp>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace kin {
namespace {

void mutable_call() {
    if (scripting_detail::reload_staging)
        throw std::runtime_error("workflow mutations are unavailable while preparing reload");
    if (scripting_detail::current_script_commands())
        throw std::runtime_error("workflow mutations run in update(), outside a registered ECS system");
}

// What tweens.to returns: task.await(tween) waits for it.
struct LuaTween2D {
    std::shared_ptr<LuaTweenState> state;

    std::string phase() const {
        switch (state->phase) {
        case LuaTweenState::Phase::Running:
            return "running";
        case LuaTweenState::Phase::Completed:
            return "completed";
        case LuaTweenState::Phase::Cancelled:
            return "cancelled";
        }
        return "cancelled";
    }

    bool cancel() {
        mutable_call();
        settle(*state);
        if (state->phase != LuaTweenState::Phase::Running) {
            return false;
        }
        stop_tween(state->entity, state->id);
        state->phase = LuaTweenState::Phase::Cancelled;
        state->reason = "the tween was cancelled";
        return true;
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
        if (!result.valid()) {
            sol::error error = result;
            throw error;
        }
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

void bind_lua_workflows2d(sol::state& lua, std::function<std::shared_ptr<ScriptWorkflows2D>()> services) {
    auto get = [services = std::move(services)] {
        auto result = services();
        if (!result)
            throw sol::error("2D workflows require a loaded scene");
        return result;
    };
    // clang-format off
    scripting_detail::new_api_type<LuaTween2D>(lua, "KinTween2D",
        sol::no_constructor,
        "state", &LuaTween2D::phase,
        "error", [](const LuaTween2D& tween) { return tween.state->reason; },
        "cancel", &LuaTween2D::cancel);
    // clang-format on
    // The entity a tween moves: a name, an entity, or a table holding one (a self).
    const auto tween_target = [](EcsWorld& world, sol::object target, sol::this_state state) {
        EcsEntity entity;
        if (target.is<std::string>()) {
            entity = world.entities().find_by_name(target.as<std::string>());
        } else {
            sol::state_view lua{state};
            if (target.get_type() == sol::type::table) {
                target = target.as<sol::table>()["entity"].get<sol::object>();
            }
            sol::protected_function unwrap = lua.registry()["kin.unwrap_entity"];
            if (!unwrap.valid()) {
                throw sol::error("this Lua host accepts entity names for tweens");
            }
            auto result = unwrap(target);
            if (!result.valid()) {
                sol::error error = result;
                throw error;
            }
            entity = result.get<EcsEntity>();
        }
        if (!entity || entity.raw().world().c_ptr() != world.raw().c_ptr()) {
            throw sol::error("stale or foreign tween entity");
        }
        return entity;
    };
    sol::table tweens = lua.create_named_table("tweens");
    tweens["to"] = [get, tween_target](sol::object target, const std::string& property, const sol::object& to,
                                       f32 seconds, const sol::object& easing_or_options, const sol::object& options,
                                       sol::this_state state) {
        mutable_call();
        auto& p = *get()->_impl;
        const EcsEntity entity = tween_target(p.world, std::move(target), state);
        std::string error;
        const u32 id =
            tween(p.world, entity,
                  scripting_detail::lua_tween_def(p.world, property, to, seconds, easing_or_options, options), &error);
        if (id == 0) {
            throw sol::error("tween: " + error);
        }
        return LuaTween2D{p.follow(entity, id)};
    };
    // A tween an entity script started (self:tween's id), to wait for.
    tweens["of"] = [get, tween_target](sol::object target, u32 id, sol::this_state state) {
        auto& p = *get()->_impl;
        return LuaTween2D{p.follow(tween_target(p.world, std::move(target), state), id)};
    };
    // Repeated std::function signatures share sol2 invocation adapters.
    // Keep unique signatures as lambdas to avoid adding another wrapper.
    auto contexts = lua.create_named_table("input_contexts");
    contexts["push"] = [get](const std::string& name, const std::string& text, sol::optional<bool> modal,
                             sol::optional<bool> consume) {
        mutable_call();
        InputMap map;
        std::string error;
        if (name.empty() || !parse_input_map(text, map, error))
            throw sol::error("invalid input context: " + error);
        auto& p = *get()->_impl;
        if (p.router.layer(name))
            throw sol::error("input context already exists: " + name);
        p.router.push({name, std::move(map), true, consume.value_or(true), modal.value_or(false)});
    };
    contexts["remove"] = std::function<bool(const std::string&)>{
        [get](const std::string& name) {
            mutable_call();
            return get()->_impl->router.remove(name);
        }};
    contexts["enable"] = [get](const std::string& name, bool enabled) {
        mutable_call();
        auto& p = *get()->_impl;
        p.layer(name);
        p.router.set_enabled(name, enabled);
    };
    contexts["held"] = std::function<bool(const std::string&, const std::string&)>{
        [get](const std::string& name, const std::string& action) {
            return get()->_impl->router.action(name, action).held;
        }};
    contexts["pressed"] = std::function<bool(const std::string&, const std::string&)>{
        [get](const std::string& name, const std::string& action) {
            return get()->_impl->router.action(name, action).pressed;
        }};
    contexts["released"] = std::function<bool(const std::string&, const std::string&)>{
        [get](const std::string& name, const std::string& action) {
            return get()->_impl->router.action(name, action).released;
        }};
    contexts["axis"] = [get](const std::string& name, const std::string& axis) {
        return get()->_impl->router.axis(name, axis);
    };
    contexts["overrides"] = [get](const std::string& name) {
        const auto& layer = get()->_impl->layer(name);
        return serialize_input_overrides(layer.defaults, layer.map);
    };
    contexts["apply_overrides"] = std::function<void(const std::string&, const std::string&)>{
        [get](const std::string& name, const std::string& text) {
            mutable_call();
            auto& p = *get()->_impl;
            InputMap map;
            std::string error;
            if (!apply_input_overrides(text, p.layer(name).defaults, map, error))
                throw sol::error(error);
            p.router.set_map(name, std::move(map));
        }};
    // The running game's own profile folder (input_profile_path), beside its saves.
    auto save_path = [](const std::string& profile) {
        std::string error;
        auto path = input_profile_path(profile, error);
        if (path.empty())
            throw sol::error(error);
        return path;
    };
    contexts["save"] = std::function<bool(const std::string&, const std::string&)>{
        [get, save_path](const std::string& name, const std::string& profile) {
            mutable_call();
            const auto& layer = get()->_impl->layer(name);
            return save_input_overrides(layer.defaults, layer.map, save_path(profile));
        }};
    contexts["load"] = std::function<void(const std::string&, const std::string&)>{
        [get, save_path](const std::string& name, const std::string& profile) {
            mutable_call();
            auto& p = *get()->_impl;
            std::string error;
            InputMap map;
            if (!load_input_overrides(p.layer(name).defaults, map, save_path(profile), error))
                throw sol::error(error);
            p.router.set_map(name, std::move(map));
        }};
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
        InputMap map = p.layer(name).map;
        if (!p.rebinder.apply(map,
                              text == "allow"     ? RebindConflict::Allow
                              : text == "replace" ? RebindConflict::Replace
                                                  : RebindConflict::Reject,
                              error))
            throw sol::error(error);
        p.router.set_map(name, std::move(map));
    };
    // clang-format off
    scripting_detail::new_api_type<LuaLease2D>(lua, "KinPrefabLease2D",
        sol::no_constructor,
        "alive", &LuaLease2D::alive,
        "release", &LuaLease2D::release,
        "entity", &LuaLease2D::entity);
    // clang-format on
    auto pools = lua.create_named_table("pools");
    pools["create"] = [get](const std::string& name, const std::string& path, std::size_t capacity) {
        mutable_call();
        auto& p = *get()->_impl;
        if (name.empty()) {
            throw sol::error("a prefab pool needs a name");
        }
        if (p.pools.contains(name)) {
            if (p.pool_sources.at(name) == path && p.pools.at(name)->capacity() == capacity)
                return;
            throw sol::error("destroy an existing pool before changing its recipe or capacity");
        }
        auto source = load_prefab_asset(p.content_path(path), p.world.components());
        if (!source.ok())
            throw sol::error(source.diagnostics.empty() ? "cannot load prefab" : source.diagnostics.front());
        std::shared_ptr<PrefabPool2D> pool;
        try {
            pool = std::make_shared<PrefabPool2D>(p.world, std::move(*source.prefab),
                                                  PrefabPoolOptions2D{.capacity = capacity});
        } catch (const std::exception& error) {
            throw sol::error(std::string("pools.create: ") + error.what());
        }
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
    pools["destroy"] = std::function<bool(const std::string&)>{
        [get](const std::string& name) {
            mutable_call();
            auto& p = *get()->_impl;
            p.pool_sources.erase(name);
            return p.pools.erase(name) != 0;
        }};
    // clang-format off
    scripting_detail::new_api_type<LuaPreload2D>(lua, "KinScenePreload2D",
        sol::no_constructor,
        "state", [](const LuaPreload2D& load) { return std::string(load_state(load.request->state())); },
        "progress", [](const LuaPreload2D& load) { return load.request->progress(); },
        "error", [](const LuaPreload2D& load) { return load.request->error(); },
        "cancel", [](LuaPreload2D& load) {
            mutable_call();
            load.request->cancel();
        },
        "commit", [](LuaPreload2D& load) {
            mutable_call();
            return load.commit();
        });
    // clang-format on
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
    controllers["devices"] = std::function<sol::table(sol::this_state)>{
        [get](sol::this_state state) {
            sol::state_view lua{state};
            auto result = lua.create_table();
            const auto* pads = get()->_impl->gamepads;
            if (pads) {
                std::size_t i = 0;
                for (const auto& d : pads->devices())
                    result[++i] = lua.create_table_with("id", d.id, "name", d.name, "connected", d.state.connected);
            }
            return result;
        }};
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
    memory["report"] = std::function<sol::table(sol::this_state)>{
        [get](sol::this_state state) {
            sol::state_view lua{state};
            auto& assets = get()->_impl->asset_service();
            return lua.create_table_with("cpu", memory_table(lua, assets.manager().memory_report()), "gpu",
                                         memory_table(lua, assets.texture_memory_report()));
        }};
    memory["residency"] = std::function<sol::table(sol::this_state)>{
        [get](sol::this_state state) {
            sol::state_view lua{state};
            auto& assets = get()->_impl->asset_service();
            return lua.create_table_with("cpu", residency_table(lua, assets.manager().residency()), "gpu",
                                         residency_table(lua, assets.texture_residency()));
        }};
    memory["trim"] = std::function<sol::table(sol::this_state)>{
        [get](sol::this_state state) {
            mutable_call();
            sol::state_view lua{state};
            auto& assets = get()->_impl->asset_service();
            return lua.create_table_with("cpu", memory_table(lua, assets.manager().trim_memory()), "gpu",
                                         memory_table(lua, assets.trim_texture_memory()));
        }};
}
TweenDef scripting_detail::lua_tween_def(EcsWorld& world, const std::string& property, const sol::object& to,
                                         f32 seconds, const sol::object& easing_or_options,
                                         const sol::object& options_given) {
    TweenDef def{.property = property, .seconds = seconds};
    // `to` as the field's kind says (a number, {x, y}, {r, g, b, a}...).
    const std::size_t dot = property.find('.');
    const ComponentDescriptor* component =
        dot == std::string::npos ? nullptr : world.components().find(property.substr(0, dot));
    if (component) {
        const auto field =
            std::ranges::find(component->fields, property.substr(dot + 1), &ComponentFieldDescriptor::name);
        if (field != component->fields.end()) {
            const std::optional<ComponentFieldValue> value = component_value_from_lua(field->kind, to);
            if (!value) {
                throw sol::error("tween: " + property + " takes a " + component_field_kind_name(field->kind));
            }
            std::visit(
                [&](const auto& typed) {
                    using V = std::decay_t<decltype(typed)>;
                    if constexpr (std::is_same_v<V, f32> || std::is_same_v<V, Vec2f> || std::is_same_v<V, Color> ||
                                  std::is_same_v<V, i32> || std::is_same_v<V, bool> || std::is_same_v<V, std::string>) {
                        def.to = typed;
                    } else {
                        throw sol::error("tween: " + property +
                                         " is not a number, a pair, a colour, true/false or text");
                    }
                },
                *value);
        }
    }
    sol::object options = options_given;
    if (easing_or_options.get_type() == sol::type::string) {
        if (!parse_easing(easing_or_options.as<std::string>(), def.easing)) {
            throw sol::error("tween: no easing is called '" + easing_or_options.as<std::string>() + "'");
        }
    } else if (easing_or_options.get_type() == sol::type::table) {
        options = easing_or_options;
    }
    if (options.get_type() == sol::type::table) {
        const sol::table given = options.as<sol::table>();
        def.delay = given.get_or("delay", 0.0f);
        def.repeats = given.get_or("repeats", 0);
        def.yoyo = given.get_or("yoyo", false);
        def.relative = given.get_or("relative", false);
        if (const sol::object easing = given["easing"];
            easing.get_type() == sol::type::string && !parse_easing(easing.as<std::string>(), def.easing)) {
            throw sol::error("tween: no easing is called '" + easing.as<std::string>() + "'");
        }
    }
    return def;
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
