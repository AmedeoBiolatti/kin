#include "lua_state_copy.hpp"

#include "lua_reload_internal.hpp"
#include "lua_tasks.hpp"

#include <cmath>
#include <set>
#include <stdexcept>
namespace kin::scripting_detail {
sol::object copy_lua_data(const sol::object& source, sol::state_view target,
                          const std::function<std::optional<sol::object>(const sol::object&)>& extra,
                          bool entity_self) {
    std::size_t count = 0, bytes = 0;
    std::set<const void*> visiting;
    std::map<const void*, sol::table> copied_tables;
    std::function<sol::object(const sol::object&, int)> copy = [&](const sol::object& value, int depth) -> sol::object {
        if (depth > 32 || ++count > 10000)
            throw std::runtime_error("reload state exceeds depth/item limit");
        switch (value.get_type()) {
        case sol::type::nil:
            return sol::make_object(target, sol::nil);
        case sol::type::boolean:
            return sol::make_object(target, value.as<bool>());
        case sol::type::string: {
            const auto text = value.as<std::string>();
            if ((bytes += text.size()) > 4 * 1024 * 1024)
                throw std::runtime_error("reload state exceeds string byte limit");
            return sol::make_object(target, text);
        }
        case sol::type::number: {
            value.push();
            const bool integer = lua_isinteger(value.lua_state(), -1);
            lua_pop(value.lua_state(), 1);
            if (integer)
                return sol::make_object(target, value.as<lua_Integer>());
            const auto number = value.as<double>();
            if (!std::isfinite(number))
                throw std::runtime_error("nonfinite reload state");
            return sol::make_object(target, number);
        }
        case sol::type::table: {
            value.push();
            const void* identity = lua_topointer(value.lua_state(), -1);
            lua_pop(value.lua_state(), 1);
            if (visiting.contains(identity))
                throw std::runtime_error("reload state contains cyclic tables");
            if (auto found = copied_tables.find(identity); found != copied_tables.end())
                return sol::make_object(target, found->second);
            visiting.insert(identity);
            auto out = target.create_table();
            copied_tables.emplace(identity, out);
            for (const auto& [key, item] : value.as<sol::table>()) {
                if (entity_self && depth == 0 && key.get_type() == sol::type::string &&
                    key.as<std::string>() == "entity")
                    continue;
                if (key.get_type() != sol::type::string && key.get_type() != sol::type::number)
                    throw std::runtime_error("reload state keys must be strings or numbers");
                out.set(copy(key, depth + 1), copy(item, depth + 1));
            }
            visiting.erase(identity);
            return sol::make_object(target, out);
        }
        case sol::type::userdata:
            if (extra)
                if (auto cloned = extra(value))
                    return *cloned;
            if (auto cloned = LuaTasks::clone_reload(value, target))
                return *cloned;
            if (auto cloned = clone_workflow_reload_value(value, target))
                return *cloned;
            throw std::runtime_error("reload state contains unsupported userdata; store authored IDs instead");
        default:
            throw std::runtime_error("reload state cannot contain Lua functions, threads or pointers");
        }
    };
    return copy(source, 0);
}
} // namespace kin::scripting_detail
