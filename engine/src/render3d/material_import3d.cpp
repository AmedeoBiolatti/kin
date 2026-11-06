#include "material_import3d_impl.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <kin/assets/image.hpp>
#include <kin/render3d/material_graph3d.hpp>
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

    // An array's text ("[a, b, ...]", kept whole by read) as its values.
    static std::vector<GodotValue> array(std::string_view raw) {
        GodotReader r(raw);
        std::vector<GodotValue> out;
        r.skip_space();
        if (r.peek() != '[')
            return out;
        ++r._at;
        for (;;) {
            r.skip_space();
            if (r._at >= r._text.size() || r.peek() == ']')
                break;
            out.push_back(r.value());
            r.skip_space();
            if (r.peek() == ',')
                ++r._at;
        }
        return out;
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

// --- Tier 2: Godot's visual shaders as kin's material graphs ---

namespace kin {
namespace {

using material_import::GraphBuilder;
namespace fs = std::filesystem;

// A value of Godot's (a number, bool, Vector2/3/4, Quaternion, Color: raw) as a graph value's JSON text.
std::optional<std::string> godot_json(const GodotValue& v) {
    if (v.kind == GodotValue::Kind::Number)
        return GraphBuilder::number(v.number);
    if (v.kind == GodotValue::Kind::Bool)
        return std::string(v.boolean ? "1" : "0");
    if (v.kind != GodotValue::Kind::Call || v.args.empty() || v.args.size() > 4)
        return std::nullopt;
    std::vector<f32> c;
    for (const auto& a : v.args)
        c.push_back(f32(a.number));
    return c.size() == 1 ? GraphBuilder::number(c[0]) : GraphBuilder::vector(c);
}

std::string godot_id(const GodotValue& v) {
    return v.kind == GodotValue::Kind::String ? v.text : std::to_string(i64(v.number));
}

// A visual shader's resources: its file's sections (or the material's, inline) by id.
struct GodotResources {
    std::map<std::string, const GodotSection*> ext, sub;
    fs::path project;
    // An ExtResource's file in the project (res:// paths), or none.
    std::optional<fs::path> file(const GodotValue& v) const {
        if (v.kind != GodotValue::Kind::Call || v.text != "ExtResource" || v.args.empty())
            return std::nullopt;
        const auto it = ext.find(godot_id(v.args[0]));
        if (it == ext.end())
            return std::nullopt;
        const auto path = it->second->heading.find("path");
        if (path == it->second->heading.end())
            return std::nullopt;
        std::string p = path->second.text;
        if (p.rfind("res://", 0) == 0)
            p = p.substr(6);
        return (fs::absolute(project) / p).lexically_normal();
    }
};

class VisualShaderGraph {
public:
    struct Texture {
        std::string param;
        std::optional<fs::path> file; // the shader's own (a Texture node's), else the material's
        bool srgb = false, normal = false;
    };
    std::vector<Texture> textures;
    std::map<int, std::pair<int, int>> outputs; // the output node's port -> (node, port)

    VisualShaderGraph(const GodotSection& shader, const GodotResources& r, GraphBuilder& b, material_import::Context& x)
        : _shader(shader), _r(r), _b(b), _x(x) {
        for (const auto& [key, v] : shader.properties) {
            if (key.rfind("nodes/fragment/", 0) != 0)
                continue;
            const std::string rest = key.substr(15);
            if (rest == "connections") {
                for (size_t i = 0; i + 3 < v.args.size(); i += 4) {
                    const int from = int(v.args[i].number), from_port = int(v.args[i + 1].number);
                    const int to = int(v.args[i + 2].number), to_port = int(v.args[i + 3].number);
                    _edges[{to, to_port}] = {from, from_port};
                    if (to == 0)
                        outputs[to_port] = {from, from_port};
                }
                continue;
            }
            const size_t slash = rest.find('/');
            if (slash == std::string::npos)
                continue;
            const int id = std::atoi(rest.substr(0, slash).c_str());
            const std::string field = rest.substr(slash + 1);
            if (field == "node" && v.kind == GodotValue::Kind::Call && v.text == "SubResource" && !v.args.empty()) {
                const auto it = r.sub.find(godot_id(v.args[0]));
                if (it != r.sub.end())
                    _nodes[id] = it->second;
            } else {
                _fields[id][field] = &v;
            }
        }
    }

    bool connected(int node, int port) const { return _edges.contains({node, port}); }

    // What feeds `node`'s input `port` (at `width`): its connection, else its value in the file
    // (default_input_values), else `fallback` (Godot's default for the port).
    std::optional<std::string> input(int node, int port, int width, const std::string& fallback, std::string& why) {
        if (const auto e = _edges.find({node, port}); e != _edges.end()) {
            auto v = value(e->second.first, e->second.second, why);
            return v ? std::optional{_b.fit(*v, width)} : std::nullopt;
        }
        if (const auto* n = section(node))
            if (const auto it = n->properties.find("default_input_values"); it != n->properties.end()) {
                const auto values = GodotReader::array(it->second.text);
                for (size_t i = 0; i + 1 < values.size(); i += 2)
                    if (int(values[i].number) == port)
                        if (const auto j = godot_json(values[i + 1]))
                            return _b.fit(*j, width);
            }
        return _b.fit(fallback, width);
    }

    std::optional<std::string> value(int node, int port, std::string& why) {
        const auto key = std::pair{node, port};
        if (const auto it = _done.find(key); it != _done.end())
            return it->second;
        if (_visiting.contains(key)) {
            why = "a cycle";
            return std::nullopt;
        }
        _visiting.insert(key);
        auto v = translate(node, port, why);
        _visiting.erase(key);
        if (v)
            _done[key] = *v;
        return v;
    }

    const GodotSection* section(int node) const {
        const auto it = _nodes.find(node);
        return it == _nodes.end() ? nullptr : it->second;
    }

private:
    static std::string type_of(const GodotSection& s) {
        const auto t = s.heading.find("type");
        return t == s.heading.end() ? "" : t->second.text;
    }
    static f64 number(const GodotSection& s, const char* key, f64 fallback) {
        const auto it = s.properties.find(key);
        if (it == s.properties.end())
            return fallback;
        if (it->second.kind == GodotValue::Kind::Bool)
            return it->second.boolean ? 1 : 0;
        return it->second.kind == GodotValue::Kind::Number ? it->second.number : fallback;
    }
    static std::string text(const GodotSection& s, const char* key) {
        const auto it = s.properties.find(key);
        return it == s.properties.end() ? "" : it->second.text;
    }
    std::string glsl(const std::string& expr, int width, std::vector<std::pair<std::string, std::string>> inputs) {
        static const char* types[] = {"float", "float", "vec2", "vec3", "vec4"};
        return _b.add(
            "glsl", std::move(inputs),
            {{"glsl", GraphBuilder::quote(expr)}, {"result", GraphBuilder::quote(types[std::clamp(width, 1, 4)])}});
    }
    // A parameter of the shader's (by its name: the material's shader_parameter/<name>).
    std::optional<std::string> parameter(const GodotSection& n, const std::string& type, std::string& why) {
        const std::string name = text(n, "parameter_name");
        if (name.empty()) {
            why = "a parameter without a name";
            return std::nullopt;
        }
        if (_b.has_param(name))
            return GraphBuilder::quote(name);
        GraphBuilder::Param p{name, "float", {}};
        const auto d = n.properties.find("default_value");
        const bool given = number(n, "default_value_enabled", 0) != 0 && d != n.properties.end();
        const auto read = [&](size_t count) {
            if (given && d->second.kind == GodotValue::Kind::Call)
                for (size_t i = 0; i < std::min(count, d->second.args.size()); ++i)
                    p.value[i] = f32(d->second.args[i].number);
        };
        if (type == "VisualShaderNodeTexture2DParameter") {
            const int t = int(number(n, "texture_type", 0)); // data, colour, normal map
            _b.params.push_back({name, "texture", {}, "", t == 1});
            textures.push_back({name, std::nullopt, t == 1, t == 2});
            return GraphBuilder::quote(name);
        }
        if (type == "VisualShaderNodeColorParameter") {
            p.type = "color";
            p.value = {1, 1, 1, 1};
            read(4);
            for (int i = 0; i < 3; ++i) // source_color: sRGB, as Godot shows it
                p.value[size_t(i)] = srgb_to_linear(p.value[size_t(i)]);
        } else if (type == "VisualShaderNodeVec2Parameter" || type == "VisualShaderNodeVec3Parameter" ||
                   type == "VisualShaderNodeVec4Parameter") {
            const size_t count = size_t(type[22] - '0');
            p.type = count == 2 ? "vec2" : count == 3 ? "vec3" : "vec4";
            read(count);
        } else if (type == "VisualShaderNodeFloatParameter" || type == "VisualShaderNodeIntParameter" ||
                   type == "VisualShaderNodeUIntParameter" || type == "VisualShaderNodeBooleanParameter") {
            if (given)
                p.value[0] = d->second.kind == GodotValue::Kind::Bool ? (d->second.boolean ? 1.0f : 0.0f)
                                                                      : f32(d->second.number);
        } else {
            why = "a " + type.substr(16) + " (kin has no parameter like it)";
            return std::nullopt;
        }
        _b.params.push_back(p);
        return GraphBuilder::quote(name);
    }

    std::optional<std::string> translate(int id, int out, std::string& why) {
        const GodotSection* n = section(id);
        if (!n) {
            why = "no node " + std::to_string(id);
            return std::nullopt;
        }
        const std::string type = type_of(*n);
        const std::string kind = type.rfind("VisualShaderNode", 0) == 0 ? type.substr(16) : type;
        const std::string label = kind + " " + std::to_string(id);
        const auto in = [&](int port, int width, const std::string& fallback) {
            return input(id, port, width, fallback, why);
        };
        const int op = int(number(*n, "op_type", -1));
        // Ops' widths by their op_type (each class's enum and default).
        const auto vector_width = [&] { return op < 0 ? 3 : op + 2; }; // VectorBase: 2D, 3D (default), 4D
        static const int scalar_types[] = {1, 2, 2, 3, 3, 4, 4};       // Mix, Step, SmoothStep, Remap
        const int mixed = scalar_types[std::clamp(op, 0, 6)];
        const bool mixed_scalar = op == 2 || op == 4 || op == 6;
        if (kind == "FloatConstant" || kind == "IntConstant" || kind == "UIntConstant")
            return GraphBuilder::number(number(*n, "constant", 0));
        if (kind == "BooleanConstant")
            return std::string(number(*n, "constant", 0) != 0 ? "1" : "0");
        if (kind == "ColorConstant" || kind == "Vec2Constant" || kind == "Vec3Constant" || kind == "Vec4Constant") {
            const auto c = n->properties.find("constant");
            const auto dflt = kind == "ColorConstant"  ? "[1, 1, 1, 1]"
                              : kind == "Vec2Constant" ? "[0, 0]"
                              : kind == "Vec3Constant" ? "[0, 0, 0]"
                                                       : "[0, 0, 0, 0]";
            const auto v = c == n->properties.end() ? std::nullopt : godot_json(c->second);
            if (kind == "ColorConstant" && out == 1) // its alpha port (Godot 3's)
                return GraphBuilder::component(v.value_or(dflt), 3);
            return v.value_or(dflt);
        }
        if (kind.size() > 9 && kind.compare(kind.size() - 9, 9, "Parameter") == 0)
            return parameter(*n, type, why);
        if (kind == "Input") {
            const std::string name = text(*n, "input_name");
            static const std::map<std::string, std::string> builtins{
                {"uv", "uv"},           {"time", "time"},     {"color", "vertex_color"}, {"screen_uv", "screen"},
                {"vertex", "position"}, {"normal", "normal"}, {"view", "view"}};
            const auto b = builtins.find(name);
            if (b == builtins.end()) {
                why = label + ": its input '" + name + "' (kin's graphs have uv, time, color, vertex, normal, view)";
                return std::nullopt;
            }
            if (name == "vertex" || name == "normal" || name == "view")
                _x.note(label + ": Godot's " + name + " is in view space: kin's world " + b->second +
                        " used (alike where only angles count)");
            return GraphBuilder::quote(b->second);
        }
        if (kind == "Texture") {
            const int source = int(number(*n, "source", 0)); // 0 its texture, 5 a sampler port
            std::string param;
            if (source == 5) {
                const auto e = _edges.find({id, 2});
                const auto t = e == _edges.end() ? std::nullopt : value(e->second.first, e->second.second, why);
                if (!t || t->size() < 3 || t->front() != '"') {
                    if (why.empty())
                        why = label + ": no sampler";
                    return std::nullopt;
                }
                param = t->substr(1, t->size() - 2);
            } else if (source == 0) {
                param = "texture_" + std::to_string(id);
                if (!_b.has_param(param)) {
                    const int t = int(number(*n, "texture_type", 0)); // data, colour, normal map
                    const auto tex = n->properties.find("texture");
                    _b.params.push_back({param, "texture", {}, "", t == 1});
                    textures.push_back(
                        {param, tex == n->properties.end() ? std::nullopt : _r.file(tex->second), t == 1, t == 2});
                }
            } else {
                why = label + ": a " +
                      std::string(source == 1   ? "screen"
                                  : source == 4 ? "depth"
                                                : "built-in or 3D texture") +
                      " source (kin's graphs sample textures)";
                return std::nullopt;
            }
            const auto uv = in(0, 2, "\"uv\"");
            if (!uv)
                return std::nullopt;
            if (connected(id, 1))
                _x.note(label + ": its level of detail (kin samples by the mip chain)");
            return _b.add("texture", {{"uv", *uv}}, {{"texture", GraphBuilder::quote(param)}});
        }
        if (kind == "FloatOp" || kind == "IntOp" || kind == "UIntOp" || kind == "VectorOp") {
            const int w = kind == "VectorOp" ? vector_width() : 1;
            const int o = int(number(*n, "operator", 0));
            const auto zero = w == 1 ? std::string("0") : GraphBuilder::vector(std::vector<f32>(size_t(w), 0.0f));
            const auto a = in(0, w, zero), b = in(1, w, zero);
            if (!a || !b)
                return std::nullopt;
            static const char* ops[] = {"add", "subtract", "multiply", "divide", "mod", "power", "max", "min"};
            if (o >= 0 && o <= 7)
                return _b.add(ops[o], {{"a", *a}, {"b", *b}});
            const bool vec = kind == "VectorOp";
            if ((!vec && o == 9) || (vec && o == 11))
                return _b.add("step", {{"edge", *a}, {"x", *b}});
            if (vec && o == 8)
                return _b.add("cross", {{"a", _b.fit(*a, 3)}, {"b", _b.fit(*b, 3)}});
            if (vec && o == 10)
                return _b.add("reflect", {{"direction", *a}, {"normal", *b}});
            if ((!vec && o == 8) || (vec && o == 9))
                return glsl("atan(a, b)", w, {{"a", *a}, {"b", *b}});
            why = label + ": operator " + std::to_string(o);
            return std::nullopt;
        }
        if (kind == "FloatFunc" || kind == "IntFunc" || kind == "VectorFunc") {
            const bool vec = kind == "VectorFunc";
            const int w = vec ? vector_width() : 1;
            const int f = int(number(*n, "function", vec ? 0 : 13));
            const auto x =
                in(0, w, w == 1 ? std::string("0") : GraphBuilder::vector(std::vector<f32>(size_t(w), 0.0f)));
            if (!x)
                return std::nullopt;
            // Godot's functions by their enum (FloatFunc's; VectorFunc's names the same ones in its own order).
            static const char* floats[] = {
                "sin",  "cos",         "tan",      "asin",    "acos",       "atan",      "sinh",  "cosh",
                "tanh", "log",         "exp",      "sqrt",    "abs",        "sign",      "floor", "round",
                "ceil", "fract",       "saturate", "negate",  "acosh",      "asinh",     "atanh", "degrees",
                "exp2", "inversesqrt", "log2",     "radians", "reciprocal", "roundEven", "trunc", "one_minus"};
            static const char* vectors[] = {
                "normalize", "saturate",    "negate", "reciprocal", "abs",     "acos",     "acosh",     "asin", "asinh",
                "atan",      "atanh",       "ceil",   "cos",        "cosh",    "degrees",  "exp",       "exp2", "floor",
                "fract",     "inversesqrt", "log",    "log2",       "radians", "round",    "roundEven", "sign", "sin",
                "sinh",      "sqrt",        "tan",    "tanh",       "trunc",   "one_minus"};
            const char* fn = vec ? (f >= 0 && f < 33 ? vectors[f] : nullptr) : (f >= 0 && f < 32 ? floats[f] : nullptr);
            if (!fn) {
                why = label + ": function " + std::to_string(f);
                return std::nullopt;
            }
            static const std::set<std::string> kins{"sin",   "cos",      "tan",    "exp",       "log",
                                                    "sqrt",  "abs",      "sign",   "floor",     "ceil",
                                                    "fract", "saturate", "negate", "one_minus", "normalize"};
            if (kins.contains(fn))
                return _b.add(fn, {{"x", *x}});
            if (std::string(fn) == "reciprocal")
                return glsl("1.0 / x", w, {{"x", *x}});
            return glsl(std::string(fn) + "(x)", w, {{"x", *x}});
        }
        if (kind == "Mix") {
            const auto a = in(0, mixed, "0"), b = in(1, mixed, "1"), t = in(2, mixed_scalar ? 1 : mixed, "0.5");
            return a && b && t ? std::optional{_b.add("mix", {{"a", *a}, {"b", *b}, {"t", *t}})} : std::nullopt;
        }
        if (kind == "Clamp") {
            static const int widths[] = {1, 1, 1, 2, 3, 4};
            const int w = widths[std::clamp(op, 0, 5)];
            const auto x = in(0, w, "0"), lo = in(1, w, "0"), hi = in(2, w, "1");
            return x && lo && hi ? std::optional{_b.add("clamp", {{"x", *x}, {"min", *lo}, {"max", *hi}})}
                                 : std::nullopt;
        }
        if (kind == "Step") {
            const auto e = in(0, mixed_scalar ? 1 : mixed, "0"), x = in(1, mixed, "0");
            return e && x ? std::optional{_b.add("step", {{"edge", *e}, {"x", *x}})} : std::nullopt;
        }
        if (kind == "SmoothStep") {
            const int ew = mixed_scalar ? 1 : mixed;
            const auto a = in(0, ew, "0"), b = in(1, ew, "1"), x = in(2, mixed, "0.5");
            return a && b && x ? std::optional{_b.add("smoothstep", {{"edge0", *a}, {"edge1", *b}, {"x", *x}})}
                               : std::nullopt;
        }
        if (kind == "Remap") {
            const int rw = mixed_scalar ? 1 : mixed;
            const auto x = in(0, mixed, "0"), a = in(1, rw, "0"), b = in(2, rw, "1"), c = in(3, rw, "0"),
                       d = in(4, rw, "1");
            return x && a && b && c && d
                       ? std::optional{_b.add(
                             "remap", {{"x", *x}, {"in_min", *a}, {"in_max", *b}, {"out_min", *c}, {"out_max", *d}})}
                       : std::nullopt;
        }
        if (kind == "MultiplyAdd") {
            const int w = std::clamp(op, 0, 3) + 1;
            const auto a = in(0, w, "0"), b = in(1, w, "1"), c = in(2, w, "0");
            return a && b && c
                       ? std::optional{_b.add("add", {{"a", _b.add("multiply", {{"a", *a}, {"b", *b}})}, {"b", *c}})}
                       : std::nullopt;
        }
        if (kind == "DotProduct") {
            const auto a = in(0, 3, "[0, 0, 0]"), b = in(1, 3, "[0, 0, 0]");
            return a && b ? std::optional{_b.add("dot", {{"a", *a}, {"b", *b}})} : std::nullopt;
        }
        if (kind == "VectorLen") {
            const auto x = in(0, vector_width(), "0");
            return x ? std::optional{_b.add("length", {{"x", *x}})} : std::nullopt;
        }
        if (kind == "VectorDistance") {
            const auto a = in(0, vector_width(), "0"), b = in(1, vector_width(), "0");
            return a && b ? std::optional{_b.add("distance", {{"a", *a}, {"b", *b}})} : std::nullopt;
        }
        if (kind == "VectorCompose") {
            const int w = vector_width();
            std::vector<std::pair<std::string, std::string>> parts;
            for (int k = 0; k < w; ++k) {
                const auto c = in(k, 1, "0");
                if (!c)
                    return std::nullopt;
                parts.push_back({std::string(1, "xyzw"[k]), *c});
            }
            return _b.add("combine", parts);
        }
        if (kind == "VectorDecompose") {
            const auto x = in(0, vector_width(), "0");
            if (!x)
                return std::nullopt;
            return x->front() == '"' ? _b.member(*x, std::string(1, "xyzw"[std::clamp(out, 0, 3)]))
                                     : GraphBuilder::component(*x, out);
        }
        if (kind == "Fresnel") {
            const auto invert = in(2, 1, "0"), power = in(3, 1, "1");
            if (!invert || !power)
                return std::nullopt;
            std::vector<std::pair<std::string, std::string>> inputs{{"power", *power}};
            if (connected(id, 0)) {
                const auto normal = in(0, 3, "\"normal\"");
                if (!normal)
                    return std::nullopt;
                inputs.push_back({"normal", *normal});
            }
            if (connected(id, 1))
                _x.note(label + ": its view input (kin's fresnel takes the view's direction)");
            const std::string f = _b.add("fresnel", inputs);
            if (*invert == "0")
                return f;
            if (invert->front() != '"' && invert->front() != '[')
                return glsl("pow(clamp(dot(normalize(n), kin_view()), 0.0, 1.0), p)", 1,
                            {{"n", inputs.size() > 1 ? inputs[1].second : std::string("\"normal\"")}, {"p", *power}});
            why = label + ": a connected invert";
            return std::nullopt;
        }
        if (kind == "UVFunc") {
            const auto uv = in(0, 2, "\"uv\""), scale = in(1, 2, "[1, 1]"), offset = in(2, 2, "[0, 0]");
            if (!uv || !scale || !offset)
                return std::nullopt;
            if (int(number(*n, "function", 0)) == 0) // panning: the offset times the scale, plus the UV
                return _b.add("add", {{"a", _b.add("multiply", {{"a", *offset}, {"b", *scale}})}, {"b", *uv}});
            return _b.add(
                "add",
                {{"a", _b.add("multiply", {{"a", _b.add("subtract", {{"a", *uv}, {"b", *offset}})}, {"b", *scale}})},
                 {"b", *offset}}); // scaling about the offset (its pivot)
        }
        if (kind == "If") {
            const auto a = in(0, 1, "0"), b = in(1, 1, "0"), tol = in(2, 1, "1e-05"), eq = in(3, 3, "[0, 0, 0]"),
                       gt = in(4, 3, "[0, 0, 0]"), lt = in(5, 3, "[0, 0, 0]");
            if (!a || !b || !tol || !eq || !gt || !lt)
                return std::nullopt;
            return glsl("abs(a - b) < tol ? eq : (a < b ? lt : gt)", 3,
                        {{"a", *a}, {"b", *b}, {"tol", *tol}, {"eq", *eq}, {"gt", *gt}, {"lt", *lt}});
        }
        if (kind == "Switch") {
            static const int widths[] = {1, 1, 1, 2, 3, 4, 1};
            if (op == 7) {
                why = label + ": of transforms";
                return std::nullopt;
            }
            const int w = widths[std::clamp(op, 0, 6)];
            const auto c = in(0, 1, "0"), t = in(1, w, "1"), f = in(2, w, "0");
            return c && t && f ? std::optional{_b.add("mix", {{"a", *f}, {"b", *t}, {"t", *c}})} : std::nullopt;
        }
        if (kind == "Compare") {
            const int t = int(number(*n, "type", 0));
            if (t > 2 && t != 6) {
                why = label + ": of vectors";
                return std::nullopt;
            }
            const int f = int(number(*n, "function", 0));
            const auto a = in(0, 1, "0"), b = in(1, 1, "0"), tol = in(2, 1, "1e-05");
            if (!a || !b || !tol)
                return std::nullopt;
            static const char* tests[] = {
                "abs(a - b) < tol", "!(abs(a - b) < tol)", "a > b", "a >= b", "a < b", "a <= b"};
            return glsl(std::string("(") + tests[std::clamp(f, 0, 5)] + ") ? 1.0 : 0.0", 1,
                        {{"a", *a}, {"b", *b}, {"tol", *tol}});
        }
        if (kind == "Reroute")
            return in(0, 4, "0");
        if (kind == "Expression") {
            // One output, one assignment: "name = expression;" with the inputs by their names.
            const auto& f = _fields[id];
            const auto field = [&](const char* k) {
                const auto it = f.find(k);
                return it == f.end() ? std::string() : it->second->text;
            };
            std::string expr = field("expression");
            if (expr.empty())
                expr = text(*n, "expression");
            const auto ports = [](const std::string& s) { // "0,0,t;1,4,c;": port, type, name
                std::vector<std::tuple<int, int, std::string>> out;
                std::stringstream in(s);
                for (std::string item; std::getline(in, item, ';');) {
                    std::stringstream p(item);
                    std::string a, b, c;
                    if (std::getline(p, a, ',') && std::getline(p, b, ',') && std::getline(p, c, ','))
                        out.push_back({std::atoi(a.c_str()), std::atoi(b.c_str()), c});
                }
                return out;
            };
            const auto ins = ports(field("input_ports")), outs = ports(field("output_ports"));
            static const int widths[] = {1, 1, 1, 2, 3, 4, 1};
            const size_t eq = expr.find('=');
            const auto trim = [](std::string s) {
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                    s.pop_back();
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                    s.erase(s.begin());
                return s;
            };
            std::string body = eq == std::string::npos ? "" : trim(expr.substr(eq + 1));
            if (!body.empty() && body.back() == ';')
                body.pop_back();
            const bool simple = outs.size() == 1 && eq != std::string::npos &&
                                trim(expr.substr(0, eq)) == std::get<2>(outs[0]) && !body.empty() &&
                                body.find_first_of(";{}=") == std::string::npos && std::get<1>(outs[0]) <= 5;
            bool builtin = false; // Godot's built-ins (TIME, UV) aren't the graph's
            for (size_t i = 0; i + 1 < body.size(); ++i)
                if (std::isupper(static_cast<unsigned char>(body[i])) &&
                    std::isupper(static_cast<unsigned char>(body[i + 1])) &&
                    (i == 0 || !std::isalnum(static_cast<unsigned char>(body[i - 1]))))
                    builtin = true;
            if (!simple || builtin) {
                why = label + ": an Expression kin can't take (one output, one assignment, no built-ins)";
                return std::nullopt;
            }
            std::vector<std::pair<std::string, std::string>> inputs;
            for (const auto& [port, t, name] : ins) {
                const auto v = in(port, widths[std::clamp(t, 0, 6)], "0");
                if (!v)
                    return std::nullopt;
                inputs.push_back({name, *v});
            }
            _x.note(label + ": its expression as GLSL (Godot's shading language alike for one expression)");
            return glsl(body, widths[std::clamp(std::get<1>(outs[0]), 0, 6)], inputs);
        }
        why = label + ": a " + kind + " (kin has no node for it)";
        return std::nullopt;
    }

    const GodotSection& _shader;
    const GodotResources& _r;
    GraphBuilder& _b;
    material_import::Context& _x;
    std::map<int, const GodotSection*> _nodes;
    std::map<int, std::map<std::string, const GodotValue*>> _fields;
    std::map<std::pair<int, int>, std::pair<int, int>> _edges;
    std::map<std::pair<int, int>, std::string> _done;
    std::set<std::pair<int, int>> _visiting;
};

struct GodotFile {
    std::string text;
    std::vector<GodotSection> sections;
    GodotResources resources;
    const GodotSection* resource = nullptr;
    std::string type;

    GodotFile(const fs::path& file, const fs::path& project) : text(read_file(file)) {
        sections = GodotReader(text).read();
        if (sections.empty() || sections.front().tag != "gd_resource")
            throw std::runtime_error(file.filename().string() + ": not a Godot text resource");
        const auto t = sections.front().heading.find("type");
        type = t == sections.front().heading.end() ? "" : t->second.text;
        resources.project = project;
        for (const auto& s : sections) {
            const auto id = s.heading.find("id");
            if (s.tag == "ext_resource" && id != s.heading.end())
                resources.ext[godot_id(id->second)] = &s;
            else if (s.tag == "sub_resource" && id != s.heading.end())
                resources.sub[godot_id(id->second)] = &s;
            else if (s.tag == "resource")
                resource = &s;
        }
    }
};

} // namespace

Result3D<ImportedMaterial3D> import_godot_graph(const std::filesystem::path& source,
                                                const MaterialImportOptions3D& options) {
    Result3D<ImportedMaterial3D> result;
    try {
        const fs::path project = fs::absolute(options.project).lexically_normal();
        const GodotFile material(source, project);
        if (material.type == "StandardMaterial3D" || material.type == "ORMMaterial3D")
            return import_godot_material(source, options);
        if (material.type != "ShaderMaterial")
            throw std::runtime_error("a " + material.type +
                                     ", not a ShaderMaterial, StandardMaterial3D or ORMMaterial3D");
        if (!material.resource)
            throw std::runtime_error("a ShaderMaterial without its resource");
        const GodotSection& m = *material.resource;
        const auto shader_it = m.properties.find("shader");
        if (shader_it == m.properties.end() || shader_it->second.kind != GodotValue::Kind::Call)
            throw std::runtime_error("a ShaderMaterial without a shader");
        // The visual shader: inline (a sub-resource) or its own file.
        std::optional<GodotFile> own;
        const GodotSection* shader = nullptr;
        const GodotResources* resources = &material.resources;
        const GodotValue& ref = shader_it->second;
        if (ref.text == "SubResource" && !ref.args.empty()) {
            if (const auto it = material.resources.sub.find(godot_id(ref.args[0])); it != material.resources.sub.end())
                shader = it->second;
        } else if (const auto file = material.resources.file(ref)) {
            const std::string e = material_import::Context::extension(*file);
            if (e == ".gdshader" || e == ".shader")
                throw std::runtime_error("a text shader (" + file->filename().string() +
                                         "): kin imports Godot's visual shaders");
            if (e != ".tres")
                throw std::runtime_error("its shader " + file->filename().string() + " is not a text resource (.tres)");
            if (!fs::exists(*file))
                throw std::runtime_error("its shader " + file->string() + " is missing");
            own.emplace(*file, project);
            if (own->type != "VisualShader")
                throw std::runtime_error("its shader is a " + own->type + ", not a VisualShader");
            shader = own->resource;
            resources = &own->resources;
        }
        const auto inline_type =
            shader ? shader->heading.find("type") : std::map<std::string, GodotValue>::const_iterator{};
        if (!shader || (!own && (inline_type == shader->heading.end() || inline_type->second.text != "VisualShader")))
            throw std::runtime_error("its shader is not a VisualShader of the file");

        ImportedMaterial3D imported;
        imported.name = source.stem().string();
        if (const auto name = m.properties.find("resource_name");
            name != m.properties.end() && name->second.kind == GodotValue::Kind::String && !name->second.text.empty())
            imported.name = name->second.text;
        material_import::Context x(imported, options, source.stem().string());
        auto& p = imported.asset.base.material;
        // Godot's spatial shader where its outputs aren't written: white, rough, not metal.
        p.color = Color{255, 255, 255, 255};
        p.metallic = 0.0f;
        p.roughness = 1.0f;

        GraphBuilder b; // named for its shader's file (shared by the materials of it), else for the material
        b.name = own ? material.resources.file(ref)->stem().string() : imported.name;
        VisualShaderGraph g(*shader, *resources, b, x);
        const auto value = [&](int port, int width) -> std::optional<std::string> {
            const auto it = g.outputs.find(port);
            if (it == g.outputs.end())
                return std::nullopt;
            std::string why;
            auto v = g.value(it->second.first, it->second.second, why);
            if (!v) {
                static const char* names[] = {"Albedo",          "Alpha", "Metallic",        "Roughness", "Specular",
                                              "Emission",        "AO",    "AO Light Affect", "Normal",    "Normal Map",
                                              "Normal Map Depth"};
                x.note(std::string(port < 11 ? names[port] : "an output") + ": " + why + ": left out");
                return std::nullopt;
            }
            return b.fit(*v, width);
        };
        static const std::vector<std::tuple<int, const char*, int>> mapped{{0, "base_color", 3}, {1, "alpha", 1},
                                                                           {2, "metallic", 1},   {3, "roughness", 1},
                                                                           {5, "emission", 3},   {6, "occlusion", 1}};
        for (const auto& [port, to, width] : mapped)
            if (const auto v = value(port, width))
                b.outputs[to] = *v;
        if (const auto nm = value(9, 3)) { // Normal Map: the texel as Godot reads it (0 to 1), its depth the strength
            const auto depth = g.outputs.contains(10) ? value(10, 1) : std::optional<std::string>("1");
            b.outputs["normal"] =
                b.add("tangent_to_world",
                      {{"normal", b.add("unpack_normal", {{"texel", *nm}, {"strength", depth.value_or("1")}})}});
        }
        if (g.outputs.contains(8))
            x.note("Normal: a view-space normal (kin's graphs write the world's; Normal Map is read): left out");
        static const std::map<int, const char*> others{{4, "Specular"},
                                                       {7, "AO Light Affect"},
                                                       {11, "Rim"},
                                                       {12, "Rim Tint"},
                                                       {13, "Clearcoat"},
                                                       {14, "Clearcoat Roughness"},
                                                       {15, "Anisotropy"},
                                                       {16, "Anisotropy Flow"},
                                                       {17, "Subsurface Scattering"},
                                                       {18, "Backlight"},
                                                       {20, "Alpha Hash Scale"},
                                                       {21, "Alpha Antialiasing Edge"},
                                                       {22, "Alpha Texture Coordinate"},
                                                       {23, "Depth"},
                                                       {24, "Bent Normal Map"}};
        for (const auto& [port, name] : others)
            if (g.outputs.contains(port))
                x.note(std::string(name) + ": not in kin's graph outputs");
        for (const char* stage : {"nodes/vertex/connections", "nodes/light/connections"})
            if (const auto it = shader->properties.find(stage);
                it != shader->properties.end() && !it->second.args.empty())
                x.note(std::string(stage[6] == 'v' ? "the vertex stage" : "the light stage") +
                       " (kin's graphs shade surfaces): left out");
        if (b.outputs.empty())
            throw std::runtime_error("none of its visual shader's outputs could be imported");

        // The textures in kin's five slots.
        const auto texture = [&](const GraphBuilder::Param& q) {
            return *std::ranges::find(g.textures, q.name, &VisualShaderGraph::Texture::param);
        };
        if (!b.assign_slots([&](const GraphBuilder::Param& q) { return texture(q).normal; }))
            throw std::runtime_error("its visual shader samples more than 21 textures (kin's five map slots and 16 "
                                     "layers)");
        for (auto& q : b.params)
            if (q.type == "texture")
                q.srgb = texture(q).srgb;
        const std::string text = b.write();
        const auto compiled = compile_material_graph3d(text);
        if (!compiled.ok())
            throw std::runtime_error("its graph did not compile in kin (" + compiled.errors.front().node + ": " +
                                     compiled.errors.front().message + ")");
        for (const auto& w : compiled.warnings)
            x.note(w.node + ": " + w.message);
        imported.graph = x.out / (b.name + ".kingraph");
        if (options.write) {
            fs::create_directories(x.out);
            std::ofstream out(imported.graph, std::ios::binary);
            out << text;
        }
        p.graph = imported.graph.lexically_relative(x.content);

        // Render modes: cull (0 back, 1 front, 2 none), blend (0 mix, 1 add), unshaded; alpha scissor.
        const auto number = [&](const char* key, f64 fallback) {
            const auto it = shader->properties.find(key);
            if (it == shader->properties.end())
                return fallback;
            return it->second.kind == GodotValue::Kind::Bool ? (it->second.boolean ? 1.0 : 0.0) : it->second.number;
        };
        const int cull = int(number("modes/cull", 0));
        p.double_sided = cull == 2;
        if (cull == 1)
            x.note("cull front: kin culls back faces (or none)");
        p.unlit = number("flags/unshaded", 0) != 0;
        const bool alpha = b.outputs.contains("alpha");
        p.blend = Blend3D::Opaque;
        if (const auto scissor = g.outputs.find(19); scissor != g.outputs.end()) {
            std::string why;
            const auto v = g.value(scissor->second.first, scissor->second.second, why);
            p.blend = Blend3D::Mask;
            if (v && v->front() != '"' && v->front() != '[')
                p.alpha_cutoff = std::clamp(f32(std::strtod(v->c_str(), nullptr)), 0.0f, 1.0f);
            else
                x.note("Alpha Scissor Threshold: not a constant: a cut-out at half");
        } else if (alpha) {
            p.blend = int(number("modes/blend", 0)) == 1 ? Blend3D::Additive : Blend3D::Alpha;
        }

        // The material's values (shader_parameter/<name>), its textures; a Texture node's own texture.
        for (const auto& q : b.params) {
            const auto it = m.properties.find("shader_parameter/" + q.name);
            const GodotValue* given =
                it == m.properties.end() || it->second.kind == GodotValue::Kind::Null ? nullptr : &it->second;
            MaterialGraphValue3D v;
            if (q.type == "texture") {
                const auto t = texture(q);
                const auto file = given ? material.resources.file(*given) : t.file;
                if (!file || !fs::exists(*file)) {
                    if (given || t.file)
                        x.note(q.name + ": its texture is missing: left out");
                    continue;
                }
                const auto path = x.readable(*file, q.name);
                if (!path)
                    continue;
                // Repeated, filtered and mipmapped, as Godot's default sampling is (a graph's UVs often run
                // past 0..1); a map without a sampler would take Material3D's legacy clamp.
                v.texture = MaterialTexture3D{.asset = *path, .sampler = TextureSampler3D{}};
            } else {
                if (!given || given->kind == GodotValue::Kind::String)
                    continue;
                if (given->kind == GodotValue::Kind::Number || given->kind == GodotValue::Kind::Bool) {
                    v.count = 1;
                    v.value[0] =
                        given->kind == GodotValue::Kind::Bool ? (given->boolean ? 1.0f : 0.0f) : f32(given->number);
                } else {
                    v.count = u32(std::min<size_t>(4, given->args.size()));
                    for (size_t i = 0; i < v.count; ++i)
                        v.value[i] = f32(given->args[i].number);
                    if (q.type == "color")
                        for (size_t i = 0; i < std::min<size_t>(3, v.count); ++i)
                            v.value[i] = srgb_to_linear(v.value[i]);
                }
            }
            p.params[q.name] = v;
        }
        x.finish();
        result.value = std::move(imported);
    } catch (const std::exception& error) {
        result.diagnostics.push_back(source.string() + ": " + error.what());
    }
    return result;
}

} // namespace kin
