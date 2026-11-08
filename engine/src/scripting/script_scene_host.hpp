#pragma once

#include "lua_reload_internal.hpp"

#include <kin/ecs/system.hpp>
#include <kin/scripting/script_engine.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace kin {

struct ScriptEngineHost : ScriptSceneBindings {
    LuaRuntime* runtime = nullptr;
    std::vector<SystemId> systems;
    std::vector<std::pair<SystemDescriptor, std::function<void(SystemContext&)>>> definitions;
    std::vector<SystemId> replaceable;
    bool closing = false;
    EcsWorld& ecs() const {
        if (scripting_detail::reload_staging)
            throw std::runtime_error("live world access is unavailable while preparing reload");
        if (closing)
            throw std::runtime_error("script state is closing");
        return *world;
    }
    const ScriptComponentRegistry* script_components() const { return components; }
    std::filesystem::path resolved_script_path() const { return script_path; }
    void clear_script_entities() const {
        if (scripting_detail::reload_staging)
            throw std::runtime_error("cannot clear entities while preparing reload");
        if (clear_entities)
            clear_entities();
    }
    void set_clear_color(Color color) const {
        if (scripting_detail::reload_staging)
            throw std::runtime_error("cannot change rendering while preparing reload");
        if (set_background)
            set_background(color);
    }
    void mark_render_cache_dirty(std::string_view) const {
        if (scripting_detail::reload_staging)
            throw std::runtime_error("cannot change rendering while preparing reload");
        if (mark_render_dirty)
            mark_render_dirty();
    }
    void clear_systems() {
        if (world && !systems.empty()) {
            const auto registered = world->systems().snapshots();
            for (const auto& id : systems)
                if (std::ranges::any_of(registered, [&](const auto& entry) { return entry.id == id; }))
                    world->systems().remove(id);
        }
        systems.clear();
    }
};

namespace scripting_detail {
extern thread_local EcsCommandBuffer* script_commands;
} // namespace scripting_detail

} // namespace kin
