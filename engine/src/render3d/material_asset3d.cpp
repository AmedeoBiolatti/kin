#include "material_asset3d_impl.hpp"

#include <algorithm>
#include <cmath>
#include <kin/assets/content.hpp>
#include <kin/core/json_value.hpp>
#include <kin/ecs/component.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace kin {
namespace {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
void fields(const JsonValue& value, std::initializer_list<std::string_view> allowed) {
    require(value.is_object(), "expected object");
    for (const auto& [key, ignored] : value.members()) {
        (void)ignored;
        require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(), "unknown field '" + key + "'");
    }
}
std::string text(const JsonValue& value) {
    require(value.is_string() && !value.as_string().empty() && value.as_string().size() <= 4096,
            "expected nonempty string (at most 4096 bytes)");
    return value.as_string();
}
f32 number(const JsonValue& value, f64 low, f64 high, std::string_view field) {
    require(value.is_number() && std::isfinite(value.as_number()) && value.as_number() >= low &&
                value.as_number() <= high,
            "invalid number '" + std::string(field) + "'");
    return f32(value.as_number());
}
template <class T> T enumeration(const JsonValue& value, std::initializer_list<std::pair<std::string_view, T>> values) {
    const auto name = text(value);
    for (const auto& [key, result] : values)
        if (key == name)
            return result;
    throw std::runtime_error("unknown enum value '" + name + "'");
}
MaterialTexture3D texture(const JsonValue& value) {
    if (value.is_null())
        return {};
    if (value.is_string())
        return {text(value)};
    fields(value, {"asset", "filter"});
    require(value.find("asset") != nullptr, "texture requires 'asset'");
    MaterialTexture3D result{.asset = text(*value.find("asset"))};
    if (const auto* filter = value.find("filter"))
        result.nearest = enumeration<bool>(*filter, {{"nearest", true}, {"mipmapped", false}});
    return result;
}
MaterialPatch3D patch(const JsonValue& value) {
    fields(value, {"color",           "metallic",
                   "roughness",       "emissive",
                   "normal_scale",    "occlusion_strength",
                   "alpha_cutoff",    "unlit",
                   "double_sided",    "casts_shadow",
                   "receives_shadow", "wireframe",
                   "blend",           "wrap_u",
                   "wrap_v",          "texture",
                   "normal",          "metallic_roughness",
                   "occlusion",       "emissive_texture",
                   "shader",          "shader_params"});
    MaterialPatch3D result;
    if (const auto* v = value.find("shader"))
        result.shader = v->is_null() ? std::filesystem::path{} : std::filesystem::path{text(*v)};
    if (const auto* v = value.find("shader_params")) {
        require(v->is_array() && v->items().size() == 16, "shader_params requires 16 finite numbers");
        std::array<f32, 16> params;
        for (size_t i = 0; i < 16; ++i)
            params[i] = number(v->items()[i], -std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(),
                               "shader_params");
        result.shader_params = params;
    }
    if (const auto* color = value.find("color")) {
        const auto channel = [](const JsonValue& value) {
            number(value, 0, 255, "color channel");
            require(std::floor(value.as_number()) == value.as_number(), "color channels must be sRGB bytes");
        };
        if (color->is_array()) {
            require(color->items().size() == 3 || color->items().size() == 4, "color requires three or four channels");
            for (const auto& value : color->items())
                channel(value);
        } else {
            fields(*color, {"r", "g", "b", "a"});
            for (const auto& [key, value] : color->members()) {
                (void)key;
                channel(value);
            }
        }
        std::string error;
        auto parsed = component_field_value_from_json(ComponentFieldKind::Color, *color, error);
        require(parsed.has_value(), "invalid color: " + error);
        result.color = std::get<Color>(*parsed);
    }
#define NUMBER(name, low, high)                                                                                        \
    if (const auto* v = value.find(#name))                                                                             \
    result.name = number(*v, low, high, #name)
    NUMBER(metallic, 0, 1);
    NUMBER(roughness, 0, 1);
    NUMBER(emissive, 0, 1e6);
    NUMBER(normal_scale, 0, 100);
    NUMBER(occlusion_strength, 0, 1);
    NUMBER(alpha_cutoff, 0, 1);
#undef NUMBER
#define BOOLEAN(name)                                                                                                  \
    if (const auto* v = value.find(#name)) {                                                                           \
        require(v->is_bool(), "expected bool '" #name "'");                                                            \
        result.name = v->as_bool();                                                                                    \
    }
    BOOLEAN(unlit);
    BOOLEAN(double_sided);
    BOOLEAN(casts_shadow);
    BOOLEAN(receives_shadow);
    BOOLEAN(wireframe);
#undef BOOLEAN
    if (const auto* v = value.find("blend"))
        result.blend = enumeration<Blend3D>(*v, {{"opaque", Blend3D::Opaque},
                                                 {"alpha", Blend3D::Alpha},
                                                 {"additive", Blend3D::Additive},
                                                 {"mask", Blend3D::Mask}});
    const auto wrap = [](const JsonValue& v) {
        return enumeration<Wrap3D>(v,
                                   {{"clamp", Wrap3D::Clamp}, {"repeat", Wrap3D::Repeat}, {"mirror", Wrap3D::Mirror}});
    };
    if (const auto* v = value.find("wrap_u"))
        result.wrap_u = wrap(*v);
    if (const auto* v = value.find("wrap_v"))
        result.wrap_v = wrap(*v);
#define TEXTURE(name)                                                                                                  \
    if (const auto* v = value.find(#name))                                                                             \
    result.name = texture(*v)
    TEXTURE(texture);
    TEXTURE(normal);
    TEXTURE(metallic_roughness);
    TEXTURE(occlusion);
    TEXTURE(emissive_texture);
#undef TEXTURE
    return result;
}
MaterialVariant3D layer(const JsonValue& value, bool root) {
    if (!root)
        fields(value, {"material", "parts"});
    MaterialVariant3D result;
    if (const auto* material = value.find("material"))
        result.material = patch(*material);
    if (const auto* parts = value.find("parts")) {
        require(parts->is_array() && parts->items().size() <= 256, "parts must be an array of at most 256 rules");
        std::set<std::string> selectors;
        for (const auto& item : parts->items()) {
            fields(item, {"part", "material"});
            const auto* selector = item.find("part");
            const auto* material = item.find("material");
            require(selector && material, "part rule requires 'part' and 'material'");
            MaterialPartPatch3D part;
            if (selector->is_number()) {
                const auto n = selector->as_number();
                require(std::isfinite(n) && n >= 0 && n <= std::numeric_limits<u32>::max() && std::floor(n) == n,
                        "invalid part index");
                part.index = u32(n);
            } else
                part.name = text(*selector);
            const auto key = part.index ? "index:" + std::to_string(*part.index) : "name:" + part.name;
            require(selectors.insert(key).second, "duplicate part selector '" + key + "'");
            part.material = patch(*material);
            result.parts.push_back(std::move(part));
        }
    }
    return result;
}
void merge(MaterialPatch3D& to, const MaterialPatch3D& from) {
#define FIELD(name)                                                                                                    \
    if (from.name)                                                                                                     \
    to.name = from.name
    FIELD(shader);
    FIELD(shader_params);
    FIELD(color);
    FIELD(metallic);
    FIELD(roughness);
    FIELD(emissive);
    FIELD(normal_scale);
    FIELD(occlusion_strength);
    FIELD(alpha_cutoff);
    FIELD(unlit);
    FIELD(double_sided);
    FIELD(casts_shadow);
    FIELD(receives_shadow);
    FIELD(wireframe);
    FIELD(blend);
    FIELD(wrap_u);
    FIELD(wrap_v);
    FIELD(texture);
    FIELD(normal);
    FIELD(metallic_roughness);
    FIELD(occlusion);
    FIELD(emissive_texture);
#undef FIELD
}
void apply_layer(std::vector<MaterialPatch3D>& patches, const MaterialVariant3D& layer, const Model3D& model) {
    for (auto& target : patches)
        merge(target, layer.material);
    std::set<size_t> selected;
    for (const auto& rule : layer.parts) {
        size_t index = model.parts.size();
        if (rule.index)
            index = *rule.index;
        else {
            for (size_t i = 0; i < model.parts.size(); ++i)
                if (model.parts[i].name == rule.name) {
                    require(index == model.parts.size(), "ambiguous part name '" + rule.name + "'; use a part index");
                    index = i;
                }
        }
        require(index < model.parts.size(),
                "part selector not found: " + (rule.index ? std::to_string(*rule.index) : rule.name));
        require(selected.insert(index).second, "multiple rules select the same part");
        merge(patches[index], rule.material);
    }
}
} // namespace

MaterialsLoadResult3D parse_materials3d_asset(std::string_view text, const std::filesystem::path& source) {
    MaterialsLoadResult3D result;
    try {
        require(text.size() <= 1024 * 1024, "material source exceeds 1 MiB");
        const auto parsed = parse_json(text);
        require(parsed.ok(), "invalid JSON: " + parsed.error);
        const auto& json = *parsed.value;
        fields(json, {"schema", "material", "parts", "variants"});
        const auto* schema = json.find("schema");
        require(schema && schema->is_string() && schema->as_string() == "kin.materials3d/1",
                "unsupported material schema");
        MaterialsAsset3D asset{.source = source.lexically_normal(), .base = layer(json, true)};
        if (const auto* variants = json.find("variants")) {
            require(variants->is_object() && variants->members().size() <= 128,
                    "variants must be an object of at most 128 variants");
            for (const auto& [name, value] : variants->members()) {
                require(!name.empty() && name.size() <= 4096, "invalid variant name");
                asset.variants.emplace(name, layer(value, false));
            }
        }
        result.asset = std::move(asset);
    } catch (const std::exception& error) {
        result.diagnostics.push_back(source.string() + ": " + error.what());
    }
    return result;
}
MaterialsLoadResult3D load_materials3d_asset(const std::filesystem::path& source) {
    const auto bytes = content_file_size(source);
    if (!bytes || *bytes > 1024 * 1024)
        return {{}, {source.string() + ": source missing or exceeds 1 MiB"}};
    const auto text = read_content_text(source);
    if (!text)
        return {{}, {source.string() + ": cannot read material source"}};
    return parse_materials3d_asset(*text, source);
}

MaterialBinding3D build_materials3d_binding(const MaterialsAsset3D& asset, const std::shared_ptr<const Model3D>& model,
                                            std::string_view variant, Assets3D& assets) {
    require(model && model->valid() && model->parts.size() <= 4096,
            "materials require a valid model with at most 4096 parts");
    std::vector<MaterialPatch3D> patches(model->parts.size());
    apply_layer(patches, asset.base, *model);
    if (!variant.empty()) {
        const auto selected = asset.variants.find(std::string(variant));
        require(selected != asset.variants.end(), "unknown material variant '" + std::string(variant) + "'");
        apply_layer(patches, selected->second, *model);
    }
    MaterialBinding3D result{.model = model};
    if (!asset.source.empty())
        result.source_files.push_back((assets.root() / asset.source).lexically_normal());
    // A per-build lookup also shares maps when the bounded library cache is off.
    std::map<std::tuple<std::filesystem::path, bool, bool>, Texture> textures;
    const auto resolve = [&](const MaterialTexture3D& reference, bool srgb) -> Texture {
        if (reference.asset.empty())
            return {};
        const auto path = (assets.root() / reference.asset).lexically_normal();
        auto [entry, added] = textures.try_emplace(std::tuple{path, reference.nearest, srgb});
        if (added) {
            std::string error;
            entry->second = assets.load_texture(path, reference.nearest, srgb);
            error = kin::diagnostics3d_text(assets.diagnostics());
            require(bool(entry->second), "texture '" + path.string() + "': " + error);
            result.source_files.push_back(path);
        }
        return entry->second;
    };
    std::map<std::filesystem::path, MaterialShader3D> shaders;
    const auto resolve_shader = [&](const std::filesystem::path& source) {
        const auto path = (assets.root() / source).lexically_normal();
        auto [entry, added] = shaders.try_emplace(path);
        if (added) {
            std::string error;
            entry->second = assets.load_material_shader(path);
            error = kin::diagnostics3d_text(assets.diagnostics());
            require(bool(entry->second), error);
            result.source_files.push_back(path);
        }
        return entry->second;
    };
    result.materials.reserve(model->parts.size());
    for (size_t i = 0; i < patches.size(); ++i) {
        const auto& p = patches[i];
        auto material = model->parts[i].material;
#define FIELD(name)                                                                                                    \
    if (p.name)                                                                                                        \
    material.name = *p.name
        FIELD(shader_params);
        FIELD(color);
        FIELD(metallic);
        FIELD(roughness);
        FIELD(emissive);
        FIELD(normal_scale);
        FIELD(occlusion_strength);
        FIELD(alpha_cutoff);
        FIELD(unlit);
        FIELD(double_sided);
        FIELD(casts_shadow);
        FIELD(receives_shadow);
        FIELD(wireframe);
        FIELD(blend);
        FIELD(wrap_u);
        FIELD(wrap_v);
#undef FIELD
#define TEXTURE(name, srgb)                                                                                            \
    if (p.name)                                                                                                        \
    material.name = resolve(*p.name, srgb)
        TEXTURE(texture, true);
        TEXTURE(emissive_texture, true);
        TEXTURE(metallic_roughness, false);
        TEXTURE(normal, false);
        TEXTURE(occlusion, false);
#undef TEXTURE
        if (p.shader) {
            material.shader = {};
            if (!p.shader->empty()) {
                material.shader = resolve_shader(*p.shader);
            }
        }
        result.materials.push_back(std::move(material));
    }
    std::ranges::sort(result.source_files);
    result.source_files.erase(std::unique(result.source_files.begin(), result.source_files.end()),
                              result.source_files.end());
    return result;
}
} // namespace kin
