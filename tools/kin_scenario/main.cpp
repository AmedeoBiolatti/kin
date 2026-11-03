#include <cmath>
#include <cstdio>
#include <iostream>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/scripting/lua_script.hpp>
#include <set>
#include <stdexcept>

namespace {
kin::JsonValue value(const sol::object& object, int depth = 0) {
    using J = kin::JsonValue;
    if (depth > 32)
        throw sol::error("scenario table nesting exceeds 32");
    switch (object.get_type()) {
    case sol::type::boolean:
        return J{object.as<bool>()};
    case sol::type::number: {
        const auto number = object.as<double>();
        if (!std::isfinite(number))
            throw sol::error("non-finite scenario number");
        return J{number};
    }
    case sol::type::string:
        return J{object.as<std::string>()};
    case sol::type::nil:
        return J{};
    case sol::type::table: {
        const auto table = object.as<sol::table>();
        J::Object fields;
        std::map<std::size_t, J> entries;
        for (const auto& [key, item] : table) {
            if (key.get_type() == sol::type::string)
                fields.emplace(key.as<std::string>(), value(item, depth + 1));
            else if (key.get_type() == sol::type::number) {
                const double index = key.as<double>();
                if (index < 1 || index > 10000 || std::floor(index) != index)
                    throw sol::error("invalid scenario array index");
                entries.emplace(static_cast<std::size_t>(index), value(item, depth + 1));
            } else
                throw sol::error("scenario keys must be strings or array indices");
            if (fields.size() + entries.size() > 10000)
                throw sol::error("scenario table exceeds 10000 items");
        }
        if (!fields.empty() && !entries.empty())
            throw sol::error("mixed scenario object/array");
        if (!entries.empty()) {
            J::Array array;
            for (auto& [index, item] : entries) {
                if (index != array.size() + 1)
                    throw sol::error("scenario array has holes");
                array.push_back(std::move(item));
            }
            return J{std::move(array)};
        }
        return J{std::move(fields)};
    }
    default:
        throw sol::error("scenario values must be JSON-compatible");
    }
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: kin_scenario scenario.lua\n");
        return 2;
    }
    try {
        std::optional<kin::JsonValue> document;
        kin::LuaScript script{
            {.setup =
                 [&](sol::state& lua) {
                     lua["test"] = [&](sol::table table) {
                         if (document)
                             throw sol::error("one test per scenario file");
                         table["schema"] = "kin.scenario/1";
                         document = value(sol::object(table));
                     };
                     lua["call"] = [&](const std::string& method, sol::optional<sol::table> params) {
                         auto out = lua.create_table_with("call", method);
                         if (params)
                             out["params"] = *params;
                         return out;
                     };
                     for (const auto& [name, kind] : std::vector<std::pair<std::string, std::string>>{
                              {"expect", "expect"}, {"wait_until", "wait"}, {"always", "always"}})
                         lua[name] = [&, kind](sol::table condition) { return lua.create_table_with(kind, condition); };
                     lua["tick"] = [&](int count) { return lua.create_table_with("tick", count); };
                     lua["capture"] = [&](const std::string& name) { return lua.create_table_with("capture", name); };
                 },
             .module_root = std::filesystem::path(argv[1]).parent_path(),
             .instruction_limit = 500'000}};
        if (!script.load_file(argv[1]))
            throw std::runtime_error(script.error());
        if (!document)
            throw std::runtime_error("scenario must call test{...}");
        kin::JsonWriter writer(std::cout, false);
        kin::write_json(writer, *document);
        std::cout << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
