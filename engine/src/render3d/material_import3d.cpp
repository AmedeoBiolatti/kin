#include "material_import3d_impl.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <kin/assets/image.hpp>
#include <kin/render3d/material_import3d.hpp>
#include <kin/renderer/color.hpp>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace kin {
namespace {

// --- Godot's text resources (.tres, .tscn): sections and their properties ---

// A property's value: a number, bool, string, a call (Color(...), ExtResource("id"), Vector3(...)) or, for what this
// reader does not take apart (arrays, dictionaries), its text.
struct GodotValue {
    enum class Kind { Number, Bool, String, Call, Null, Raw } kind = Kind::Null;
    double number = 0.0;
    bool boolean = false;
    std::string text; // a string's, a call's name, the raw text
    std::vector<GodotValue> args;
};

struct GodotSection {
    std::string tag;                           // gd_resource, ext_resource, sub_resource, resource
    std::map<std::string, GodotValue> heading; // the [tag key=value ...] pairs
    std::map<std::string, GodotValue> properties;
    std::vector<std::string> order; // the properties in the file's order
};

class GodotReader {
public:
    explicit GodotReader(std::string_view text) : _text(text) {}

    std::vector<GodotSection> read() {
        std::vector<GodotSection> sections;
        while (skip_space_and_comments(), _at < _text.size()) {
            if (_text[_at] == '[') {
                ++_at;
                GodotSection section;
                section.tag = identifier();
                for (;;) {
                    skip_inline_space();
                    if (peek() == ']') {
                        ++_at;
                        break;
                    }
                    const std::string key = identifier();
                    expect('=');
                    section.heading[key] = value();
                }
                sections.push_back(std::move(section));
            } else {
                if (sections.empty())
                    fail("a property before any section");
                const std::string key = property_name();
                expect('=');
                auto& section = sections.back();
                section.order.push_back(key);
                section.properties[key] = value();
            }
        }
        return sections;
    }

private:
    [[noreturn]] void fail(const std::string& why) const {
        const auto line = 1 + std::count(_text.begin(), _text.begin() + std::min(_at, _text.size()), '\n');
        throw std::runtime_error("line " + std::to_string(line) + ": " + why);
    }
    char peek() const { return _at < _text.size() ? _text[_at] : '\0'; }
    void skip_inline_space() {
        while (_at < _text.size() && (_text[_at] == ' ' || _text[_at] == '\t'))
            ++_at;
    }
    void skip_space() {
        while (_at < _text.size() && std::isspace(static_cast<unsigned char>(_text[_at])))
            ++_at;
    }
    void skip_space_and_comments() {
        for (;;) {
            skip_space();
            if (peek() != ';')
                return;
            while (_at < _text.size() && _text[_at] != '\n')
                ++_at;
        }
    }
    void expect(char c) {
        skip_space();
        if (peek() != c)
            fail(std::string("expected '") + c + "'");
        ++_at;
    }
    std::string identifier() {
        skip_inline_space();
        const size_t start = _at;
        while (_at < _text.size() &&
               (std::isalnum(static_cast<unsigned char>(_text[_at])) || _text[_at] == '_' || _text[_at] == '/'))
            ++_at;
        if (_at == start)
            fail("expected a name");
        return std::string(_text.substr(start, _at - start));
    }
    // A property's name runs to the '=' (it may hold '/', ':' and digits: "shader_parameter/albedo", "0/name").
    std::string property_name() {
        const size_t start = _at;
        while (_at < _text.size() && _text[_at] != '=' && _text[_at] != '\n')
            ++_at;
        std::string name(_text.substr(start, _at - start));
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
            name.pop_back();
        if (name.empty() || peek() != '=')
            fail("expected 'name = value'");
        return name;
    }
    std::string quoted() {
        ++_at; // the opening quote
        std::string out;
        while (_at < _text.size() && _text[_at] != '"') {
            if (_text[_at] == '\\' && _at + 1 < _text.size()) {
                const char e = _text[++_at];
                out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            } else {
                out += _text[_at];
            }
            ++_at;
        }
        if (_at >= _text.size())
            fail("an unterminated string");
        ++_at;
        return out;
    }
    // Brackets kept whole, strings inside them included.
    std::string bracketed() {
        const size_t start = _at;
        int depth = 0;
        do {
            const char c = _text[_at];
            if (c == '"') {
                quoted();
                continue;
            }
            depth += c == '[' || c == '{' || c == '(';
            depth -= c == ']' || c == '}' || c == ')';
            ++_at;
        } while (_at < _text.size() && depth > 0);
        if (depth > 0)
            fail("unbalanced brackets");
        return std::string(_text.substr(start, _at - start));
    }
    GodotValue value() {
        skip_space();
        GodotValue v;
        const char c = peek();
        if (c == '"' || (c == '&' && _at + 1 < _text.size() && _text[_at + 1] == '"')) {
            if (c == '&')
                ++_at; // a StringName
            v.kind = GodotValue::Kind::String;
            v.text = quoted();
        } else if (c == '[' || c == '{') {
            v.kind = GodotValue::Kind::Raw;
            v.text = bracketed();
        } else if (c == '-' || c == '+' || c == '.' || std::isdigit(static_cast<unsigned char>(c))) {
            const char* begin = _text.data() + _at;
            char* end = nullptr;
            v.number = std::strtod(begin, &end);
            if (end == begin)
                fail("expected a number");
            _at += size_t(end - begin);
            v.kind = GodotValue::Kind::Number;
        } else {
            const std::string name = identifier();
            skip_inline_space();
            if (name == "true" || name == "false") {
                v.kind = GodotValue::Kind::Bool;
                v.boolean = name == "true";
            } else if (name == "null") {
                v.kind = GodotValue::Kind::Null;
            } else if (name == "inf" || name == "nan") {
                v.kind = GodotValue::Kind::Number;
                v.number = name == "inf" ? INFINITY : NAN;
            } else if (peek() == '(') {
                ++_at;
                v.kind = GodotValue::Kind::Call;
                v.text = name;
                skip_space();
                while (peek() != ')') {
                    v.args.push_back(value());
                    skip_space();
                    if (peek() == ',')
                        ++_at;
                    else if (peek() != ')')
                        fail("expected ',' or ')' in " + name + "(...)");
                    skip_space();
                }
                ++_at;
            } else {
                v.kind = GodotValue::Kind::Raw;
                v.text = name;
            }
        }
        return v;
    }

