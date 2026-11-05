#include "automation_internal.hpp"

#include "runtime_internal.hpp"

#include <kin/core/json.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>
#include <kin/ui2/context.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

namespace kin {
namespace {

using J = JsonValue;
using O = J::Object;
using A = J::Array;
using runtime_detail::error_result;
using runtime_detail::exact_integer;
using runtime_detail::ok_result;

constexpr i32 max_ui_scope_depth = 16;

J str(std::string_view text) {
    return J{std::string(text)};
}

J number(f64 value) {
    return J{value};
}

std::string encode(const J& value) {
    std::ostringstream out;
    JsonWriter json(out, false);
    write_json(json, value);
    return out.str();
}

J decode(const std::string& text) {
    auto parsed = parse_json(text);
    if (!parsed.ok()) {
        throw std::runtime_error(parsed.error);
    }
    return std::move(*parsed.value);
}

template <typename Write> std::string written(Write&& write) {
    std::ostringstream out;
    JsonWriter json(out, false);
    write(json);
    return out.str();
}

void keys(const J& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) {
        throw std::runtime_error("expected an object");
    }
    for (const auto& [key, item] : value.members()) {
        if (std::ranges::find(allowed, key) == allowed.end()) {
            throw std::runtime_error("unknown selector/parameter: " + key);
        }
    }
}

// The scene `params` names (an index), else the top one; with `world`, else
// the topmost with an ECS world (an overlay without one shows the game's).
i32 scene_index(ServerContext& ctx, const J& params, bool world = false) {
    i32 index = ctx.scenes().depth() - 1;
    if (const J* value = params.find("scene")) {
        const auto chosen = exact_integer(*value, 0, ctx.scenes().depth() - 1);
        if (!chosen) {
            throw std::runtime_error("scene must be an existing integer index");
        }
        return static_cast<i32>(*chosen);
    }
    while (world && index > 0 && !ctx.scenes().at_mut(index)->world()) {
        --index;
    }
    return index;
}

std::string snapshot(ServerContext& ctx, bool detailed) {
    return written([&](JsonWriter& writer) {
        writer.begin_object()
            .field("frame", ctx.frame())
            .field("failed", ctx.report().failed)
            .field("failure_reason", ctx.report().failure_reason);
        writer.key("scenes").begin_array();
        for (i32 i = 0; i < ctx.scenes().depth(); ++i) {
            Scene* scene = ctx.scenes().at_mut(i);
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
#if KIN_ENABLE_DEVELOPMENT
            if (const DevelopmentSession* development = scene->development_session()) {
                writer.key("development");
                development->write_summary(writer);
            }
#endif
            writer.end_object();
        }
        writer.end_array().end_object();
    });
}

// UI selectors: text fields match exactly, flags match a boolean, and
// `within` requires an ancestor matching a nested selector.
constexpr std::array<std::string_view, 5> ui_text_fields{"id", "key", "role", "label", "value"};
constexpr std::array<std::string_view, 5> ui_flag_fields{"visible", "enabled", "blocked", "checked", "focused"};

void validate_ui_selector(const J& selector, i32 depth = 0) {
    if (depth > max_ui_scope_depth) {
        throw std::runtime_error("UI scope nesting exceeds 16");
    }
    if (!selector.is_object()) {
        throw std::runtime_error("UI selector must be an object");
    }
    for (const auto& [key, value] : selector.members()) {
        if (key == "within") {
            validate_ui_selector(value, depth + 1);
        } else if (std::ranges::find(ui_text_fields, key) != ui_text_fields.end()) {
            if (!value.is_string()) {
                throw std::runtime_error(key + " must be a string");
            }
        } else if (std::ranges::find(ui_flag_fields, key) != ui_flag_fields.end()) {
            if (!value.is_bool()) {
                throw std::runtime_error(key + " must be boolean");
            }
        } else {
            throw std::runtime_error("unknown selector/parameter: " + key);
        }
    }
}

bool ui_flag(const ui2::SemanticElement& e, std::string_view key) {
    if (key == "visible") {
        return e.visible;
    }
    if (key == "enabled") {
        return e.enabled;
    }
    if (key == "blocked") {
        return e.blocked;
    }
    if (key == "checked") {
        return e.checked;
    }
    return e.focused;
}

std::string ui_text(const ui2::SemanticElement& e, std::string_view key) {
    if (key == "id") {
        return std::to_string(e.id.value);
    }
    if (key == "key") {
        return e.key;
    }
    if (key == "role") {
        return e.role;
    }
    if (key == "label") {
        return e.label;
    }
    return e.value;
}

// Whether `e` matches a selector validate_ui_selector accepted.
bool ui_match(const ui2::SemanticElement& e, const J& selector, const std::vector<ui2::SemanticElement>& all) {
    for (const auto& [key, value] : selector.members()) {
        if (key == "within") {
            continue;
        }
        if (value.is_string() ? ui_text(e, key) != value.as_string() : ui_flag(e, key) != value.as_bool()) {
            return false;
        }
    }
    const J* scope = selector.find("within");
    if (!scope) {
        return true;
    }
    std::set<u64> visited;
    for (ui2::Id parent = e.parent; parent.value && visited.insert(parent.value).second;) {
        const auto found = std::ranges::find(all, parent, &ui2::SemanticElement::id);
        if (found == all.end()) {
            break;
        }
        if (ui_match(*found, *scope, all)) {
            return true;
        }
        parent = found->parent;
    }
    return false;
}

// The scene's semantic UI. With `initialize`, a scene that has not laid out its
// UI yet draws a frame first (UI built while rendering needs one, even in a
// server that does not render).
ui2::Context& ui_context(ServerContext& ctx, const J& params, bool initialize) {
    const i32 index = scene_index(ctx, params);
    Scene* scene = ctx.scenes().at_mut(index);
    if (scene && scene->ui_context() && initialize && scene->ui_context()->elements().empty()) {
        ctx.render_frame();
        scene = ctx.scenes().at_mut(index);
    }
    if (!scene || !scene->ui_context()) {
        throw std::runtime_error("scene has no semantic UI (Scene::ui_context)");
    }
    return *scene->ui_context();
}

// The one element `selector` names: none or several are errors.
const ui2::SemanticElement& select_ui_element(const ui2::Context& ui, const J& selector) {
    validate_ui_selector(selector);
    const auto& all = ui.elements();
    const ui2::SemanticElement* target = nullptr;
    for (const auto& e : all) {
        if (!ui_match(e, selector, all)) {
            continue;
        }
        if (target) {
            throw std::runtime_error("ambiguous UI selector");
        }
        target = &e;
    }
    if (!target) {
        throw std::runtime_error("UI selector matched no elements");
    }
    return *target;
}

// What ui2 requires of an action's target, checked when it is requested.
void check_ui_target(const ui2::SemanticElement& e, ui2::SemanticAction action) {
    if (!e.available()) {
        throw std::runtime_error("UI element is disabled, hidden or blocked");
    }
    if (!e.focusable) {
        throw std::runtime_error("UI element does not accept actions");
    }
    if (!ui2::semantic_role_accepts(e.role, action)) {
        throw std::runtime_error(action == ui2::SemanticAction::SetText
                                     ? "set_text requires a text input"
                                     : "numeric action requires a slider or number input");
    }
}

struct UiActionName {
    std::string_view name;
    ui2::SemanticAction action;
};

constexpr std::array<UiActionName, 6> ui_actions{{
    {"click", ui2::SemanticAction::Click},
    {"focus", ui2::SemanticAction::Focus},
    {"set_text", ui2::SemanticAction::SetText},
    {"increment", ui2::SemanticAction::Increment},
    {"decrement", ui2::SemanticAction::Decrement},
    {"set_number", ui2::SemanticAction::SetNumber},
}};

ServerResponse ui_snapshot(ServerContext& ctx, const J& params, bool initialize) {
    keys(params, {"scene"});
    const ui2::Context& ui = ui_context(ctx, params, initialize);
    return ok_result(written([&](JsonWriter& json) { ui.write_snapshot(json); }));
}

ServerResponse ui_query(ServerContext& ctx, const J& params, bool initialize) {
    keys(params, {"scene", "selector"});
    const ui2::Context& ui = ui_context(ctx, params, initialize);
    const J empty{O{}};
    const J& selector = params.find("selector") ? *params.find("selector") : empty;
    validate_ui_selector(selector);
    std::set<std::string> ids;
    for (const auto& e : ui.elements()) {
        if (ui_match(e, selector, ui.elements())) {
            ids.insert(std::to_string(e.id.value));
        }
    }
    const J document = decode(written([&](JsonWriter& json) { ui.write_snapshot(json); }));
    A elements;
    for (const J& e : document.find("elements")->items()) {
        if (ids.contains(e.string_at("id"))) {
            elements.push_back(e);
        }
    }
    const auto count = static_cast<f64>(elements.size());
    return ok_result(encode(J{O{{"count", number(count)}, {"elements", J{std::move(elements)}}}}));
}

// Queued for the next step, like physical input, so a screenshot between the
// request and sim.tick cannot activate the control.
ServerResponse ui_action(ServerContext& ctx, const J& params, bool) {
    keys(params, {"scene", "id", "selector", "action", "text"});
    const std::string name = params.string_at("action", "click");
    const auto known = std::ranges::find(ui_actions, name, &UiActionName::name);
    if (known == ui_actions.end()) {
        throw std::runtime_error("UI action must be click, focus, set_text, increment, decrement or set_number");
    }
    const J* selector = params.find("selector");
    if (params.contains("id") == (selector != nullptr)) {
        throw std::runtime_error("ui.action requires exactly one of id or selector");
    }
    const std::string text = params.string_at("text");
    f64 value = 0;
    if (known->action == ui2::SemanticAction::SetNumber) {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) {
            throw std::runtime_error("set_number requires finite numeric text");
        }
    }
    const i32 index = scene_index(ctx, params);
    const ui2::Context& ui = ui_context(ctx, params, true);
    std::optional<ui2::Id> target;
    if (selector) {
        const ui2::SemanticElement& element = select_ui_element(ui, *selector);
        check_ui_target(element, known->action);
        target = element.id;
    } else {
        std::string error;
        target = ui.resolve_action(params.string_at("id"), known->action, error); // key, label or numeric id
        if (!target) {
            throw std::runtime_error(error);
        }
    }
    Scene* scene = ctx.scenes().at_mut(index);
    const bool queued = ctx.queue_ui_action([&ctx, scene, index, id = *target, action = known->action, text, value] {
        if (ctx.scenes().at_mut(index) != scene || !scene->ui_context()) {
            return; // the scene went away first
        }
        std::string ignored;
        scene->ui_context()->queue_action(id, action, text, value, ignored);
    });
    if (!queued) {
        throw std::runtime_error("a UI action is already pending; advance a frame first");
    }
    return ok_result(R"({"queued":true})");
}

