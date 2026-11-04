#include "automation_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <kin/core/json.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>
#include <kin/runtime/development.hpp>
#include <kin/ui2/context.hpp>
#include <set>
#include <sstream>
#include <stdexcept>

namespace kin {
namespace {
using J = JsonValue;
using O = J::Object;
using A = J::Array;
J str(std::string_view text) {
    return J{std::string(text)};
}
std::string encode(const J& value) {
    std::ostringstream out;
    JsonWriter json(out, false);
    write_json(json, value);
    return out.str();
}
J decode(const std::string& text) {
    auto parsed = parse_json(text);
    if (!parsed.ok())
        throw std::runtime_error(parsed.error);
    return std::move(*parsed.value);
}
ServerResponse success(const J& value) {
    return {.result_json = encode(value)};
}
ServerResponse failure(std::string text) {
    return {.ok = false, .error = std::move(text)};
}
void keys(const J& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object())
        throw std::runtime_error("expected an object");
    for (const auto& [key, v] : value.members())
        if (std::ranges::find(allowed, key) == allowed.end())
            throw std::runtime_error("unknown selector/parameter: " + key);
}
bool text_match(const J& selector, std::string_view key, std::string_view actual) {
    const auto* value = selector.find(key);
    if (!value)
        return true;
    if (!value->is_string())
        throw std::runtime_error(std::string(key) + " must be a string");
    return value->as_string() == actual;
}
bool bool_match(const J& selector, std::string_view key, bool actual) {
    const auto* value = selector.find(key);
    if (!value)
        return true;
    if (!value->is_bool())
        throw std::runtime_error(std::string(key) + " must be boolean");
    return value->as_bool() == actual;
}
i32 scene_index(ServerContext& ctx, const J& params, bool world = false) {
    i32 index = ctx.scenes().depth() - 1;
    if (const auto* v = params.find("scene")) {
        if (!v->is_number() || !std::isfinite(v->as_number()) || std::floor(v->as_number()) != v->as_number() ||
            v->as_number() < 0 || v->as_number() >= ctx.scenes().depth())
            throw std::runtime_error("scene must be an existing integer index");
        index = static_cast<i32>(v->as_number());
    } else if (world)
        while (index > 0 && !ctx.scenes().at_mut(index)->world())
            --index;
    return index;
}
std::string snapshot(ServerContext& ctx, bool detailed) {
    std::ostringstream out;
    JsonWriter writer(out, false);
    writer.begin_object()
        .field("frame", ctx.frame())
        .field("failed", ctx.report().failed)
        .field("failure_reason", ctx.report().failure_reason);
    writer.key("scenes").begin_array();
    for (i32 i = 0; i < ctx.scenes().depth(); ++i) {
        auto* scene = ctx.scenes().at_mut(i);
        writer.begin_object().field("index", i).field("name", scene->name());
        writer.key("state").begin_object();
        scene->write_report(writer);
        writer.end_object();
        if (detailed && scene->world()) {
            writer.key("world");
            write_world_snapshot_json(writer, scene->world()->snapshot());
        }
        if (scene->ui_context()) {
            writer.key("ui");
            scene->ui_context()->write_snapshot(writer);
        }
        if (auto* development = scene->development_session()) {
            writer.key("development");
            development->write_status(writer);
        }
        writer.end_object();
    }
    writer.end_array().end_object();
    return out.str();
}
bool ui_match(const ui2::SemanticElement& e, const J& selector, const std::vector<ui2::SemanticElement>& all,
              int depth = 0) {
    if (depth > 16)
        throw std::runtime_error("UI scope nesting exceeds 16");
    keys(selector,
         {"id", "key", "role", "label", "value", "visible", "enabled", "blocked", "checked", "focused", "within"});
    if (!text_match(selector, "id", std::to_string(e.id.value)) || !text_match(selector, "key", e.key) ||
        !text_match(selector, "role", e.role) || !text_match(selector, "label", e.label) ||
        !text_match(selector, "value", e.value) || !bool_match(selector, "visible", e.visible) ||
        !bool_match(selector, "enabled", e.enabled) || !bool_match(selector, "blocked", e.blocked) ||
        !bool_match(selector, "checked", e.checked) || !bool_match(selector, "focused", e.focused))
        return false;
    if (const auto* scope = selector.find("within")) {
        auto parent = e.parent;
        std::set<u64> visited;
        while (parent.value && visited.insert(parent.value).second) {
            const auto found = std::ranges::find(all, parent, &ui2::SemanticElement::id);
            if (found == all.end())
                break;
            if (ui_match(*found, *scope, all, depth + 1))
                return true;
            parent = found->parent;
        }
        return false;
    }
    return true;
}
void validate_ui(const J& selector, int depth = 0) {
    if (depth > 16)
        throw std::runtime_error("UI scope nesting exceeds 16");
    keys(selector,
         {"id", "key", "role", "label", "value", "visible", "enabled", "blocked", "checked", "focused", "within"});
    for (const auto& [key, value] : selector.members()) {
        if (key == "within")
            validate_ui(value, depth + 1);
        else if (key == "id" || key == "key" || key == "role" || key == "label" || key == "value") {
            if (!value.is_string())
                throw std::runtime_error(key + " must be a string");
        } else if (!value.is_bool())
            throw std::runtime_error(key + " must be boolean");
    }
}
std::vector<ui2::SemanticElement> current_elements(ui2::Context& ui) {
    auto all = ui.elements();
    for (auto& e : all)
        e.focused = ui.state().is_focused(e.id);
    return all;
}
ui2::Context& ui_context(ServerContext& ctx, const J& params, bool initialize) {
    auto* scene = ctx.scenes().at_mut(scene_index(ctx, params));
    if (!scene || !scene->ui_context())
        throw std::runtime_error("scene has no semantic UI (Scene::ui_context)");
    if (initialize && scene->ui_context()->elements().empty()) {
        ctx.render_frame();
        scene = ctx.scenes().at_mut(scene_index(ctx, params));
        if (!scene || !scene->ui_context())
            throw std::runtime_error("scene has no semantic UI");
    }
    return *scene->ui_context();
}
bool has_component(const EntitySnapshot& entity, const std::string& name) {
    return std::ranges::any_of(entity.components, [&](const auto& c) { return c.descriptor.name == name; });
}
J query_world(ServerContext& ctx, const J& params) {
    keys(params, {"scene", "selector", "limit"});
    auto* scene = ctx.scenes().at_mut(scene_index(ctx, params, true));
    if (!scene || !scene->world())
        throw std::runtime_error("scene has no ECS world");
    const auto& selector = params.find("selector") ? *params.find("selector") : J{O{}};
    keys(selector, {"id", "authored_id", "name", "enabled", "all", "none"});
    for (const auto key : {"id", "authored_id", "name"}) {
        if (const auto* value = selector.find(key); value && !value->is_string())
            throw std::runtime_error(std::string(key) + " must be a string");
    }
    if (const auto* value = selector.find("enabled"); value && !value->is_bool())
        throw std::runtime_error("enabled must be boolean");
    for (auto key : {"all", "none"})
        if (const auto* terms = selector.find(key)) {
            if (!terms->is_array())
                throw std::runtime_error(std::string(key) + " must be an array");
            for (const auto& term : terms->items())
                if (!term.is_string() || !scene->world()->components().find(term.as_string()))
                    throw std::runtime_error("unknown component in selector");
        }
    auto world = scene->world()->snapshot();
    std::erase_if(world.entities, [&](const EntitySnapshot& e) {
        if (!text_match(selector, "id", std::to_string(e.descriptor.id)) ||
            !text_match(selector, "authored_id", e.descriptor.authored_id) ||
            !text_match(selector, "name", e.descriptor.name) || !bool_match(selector, "enabled", e.descriptor.enabled))
            return true;
        if (const auto* all = selector.find("all"))
            for (const auto& name : all->items())
                if (!has_component(e, name.as_string()))
                    return true;
        if (const auto* none = selector.find("none"))
            for (const auto& name : none->items())
                if (has_component(e, name.as_string()))
                    return true;
        return false;
    });
    const auto total = world.entities.size();
    if (const auto* limit = params.find("limit");
        limit && (!limit->is_number() || limit->as_number() < 1 || limit->as_number() > 4096 ||
                  std::floor(limit->as_number()) != limit->as_number()))
        throw std::runtime_error("limit must be an integer in 1..4096");
    const auto limit = params.int_at("limit", 256);
    if (limit < 1 || limit > 4096)
        throw std::runtime_error("limit must be 1..4096");
    if (world.entities.size() > static_cast<std::size_t>(limit))
        world.entities.resize(static_cast<std::size_t>(limit));
    std::ostringstream out;
    JsonWriter writer(out, false);
    write_world_snapshot_json(writer, world);
    auto document = decode(out.str());
    auto members = document.members();
    // Runtime identity is a decimal string in automation responses.
    if (auto found = members.find("entities"); found != members.end()) {
        A converted;
        std::size_t index = 0;
        for (const auto& e : found->second.items()) {
            auto row = e.members();
            const auto& entity = world.entities[index++];
            const auto& actual = entity.descriptor;
            row["id"] = str(std::to_string(actual.id));
            row["parent"] = str(std::to_string(actual.parent));
            row["handle_generation"] = str(std::to_string(actual.handle_generation));
            row["creation_sequence"] = str(std::to_string(actual.creation_sequence));
            A children;
            for (auto child : actual.children)
                children.push_back(str(std::to_string(child)));
            row["children"] = J{std::move(children)};
            A components;
            std::size_t component_index = 0;
            for (const auto& encoded : row.at("components").items()) {
                auto component = encoded.members();
                const auto& original = entity.components[component_index++];
                component["id"] = str(std::to_string(original.descriptor.id));
                A fields;
                std::size_t field_index = 0;
                for (const auto& encoded_field : component.at("fields").items()) {
                    auto field = encoded_field.members();
                    const auto& value = original.fields[field_index++].value;
                    std::visit(
                        [&](const auto& v) {
                            using T = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<T, u64> || std::is_same_v<T, i64>)
                                field["value"] = str(std::to_string(v));
                            else if constexpr (std::is_same_v<T, ComponentEntityRef>)
                                field["value"] =
                                    J{O{{"id", str(std::to_string(v.id))}, {"authored_id", str(v.authored_id)}}};
                        },
                        value);
                    fields.emplace_back(std::move(field));
                }
                component["fields"] = J{std::move(fields)};
                components.emplace_back(std::move(component));
            }
            row["components"] = J{std::move(components)};
            converted.emplace_back(std::move(row));
        }
        found->second = J{std::move(converted)};
    }
    members["count"] = J{static_cast<f64>(total)};
    members["truncated"] = J{total > static_cast<std::size_t>(limit)};
    return J{std::move(members)};
}
} // namespace