    std::string_view _text;
    size_t _at = 0;
};

using material_import::byte;
using material_import::read_file;

// --- Godot's StandardMaterial3D / ORMMaterial3D ---

// The properties read, with Godot 4's defaults (BaseMaterial3D), for those this import maps or checks.
struct GodotMaterial {
    const GodotSection& resource;
    const std::map<std::string, const GodotSection*>& ext;
    const std::map<std::string, const GodotSection*>& sub;

    const GodotValue* find(const std::string& key) const {
        const auto it = resource.properties.find(key);
        return it == resource.properties.end() ? nullptr : &it->second;
    }
    double number(const std::string& key, double fallback) const {
        const auto* v = find(key);
        if (!v)
            return fallback;
        if (v->kind == GodotValue::Kind::Number)
            return v->number;
        if (v->kind == GodotValue::Kind::Bool)
            return v->boolean ? 1.0 : 0.0;
        throw std::runtime_error("'" + key + "' is not a number");
    }
    bool flag(const std::string& key, bool fallback = false) const { return number(key, fallback ? 1 : 0) != 0.0; }
    std::array<f32, 4> color(const std::string& key, std::array<f32, 4> fallback) const {
        const auto* v = find(key);
        if (!v)
            return fallback;
        if (v->kind != GodotValue::Kind::Call || v->text != "Color" || v->args.size() < 3)
            throw std::runtime_error("'" + key + "' is not a Color");
        std::array<f32, 4> c{1, 1, 1, 1};
        for (size_t i = 0; i < std::min<size_t>(4, v->args.size()); ++i)
            c[i] = f32(v->args[i].number);
        return c;
    }
    std::array<f32, 3> vector3(const std::string& key, std::array<f32, 3> fallback) const {
        const auto* v = find(key);
        if (!v || v->kind != GodotValue::Kind::Call || v->args.size() < 3)
            return fallback;
        return {f32(v->args[0].number), f32(v->args[1].number), f32(v->args[2].number)};
    }
};

} // namespace