// Runtime identity (ids, generations, 64-bit fields) is a decimal string in
// automation responses: JSON numbers above 2^53 would lose digits.
void stringify_identities(J& encoded, const EntitySnapshot& entity) {
    auto row = encoded.members();
    const EntityDescriptor& actual = entity.descriptor;
    row["id"] = str(std::to_string(actual.id));
    row["parent"] = str(std::to_string(actual.parent));
    row["handle_generation"] = str(std::to_string(actual.handle_generation));
    row["creation_sequence"] = str(std::to_string(actual.creation_sequence));
    A children;
    for (const auto child : actual.children) {
        children.push_back(str(std::to_string(child)));
    }
    row["children"] = J{std::move(children)};
    A components;
    std::size_t component_index = 0;
    for (const J& encoded_component : row.at("components").items()) {
        auto component = encoded_component.members();
        const auto& original = entity.components[component_index++];
        component["id"] = str(std::to_string(original.descriptor.id));
        A fields;
        std::size_t field_index = 0;
        for (const J& encoded_field : component.at("fields").items()) {
            auto field = encoded_field.members();
            std::visit(
                [&](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, u64> || std::is_same_v<T, i64>) {
                        field["value"] = str(std::to_string(v));
                    } else if constexpr (std::is_same_v<T, ComponentEntityRef>) {
                        field["value"] = J{O{{"id", str(std::to_string(v.id))}, {"authored_id", str(v.authored_id)}}};
                    }
                },
                original.fields[field_index++].value);
            fields.emplace_back(std::move(field));
        }
        component["fields"] = J{std::move(fields)};
        components.emplace_back(std::move(component));
    }
    row["components"] = J{std::move(components)};
    encoded = J{std::move(row)};
}

