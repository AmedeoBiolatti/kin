#pragma once

// Shared native userdata definitions: each translation unit must use the same
// C++ types when registering, pushing and checking a Lua value.
#include "script_scene_host.hpp"

#include <kin/dialogue/dialogue.hpp>
#include <kin/platform/input.hpp>

#include <functional>
#include <sol/sol.hpp>
#include <string>
#include <string_view>
#include <vector>

// These shared C++ identities are private to the engine's Lua bridge.
#if defined(__GNUC__) && !defined(_WIN32)
#define KIN_SCRIPT_API_TYPE __attribute__((visibility("hidden")))
#else
#define KIN_SCRIPT_API_TYPE
#endif

// A short private namespace keeps sol2 registry keys within Lua's short-string
// limit, including reference keys. Public Lua names are set by registration.
namespace kin::s {

struct KIN_SCRIPT_API_TYPE ScriptComponentFilter;
struct KIN_SCRIPT_API_TYPE ScriptCommandTarget;

struct KIN_SCRIPT_API_TYPE ScriptEntityApi {
    ScriptEngineHost* scene = nullptr;
    EcsEntity entity;
    std::function<bool()> lease;

    bool lease_valid() const { return !lease || lease(); }

    EcsId id() const {
        return entity.id();
    }

    std::string name() const;

    bool valid() const {
        return lease_valid() && entity.valid();
    }

    bool alive() const {
        return lease_valid() && entity.alive();
    }

    void destroy();

    bool has(std::string_view component) const;

    ScriptEntityApi& add_script_owned();

    ScriptEntityApi& remove(std::string_view component);

    sol::object get(sol::this_state state, std::string_view component) const;

    sol::object child(sol::this_state state, std::string_view name) const;

    ScriptEntityApi& patch(std::string_view component, sol::table values);

    sol::object component(sol::this_state state, std::string_view component) const;

    ScriptEntityApi& timer(std::string_view, f32 duration, sol::optional<bool> repeating);

    ScriptEntityApi& cooldown(std::string_view name, f32 seconds);

    bool ready(std::string_view) const;

    ScriptEntityApi& set_transform(f32 x, f32 y);

    sol::object get_transform(sol::this_state state) const;

    ScriptEntityApi& translate(f32 x, f32 y);

    ScriptEntityApi& set_rect(f32 w,
                              f32 h,
                              i32 r,
                              i32 g,
                              i32 b,
                              sol::optional<i32> layer,
                              sol::optional<i32> order);

    sol::object get_rect(sol::this_state state) const;

    ScriptEntityApi& set_line(f32 ax,
                              f32 ay,
                              f32 bx,
                              f32 by,
                              i32 r,
                              i32 g,
                              i32 b,
                              sol::optional<i32> layer,
                              sol::optional<i32> order);

    sol::object get_line(sol::this_state state) const;
};

struct KIN_SCRIPT_API_TYPE ScriptDeferredEntityApi {
    EcsDeferredEntity entity;

    u32 index() const {
        return entity.index;
    }
};

struct KIN_SCRIPT_API_TYPE ScriptCommandBufferApi {
    ScriptEngineHost* scene = nullptr;
    EcsCommandBuffer* commands = nullptr;

    bool valid() const {
        return commands != nullptr;
    }

    i32 size() const {
        return commands ? static_cast<i32>(commands->size()) : 0;
    }

    ScriptDeferredEntityApi create(sol::optional<std::string> name);

    ScriptCommandBufferApi& destroy(sol::object target);

    ScriptCommandBufferApi& add(sol::object target, std::string_view component);

    ScriptCommandBufferApi& remove(sol::object target, std::string_view component);

    ScriptCommandBufferApi& patch(sol::object target, std::string_view component, sol::table values);

private:
    void require_commands() const;

    ScriptCommandTarget require_target(const sol::object& target) const;
};

struct KIN_SCRIPT_API_TYPE ScriptSystemContextApi {
    ScriptEngineHost* scene = nullptr;
    EcsCommandBuffer* commands_ptr = nullptr;
    f32 delta_time = 0.0f;
    std::string id;
    std::string phase_name;
    bool parallel = false;

    f32 dt() const {
        return delta_time;
    }

    std::string system_id() const {
        return id;
    }

    std::string phase() const {
        return phase_name;
    }

    bool running_parallel() const {
        return parallel;
    }

    ScriptCommandBufferApi commands() const {
        return ScriptCommandBufferApi{scene, commands_ptr};
    }
};

struct KIN_SCRIPT_API_TYPE ScriptSceneApi {
    ScriptEngineHost* scene = nullptr;

    ScriptEntityApi entity(sol::optional<std::string> name);

    sol::object find(sol::this_state state, std::string_view name);

    sol::object authored(sol::this_state state, std::string_view id);

    sol::table entities(sol::this_state state);

    sol::table with(sol::this_state state, sol::variadic_args args);

    i32 count(sol::variadic_args args);

    void clear_script_entities();

    void set_clear_color(i32 r, i32 g, i32 b);

    void mark_render_cache_dirty();

    bool register_system(std::string_view id, sol::table descriptor, sol::protected_function run);

private:
    std::vector<ScriptComponentFilter> component_filters(sol::variadic_args args) const;

    void collect_matching(const std::vector<ScriptComponentFilter>& components, auto&& callback);
};

struct KIN_SCRIPT_API_TYPE ScriptInputApi {
    Input* input = nullptr;

    bool pressed(std::string_view action) const { return input && input->pressed(action); }

    bool held(std::string_view action) const { return input && input->held(action); }

    bool released(std::string_view action) const { return input && input->released(action); }
};

struct KIN_SCRIPT_API_TYPE ScriptAppApi {
    App* app = nullptr;

    void quit();
};

struct KIN_SCRIPT_API_TYPE ScriptContextApi {
    ScriptFrameContext ctx;

    f32 dt() const { return ctx.dt; }
    ScriptInputApi input() const { return ScriptInputApi{ctx.input}; }
    ScriptAppApi app() const { return ScriptAppApi{ctx.app}; }
};

struct KIN_SCRIPT_API_TYPE ScriptActionsApi {
    InputActionContext* actions = nullptr;

    void add(std::string_view action, sol::optional<std::string> label) {
        actions->add(action, label.value_or(""));
    }
};

struct KIN_SCRIPT_API_TYPE ScriptDialoguePlayerApi {
    DialogueDocument document;
    DialoguePlayer player;

    bool load(std::string_view path);

    void start(sol::optional<std::string> start_node);

    void advance();

    bool choose_index(i32 index);

    bool choose_id(std::string_view id);

    void skip_line();

    void reset();

    bool ended() const {
        return player.state().ended;
    }

    std::string current_node() const {
        return player.state().current_node;
    }

    sol::table view(sol::this_state state) const;

    sol::table consume_events(sol::this_state state);
};

} // namespace kin::s

namespace kin::scripting_detail {
namespace scene_api = ::kin::s;
using scene_api::ScriptEntityApi;
using scene_api::ScriptDeferredEntityApi;
using scene_api::ScriptCommandBufferApi;
using scene_api::ScriptSystemContextApi;
using scene_api::ScriptSceneApi;
using scene_api::ScriptInputApi;
using scene_api::ScriptAppApi;
using scene_api::ScriptContextApi;
using scene_api::ScriptActionsApi;
using scene_api::ScriptDialoguePlayerApi;
} // namespace kin::scripting_detail

#undef KIN_SCRIPT_API_TYPE
