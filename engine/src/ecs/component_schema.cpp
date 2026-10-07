#include <kin/ecs/component_schema.hpp>

#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>

#include <algorithm>
#include <unordered_set>

namespace kin {
namespace {

ComponentSchemaField parse_field(const JsonValue& json, std::vector<std::string>& diagnostics) {
    ComponentSchemaField field;
    if (!json.is_object()) {
        diagnostics.push_back("component field entry must be an object");
        return field;
    }

    field.name = json.string_at("name");
    const std::string kind_name = json.string_at("kind");
    std::optional<ComponentFieldKind> kind = component_field_kind_from_name(kind_name);
    if (!kind) {
        diagnostics.push_back("unknown component field kind '" + kind_name + "'");
        return field;
    }
    field.kind = *kind;

    const JsonValue* default_json = json.find("default");
    if (!default_json) {
        diagnostics.push_back("component field '" + field.name + "' is missing default");
        return field;
    }
    std::string error;
    std::optional<ComponentFieldValue> default_value = component_field_value_from_json(field.kind, *default_json, error);
    if (!default_value) {
        diagnostics.push_back("component field '" + field.name + "' default: " + error);
        return field;
    }
    field.default_value = std::move(*default_value);

    field.label = json.string_at("label");
    field.category = json.string_at("category");
    field.description = json.string_at("description");
    field.units = json.string_at("units");
    field.readonly = json.bool_at("readonly", false);
    field.hidden = json.bool_at("hidden", false);
    if (const JsonValue* min = json.find("min"); min && min->is_number()) {
        field.min_value = min->as_number();
    }
    if (const JsonValue* max = json.find("max"); max && max->is_number()) {
        field.max_value = max->as_number();
    }
    return field;
}

ComponentSchema parse_component(const JsonValue& json, std::vector<std::string>& diagnostics) {
    ComponentSchema component;
    if (!json.is_object()) {
        diagnostics.push_back("component schema entry must be an object");
        return component;
    }
    component.name = json.string_at("name");
    const JsonValue* fields = json.find("fields");
    if (!fields || !fields->is_array()) {
        diagnostics.push_back("component '" + component.name + "' requires a fields array");
        return component;
    }
    for (const JsonValue& field_json : fields->items()) {
        component.fields.push_back(parse_field(field_json, diagnostics));
    }
    return component;
}

void apply_metadata(EcsComponentRegistry::DataComponentBuilder& builder, const ComponentSchemaField& field) {
    if (!field.label.empty()) {
        builder.label(field.label);
    }
    if (!field.category.empty()) {
        builder.category(field.category);
    }
    if (!field.description.empty()) {
        builder.description(field.description);
    }
    if (!field.units.empty()) {
        builder.units(field.units);
    }
    if (field.readonly) {
        builder.readonly();
    }
    if (field.hidden) {
        builder.hidden();
    }
    if (field.min_value && field.max_value) {
        builder.range(*field.min_value, *field.max_value);
    }
}

} // namespace

ComponentSchemaLoadResult load_component_schema_asset(const std::filesystem::path& path) {
    const std::optional<std::string> text = read_content_text(path);
    if (!text) {
        return {.diagnostics = {"failed to open component schema '" + path.string() + "'"}};
    }
    return parse_component_schema_asset(*text, path.string());
}

ComponentSchemaLoadResult parse_component_schema_asset(std::string_view text, std::string_view source) {
    JsonParseResult parsed = parse_json(text);
    if (!parsed.ok()) {
        const std::string prefix = source.empty() ? std::string{} : std::string{source} + ": ";
        return {.diagnostics = {prefix + parsed.error}};
    }
    if (!parsed.value->is_object()) {
        return {.diagnostics = {"component schema root must be an object"}};
    }

    ComponentSchemaDocument document;
    document.schema = parsed.value->string_at("schema", "kin.components/1");
    std::vector<std::string> diagnostics;
    const JsonValue* components = parsed.value->find("components");
    if (!components || !components->is_array()) {
        diagnostics.push_back("component schema requires a components array");
    } else {
        for (const JsonValue& component_json : components->items()) {
            document.components.push_back(parse_component(component_json, diagnostics));
        }
    }

    std::vector<std::string> validation = validate_component_schema_asset(document);
    diagnostics.insert(diagnostics.end(), validation.begin(), validation.end());
    if (!diagnostics.empty()) {
        return {.diagnostics = std::move(diagnostics)};
    }
    return {.document = std::move(document)};
}

std::vector<std::string> validate_component_schema_asset(const ComponentSchemaDocument& document) {
    std::vector<std::string> diagnostics;
    if (document.schema != "kin.components/1") {
        diagnostics.push_back("unsupported component schema '" + document.schema + "'");
    }

    std::unordered_set<std::string> component_names;
    for (const ComponentSchema& component : document.components) {
        if (component.name.empty()) {
            diagnostics.push_back("component name is required");
            continue;
        }
        if (!component_names.insert(component.name).second) {
            diagnostics.push_back("duplicate component '" + component.name + "'");
        }
        std::unordered_set<std::string> field_names;
        for (const ComponentSchemaField& field : component.fields) {
            if (field.name.empty()) {
                diagnostics.push_back("component '" + component.name + "' has an empty field name");
                continue;
            }
            if (!field_names.insert(field.name).second) {
                diagnostics.push_back("duplicate field '" + field.name + "' on component '" + component.name + "'");
            }
            if (!detail::field_value_matches_kind(field.default_value, field.kind)) {
                diagnostics.push_back("default value type mismatch for field '" + field.name + "' on component '" + component.name + "'");
            }
        }
    }
    return diagnostics;
}

bool apply_component_schema_asset(EcsComponentRegistry& registry,
                                  const ComponentSchemaDocument& document,
                                  std::vector<std::string>& diagnostics) {
    diagnostics = validate_component_schema_asset(document);
    if (!diagnostics.empty()) {
        return false;
    }

    for (const ComponentSchema& component : document.components) {
        std::vector<ComponentFieldSnapshot> fields;
        fields.reserve(component.fields.size());
        for (const ComponentSchemaField& field : component.fields) {
            fields.push_back({
                .name = field.name,
                .kind = field.kind,
                .value = field.default_value,
            });
        }

        if (const ComponentDescriptor* existing = registry.find(component.name)) {
            if (existing->kind != ComponentKind::Data) {
                diagnostics.push_back("component '" + component.name + "' already exists and is not a data component");
                return false;
            }
            for (const ComponentSchemaField& field : component.fields) {
                const auto found = std::ranges::find_if(existing->fields, [&](const ComponentFieldDescriptor& existing_field) {
                    return existing_field.name == field.name;
                });
                if (found != existing->fields.end() && found->kind != field.kind) {
                    diagnostics.push_back("incompatible kind change for field '" + field.name + "' on component '" + component.name + "'");
                    return false;
                }
            }
            if (!registry.migrate_data_schema(component.name, fields)) {
                diagnostics.push_back(registry.last_error());
                return false;
            }
            continue;
        }

        auto builder = registry.data(component.name);
        if (!builder) {
            diagnostics.push_back(registry.last_error());
            return false;
        }
        for (const ComponentSchemaField& field : component.fields) {
            builder.field(field.name, field.kind, field.default_value);
            if (!registry.last_error().empty()) {
                diagnostics.push_back(registry.last_error());
                return false;
            }
            apply_metadata(builder, field);
        }
    }
    diagnostics.clear();
    return true;
}

} // namespace kin