bool text_match(const J& selector, std::string_view key, std::string_view actual) {
    const J* value = selector.find(key);
    return !value || value->as_string() == actual;
}

ServerResponse world_query(ServerContext& ctx, const J& params, bool) {
    keys(params, {"scene", "selector", "limit"});
    Scene* scene = ctx.scenes().at_mut(scene_index(ctx, params, true));
    if (!scene || !scene->world()) {
        throw std::runtime_error("scene has no ECS world");
    }
    EcsWorld& world = *scene->world();
    const J empty{O{}};
    const J& selector = params.find("selector") ? *params.find("selector") : empty;
    keys(selector, {"id", "authored_id", "name", "enabled", "all", "none"});
    for (const auto key : {"id", "authored_id", "name"}) {
        if (const J* value = selector.find(key); value && !value->is_string()) {
            throw std::runtime_error(std::string(key) + " must be a string");
        }
    }
    const J* enabled = selector.find("enabled");
    if (enabled && !enabled->is_bool()) {
        throw std::runtime_error("enabled must be boolean");
    }
    // A plan without a compiled query: build_query_plan keeps one per call.
    EcsQueryPlan plan{.valid = true};
    for (const auto& [key, ids] : {std::pair{"all", &plan.all}, std::pair{"none", &plan.none}}) {
        const J* terms = selector.find(key);
        if (!terms) {
            continue;
        }
        if (!terms->is_array()) {
            throw std::runtime_error(std::string(key) + " must be an array");
        }
        for (const J& term : terms->items()) {
            const ComponentDescriptor* component =
                term.is_string() ? world.components().find(term.as_string()) : nullptr;
            if (!component) {
                throw std::runtime_error("unknown component in selector");
            }
            ids->push_back(component->id);
        }
    }
    i64 limit = 256;
    if (const J* value = params.find("limit")) {
        const auto chosen = exact_integer(*value, 1, 4096);
        if (!chosen) {
            throw std::runtime_error("limit must be an integer in 1..4096");
        }
        limit = *chosen;
    }
    WorldSnapshot matches{.components = world.components().descriptors()};
    std::size_t total = 0;
    for (EcsEntity entity : world.query_entities(plan)) {
        if (!text_match(selector, "id", std::to_string(entity.id())) ||
            !text_match(selector, "authored_id", world.entities().authored_id(entity).value_or(EntityAuthoredId{})) ||
            !text_match(selector, "name", entity.name()) || (enabled && enabled->as_bool() != entity.raw().enabled())) {
            continue;
        }
        if (total++ < static_cast<std::size_t>(limit)) {
            matches.entities.push_back(world.snapshot(entity));
        }
    }
    auto document = decode(written([&](JsonWriter& json) { write_world_snapshot_json(json, matches); })).members();
    A entities;
    std::size_t index = 0;
    for (const J& encoded : document.at("entities").items()) {
        J row = encoded;
        stringify_identities(row, matches.entities[index++]);
        entities.push_back(std::move(row));
    }
    document["entities"] = J{std::move(entities)};
    document["count"] = number(static_cast<f64>(total));
    document["truncated"] = J{total > static_cast<std::size_t>(limit)};
    return ok_result(encode(J{std::move(document)}));
}

