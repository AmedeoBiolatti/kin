#include "script_engine_internal.hpp"
#include "script_scene_api.hpp"
#include "lua_api_surface.hpp"
#include "lua_l10n.hpp"
#include "lua_services.hpp"
#include "lua_scene_context.hpp"

#include <kin/scripting/workflows2d.hpp>

#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace kin {

using namespace scripting_detail;

namespace {

// Check self without changing legacy argument conversions (including numeric
// colours). Sol2's unchecked member thunk dereferences a missing receiver.
template <class C> C& script_self(const sol::stack_object& value, bool property = false) {
    if (!value.is<C>())
        throw std::runtime_error(property ? "invalid property receiver" : "invalid method receiver; call the method with ':'");
    return value.as<C&>();
}
template <class Method, class C, class... A> decltype(auto) script_invoke(Method method, C& self, A&&... args) {
    if constexpr (std::is_same_v<C, ScriptEntityApi>) {
        bool checks_liveness = false;
        if constexpr (std::is_same_v<Method, bool (ScriptEntityApi::*)() const>)
            checks_liveness = method == &ScriptEntityApi::alive || method == &ScriptEntityApi::valid;
        if constexpr (std::is_same_v<Method, EcsId (ScriptEntityApi::*)() const>)
            checks_liveness = method == &ScriptEntityApi::id;
        if (!checks_liveness && (!self.scene || self.scene->closing || !self.alive()))
            throw std::runtime_error(scripting_detail::stale_entity_message);
    }
    return std::invoke(method, self, std::forward<A>(args)...);
}
// Share sol2's invocation code by Lua-visible signature, instead of generating
// another adapter for each native class's wrapper lambda.
template <class R, class C, class... A> auto script_method(R (C::*method)(A...)) {
    return std::function{[method](sol::stack_object self, A... arguments) -> R {
        return script_invoke(method, script_self<C>(self), std::forward<A>(arguments)...);
    }};
}
template <class R, class C, class... A> auto script_method(R (C::*method)(A...) const) {
    return std::function{[method](sol::stack_object self, A... arguments) -> R {
        return script_invoke(method, script_self<C>(self), std::forward<A>(arguments)...);
    }};
}
template <class R, class C> auto script_property(R (C::*method)() const) {
    return sol::property(std::function{[method](sol::this_state state) -> R {
        return std::invoke(method, script_self<C>(sol::stack_object(state, 1), true));
    }});
}


} // namespace