Result3D<ImportedMaterial3D> import_godot_material(const std::filesystem::path& source,
                                                   const MaterialImportOptions3D& options) {
    Result3D<ImportedMaterial3D> result;
    try {
        const std::string text = read_file(source);
        const auto sections = GodotReader(text).read();
        if (sections.empty() || sections.front().tag != "gd_resource")
            throw std::runtime_error("not a Godot text resource");
        const auto type_it = sections.front().heading.find("type");
        const std::string type = type_it != sections.front().heading.end() ? type_it->second.text : "";
        if (type != "StandardMaterial3D" && type != "ORMMaterial3D")
            throw std::runtime_error("a " + type + ", not a StandardMaterial3D or ORMMaterial3D");
        std::map<std::string, const GodotSection*> ext, sub;
        const GodotSection* resource = nullptr;
        for (const auto& section : sections) {
            const auto id = section.heading.find("id");
            if (section.tag == "ext_resource" && id != section.heading.end())
                ext[id->second.kind == GodotValue::Kind::String ? id->second.text
                                                                : std::to_string(int(id->second.number))] = &section;
            else if (section.tag == "sub_resource" && id != section.heading.end())
                sub[id->second.text] = &section;
            else if (section.tag == "resource")
                resource = &section;
        }
        static const GodotSection empty;
        const GodotMaterial m{resource ? *resource : empty, ext, sub};

        ImportedMaterial3D imported;
        imported.name = source.stem().string();
        if (const auto* name = m.find("resource_name");
            name && name->kind == GodotValue::Kind::String && !name->text.empty())
            imported.name = name->text;
        material_import::Context x(imported, options, source.stem().string());
        const auto note = [&](std::string what) { x.note(std::move(what)); };

        // Physical light units: emission in nits (emission_intensity), as the project says.
        bool physical = false;
        if (const auto project = options.project / "project.godot"; std::filesystem::exists(project)) {
            const std::string settings = read_file(project);
            physical = settings.find("lights_and_shadows/use_physical_light_units=true") != std::string::npos;
        }

        // --- textures: a property's file, where kin reads it, and what is made from it
        const auto texture_file = [&](const std::string& key) -> std::optional<std::filesystem::path> {
            const auto* v = m.find(key);
            if (!v || v->kind == GodotValue::Kind::Null)
                return std::nullopt;
            if (v->kind != GodotValue::Kind::Call || v->args.empty())
                throw std::runtime_error("'" + key + "' is not a resource");
            const std::string id =
                v->args[0].kind == GodotValue::Kind::String ? v->args[0].text : std::to_string(int(v->args[0].number));
            if (v->text == "SubResource") {
                std::string what = "sub-resource";
                if (const auto it = sub.find(id); it != sub.end())
                    if (const auto t = it->second->heading.find("type"); t != it->second->heading.end())
                        what = t->second.text;
                note(key + ": a " + what + " made in the material, not a file: left out");
                return std::nullopt;
            }
            const auto it = ext.find(id);
            if (v->text != "ExtResource" || it == ext.end())
                throw std::runtime_error("'" + key + "' names no resource of the file");
            const auto path = it->second->heading.find("path");
            if (path == it->second->heading.end())
                throw std::runtime_error("'" + key + "''s resource has no path");
            std::string p = path->second.text;
            if (p.rfind("res://", 0) == 0)
                p = p.substr(6);
            const auto file = (std::filesystem::absolute(options.project) / p).lexically_normal();
            if (!std::filesystem::exists(file)) {
                note(key + ": " + file.string() + " is missing: left out");
                return std::nullopt;
            }
            return file;
        };
        const auto place = [&](const std::filesystem::path& file) { return x.place(file); };
        const auto made = [&](const std::string& suffix, const Image& image) { return x.made(suffix, image); };
        const auto load = [&](const std::filesystem::path& file, const std::string& role) {
            return x.load(file, role);
        };
        using material_import::channel; // Godot's channel selectors are these: R, G, B, A, grayscale
        using material_import::texel;

        // The sampler of every map: texture_filter (0 nearest, 1 linear, then each with mipmaps, then anisotropic)
        // and texture_repeat.
        const int filter = int(m.number("texture_filter", 3));
        const bool repeat = m.flag("texture_repeat", true);
        TextureSampler3D sampler;
        sampler.wrap_u = sampler.wrap_v = repeat ? Wrap3D::Repeat : Wrap3D::Clamp;
        sampler.min = sampler.mag = filter % 2 == 0 ? TextureFilter3D::Nearest : TextureFilter3D::Linear;
        sampler.mip = filter >= 2 ? MipFilter3D::Linear : MipFilter3D::None;
        const auto map = [&](std::filesystem::path path) {
            return MaterialTexture3D{.asset = std::move(path), .sampler = sampler};
        };
        const MaterialTexture3D none{};

        auto& p = imported.asset.base.material;
        // Albedo: its colour (sRGB, as kin's) times its map.
        const auto albedo = m.color("albedo_color", {1, 1, 1, 1});
        p.color = Color{byte(albedo[0]), byte(albedo[1]), byte(albedo[2]), byte(albedo[3])};
        const auto albedo_texture = texture_file("albedo_texture");
        p.texture = albedo_texture ? map(place(*albedo_texture)) : none;

        // Metallic and roughness: factors times a channel of each map; kin's one map has roughness in G and metallic
        // in B (glTF's). ORMMaterial3D's orm_texture is that already (and its R the occlusion).
        p.metallic = f32(std::clamp(m.number("metallic", 0.0), 0.0, 1.0));
        p.roughness = f32(std::clamp(m.number("roughness", 1.0), 0.0, 1.0));
        const auto orm = type == "ORMMaterial3D" ? texture_file("orm_texture") : std::nullopt;
        std::optional<std::filesystem::path> occlusion_from_orm;
        if (orm) {
            p.metallic_roughness = map(place(*orm));
            occlusion_from_orm = orm;
        } else {
            const auto metal = texture_file("metallic_texture"), rough = texture_file("roughness_texture");
            const int metal_channel = int(m.number("metallic_texture_channel", 0)),
                      rough_channel = int(m.number("roughness_texture_channel", 0));
            if (metal && rough && *metal == *rough && metal_channel == 2 && rough_channel == 1) {
                p.metallic_roughness = map(place(*metal)); // packed as kin's already
            } else if (metal || rough) {
                const auto metal_loaded = metal ? load(*metal, "metallic_texture") : std::nullopt;
                const auto rough_loaded = rough ? load(*rough, "roughness_texture") : std::nullopt;
                const bool use_metal = bool(metal_loaded), use_rough = bool(rough_loaded);
                const Image metal_image = metal_loaded.value_or(Image{}), rough_image = rough_loaded.value_or(Image{});
                const Vec2i size{std::max(metal_image.size.x, rough_image.size.x),
                                 std::max(metal_image.size.y, rough_image.size.y)};
                if (use_metal && use_rough && metal_image.size != rough_image.size)
                    note("metallic and roughness maps of different sizes: packed at the larger, the other resampled "
                         "(nearest)");
                Image packed{size, std::vector<u8>(size_t(size.x) * size.y * 4, 255)};
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x) {
                        u8* out_pixel = &packed.rgba[(size_t(y) * size.x + x) * 4];
                        if (use_rough)
                            out_pixel[1] = channel(rough_image, texel(rough_image, size, x, y), rough_channel);
                        if (use_metal)
                            out_pixel[2] = channel(metal_image, texel(metal_image, size, x, y), metal_channel);
                    }
                if (use_metal || use_rough)
                    p.metallic_roughness = map(made("_mr", packed));
                else
                    p.metallic_roughness = none;
            } else {
                p.metallic_roughness = none;
            }
        }
        if (std::abs(m.number("metallic_specular", 0.5) - 0.5) > 1e-6)
            note("metallic_specular (a dielectric's reflectance) is not 0.5: kin's is always 4% (0.5)");

        // Normal map (Godot's are as glTF's: green up).
        const auto normal = m.flag("normal_enabled") ? texture_file("normal_texture") : std::nullopt;
        p.normal = normal ? map(place(*normal)) : none;
        p.normal_scale = f32(std::clamp(m.number("normal_scale", 1.0), 0.0, 100.0));

        // Ambient occlusion: kin reads it from R.
        p.occlusion = none;
        p.occlusion_strength = 1.0f;
        if (m.flag("ao_enabled")) {
            const auto ao = occlusion_from_orm ? occlusion_from_orm : texture_file("ao_texture");
            const int ao_channel = occlusion_from_orm ? 0 : int(m.number("ao_texture_channel", 0));
            if (ao && ao_channel == 0) {
                p.occlusion = map(place(*ao));
            } else if (const auto loaded = ao ? load(*ao, "ao_texture") : std::nullopt) {
                Image image = *loaded;
                for (size_t i = 0; i < size_t(image.size.x) * image.size.y; ++i)
                    image.rgba[i * 4] = channel(image, i, ao_channel);
                p.occlusion = map(made("_ao", image));
            }
            if (m.number("ao_light_affect", 0.0) > 0.0)
                note("ao_light_affect: kin's occlusion darkens the ambient light only");
        }

        // Emission: (colour + map) or colour times map, by its operator, times its energy (and, with physical light
        // units, its intensity in nits). kin's is a colour times a map times a strength: an added colour is put into a
        // map made of both, scaled by its brightest texel.
        p.emission_mode = EmissionMode3D::Independent;
        p.emission_color = LinearRgb{1, 1, 1};
        p.emissive = 0.0f;
        p.emissive_texture = none;
        if (m.flag("emission_enabled")) {
            const auto c = m.color("emission", {0, 0, 0, 1});
            const LinearRgb colour{srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2])};
            const f32 energy = f32(m.number("emission_energy_multiplier", 1.0) *
                                   (physical ? m.number("emission_intensity", 1000.0) : 1.0));
            const auto texture = texture_file("emission_texture");
            const bool add = int(m.number("emission_operator", 0)) == 0;
            const bool black = colour.r == 0 && colour.g == 0 && colour.b == 0;
            if (!texture) {
                p.emission_color = colour;
                p.emissive = black ? 0.0f : energy;
            } else if (!add || black) {
                p.emission_color = add ? LinearRgb{1, 1, 1} : colour;
                p.emissive = energy;
                p.emissive_texture = map(place(*texture));
            } else if (const auto loaded = load(*texture, "emission_texture"); !loaded) {
                p.emission_color = colour;
                p.emissive = energy; // the colour alone
            } else {
                Image image = *loaded;
                std::vector<f32> linear(size_t(image.size.x) * image.size.y * 3);
                f32 peak = 0.0f;
                for (size_t i = 0; i < linear.size() / 3; ++i)
                    for (int k = 0; k < 3; ++k) {
                        const f32 v = srgb_to_linear(image.rgba[i * 4 + k] / 255.0f) + (&colour.r)[k];
                        linear[i * 3 + k] = v;
                        peak = std::max(peak, v);
                    }
                for (size_t i = 0; i < linear.size() / 3; ++i)
                    for (int k = 0; k < 3; ++k)
                        image.rgba[i * 4 + k] = byte(linear_to_srgb(peak > 0 ? linear[i * 3 + k] / peak : 0.0f));
                p.emissive = energy * peak;
                p.emissive_texture = map(made("_emission", image));
            }
            p.emissive = std::min(*p.emissive, 1e6f);
            if (physical)
                note("emission in nits (the project's physical light units): match kin's exposure to Godot's");
        }

        else if (m.find("emission_texture") || m.find("emission") || m.find("emission_energy_multiplier"))
            note("emission set up but switched off (emission_enabled): imported off (a script may switch it on: "
                 "set emissive then)");

        // Transparency (0 none, 1 alpha, 2 scissor, 3 hash, 4 depth pre-pass), blend mode, cull mode, shading.
        const int transparency = int(m.number("transparency", 0));
        p.blend = transparency == 1 || transparency == 4   ? Blend3D::Alpha
                  : transparency == 2 || transparency == 3 ? Blend3D::Mask
                                                           : Blend3D::Opaque;
        p.alpha_cutoff = transparency == 2 ? f32(m.number("alpha_scissor_threshold", 0.5)) : 0.5f;
        if (transparency == 3)
            note("alpha hash: a cut-out at half (kin has no hashed alpha)");
        if (transparency == 4)
            note("depth pre-pass transparency: blended (kin draws no depth for it)");
        const int blend_mode = int(m.number("blend_mode", 0));
        if (blend_mode == 1)
            p.blend = Blend3D::Additive;
        else if (blend_mode > 1)
            note(std::string("blend mode ") + (blend_mode == 2 ? "subtract" : "multiply") +
                 ": kin blends by alpha (or adds)");
        const int cull = int(m.number("cull_mode", 0));
        p.double_sided = cull == 2;
        if (cull == 1)
            note("cull_mode front: kin culls back faces (or none)");
        p.unlit = int(m.number("shading_mode", 1)) == 0;
        p.receives_shadow = !m.flag("disable_receive_shadows");

        // What kin does not show: said once each.
        static const std::set<std::string> read{
            "resource_name", "albedo_color", "albedo_texture", "metallic", "metallic_texture",
            "metallic_texture_channel", "metallic_specular", "roughness", "roughness_texture",
            "roughness_texture_channel", "orm_texture", "normal_enabled", "normal_texture", "normal_scale",
            "ao_enabled", "ao_texture", "ao_texture_channel", "ao_light_affect", "emission_enabled", "emission",
            "emission_energy_multiplier", "emission_intensity", "emission_texture", "emission_operator", "transparency",
            "alpha_scissor_threshold", "blend_mode", "cull_mode", "shading_mode", "disable_receive_shadows",
            "texture_filter", "texture_repeat",
            // Harmless where they are: kin's anisotropy and alpha antialiasing are its own; vertex colours always
            // multiply (white without); the resource's local data.
            "alpha_antialiasing_mode", "alpha_antialiasing_edge", "vertex_color_use_as_albedo",
            "resource_local_to_scene", "render_priority", "uv1_scale", "uv1_offset"};
        for (const auto& key : m.resource.order)
            if (!read.contains(key)) {
                const auto* v = m.find(key);
                const bool off = v && ((v->kind == GodotValue::Kind::Bool && !v->boolean) ||
                                       (v->kind == GodotValue::Kind::Number && v->number == 0.0) ||
                                       v->kind == GodotValue::Kind::Null);
                if (!off)
                    note(key + ": not shown by kin");
            }
        // Tiling: Godot's UV is the mesh's times uv1_scale plus uv1_offset (their x and y), as kin's.
        const auto scale = m.vector3("uv1_scale", {1, 1, 1});
        const auto offset = m.vector3("uv1_offset", {0, 0, 0});
        p.uv_scale = Vec2f{scale[0], scale[1]};
        p.uv_offset = Vec2f{offset[0], offset[1]};

        x.finish();
        result.value = std::move(imported);
    } catch (const std::exception& error) {
        result.diagnostics.push_back(source.string() + ": " + error.what());
    }
    return result;
}

} // namespace kin
