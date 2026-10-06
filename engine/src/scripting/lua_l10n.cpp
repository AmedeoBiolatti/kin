#include "lua_l10n.hpp"

#include <kin/l10n/localization.hpp>

#include <sol/sol.hpp>

#include <string>
#include <vector>

namespace kin::scripting_detail {
namespace {

// A Lua table of values as message arguments. `strings` keeps the text the
// arguments view.
std::vector<MessageArg> table_args(const sol::optional<sol::table>& table, std::vector<std::string>& names,
                                   std::vector<std::string>& strings) {
    std::vector<MessageArg> args;
    if (!table) {
        return args;
    }
    // Two passes, so the vectors stop growing before views into them are taken.
    for (const auto& [key, value] : *table) {
        if (key.get_type() == sol::type::string) {
            names.push_back(key.as<std::string>());
            if (value.get_type() == sol::type::string) {
                strings.push_back(value.as<std::string>());
            } else if (value.get_type() == sol::type::boolean) {
                strings.emplace_back(value.as<bool>() ? "true" : "false");
            }
        }
    }
    std::size_t name = 0;
    std::size_t text = 0;
    for (const auto& [key, value] : *table) {
        if (key.get_type() != sol::type::string) {
            continue;
        }
        const std::string_view n = names[name++];
        if (value.get_type() == sol::type::number) {
            args.emplace_back(n, value.as<f64>());
        } else if (value.get_type() == sol::type::string || value.get_type() == sol::type::boolean) {
            args.emplace_back(n, std::string_view{strings[text++]});
        }
    }
    return args;
}

std::string translate(const std::string& key, const sol::optional<sol::table>& values) {
    const Localization* l10n = active_localization();
    if (!l10n) {
        return key;
    }
    std::vector<std::string> names;
    std::vector<std::string> strings;
    const std::vector<MessageArg> args = table_args(values, names, strings);
    return l10n->tr(key, args);
}

} // namespace

void bind_l10n(sol::state_view lua, bool allow_switching) {
    lua["tr"] = &translate;
    sol::table l10n = lua.create_named_table("l10n");
    l10n["tr"] = &translate;
    l10n["locale"] = []() -> std::string {
        const Localization* active = active_localization();
        return active ? active->locale() : std::string{};
    };
    l10n["direction"] = []() -> std::string {
        const Localization* active = active_localization();
        return active && active->direction() == TextDirection::RightToLeft ? "rtl" : "ltr";
    };
    l10n["has"] = [](const std::string& key) {
        const Localization* active = active_localization();
        return active && active->has(key);
    };
    l10n["format"] = [](const std::string& pattern, const sol::optional<sol::table>& values) -> std::string {
        std::vector<std::string> names;
        std::vector<std::string> strings;
        const std::vector<MessageArg> args = table_args(values, names, strings);
        const Localization* active = active_localization();
        return active ? active->format(pattern, args) : format_message(pattern, "en", args);
    };
    l10n["languages"] = [](sol::this_state state) {
        sol::state_view view{state};
        sol::table out = view.create_table();
        if (const Localization* active = active_localization()) {
            int i = 1;
            for (const LanguageInfo& info : active->languages()) {
                sol::table row = view.create_table();
                row["locale"] = info.locale;
                row["name"] = info.name;
                row["direction"] = info.direction == TextDirection::RightToLeft ? "rtl" : "ltr";
                out[i++] = row;
            }
        }
        return out;
    };
    if (allow_switching) {
        l10n["set_locale"] = [](const std::string& tag) -> std::string {
            Localization* active = active_localization();
            return active ? active->set_locale(tag) : std::string{};
        };
    }
}

} // namespace kin::scripting_detail