ServerResponse automation_snapshot(ServerContext& ctx, const J& params, bool) {
    keys(params, {"world"});
    return ok_result(snapshot(ctx, params.bool_at("world", false)));
}

ServerResponse server_schema(ServerContext&, const J& params, bool) {
    keys(params, {});
    return ok_result(encode(automation_protocol()));
}

ServerResponse server_capabilities(ServerContext& ctx, const J& params, bool) {
    keys(params, {});
    A scenes;
    bool any_ui = false;
    bool any_world = false;
    bool any_development = false;
    for (i32 i = 0; i < ctx.scenes().depth(); ++i) {
        Scene* scene = ctx.scenes().at_mut(i);
        const bool ui = scene->ui_context() != nullptr;
        const bool world = scene->world() != nullptr;
        any_ui |= ui;
        any_world |= world;
        any_development |= scene->development_session() != nullptr;
        scenes.emplace_back(O{{"index", number(i)}, {"name", str(scene->name())}, {"ui", J{ui}}, {"world", J{world}}});
    }
    O available;
    for (const auto& [name, descriptor] : automation_protocol().find("methods")->members()) {
        bool enabled = ctx.automation_mode() == AutomationMode::Inspect ? descriptor.bool_at("inspect", false)
                                                                        : descriptor.bool_at("driven", true);
        if (name.starts_with("ui.")) {
            enabled &= any_ui;
        }
        if (name.starts_with("world.")) {
            enabled &= any_world;
        }
        if (name.starts_with("dev.")) {
            enabled &= any_development;
        }
        if (name == "sim.reset") {
            enabled &= ctx.can_reset();
        }
        if (name == "view.screenshot") {
            enabled &= static_cast<bool>(KIN_ENABLE_IMAGE_IO);
        }
        available[name] = J{enabled};
    }
    const RngKey rng = ctx.rng();
    return ok_result(encode(J{O{{"schema", str("kin.automation-capabilities/1")},
                                {"protocol_version", number(1)},
                                {"mode", str(automation_mode_name(ctx.automation_mode()))},
                                {"fixed_dt", number(ctx.fixed_dt())},
                                {"frame", number(static_cast<f64>(ctx.frame()))},
                                {"rng_key", J{A{number(static_cast<f64>(rng.lo)), number(static_cast<f64>(rng.hi))}}},
                                {"renderer", str(ctx.renderer().backend_name())},
                                {"world", J{any_world}},
                                {"ui", J{any_ui}},
                                {"reset", J{ctx.can_reset()}},
                                {"image_io", J{static_cast<bool>(KIN_ENABLE_IMAGE_IO)}},
                                {"recording", J{ctx.is_recording()}},
                                {"development", J{any_development}},
                                {"methods", J{std::move(available)}},
                                {"scenes", J{std::move(scenes)}}}}));
}