const JsonValue& automation_protocol() {
    static const J protocol = [] {
        auto document = decode(R"JSON({"schema":"kin.automation/1","version":1,"methods":{
      "server.schema":{"effect":"read","inspect":true,"params":{"type":"object","properties":{},"additionalProperties":false}},
      "server.capabilities":{"effect":"read","inspect":true,"params":{"type":"object","properties":{},"additionalProperties":false}},
      "automation.snapshot":{"effect":"read","inspect":true,"params":{"type":"object","properties":{"world":{"type":"boolean"}},"additionalProperties":false}},
      "world.query":{"effect":"read","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0},"selector":{"type":"object","properties":{"id":{"type":"string"},"authored_id":{"type":"string"},"name":{"type":"string"},"enabled":{"type":"boolean"},"all":{"type":"array","items":{"type":"string"}},"none":{"type":"array","items":{"type":"string"}}},"additionalProperties":false},"limit":{"type":"integer","minimum":1,"maximum":4096}},"additionalProperties":false}},
      "ui.query":{"effect":"layout","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0},"selector":{"type":"object"}},"additionalProperties":false}},
      "game.info":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "scene.stack":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "scene.current":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "scene.actions":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "world.snapshot":{"effect":"read","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0},"raw":{"type":"boolean"},"kin":{"type":"boolean"},"events":{"type":"boolean"},"animations":{"type":"boolean"}}}},
      "animation.diagnostics":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "ui.snapshot":{"effect":"layout","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0}}}},
      "ui.action":{"effect":"input","inspect":false,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0},"id":{"type":"string"},"selector":{"type":"object"},"action":{"type":"string","enum":["click","focus","set_text","increment","decrement","set_number"]},"text":{"type":"string"}},"additionalProperties":false}},
      "sim.tick":{"effect":"step","inspect":false,"params":{"type":"object","properties":{"count":{"type":"integer","minimum":0,"maximum":1000000}},"additionalProperties":false}},
      "sim.reset":{"effect":"reset","inspect":false,"params":{"type":"object","properties":{"seed":{"anyOf":[{"type":"integer","minimum":0,"maximum":9007199254740991},{"type":"string"}]}},"additionalProperties":false}},
      "input.action":{"effect":"input","inspect":false,"params":{"type":"object","required":["name"],"properties":{"name":{"type":"string"},"mode":{"type":"string","enum":["press","hold","release"]}},"additionalProperties":false}},
      "input.key":{"effect":"input","inspect":false,"params":{"type":"object","required":["name"],"properties":{"name":{"type":"string"},"mode":{"type":"string","enum":["press","hold","release"]},"modifiers":{"type":"array","items":{"type":"string"}}},"additionalProperties":false}},
      "input.mouse":{"effect":"input","inspect":false,"params":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"dx":{"type":"number"},"dy":{"type":"number"},"wheel":{"type":"number"},"button":{"type":"string","enum":["left","middle","right","x1","x2"]},"mode":{"type":"string","enum":["press","hold","release"]},"space":{"type":"string","enum":["logical","window"]}},"additionalProperties":false}},
      "input.text":{"effect":"input","inspect":false,"params":{"type":"object","required":["text"],"properties":{"text":{"type":"string"}},"additionalProperties":false}},
      "input.drop":{"effect":"input","inspect":false,"params":{"type":"object","required":["path"],"properties":{"path":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"space":{"type":"string","enum":["logical","window"]}},"additionalProperties":false}},
      "view.screenshot":{"effect":"capture","inspect":false,"params":{"type":"object","properties":{"path":{"type":"string"}},"additionalProperties":false}},
      "view.scene":{"effect":"scene","inspect":false,"params":{"type":"object","required":["path"],"properties":{"path":{"type":"string"},"content":{"type":"string"}},"additionalProperties":false}},
      "view.frame":{"effect":"capture","inspect":false,"params":{"type":"object","required":["width","height"],"properties":{"path":{"type":"string"},"width":{"type":"integer","minimum":1},"height":{"type":"integer","minimum":1},"x":{"type":"number"},"y":{"type":"number"},"zoom":{"type":"number","exclusiveMinimum":0},"rotation":{"type":"number"}},"additionalProperties":false}},
      "server.status":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "frame.timing":{"effect":"diagnostic","inspect":true,"params":{"type":"object","properties":{"since":{"type":"integer"},"reset":{"type":"boolean"}}}},
      "ping":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "server.shutdown":{"effect":"shutdown","inspect":true,"params":{"type":"object"}},
      "sim.pause":{"effect":"control","inspect":true,"driven":false,"params":{"type":"object"}},
      "sim.resume":{"effect":"control","inspect":true,"driven":false,"params":{"type":"object"}},
      "sim.step":{"effect":"control","inspect":true,"driven":false,"params":{"type":"object","properties":{"count":{"type":"integer","minimum":0,"maximum":1000000}}}},
      "debug.breakpoints":{"effect":"debug","inspect":true,"driven":false,"params":{"type":"object","required":["files"],"properties":{"files":{"type":"object"}}}},
      "debug.pause":{"effect":"debug","inspect":true,"driven":false,"params":{"type":"object"}},
      "debug.state":{"effect":"debug","inspect":true,"driven":false,"params":{"type":"object"}},
      "debug.continue":{"effect":"debug","inspect":true,"driven":false,"params":{"type":"object"}},
      "debug.step":{"effect":"debug","inspect":true,"driven":false,"params":{"type":"object","properties":{"how":{"type":"string","enum":["into","over","out"]}}}}
    },"ui_selector":{"type":"object","properties":{"id":{"type":"string"},"key":{"type":"string"},"role":{"type":"string"},"label":{"type":"string"},"value":{"type":"string"},"visible":{"type":"boolean"},"enabled":{"type":"boolean"},"blocked":{"type":"boolean"},"focused":{"type":"boolean"},"checked":{"type":"boolean"},"within":{"$ref":"#/ui_selector"}},"additionalProperties":false}})JSON");
        const auto results = decode(R"JSON({
      "dev.status":{"type":"object","required":["generation","attempt","pending","status","error","revision","activation_tick","sources","targets"],"properties":{"generation":{"type":"integer","minimum":0},"attempt":{"type":"integer","minimum":0},"pending":{"type":"boolean"},"status":{"type":"string","enum":["idle","queued","applied","rejected"]},"error":{"type":"string"},"revision":{"type":"string"},"activation_tick":{"type":"integer"},"sources":{"type":"array"},"targets":{"type":"array","items":{"type":"string"}}}},
      "dev.reload":{"type":"object","required":["queued","generation","changes"],"properties":{"queued":{"type":"boolean"},"generation":{"type":"integer"},"changes":{"type":"object"}}},
      "server.capabilities":{"type":"object","required":["schema","protocol_version","mode","fixed_dt","rng_key","methods","scenes"],"properties":{"schema":{"type":"string"},"protocol_version":{"type":"integer"},"mode":{"type":"string","enum":["driven","realtime","inspect"]},"fixed_dt":{"type":"number"},"rng_key":{"type":"array","items":{"type":"integer"}},"methods":{"type":"object"},"scenes":{"type":"array"},"ui":{"type":"boolean"},"world":{"type":"boolean"},"reset":{"type":"boolean"},"image_io":{"type":"boolean"}}},
      "scene.current":{"type":"object","required":["name","state"],"properties":{"name":{"type":"string"},"state":{"type":"object"}}},
      "scene.stack":{"type":"object","required":["depth","scenes"],"properties":{"depth":{"type":"integer"},"scenes":{"type":"array"}}},
      "automation.snapshot":{"type":"object","required":["frame","failed","scenes"],"properties":{"frame":{"type":"integer"},"failed":{"type":"boolean"},"failure_reason":{"type":"string"},"scenes":{"type":"array"}}},
      "world.query":{"type":"object","required":["count","truncated","entities","components"],"properties":{"count":{"type":"integer"},"truncated":{"type":"boolean"},"entities":{"type":"array","items":{"type":"object","required":["id","authored_id","components"],"properties":{"id":{"type":"string"},"authored_id":{"type":"string"},"components":{"type":"array"}}}},"components":{"type":"array"}}},
      "ui.query":{"type":"object","required":["count","elements"],"properties":{"count":{"type":"integer"},"elements":{"type":"array","items":{"type":"object","required":["id","role","label"],"properties":{"id":{"type":"string"},"role":{"type":"string"},"label":{"type":"string"},"key":{"type":"string"},"parent":{"type":"string"},"enabled":{"type":"boolean"},"visible":{"type":"boolean"},"blocked":{"type":"boolean"},"checked":{"type":"boolean"},"focused":{"type":"boolean"},"value":{"type":"string"}}}}}},
      "ui.snapshot":{"type":"object","required":["frame","elements"],"properties":{"frame":{"type":"integer"},"elements":{"type":"array"}}},
      "ui.action":{"type":"object","required":["queued"],"properties":{"queued":{"type":"boolean"}}},
      "server.status":{"type":"object","required":["frame","failed"],"properties":{"frame":{"type":"integer"},"failed":{"type":"boolean"}}},
      "sim.tick":{"type":"object","required":["frame","failed","ticked"],"properties":{"frame":{"type":"integer"},"failed":{"type":"boolean"},"ticked":{"type":"integer"}}},
      "sim.reset":{"type":"object","required":["frame","reseeded"],"properties":{"frame":{"type":"integer"},"reseeded":{"type":"boolean"}}},
      "view.screenshot":{"type":"object","required":["path","width","height"],"properties":{"path":{"type":"string"},"width":{"type":"integer"},"height":{"type":"integer"}}},
      "view.frame":{"type":"object","required":["path","width","height"],"properties":{"path":{"type":"string"},"width":{"type":"integer"},"height":{"type":"integer"}}}
    })JSON");
        auto root = document.members();
        auto methods = root.at("methods").members();
        methods["dev.status"] = decode(
            R"({"effect":"read","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0}}}})");
        methods["dev.reload"] = decode(
            R"({"effect":"reload","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0},"expected_revision":{"type":"string"},"changes":{"type":"object"}}}})");
        for (auto& [name, entry] : methods) {
            auto fields = entry.members();
            auto params = fields.at("params").members();
            params["additionalProperties"] = J{false};
            fields["params"] = J{std::move(params)};
            const auto* result = results.find(name);
            fields["result"] = result ? *result : J{O{{"type", str("object")}}};
            fields["driven"] = J{entry.bool_at("driven", true)};
            entry = J{std::move(fields)};
        }
        root["methods"] = J{std::move(methods)};
        return J{std::move(root)};
    }();
    return protocol;
}

