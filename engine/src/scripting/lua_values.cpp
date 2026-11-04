#include <kin/scripting/lua_values.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace kin {

sol::object component_value_to_lua(sol::state_view lua, const ComponentFieldValue& value) {
    return std::visit(
        [&](const auto& v) -> sol::object {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, bool> || std::is_same_v<V, std::string>) {
                return sol::make_object(lua, v);
            } else if constexpr (std::is_arithmetic_v<V>) {
                return sol::make_object(lua, static_cast<f64>(v));
            } else if constexpr (std::is_same_v<V, Vec2f> || std::is_same_v<V, Vec2i>) {
                return sol::make_object(lua, lua.create_table_with("x", v.x, "y", v.y));
            } else if constexpr (std::is_same_v<V, Color>) {
                return sol::make_object(lua, lua.create_table_with("r", v.r, "g", v.g, "b", v.b, "a", v.a));
            } else if constexpr (std::is_same_v<V, ComponentAssetRef>) {
                return sol::make_object(lua, v.id);
            } else if constexpr (std::is_same_v<V, ComponentEntityRef>) {
                return sol::make_object(lua, static_cast<f64>(v.id));
            } else if constexpr (std::is_same_v<V, std::vector<Vec2f>>) {
                sol::table points = lua.create_table(static_cast<int>(v.size()), 0);
                for (std::size_t i = 0; i < v.size(); ++i) {
                    points[i + 1] = lua.create_table_with("x", v[i].x, "y", v[i].y);
                }
                return sol::make_object(lua, points);
            } else {
                return sol::lua_nil; // 3D values: not for 2D scripts
            }
        },
        value);
}

std::optional<ComponentFieldValue> component_value_from_lua(ComponentFieldKind kind, const sol::object& value) {
    const auto number = [&]() -> std::optional<f64> {
        return value.get_type() == sol::type::number ? std::optional(value.as<f64>()) : std::nullopt;
    };
    const auto field = [&](const char* name, f64 fallback) {
        if (value.get_type() != sol::type::table) {
            return fallback;
        }
        const sol::object part = value.as<sol::table>()[name];
        return part.get_type() == sol::type::number ? part.as<f64>() : fallback;
    };
    const bool table = value.get_type() == sol::type::table;
    switch (kind) {
    case ComponentFieldKind::Bool:
        if (value.get_type() == sol::type::boolean) {
            return value.as<bool>();
        }
        break;
    case ComponentFieldKind::I32:
        if (const auto n = number()) {
            return static_cast<i32>(*n);
        }
        break;
    case ComponentFieldKind::I64:
        if (const auto n = number()) {
            return static_cast<i64>(*n);
        }
        break;
    case ComponentFieldKind::U32:
        if (const auto n = number(); n && *n >= 0) {
            return static_cast<u32>(*n);
        }
        break;
    case ComponentFieldKind::U64:
        if (const auto n = number(); n && *n >= 0) {
            return static_cast<u64>(*n);
        }
        break;
    case ComponentFieldKind::F32:
        if (const auto n = number()) {
            return static_cast<f32>(*n);
        }
        break;
    case ComponentFieldKind::F64:
        if (const auto n = number()) {
            return *n;
        }
        break;
    case ComponentFieldKind::String:
        if (value.get_type() == sol::type::string) {
            return value.as<std::string>();
        }
        break;
    case ComponentFieldKind::AssetRef:
        if (value.get_type() == sol::type::string) {
            return ComponentAssetRef{value.as<std::string>()};
        }
        break;
    case ComponentFieldKind::Vec2f:
        if (table) {
            return Vec2f{static_cast<f32>(field("x", 0.0)), static_cast<f32>(field("y", 0.0))};
        }
        break;
    case ComponentFieldKind::Vec2i:
        if (table) {
            return Vec2i{static_cast<i32>(field("x", 0.0)), static_cast<i32>(field("y", 0.0))};
        }
        break;
    case ComponentFieldKind::Color:
        if (table) {
            const auto channel = [&](const char* name) {
                return static_cast<u8>(std::clamp(field(name, 255.0), 0.0, 255.0));
            };
            return Color{channel("r"), channel("g"), channel("b"), channel("a")};
        }
        break;
    case ComponentFieldKind::Points:
        if (table) {
            const sol::table list = value.as<sol::table>();
            std::vector<Vec2f> points;
            for (std::size_t i = 1; i <= list.size(); ++i) {
                const sol::object item = list[i];
                if (item.get_type() != sol::type::table) {
                    return std::nullopt;
                }
                const sol::table point = item.as<sol::table>();
                points.push_back({point.get_or("x", 0.0f), point.get_or("y", 0.0f)});
            }
            return points;
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

std::optional<JsonValue> lua_to_json(const sol::object& value, int depth) {
    switch (value.get_type()) {
    case sol::type::boolean:
        return JsonValue{value.as<bool>()};
    case sol::type::number: {
        const f64 number = value.as<f64>();
        return std::isfinite(number) ? std::optional(JsonValue{number}) : std::nullopt;
    }
    case sol::type::string:
        return JsonValue{value.as<std::string>()};
    case sol::type::table: {
        if (depth >= 16) {
            return std::nullopt;
        }
        const sol::table table = value.as<sol::table>();
        const std::size_t length = table.size();
        std::size_t count = 0;
        table.for_each([&](const sol::object&, const sol::object&) { ++count; });
        if (length > 0 && count == length) {
            JsonValue::Array items;
            for (std::size_t i = 1; i <= length; ++i) {
                std::optional<JsonValue> item = lua_to_json(sol::object(table[i]), depth + 1);
                items.push_back(item ? std::move(*item) : JsonValue{});
            }
            return JsonValue{std::move(items)};
        }
        JsonValue::Object members;
        table.for_each([&](const sol::object& key, const sol::object& item) {
            std::string name;
            if (key.get_type() == sol::type::string) {
                name = key.as<std::string>();
            } else if (key.get_type() == sol::type::number) {
                char text[32];
                std::snprintf(text, sizeof(text), "%.17g", key.as<f64>());
                name = text;
            } else {
                return;
            }
            if (std::optional<JsonValue> kept = lua_to_json(item, depth + 1)) {
                members.emplace(std::move(name), std::move(*kept));
            }
        });
        return JsonValue{std::move(members)};
    }
    default:
        return std::nullopt;
    }
}

sol::object json_to_lua(sol::state_view lua, const JsonValue& value) {
    switch (value.type()) {
    case JsonValue::Type::Bool:
        return sol::make_object(lua, value.as_bool());
    case JsonValue::Type::Number: {
        const f64 number = value.as_number();
        if (std::trunc(number) == number && std::abs(number) < 9.0e15) {
            return sol::make_object(lua, static_cast<lua_Integer>(number));
        }
        return sol::make_object(lua, number);
    }
    case JsonValue::Type::String:
        return sol::make_object(lua, value.as_string());
    case JsonValue::Type::Array: {
        sol::table table = lua.create_table(static_cast<int>(value.items().size()), 0);
        for (std::size_t i = 0; i < value.items().size(); ++i) {
            table[i + 1] = json_to_lua(lua, value.items()[i]);
        }
        return sol::make_object(lua, table);
    }
    case JsonValue::Type::Object: {
        sol::table table = lua.create_table(0, static_cast<int>(value.members().size()));
        for (const auto& [name, member] : value.members()) {
            table[name] = json_to_lua(lua, member);
        }
        return sol::make_object(lua, table);
    }
    default:
        return sol::lua_nil;
    }
}

} // namespace kin