#if KIN_ENABLE_DEVELOPMENT
// The scene `params` names, else the topmost with a development session.
DevelopmentSession& development_session(ServerContext& ctx, const J& params) {
    i32 index = scene_index(ctx, params);
    Scene* scene = ctx.scenes().at_mut(index);
    while (scene && !scene->development_session() && !params.contains("scene") && index > 0) {
        scene = ctx.scenes().at_mut(--index);
    }
    DevelopmentSession* session = scene ? scene->development_session() : nullptr;
    if (!session) {
        throw std::runtime_error("scene has no development session; start with --dev");
    }
    return *session;
}

ServerResponse dev_status(ServerContext& ctx, const J& params, bool) {
    keys(params, {"scene"});
    const DevelopmentSession& session = development_session(ctx, params);
    return ok_result(written([&](JsonWriter& json) { session.write_status(json); }));
}

ServerResponse dev_reload(ServerContext& ctx, const J& params, bool) {
    keys(params, {"scene", "changes", "expected_revision"});
    DevelopmentSession& session = development_session(ctx, params);
    if (const J* expected = params.find("expected_revision");
        expected && (!expected->is_string() || expected->as_string() != session.active().id)) {
        throw std::runtime_error("development revision changed; refresh dev.status before editing");
    }
    std::string error;
    const J* changes = params.find("changes");
    if (!(changes ? session.request(*changes, error) : session.request(error))) {
        throw std::runtime_error(error);
    }
    // The recording replays exactly these edits, whatever the folder holds then.
    J recorded;
    try {
        recorded = session.pending_changes();
    } catch (const std::exception& e) {
        session.reject_request(e.what());
        throw;
    }
    return ok_result(encode(J{O{{"queued", J{true}},
                                {"generation", number(static_cast<f64>(session.generation()))},
                                {"changes", std::move(recorded)}}}));
}
#endif

using AutomationHandler = ServerResponse (*)(ServerContext&, const J&, bool initialize_ui);

struct AutomationMethod {
    std::string_view name;
    AutomationHandler handle;
};

constexpr std::array automation_methods{
    AutomationMethod{"server.schema", server_schema},
    AutomationMethod{"server.capabilities", server_capabilities},
    AutomationMethod{"automation.snapshot", automation_snapshot},
    AutomationMethod{"world.query", world_query},
    AutomationMethod{"ui.snapshot", ui_snapshot},
    AutomationMethod{"ui.query", ui_query},
    AutomationMethod{"ui.action", ui_action},
#if KIN_ENABLE_DEVELOPMENT
    AutomationMethod{"dev.status", dev_status},
    AutomationMethod{"dev.reload", dev_reload},
#endif
};

} // namespace