std::optional<ui2::Id> automation_ui_selector(ServerContext& ctx, const JsonValue& params, std::string& error) {
    try {
        auto& ui = ui_context(ctx, params, true);
        const auto* selector = params.find("selector");
        if (!selector)
            throw std::runtime_error("selector is required");
        validate_ui(*selector);
        const auto all = current_elements(ui);
        std::optional<ui2::Id> target;
        for (const auto& e : all)
            if (ui_match(e, *selector, all)) {
                if (target)
                    throw std::runtime_error("ambiguous UI selector");
                target = e.id;
            }
        if (!target)
            throw std::runtime_error("UI selector matched no elements");
        return target;
    } catch (const std::exception& e) {
        error = e.what();
        return {};
    }
}
std::optional<ServerResponse> dispatch_automation_command(ServerContext& ctx, const GameInfo*, std::string_view method,
                                                          const J& params, bool initialize) {
    if (method != "server.schema" && method != "server.capabilities" && method != "world.query" &&
        method != "ui.query" && method != "automation.snapshot" && method != "dev.status" && method != "dev.reload")
        return {};
    try {
        if (method == "dev.status" || method == "dev.reload") {
            keys(params, method == "dev.status"
                             ? std::initializer_list<std::string_view>{"scene"}
                             : std::initializer_list<std::string_view>{"scene", "changes", "expected_revision"});
            auto index = scene_index(ctx, params);
            auto* scene = ctx.scenes().at_mut(index);
            while (scene && !scene->development_session() && !params.contains("scene") && index > 0)
                scene = ctx.scenes().at_mut(--index);
            auto* development = scene ? scene->development_session() : nullptr;
            if (!development)
                throw std::runtime_error("scene has no development session; start with --dev");
            if (method == "dev.status") {
                std::ostringstream out;
                JsonWriter writer(out, false);
                development->write_status(writer);
                return ServerResponse{.result_json = out.str()};
            }
            if (const auto* expected = params.find("expected_revision");
                expected && (!expected->is_string() || expected->as_string() != development->active().id))
                throw std::runtime_error("development revision changed; refresh dev.status before editing");
            std::string error;
            const auto* changes = params.find("changes");
            if (!(changes ? development->request(*changes, error) : development->request(error)))
                throw std::runtime_error(error);
            J recorded;
            try {
                recorded = development->pending_changes();
            } catch (const std::exception& e) {
                development->reject_request(e.what());
                throw;
            }
            return success(J{O{{"queued", J{true}},
                               {"generation", J{static_cast<f64>(development->generation())}},
                               {"changes", std::move(recorded)}}});
        }
        if (method == "server.schema") {
            keys(params, {});
            return success(automation_protocol());
        }
        if (method == "automation.snapshot") {
            keys(params, {"world"});
            return ServerResponse{.result_json = snapshot(ctx, params.bool_at("world", false))};
        }
        if (method == "world.query")
            return success(query_world(ctx, params));
        if (method == "ui.query") {
            keys(params, {"scene", "selector"});
            auto& ui = ui_context(ctx, params, initialize);
            const auto& selector = params.find("selector") ? *params.find("selector") : J{O{}};
            validate_ui(selector);
            const auto all = current_elements(ui);
            std::set<std::string> ids;
            for (const auto& e : all)
                if (ui_match(e, selector, all))
                    ids.insert(std::to_string(e.id.value));
            std::ostringstream out;
            JsonWriter writer(out, false);
            ui.write_snapshot(writer);
            const auto document = decode(out.str());
            A elements;
            for (const auto& e : document.find("elements")->items())
                if (ids.contains(e.string_at("id")))
                    elements.push_back(e);
            return success(J{O{{"count", J{static_cast<f64>(elements.size())}}, {"elements", J{std::move(elements)}}}});
        }
        keys(params, {});
        A scenes;
        bool any_ui = false, any_world = false, any_development = false;
        for (i32 i = 0; i < ctx.scenes().depth(); ++i) {
            auto* scene = ctx.scenes().at_mut(i);
            const bool ui = scene->ui_context() != nullptr, world = scene->world() != nullptr;
            any_ui |= ui;
            any_world |= world;
            any_development |= scene->development_session() != nullptr;
            scenes.emplace_back(
                O{{"index", J{static_cast<f64>(i)}}, {"name", str(scene->name())}, {"ui", J{ui}}, {"world", J{world}}});
        }
        O available;
        for (const auto& [name, descriptor] : automation_protocol().find("methods")->members()) {
            bool enabled = ctx.automation_mode() == "inspect" ? descriptor.bool_at("inspect", false)
                                                              : descriptor.bool_at("driven", true);
            if (name.starts_with("ui."))
                enabled &= any_ui;
            if (name.starts_with("world."))
                enabled &= any_world;
            if (name.starts_with("dev."))
                enabled &= any_development;
            if (name == "sim.reset")
                enabled &= ctx.can_reset();
            if (name == "view.screenshot")
                enabled &= static_cast<bool>(KIN_ENABLE_IMAGE_IO);
            available[name] = J{enabled};
        }
        return success(J{O{{"schema", str("kin.automation-capabilities/1")},
                           {"protocol_version", J{1.0}},
                           {"mode", str(ctx.automation_mode())},
                           {"fixed_dt", J{ctx.fixed_dt()}},
                           {"frame", J{static_cast<f64>(ctx.frame())}},
                           {"rng_key", J{A{J{static_cast<f64>(ctx.rng().lo)}, J{static_cast<f64>(ctx.rng().hi)}}}},
                           {"renderer", str(ctx.renderer().backend_name())},
                           {"world", J{any_world}},
                           {"ui", J{any_ui}},
                           {"reset", J{ctx.can_reset()}},
                           {"image_io", J{static_cast<bool>(KIN_ENABLE_IMAGE_IO)}},
                           {"recording", J{ctx.is_recording()}},
                           {"development", J{any_development}},
                           {"methods", J{std::move(available)}},
                           {"scenes", J{std::move(scenes)}}}});
    } catch (const std::exception& e) {
        return failure(e.what());
    }
}

