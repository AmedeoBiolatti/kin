#pragma once

#include <kin/ecs/component.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

struct ComponentSchemaField {
    std::string name;
    ComponentFieldKind kind = ComponentFieldKind::Bool;
    ComponentFieldValue default_value = false;
    std::string label;
    std::string category;
    std::string description;
    std::string units;
    bool readonly = false;
    bool hidden = false;
    std::optional<f64> min_value;
    std::optional<f64> max_value;
};

struct ComponentSchema {
    std::string name;
    std::vector<ComponentSchemaField> fields;
};

struct ComponentSchemaDocument {
    std::string schema = "kin.components/1";
    std::vector<ComponentSchema> components;
};

struct ComponentSchemaLoadResult {
    std::optional<ComponentSchemaDocument> document;
    std::vector<std::string> diagnostics;

    bool ok() const { return document.has_value() && diagnostics.empty(); }
};

ComponentSchemaLoadResult load_component_schema_asset(const std::filesystem::path& path);
ComponentSchemaLoadResult parse_component_schema_asset(std::string_view text, std::string_view source = {});

std::vector<std::string> validate_component_schema_asset(const ComponentSchemaDocument& document);
bool apply_component_schema_asset(EcsComponentRegistry& registry,
                                  const ComponentSchemaDocument& document,
                                  std::vector<std::string>& diagnostics);

} // namespace kin