const JsonValue& automation_protocol() {
    // Every method's "params" refuses unknown fields: the loop below adds
    // "additionalProperties": false to each.
    static const J protocol = [] {
        auto document = decode(R"JSON({"schema":"kin.automation/1","version":1,"methods":{
      "server.schema":{"effect":"read","inspect":true,"params":{"type":"object","properties":{}}},
      "server.capabilities":{"effect":"read","inspect":true,"params":{"type":"object","properties":{}}},
      "automation.snapshot":{"effect":"read","inspect":true,"params":{"type":"object","properties":{"world":{"type":"boolean"}}}},
      "world.query":{"effect":"read","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0},"selector":{"type":"object","properties":{"id":{"type":"string"},"authored_id":{"type":"string"},"name":{"type":"string"},"enabled":{"type":"boolean"},"all":{"type":"array","items":{"type":"string"}},"none":{"type":"array","items":{"type":"string"}}},"additionalProperties":false},"limit":{"type":"integer","minimum":1,"maximum":4096}}}},
      "ui.query":{"effect":"layout","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0},"selector":{"type":"object"}}}},
      "game.info":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "scene.stack":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "scene.current":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "scene.actions":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "world.snapshot":{"effect":"read","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0},"raw":{"type":"boolean"},"kin":{"type":"boolean"},"events":{"type":"boolean"},"animations":{"type":"boolean"}}}},
      "animation.diagnostics":{"effect":"read","inspect":true,"params":{"type":"object"}},
      "ui.snapshot":{"effect":"layout","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0}}}},
      "ui.action":{"effect":"input","inspect":false,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0},"id":{"type":"string"},"selector":{"type":"object"},"action":{"type":"string"},"text":{"type":"string"}}}},
      "sim.tick":{"effect":"step","inspect":false,"params":{"type":"object","properties":{"count":{"type":"integer","minimum":0,"maximum":1000000}}}},
      "sim.reset":{"effect":"reset","inspect":false,"params":{"type":"object","properties":{"seed":{"anyOf":[{"type":"integer","minimum":0,"maximum":9007199254740991},{"type":"string"}]}}}},
      "input.action":{"effect":"input","inspect":false,"params":{"type":"object","required":["name"],"properties":{"name":{"type":"string"},"mode":{"type":"string","enum":["press","hold","release"]}}}},
      "input.key":{"effect":"input","inspect":false,"params":{"type":"object","required":["name"],"properties":{"name":{"type":"string"},"mode":{"type":"string","enum":["press","hold","release"]},"modifiers":{"type":"array","items":{"type":"string"}}}}},
      "input.mouse":{"effect":"input","inspect":false,"params":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"dx":{"type":"number"},"dy":{"type":"number"},"wheel":{"type":"number"},"button":{"type":"string","enum":["left","middle","right","x1","x2"]},"mode":{"type":"string","enum":["press","hold","release"]},"space":{"type":"string","enum":["logical","window"]}}}},
      "input.text":{"effect":"input","inspect":false,"params":{"type":"object","required":["text"],"properties":{"text":{"type":"string"}}}},
      "input.drop":{"effect":"input","inspect":false,"params":{"type":"object","required":["path"],"properties":{"path":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"space":{"type":"string","enum":["logical","window"]}}}},
      "view.screenshot":{"effect":"capture","inspect":false,"params":{"type":"object","properties":{"path":{"type":"string"}}}},
      "view.scene":{"effect":"scene","inspect":false,"params":{"type":"object","required":["path"],"properties":{"path":{"type":"string"},"content":{"type":"string"}}}},
      "view.frame":{"effect":"capture","inspect":false,"params":{"type":"object","required":["width","height"],"properties":{"path":{"type":"string"},"width":{"type":"integer","minimum":1},"height":{"type":"integer","minimum":1},"x":{"type":"number"},"y":{"type":"number"},"zoom":{"type":"number","exclusiveMinimum":0},"rotation":{"type":"number"}}}},
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
      "debug.step":{"effect":"debug","inspect":true,"driven":false,"params":{"type":"object","properties":{"how":{"type":"string","enum":["into","over","out"]}}}},
      "dev.status":{"effect":"read","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0}}}},
      "dev.reload":{"effect":"reload","inspect":true,"params":{"type":"object","properties":{"scene":{"type":"integer","minimum":0},"expected_revision":{"type":"string"},"changes":{"type":"object"}}}}
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
#if !KIN_ENABLE_DEVELOPMENT
        methods.erase("dev.status");
        methods.erase("dev.reload");
#endif
        A action_names;
        for (const UiActionName& action : ui_actions) {
            action_names.push_back(str(action.name));
        }
        for (auto& [name, entry] : methods) {
            auto fields = entry.members();
            auto params = fields.at("params").members();
            params["additionalProperties"] = J{false};
            if (name == "ui.action") {
                auto properties = params.at("properties").members();
                properties["action"] = J{O{{"type", str("string")}, {"enum", J{action_names}}}};
                params["properties"] = J{std::move(properties)};
            }
            fields["params"] = J{std::move(params)};
            const J* result = results.find(name);
            fields["result"] = result ? *result : J{O{{"type", str("object")}}};
            fields["driven"] = J{entry.bool_at("driven", true)};
            entry = J{std::move(fields)};
        }
        root["methods"] = J{std::move(methods)};
        return J{std::move(root)};
    }();
    return protocol;
}

std::optional<ServerResponse> dispatch_automation_command(ServerContext& ctx, std::string_view method,
                                                          const JsonValue& params, bool initialize_ui) {
    const auto found = std::ranges::find(automation_methods, method, &AutomationMethod::name);
    if (found == automation_methods.end()) {
        return std::nullopt;
    }
    try {
        const J none{O{}};
        return found->handle(ctx, params.is_null() ? none : params, initialize_ui); // a request without params
    } catch (const std::exception& e) {
        return error_result(e.what());
    }
}

AutomationRecorder::AutomationRecorder(const std::filesystem::path& path, ServerContext& ctx, const GameInfo* game,
                                       std::optional<u64> seed) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    _stream.open(path, std::ios::binary | std::ios::trunc);
    if (!_stream) {
        _error = "cannot open automation recording: " + path.string();
        return;
    }
    // The header Python recordings (tools/automation) share: they add their
    // command line and build/content identities.
    const RngKey rng = ctx.rng();
    append(J{O{{"kind", str("header")},
               {"schema", str("kin.automation-recording/1")},
               {"seed", seed ? str(std::to_string(*seed)) : J{}},
               {"fixed_dt", number(ctx.fixed_dt())},
               {"rng_key", J{A{number(static_cast<f64>(rng.lo)), number(static_cast<f64>(rng.hi))}}},
               {"renderer", str(ctx.renderer().backend_name())},
               {"game_id", str(game ? game->id : "")},
               {"game_version", str(game ? game->version : "")}}});
}