AutomationRecorder::AutomationRecorder(const std::filesystem::path& path, ServerContext& ctx, const GameInfo* game,
                                       std::optional<u64> seed) {
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    stream.open(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot open automation recording: " + path.string();
        return;
    }
    append(J{O{{"kind", str("header")},
               {"schema", str("kin.automation-recording/1")},
               {"fixed_dt", J{ctx.fixed_dt()}},
               {"rng_key", J{A{J{static_cast<f64>(ctx.rng().lo)}, J{static_cast<f64>(ctx.rng().hi)}}}},
               {"seed", seed ? str(std::to_string(*seed)) : J{}},
               {"renderer", str(ctx.renderer().backend_name())},
               {"game_id", str(game ? game->id : "")},
               {"game_version", str(game ? game->version : "")}}});
}
bool AutomationRecorder::good() const {
    return error.empty() && stream.good();
}
void AutomationRecorder::append(const J& value) {
    append_text(encode(value));
}
void AutomationRecorder::append_text(const std::string& line) {
    if (!error.empty())
        return;
    if (bytes + line.size() > 256ull * 1024 * 1024) {
        error = "automation recording exceeded 256 MiB";
        return;
    }
    stream << line << '\n';
    stream.flush();
    bytes += line.size() + 1;
    if (!stream)
        error = "automation recording write failed";
}
void AutomationRecorder::command(ServerContext& ctx, std::string_view method, const J& params,
                                 const ServerResponse& response) {
    const auto serial = pending.empty() ? sequence++ : pending.back();
    if (!pending.empty())
        pending.pop_back();
    std::ostringstream out;
    JsonWriter writer(out, false);
    writer.begin_object()
        .field("kind", "command")
        .field("sequence", serial)
        .field("frame", ctx.frame())
        .field("method", method)
        .field("ok", response.ok);
    writer.key("params");
    write_json(writer, params);
    if (response.ok)
        writer.key("result").raw(response.result_json);
    else
        writer.field("error", response.error);
    writer.end_object();
    append_text(out.str());
}
void AutomationRecorder::begin_command(ServerContext& ctx, std::string_view method, const J& params) {
    const auto serial = sequence++;
    pending.push_back(serial);
    append(J{O{{"kind", str("request")},
               {"sequence", J{static_cast<f64>(serial)}},
               {"frame", J{static_cast<f64>(ctx.frame())}},
               {"method", str(method)},
               {"params", params}}});
}
void AutomationRecorder::frame(ServerContext& ctx) {
    std::ostringstream out;
    JsonWriter writer(out, false);
    writer.begin_object()
        .field("kind", "frame")
        .field("frame", ctx.frame())
        .key("snapshot")
        .raw(snapshot(ctx, false))
        .end_object();
    append_text(out.str());
}
} // namespace kin