void ScriptEngine::register_bindings() {
    sol::state& lua = _impl->lua();
    bind_lua_workflows2d(lua, [this] { return _impl->workflows; });
    lua.registry()["kin.unwrap_entity"] = [](const sol::object& object) {
        return scripting_detail::script_entity(object);
    };
    lua.registry()["kin.entity"] = [this](EcsEntity e, std::function<bool()> lease) {
        if (!_impl->host || _impl->host->closing || !e || e.raw().world().c_ptr() != _impl->host->world->raw().c_ptr())
            throw std::runtime_error("entity does not belong to this Lua scene");
        return ScriptEntityApi{_impl->host.get(), e, std::move(lease)};
    };
    scripting_detail::bind_l10n(lua, true);
    scripting_detail::bind_services(lua);
    using scripting_detail::define_api_type;
    // clang-format off
    define_api_type<ScriptEntityApi>(lua, "KinEntity", sol::no_constructor, [](auto& type) {
        type.member("id", script_method(&ScriptEntityApi::id));
        type.member("name", script_method(&ScriptEntityApi::name));
        type.member("valid", script_method(&ScriptEntityApi::valid));
        type.member("alive", script_method(&ScriptEntityApi::alive));
        type.member("destroy", script_method(&ScriptEntityApi::destroy));
        type.member("has", script_method(&ScriptEntityApi::has));
        type.member("get", script_method(&ScriptEntityApi::get));
        type.member("component", script_method(&ScriptEntityApi::component));
        type.member("timer", script_method(&ScriptEntityApi::timer));
        type.member("cooldown", script_method(&ScriptEntityApi::cooldown));
        type.member("ready", script_method(&ScriptEntityApi::ready));
        type.member("child", script_method(&ScriptEntityApi::child));
        type.member("patch", script_method(&ScriptEntityApi::patch));
        type.member("add_script_owned", script_method(&ScriptEntityApi::add_script_owned));
        type.member("remove", script_method(&ScriptEntityApi::remove));
        type.member("set_transform", script_method(&ScriptEntityApi::set_transform));
        type.member("get_transform", script_method(&ScriptEntityApi::get_transform));
        type.member("translate", script_method(&ScriptEntityApi::translate));
        type.member("set_rect", script_method(&ScriptEntityApi::set_rect));
        type.member("get_rect", script_method(&ScriptEntityApi::get_rect));
        type.member("set_line", script_method(&ScriptEntityApi::set_line));
        type.member("get_line", script_method(&ScriptEntityApi::get_line));
    });

    define_api_type<ScriptDeferredEntityApi>(lua, "KinDeferredEntity", sol::no_constructor, [](auto& type) {
        type.member("index", script_property(&ScriptDeferredEntityApi::index));
    });

    define_api_type<ScriptCommandBufferApi>(lua, "KinCommandBuffer", sol::no_constructor, [](auto& type) {
        type.member("valid", script_property(&ScriptCommandBufferApi::valid));
        type.member("size", script_property(&ScriptCommandBufferApi::size));
        type.member("create", script_method(&ScriptCommandBufferApi::create));
        type.member("destroy", script_method(&ScriptCommandBufferApi::destroy));
        type.member("add", script_method(&ScriptCommandBufferApi::add));
        type.member("remove", script_method(&ScriptCommandBufferApi::remove));
        type.member("patch", script_method(&ScriptCommandBufferApi::patch));
    });

    define_api_type<ScriptSystemContextApi>(lua, "KinSystemContext", sol::no_constructor, [](auto& type) {
        type.member("dt", script_property(&ScriptSystemContextApi::dt));
        type.member("system_id", script_property(&ScriptSystemContextApi::system_id));
        type.member("phase", script_property(&ScriptSystemContextApi::phase));
        type.member("running_parallel", script_property(&ScriptSystemContextApi::running_parallel));
        type.member("commands", script_property(&ScriptSystemContextApi::commands));
    });

    define_api_type<ScriptSceneApi>(lua, "KinScene", sol::no_constructor, [](auto& type) {
        type.member("entity", script_method(&ScriptSceneApi::entity));
        type.member("find", script_method(&ScriptSceneApi::find));
        type.member("authored", script_method(&ScriptSceneApi::authored));
        type.member("entities", script_method(&ScriptSceneApi::entities));
        type.member("with", script_method(&ScriptSceneApi::with));
        type.member("count", script_method(&ScriptSceneApi::count));
        type.member("register_system", script_method(&ScriptSceneApi::register_system));
        type.member("clear_script_entities", script_method(&ScriptSceneApi::clear_script_entities));
        type.member("set_clear_color", script_method(&ScriptSceneApi::set_clear_color));
        type.member("mark_render_cache_dirty", script_method(&ScriptSceneApi::mark_render_cache_dirty));
    });

    define_api_type<ScriptInputApi>(lua, "KinInput", sol::no_constructor, [](auto& type) {
        type.member("pressed", script_method(&ScriptInputApi::pressed));
        type.member("held", script_method(&ScriptInputApi::held));
        type.member("released", script_method(&ScriptInputApi::released));
    });

    define_api_type<ScriptAppApi>(lua, "KinApp", sol::no_constructor, [](auto& type) {
        type.member("quit", script_method(&ScriptAppApi::quit));
    });

    define_api_type<ScriptContextApi>(lua, "KinContext", sol::no_constructor, [](auto& type) {
        type.member("dt", script_property(&ScriptContextApi::dt));
        type.member("input", script_property(&ScriptContextApi::input));
        type.member("app", script_property(&ScriptContextApi::app));
    });

    define_api_type<ScriptActionsApi>(lua, "KinActions", sol::no_constructor, [](auto& type) {
        type.member("add", script_method(&ScriptActionsApi::add));
    });

    define_api_type<ScriptDialoguePlayerApi>(lua, "KinDialoguePlayer", sol::constructors<ScriptDialoguePlayerApi()>(), [](auto& type) {
        type.member("load", script_method(&ScriptDialoguePlayerApi::load));
        type.member("start", script_method(&ScriptDialoguePlayerApi::start));
        type.member("advance", script_method(&ScriptDialoguePlayerApi::advance));
        type.member("choose_index", script_method(&ScriptDialoguePlayerApi::choose_index));
        type.member("choose_id", script_method(&ScriptDialoguePlayerApi::choose_id));
        type.member("skip_line", script_method(&ScriptDialoguePlayerApi::skip_line));
        type.member("reset", script_method(&ScriptDialoguePlayerApi::reset));
        type.member("ended", script_property(&ScriptDialoguePlayerApi::ended));
        type.member("current_node", script_property(&ScriptDialoguePlayerApi::current_node));
        type.member("view", script_method(&ScriptDialoguePlayerApi::view));
        type.member("consume_events", script_method(&ScriptDialoguePlayerApi::consume_events));
    });
    // clang-format on
}

} // namespace kin