void AutomationRecorder::append(const J& value) {
    append_text(encode(value));
}

void AutomationRecorder::append_text(const std::string& line) {
    if (!_error.empty()) {
        return;
    }
    if (_bytes + line.size() > 256ull * 1024 * 1024) {
        _error = "automation recording exceeded 256 MiB";
        return;
    }
    _stream << line << '\n';
    _stream.flush();
    _bytes += line.size() + 1;
    if (!_stream) {
        _error = "automation recording write failed";
    }
}

void AutomationRecorder::begin_command(ServerContext& ctx, std::string_view method, const J& params) {
    _running = _sequence++;
    append(J{O{{"kind", str("request")},
               {"sequence", number(static_cast<f64>(*_running))},
               {"frame", number(static_cast<f64>(ctx.frame()))},
               {"method", str(method)},
               {"params", params}}});
}

void AutomationRecorder::command(ServerContext& ctx, std::string_view method, const J& params,
                                 const ServerResponse& response) {
    const u64 sequence = _running ? *_running : _sequence++;
    _running.reset();
    append_text(written([&](JsonWriter& json) {
        json.begin_object()
            .field("kind", "command")
            .field("sequence", sequence)
            .field("frame", ctx.frame())
            .field("method", method)
            .field("ok", response.ok);
        json.key("params");
        write_json(json, params);
        if (response.ok) {
            json.key("result").raw(response.result_json);
        } else {
            json.field("error", response.error);
        }
        json.end_object();
    }));
}

void AutomationRecorder::frame(ServerContext& ctx) {
    append_text(written([&](JsonWriter& json) {
        json.begin_object()
            .field("kind", "frame")
            .field("frame", ctx.frame())
            .key("snapshot")
            .raw(snapshot(ctx, false));
        json.end_object();
    }));
}

} // namespace kin
