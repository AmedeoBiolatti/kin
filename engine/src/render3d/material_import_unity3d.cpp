// Unity's materials (.mat) as kin's: the built-in pipeline's Standard, URP's Lit (and Simple Lit, Baked Lit, Unlit),
// HDRP's Lit, read by their properties (kin/render3d/material_import3d.hpp).
#include "material_import3d_impl.hpp"

#include <array>
#include <kin/core/json_value.hpp>
#include <kin/render3d/material_graph3d.hpp>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace kin {
namespace {

using namespace material_import;
namespace fs = std::filesystem;

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return std::string(s.substr(a, b - a));
}
size_t indent_of(std::string_view line) {
    size_t n = 0;
    while (n < line.size() && line[n] == ' ')
        ++n;
    return n;
}

// A YAML flow mapping as Unity writes them: {fileID: 2800000, guid: abc, type: 3}, {r: 1, g: 0.5, b: 0, a: 1}.
std::map<std::string, std::string> flow(std::string_view text) {
    std::map<std::string, std::string> out;
    const size_t open = text.find('{'), close = text.rfind('}');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
        return out;
    std::string_view body = text.substr(open + 1, close - open - 1);
    while (!body.empty()) {
        const size_t comma = body.find(',');
        const std::string_view item = body.substr(0, comma);
        if (const size_t colon = item.find(':'); colon != std::string_view::npos)
            out[trim(item.substr(0, colon))] = trim(item.substr(colon + 1));
        if (comma == std::string_view::npos)
            break;
        body.remove_prefix(comma + 1);
    }
    return out;
}

double to_number(const std::string& s, double fallback = 0.0) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}

// A texture property's slot of m_TexEnvs.
struct UnityTexEnv {
    std::string guid; // empty: none
    long long file_id = 0;
    Vec2f scale{1, 1}, offset{0, 0};
};

struct UnityMaterial {
    std::string name;
    long long shader_file_id = 0;
    std::string shader_guid;
    std::set<std::string> keywords;
    std::map<std::string, UnityTexEnv> textures;
    std::map<std::string, double> floats;
    std::map<std::string, std::array<f32, 4>> colors;

    double number(const std::string& key, double fallback) const {
        const auto it = floats.find(key);
        return it == floats.end() ? fallback : it->second;
    }
    bool has(const std::string& key) const {
        return floats.contains(key) || colors.contains(key) || textures.contains(key);
    }
    const UnityTexEnv* texture(const std::string& key) const {
        const auto it = textures.find(key);
        return it != textures.end() && !it->second.guid.empty() ? &it->second : nullptr;
    }
    std::optional<std::array<f32, 4>> color(const std::string& key) const {
        const auto it = colors.find(key);
        return it == colors.end() ? std::nullopt : std::optional{it->second};
    }
};

// The Material document of a .mat (class 21): its name, shader, keywords and saved properties, in Unity's
// serialization 3 (`- _Name: value`) or 2 (`- first: {name: _Name}` / `second: value`).
UnityMaterial read_unity_material(const std::string& text) {
    // Logical lines: a flow mapping or a plain scalar that wraps continues on the lines after.
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string line;
        bool in_material = false;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("--- ", 0) == 0) {
                in_material = line.find("!u!21 ") != std::string::npos;
                continue;
            }
            if (!in_material || trim(line).empty())
                continue;
            // Braces still open on the line before: this one continues its flow mapping.
            const auto open = lines.empty() ? 0 : std::count(lines.back().begin(), lines.back().end(), '{');
            const auto close = lines.empty() ? 0 : std::count(lines.back().begin(), lines.back().end(), '}');
            const std::string t = trim(line);
            const bool continues_flow = !lines.empty() && open > close;
            // A plain scalar's continuation: deeper than its key's line, neither a key nor an item.
            const bool continues_scalar = !lines.empty() && indent_of(line) > indent_of(lines.back()) &&
                                          lines.back().back() != ':' && t[0] != '-' &&
                                          t.find(": ") == std::string::npos && t.back() != ':';
            if (continues_flow || continues_scalar)
                lines.back() += " " + t;
            else
                lines.push_back(line);
        }
    }
    UnityMaterial m;
    std::string section, item; // the saved properties' list, the current item's name
    UnityTexEnv* env = nullptr;
    const auto value_of = [](const std::string& t) {
        const size_t colon = t.find(':');
        return colon == std::string::npos ? std::string() : trim(std::string_view(t).substr(colon + 1));
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trim(lines[i]);
        const size_t depth = indent_of(lines[i]);
        if (depth == 2) { // the material's own fields
            section.clear();
            env = nullptr;
            if (t.rfind("m_Name:", 0) == 0)
                m.name = value_of(t);
            else if (t.rfind("m_Shader:", 0) == 0) {
                const auto f = flow(t);
                m.shader_file_id = f.contains("fileID") ? std::stoll(f.at("fileID")) : 0;
                m.shader_guid = f.contains("guid") ? f.at("guid") : "";
            } else if (t.rfind("m_ShaderKeywords:", 0) == 0) {
                std::istringstream words(value_of(t));
                for (std::string w; words >> w;)
                    m.keywords.insert(w);
            } else if (t.rfind("m_ValidKeywords:", 0) == 0) {
                section = "keywords";
            }
            continue;
        }
        if (t == "m_TexEnvs:" || t == "m_Floats:" || t == "m_Colors:" || t == "m_Ints:") {
            section = t.substr(0, t.size() - 1);
            env = nullptr;
            continue;
        }
        if (section == "keywords" && t.rfind("- ", 0) == 0) {
            m.keywords.insert(trim(std::string_view(t).substr(2)));
            continue;
        }
        if (section.empty())
            continue;
        // An item: "- _Key: value", or (serialization 2) "- first:" with "name: _Key" then "second: value".
        std::string key, value;
        if (t.rfind("- ", 0) == 0) {
            const std::string body = trim(std::string_view(t).substr(2));
            if (body.rfind("first:", 0) == 0) {
                const auto f = flow(body);
                key = f.contains("name") ? f.at("name") : "";
                if (key.empty() && i + 1 < lines.size() && trim(lines[i + 1]).rfind("name:", 0) == 0)
                    key = value_of(trim(lines[++i]));
                item = key;
                if (i + 1 < lines.size() && trim(lines[i + 1]).rfind("second:", 0) == 0)
                    value = value_of(trim(lines[++i]));
            } else {
                const size_t colon = body.find(':');
                if (colon == std::string::npos)
                    continue;
                key = item = trim(std::string_view(body).substr(0, colon));
                value = trim(std::string_view(body).substr(colon + 1));
            }
            env = nullptr;
            if (section == "m_TexEnvs")
                env = &m.textures[key];
            else if (section == "m_Colors") {
                auto f = flow(value);
                m.colors[key] = {f32(to_number(f["r"])), f32(to_number(f["g"])), f32(to_number(f["b"])),
                                 f32(to_number(f.contains("a") ? f["a"] : "1", 1.0))};
            } else if (!value.empty()) {
                m.floats[key] = to_number(value);
            }
            continue;
        }
        // A texture's fields.
        if (env) {
            if (t.rfind("m_Texture:", 0) == 0) {
                auto f = flow(t);
                env->file_id = f.contains("fileID") ? std::stoll(f.at("fileID")) : 0;
                env->guid = env->file_id != 0 && f.contains("guid") ? f.at("guid") : "";
            } else if (t.rfind("m_Scale:", 0) == 0) {
                auto f = flow(t);
                env->scale = {f32(to_number(f["x"], 1)), f32(to_number(f["y"], 1))};
            } else if (t.rfind("m_Offset:", 0) == 0) {
                auto f = flow(t);
                env->offset = {f32(to_number(f["x"])), f32(to_number(f["y"]))};
            }
        }
    }
    return m;
}

// The project's assets by GUID (each .meta's `guid:`), made once a project (and again when asked for one it lacks).
class GuidIndex {
public:
    static std::optional<fs::path> find(const fs::path& project, const std::string& guid) {
        static std::mutex lock;
        static std::map<fs::path, std::map<std::string, fs::path>> projects;
        std::lock_guard hold(lock);
        auto& index = projects[project];
        if (const auto it = index.find(guid); it != index.end())
            return it->second;
        static std::map<fs::path, bool> scanned;
        if (scanned[project])
            return std::nullopt;
        scanned[project] = true;
        std::error_code error;
        for (fs::recursive_directory_iterator it(project, fs::directory_options::skip_permission_denied, error), end;
             it != end; it.increment(error)) {
            if (error || !it->is_regular_file(error) || it->path().extension() != ".meta")
                continue;
            std::ifstream in(it->path());
            std::string line;
            for (int n = 0; n < 4 && std::getline(in, line); ++n)
                if (line.rfind("guid: ", 0) == 0) {
                    fs::path asset = it->path();
                    asset.replace_extension();
                    index[trim(std::string_view(line).substr(6))] = asset;
                    break;
                }
        }
        const auto it = index.find(guid);
        return it == index.end() ? std::nullopt : std::optional{it->second};
    }
};

// Unity's shaders kin knows the properties of, by their GUIDs (the render pipelines' packages) or built-in file ids.
enum class Family { Standard, Urp, Hdrp };
struct ShaderKind {
    Family family;
    std::string name;
    bool unlit = false, simple = false;
};

// A texture's colour space by its import settings (its .meta): sRGB or linear; none without them.
std::optional<bool> unity_srgb(const fs::path& texture) {
    std::ifstream meta(texture.string() + ".meta");
    for (std::string line; meta && std::getline(meta, line);)
        if (const size_t at = line.find("sRGBTexture: "); at != std::string::npos)
            return line.compare(at + 13, 1, "1") == 0;
    return std::nullopt;
}

f32 unity_linear(f32 gamma) {
    // A colour property as the inspector shows it (gamma), in linear light; HDR values past 1 by the same curve.
    return gamma <= 0.04045f ? gamma / 12.92f : std::pow((gamma + 0.055f) / 1.055f, 2.4f);
}

} // namespace

Result3D<ImportedMaterial3D> import_unity_material(const std::filesystem::path& source,
                                                   const MaterialImportOptions3D& options) {
    Result3D<ImportedMaterial3D> result;
    try {
        const std::string text = read_file(source);
        if (text.find("!u!21 ") == std::string::npos)
            throw std::runtime_error("not a Unity material (.mat)");
        const UnityMaterial m = read_unity_material(text);
        ImportedMaterial3D imported;
        imported.name = m.name.empty() ? source.stem().string() : m.name;
        Context x(imported, options, source.stem().string());
        const fs::path project = fs::absolute(options.project).lexically_normal();

        // The shader: a pipeline's own, a project's (its file's name said), or a package's we don't have.
        static const std::map<std::string, ShaderKind> known{
            {"933532a4fcc9baf4fa0491de14d08ed7", {Family::Urp, "URP Lit"}},
            {"8d2bb70cbf9db8d4da26e15b26e74248", {Family::Urp, "URP Simple Lit", false, true}},
            {"0ca6dca7396eb48e5849247ffd444914", {Family::Urp, "URP Baked Lit"}},
            {"650dd9526735d5b46b79224bc6e94025", {Family::Urp, "URP Unlit", true}},
            {"6e4ae4064600d784cac1e41a9e6f2e59", {Family::Hdrp, "HDRP Lit"}},
            {"c4edd00ff2db5b24391a4fcb1762e459", {Family::Hdrp, "HDRP Unlit", true}},
        };
        bool own_shader = true; // one of Unity's, whose keywords we know
        ShaderKind kind{m.has("_BaseColorMap") || m.has("_MaskMap") ? Family::Hdrp
                        : m.has("_BaseMap") || m.has("_Surface")    ? Family::Urp
                                                                    : Family::Standard,
                        ""};
        if (const auto it = known.find(m.shader_guid); it != known.end()) {
            kind = it->second;
        } else if (m.shader_guid == "0000000000000000f000000000000000") {
            kind.family = Family::Standard;
            kind.name = m.shader_file_id == 45 ? "Standard (Specular setup)" : "Standard";
            if (m.shader_file_id != 46 && m.shader_file_id != 45)
                x.note("a built-in shader (file id " + std::to_string(m.shader_file_id) +
                       ") other than Standard: imported by its properties as Standard");
        } else if (const auto file = GuidIndex::find(project, m.shader_guid)) {
            kind.name = file->filename().string();
            own_shader = false;
            const bool graph = Context::extension(*file) == ".shadergraph";
            kind.unlit = kind.name.find("Unlit") != std::string::npos;
            x.note(std::string(graph ? "a Shader Graph" : "a custom shader") + " (" + kind.name +
                   "): imported from its usual property names; its own look is " +
                   (graph ? "Tier 2 (the graph's import)" : "not read"));
        } else {
            kind.name = "a package's shader";
            own_shader = false;
            x.note("shader " + m.shader_guid + " is not in the project (a package's): imported by its properties");
        }

        // A texture property's file (by GUID), and where kin reads it.
        const auto texture_file = [&](const std::string& key) -> std::optional<fs::path> {
            const UnityTexEnv* env = m.texture(key);
            if (!env)
                return std::nullopt;
            if (env->guid == "0000000000000000f000000000000000") {
                x.note(key + ": a built-in texture of Unity's: left out");
                return std::nullopt;
            }
            const auto file = GuidIndex::find(project, env->guid);
            if (!file) {
                x.note(key + ": texture " + env->guid + " is not in the project: left out");
                return std::nullopt;
            }
            if (!fs::exists(*file)) {
                x.note(key + ": " + file->filename().string() + " is missing (its .meta is there): left out");
                return std::nullopt;
            }
            return *file;
        };
        TextureSampler3D sampler; // Unity's are the texture's own import settings: repeat, linear, mipmaps
        sampler.wrap_u = sampler.wrap_v = Wrap3D::Repeat;
        const auto map = [&](fs::path p) { return MaterialTexture3D{.asset = std::move(p), .sampler = sampler}; };
        const MaterialTexture3D none{};
        const auto first = [&](std::initializer_list<const char*> keys) -> std::string {
            for (const char* k : keys)
                if (m.has(k))
                    return k;
            return *keys.begin();
        };

        auto& p = imported.asset.base.material;
        const bool hdrp = kind.family == Family::Hdrp, urp = kind.family == Family::Urp;
        // Base colour and map; its tiling and offset are every map's (Unity's Lit and Standard read all maps with the
        // base map's).
        const std::string base_map = hdrp ? "_BaseColorMap" : first({"_BaseMap", "_MainTex"});
        const auto base =
            m.color(hdrp ? "_BaseColor" : first({"_BaseColor", "_Color"})).value_or(std::array<f32, 4>{1, 1, 1, 1});
        p.color = Color{byte(base[0]), byte(base[1]), byte(base[2]), byte(base[3])};
        const auto albedo = texture_file(base_map);
        const auto albedo_path = albedo ? x.readable(*albedo, base_map) : std::nullopt;
        p.texture = albedo_path ? map(*albedo_path) : none;
        if (const auto it = m.textures.find(base_map); it != m.textures.end()) {
            p.uv_scale = it->second.scale;
            p.uv_offset = it->second.offset;
        }
        for (const auto& [key, env] : m.textures)
            if (key != base_map && !env.guid.empty() && (env.scale != Vec2f{1, 1} || env.offset != Vec2f{0, 0}) &&
                (env.scale != p.uv_scale.value_or(Vec2f{1, 1}) || env.offset != p.uv_offset.value_or(Vec2f{0, 0})))
                x.note(key + "'s tiling or offset differs from the base map's: kin reads every map with the base "
                             "map's");

        // Metallic and smoothness: kin's roughness is 1 - smoothness. With a map, the factors go into the map kin
        // makes (G roughness, B metallic): Unity's metallic is the map's R alone, its smoothness the chosen alpha
        // times the smoothness (Standard: its gloss map scale); HDRP's mask map remaps R and A.
        const bool specular_setup =
            kind.name == "Standard (Specular setup)" || (urp && !kind.simple && m.number("_WorkflowMode", 1) == 0);
        if (specular_setup || kind.simple)
            x.note(std::string(kind.simple ? "Simple Lit's Blinn-Phong specular" : "the specular workflow") +
                   ": imported as a dielectric (metallic 0) of its smoothness");
        const f32 smooth_factor =
            f32(hdrp ? m.number("_Smoothness", 0.5)
                : kind.family == Family::Standard
                    ? m.number(m.texture("_MetallicGlossMap") ? "_GlossMapScale" : "_Glossiness", 0.5)
                    : m.number(m.has("_Smoothness") ? "_Smoothness" : "_Glossiness", 0.5));
        const f32 metal_factor = specular_setup || kind.simple ? 0.0f : f32(m.number("_Metallic", 0.0));
        const auto metal_file = hdrp                            ? texture_file("_MaskMap")
                                : specular_setup || kind.simple ? std::nullopt
                                                                : texture_file("_MetallicGlossMap");
        const bool from_albedo = !hdrp && m.number("_SmoothnessTextureChannel", 0) == 1;
        const auto metal_image =
            metal_file ? x.load(*metal_file, hdrp ? "_MaskMap" : "_MetallicGlossMap") : std::nullopt;
        const auto albedo_image =
            from_albedo && albedo ? x.load(*albedo, base_map + " (its alpha, the smoothness)") : std::nullopt;
        const auto remap = [&](const char* low, const char* high, f32 v) {
            return f32(m.number(low, 0.0) + (m.number(high, 1.0) - m.number(low, 0.0)) * v);
        };
        p.metallic = std::clamp(metal_image ? 1.0f : metal_factor, 0.0f, 1.0f);
        p.roughness = std::clamp(metal_image || albedo_image ? 1.0f : 1.0f - smooth_factor, 0.0f, 1.0f);
        p.metallic_roughness = none;
        if (metal_image || albedo_image) {
            const Image& a = metal_image ? *metal_image : *albedo_image;
            const Vec2i size{std::max(a.size.x, albedo_image ? albedo_image->size.x : 0),
                             std::max(a.size.y, albedo_image ? albedo_image->size.y : 0)};
            Image packed{size, std::vector<u8>(size_t(size.x) * size.y * 4, 255)};
            for (int y = 0; y < size.y; ++y)
                for (int xx = 0; xx < size.x; ++xx) {
                    u8* o = &packed.rgba[(size_t(y) * size.x + xx) * 4];
                    f32 smooth = smooth_factor;
                    if (albedo_image)
                        smooth *= albedo_image->rgba[texel(*albedo_image, size, xx, y) * 4 + 3] / 255.0f;
                    else if (metal_image) {
                        const f32 a_value = metal_image->rgba[texel(*metal_image, size, xx, y) * 4 + 3] / 255.0f;
                        smooth = hdrp ? remap("_SmoothnessRemapMin", "_SmoothnessRemapMax", a_value)
                                      : a_value * smooth_factor;
                    }
                    o[1] = byte(1.0f - smooth);
                    if (metal_image) {
                        const f32 r = metal_image->rgba[texel(*metal_image, size, xx, y) * 4] / 255.0f;
                        o[2] = byte(hdrp ? remap("_MetallicRemapMin", "_MetallicRemapMax", r) : r);
                    } else {
                        o[2] = 255; // the metallic factor alone
                    }
                }
            if (!metal_image)
                p.metallic = std::clamp(metal_factor, 0.0f, 1.0f);
            p.metallic_roughness = map(x.made("_mr", packed));
        }

        // Normal map (Unity's are as glTF's: green up).
        const std::string normal_key = hdrp ? "_NormalMap" : "_BumpMap";
        const auto normal = texture_file(normal_key);
        const auto normal_path = normal ? x.readable(*normal, normal_key) : std::nullopt;
        p.normal = normal_path ? map(*normal_path) : none;
        p.normal_scale = f32(std::clamp(m.number(hdrp ? "_NormalScale" : "_BumpScale", 1.0), 0.0, 100.0));

        // Occlusion: Unity's maps hold it in G (HDRP's mask map too, remapped); kin reads R.
        p.occlusion = none;
        p.occlusion_strength = 1.0f;
        const auto ao_source = hdrp ? metal_image : [&]() -> std::optional<Image> {
            const auto file = texture_file("_OcclusionMap");
            return file ? x.load(*file, "_OcclusionMap") : std::nullopt;
        }();
        if (ao_source) {
            Image ao = *ao_source;
            for (size_t i = 0; i < size_t(ao.size.x) * ao.size.y; ++i) {
                const f32 g = ao.rgba[i * 4 + 1] / 255.0f;
                ao.rgba[i * 4] = byte(hdrp ? remap("_AORemapMin", "_AORemapMax", g) : g);
            }
            p.occlusion = map(x.made("_ao", ao));
            p.occlusion_strength = hdrp ? 1.0f : f32(std::clamp(m.number("_OcclusionStrength", 1.0), 0.0, 1.0));
        }

        // Emission: a colour (HDR) times a map. Standard and URP need the _EMISSION keyword; HDRP's colour is linear
        // light already (its intensity in it).
        p.emission_mode = EmissionMode3D::Independent;
        p.emission_color = LinearRgb{1, 1, 1};
        p.emissive = 0.0f;
        p.emissive_texture = none;
        const auto emission = m.color(hdrp ? "_EmissiveColor" : "_EmissionColor");
        const bool emits = hdrp ? emission && (emission->at(0) > 0 || emission->at(1) > 0 || emission->at(2) > 0)
                                : m.keywords.contains("_EMISSION");
        if (emits && emission) {
            const auto& e = *emission;
            p.emission_color = hdrp ? LinearRgb{e[0], e[1], e[2]}
                                    : LinearRgb{unity_linear(e[0]), unity_linear(e[1]), unity_linear(e[2])};
            p.emissive = 1.0f;
            const std::string key = hdrp ? "_EmissiveColorMap" : "_EmissionMap";
            if (const auto file = texture_file(key))
                if (const auto path = x.readable(*file, key))
                    p.emissive_texture = map(*path);
            if (hdrp)
                x.note("HDRP's emission is in its physical units: match kin's exposure to the scene's");
        } else if (emission && !hdrp && (emission->at(0) > 0 || emission->at(1) > 0 || emission->at(2) > 0)) {
            x.note("an emission colour without the _EMISSION keyword: imported off");
        }

        // Surface: opaque, cut-out, transparent (alpha, premultiplied, additive, multiply); faces; shadows.
        p.alpha_cutoff = f32(m.number(hdrp ? "_AlphaCutoff" : "_Cutoff", 0.5));
        p.blend = Blend3D::Opaque;
        if (kind.family == Family::Standard) {
            const int mode = int(m.number("_Mode", 0));
            p.blend = mode == 1 ? Blend3D::Mask : mode >= 2 ? Blend3D::Alpha : Blend3D::Opaque;
            if (mode == 3)
                x.note("Transparent mode keeps its reflections at full strength in Unity: kin fades them with alpha");
        } else {
            const bool transparent = m.number(hdrp ? "_SurfaceType" : "_Surface", 0) == 1;
            const bool clip = m.number(hdrp ? "_AlphaCutoffEnable" : "_AlphaClip", 0) == 1;
            const int blend = int(m.number(hdrp ? "_BlendMode" : "_Blend", 0));
            if (transparent) {
                const bool additive = hdrp ? blend == 1 : blend == 2;
                p.blend = additive ? Blend3D::Additive : Blend3D::Alpha;
                if ((hdrp && blend == 4) || (!hdrp && blend == 1))
                    x.note("premultiplied blending: kin blends by straight alpha");
                if (!hdrp && blend == 3)
                    x.note("multiply blending: kin blends by alpha (or adds)");
                if (clip)
                    x.note("alpha clipping on a transparent surface: blended only");
            } else if (clip) {
                p.blend = Blend3D::Mask;
            }
        }
        const int cull = int(m.number("_Cull", 2));
        p.double_sided = hdrp ? m.number("_DoubleSidedEnable", 0) == 1 : cull == 0;
        if (!hdrp && cull == 1)
            x.note("front-face culling: kin culls back faces (or none)");
        p.receives_shadow = m.number("_ReceiveShadows", 1) != 0;
        p.unlit = kind.unlit;

        // What kin does not show: maps and keywords of features it lacks.
        for (const char* key : {"_DetailAlbedoMap", "_DetailNormalMap", "_DetailMap", "_ParallaxMap", "_HeightMap",
                                "_ClearCoatMap", "_CoatMaskMap", "_SpecGlossMap", "_ThicknessMap", "_SubsurfaceMaskMap",
                                "_IridescenceThicknessMap", "_AnisotropyMap", "_BentNormalMap"})
            if (m.texture(key))
                x.note(std::string(key) + ": not shown by kin");
        static const std::set<std::string> understood{"_EMISSION",
                                                      "_NORMALMAP",
                                                      "_METALLICGLOSSMAP",
                                                      "_METALLICSPECGLOSSMAP",
                                                      "_OCCLUSIONMAP",
                                                      "_ALPHATEST_ON",
                                                      "_ALPHABLEND_ON",
                                                      "_ALPHAPREMULTIPLY_ON",
                                                      "_SURFACE_TYPE_TRANSPARENT",
                                                      "_SMOOTHNESS_TEXTURE_ALBEDO_CHANNEL_A",
                                                      "_RECEIVE_SHADOWS_OFF",
                                                      "_SPECULARHIGHLIGHTS_OFF",
                                                      "_ENVIRONMENTREFLECTIONS_OFF",
                                                      "_GLOSSYREFLECTIONS_OFF",
                                                      "_SPECULAR_SETUP",
                                                      "_NORMALMAP_TANGENT_SPACE",
                                                      "_MASKMAP",
                                                      "_DOUBLESIDED_ON",
                                                      "_EMISSIVE_COLOR_MAP",
                                                      "_ENABLE_FOG_ON_TRANSPARENT",
                                                      "_BLENDMODE_ALPHA",
                                                      "_BLENDMODE_ADD",
                                                      "_BLENDMODE_PRE_MULTIPLY",
                                                      "_ALPHATEST_ON",
                                                      "_DISABLE_SSR_TRANSPARENT"};
        for (const auto& k : m.keywords)
            if (own_shader && !understood.contains(k))
                x.note("keyword " + k + ": not read");

        x.finish();
        result.value = std::move(imported);
    } catch (const std::exception& error) {
        result.diagnostics.push_back(source.string() + ": " + error.what());
    }
    return result;
}

} // namespace kin

// --- Tier 2: Unity's Shader Graphs as kin's material graphs ---

namespace kin {
namespace {

using material_import::GraphBuilder;

// A .shadergraph: JSON objects one after another (Shader Graph 10+), each by its m_ObjectId.
std::vector<JsonValue> shader_graph_objects(const std::string& text) {
    std::vector<JsonValue> out;
    int depth = 0;
    bool in_string = false, escaped = false;
    size_t start = std::string::npos;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            escaped = !escaped && c == '\\';
            if (c == '"' && !escaped)
                in_string = false;
            if (c != '\\')
                escaped = false;
            continue;
        }
        if (c == '"')
            in_string = true;
        else if (c == '{') {
            if (depth++ == 0)
                start = i;
        } else if (c == '}' && --depth == 0 && start != std::string::npos) {
            auto parsed = parse_json(std::string_view(text).substr(start, i - start + 1));
            if (parsed.ok())
                out.push_back(std::move(*parsed.value));
            start = std::string::npos;
        }
    }
    return out;
}

// What a graph and the sub-graphs inlined in it share.
struct GraphShared {
    struct Texture {
        std::string param, guid;
        bool normal = false;
    };
    std::vector<Texture> textures;              // its texture parameters'
    std::map<std::string, std::string> ref_of;  // graph parameter -> Unity reference name
    const std::set<std::string>* keywords = {}; // the material's, on
    fs::path project;
    std::set<std::string> keywords_on;                          // the material's keywords Keyword nodes read
    std::vector<std::unique_ptr<std::vector<JsonValue>>> files; // sub-graphs' objects, kept while they are read
};

class ShaderGraph {
public:
    // A sub-graph's property (by its guid) bound to what feeds its node's slot in the graph calling it.
    struct Binding {
        ShaderGraph* caller = nullptr;
        std::string node;
        int slot = 0;
    };

    ShaderGraph(const std::vector<JsonValue>& objects, GraphBuilder& b, material_import::Context& x, GraphShared& s,
                int depth = 0, std::map<std::string, Binding> bindings = {})
        : _b(b), _x(x), _s(s), _depth(depth), _bindings(std::move(bindings)) {
        for (const auto& o : objects) {
            const std::string id = text_of(o, "m_ObjectId");
            _objects[id] = &o;
            const std::string type = text_of(o, "m_Type");
            if (type == "UnityEditor.ShaderGraph.GraphData")
                graph = &o;
            if (type.find("UniversalTarget") != std::string::npos)
                target = &o;
            if (type.find("UniversalUnlitSubTarget") != std::string::npos)
                unlit = true;
            if (type == "UnityEditor.ShaderGraph.BlockNode")
                blocks[text_of(o, "m_SerializedDescriptor")] = id;
        }
        if (graph && graph->find("m_Edges"))
            for (const auto& e : graph->find("m_Edges")->items()) {
                const auto* out = e.find("m_OutputSlot");
                const auto* in = e.find("m_InputSlot");
                if (!out || !in || !out->find("m_Node") || !in->find("m_Node"))
                    continue;
                _edges[{text_of(*in->find("m_Node"), "m_Id"), int(in->int_at("m_SlotId"))}] = {
                    text_of(*out->find("m_Node"), "m_Id"), int(out->int_at("m_SlotId"))};
            }
    }
    const JsonValue* graph = nullptr;
    const JsonValue* target = nullptr;
    bool unlit = false;
    std::map<std::string, std::string> blocks; // descriptor -> node id

    bool fed(const std::string& node, int slot) const { return _edges.contains({node, slot}); }

    // What feeds `node`'s input slot `slot` (else the slot's own value) as JSON text, or none with why.
    std::optional<std::string> input(const std::string& node, int slot, std::string& why) {
        const JsonValue* s = slot_of(node, slot);
        if (const auto e = _edges.find({node, slot}); e != _edges.end()) {
            auto v = value(e->second.first, e->second.second, why);
            if (v && s)
                if (const int n = slot_width(text_of(*s, "m_Type")); n > 0)
                    v = fit(*v, n);
            return v;
        }
        if (!s) {
            why = "a slot " + std::to_string(slot) + " not found";
            return std::nullopt;
        }
        const std::string type = text_of(*s, "m_Type");
        if (type.find("UVMaterialSlot") != std::string::npos)
            return GraphBuilder::quote("uv");
        if (type.find("PositionMaterialSlot") != std::string::npos)
            return GraphBuilder::quote("position");
        if (type.find("NormalMaterialSlot") != std::string::npos)
            return std::string("[0, 0, 1]"); // the tangent-space normal blocks'
        if (type.find("Texture2DInputMaterialSlot") != std::string::npos ||
            type.find("Texture2DMaterialSlot") != std::string::npos) {
            const JsonValue* t = s->find("m_Texture");
            const std::string guid = t ? texture_guid(text_of(*t, "m_SerializedTexture")) : "";
            if (guid.empty()) {
                why = "a texture slot of no texture";
                return std::nullopt;
            }
            return GraphBuilder::quote(texture_param("t_" + guid.substr(0, 8), guid, ""));
        }
        return literal(s->find("m_Value"), type.find("ColorRGB") != std::string::npos);
    }

    std::optional<std::string> value(const std::string& node, int slot, std::string& why) {
        const auto key = node + "|" + std::to_string(slot);
        if (const auto it = _done.find(key); it != _done.end())
            return it->second;
        if (_visiting.contains(key)) {
            why = "a cycle";
            return std::nullopt;
        }
        _visiting.insert(key);
        auto v = translate(node, slot, why);
        _visiting.erase(key);
        if (v)
            _done[key] = *v;
        return v;
    }

    int width(const std::string& v) const { return _b.width(v); }
    std::string fit(const std::string& v, int n) { return _b.fit(v, n); }

private:
    // A slot type's fixed width, else 0 (dynamic, or not a value).
    static int slot_width(const std::string& type) {
        static const std::vector<std::pair<const char*, int>> widths{
            {"Vector1MaterialSlot", 1}, {"BooleanMaterialSlot", 1},
            {"Vector2MaterialSlot", 2}, {"UVMaterialSlot", 2},
            {"Vector3MaterialSlot", 3}, {"ColorRGBMaterialSlot", 3},
            {"NormalMaterialSlot", 3},  {"PositionMaterialSlot", 3},
            {"TangentMaterialSlot", 3}, {"ViewDirectionMaterialSlot", 3},
            {"Vector4MaterialSlot", 4}, {"ColorRGBAMaterialSlot", 4}};
        const std::string kind = type.substr(type.rfind('.') + 1);
        for (const auto& [name, n] : widths)
            if (kind == name)
                return n;
        return 0;
    }
    std::string add(const std::string& type, std::vector<std::pair<std::string, std::string>> inputs,
                    std::vector<std::pair<std::string, std::string>> fields = {}) {
        return _b.add(type, std::move(inputs), std::move(fields));
    }
    // Dynamic slots' inputs as Unity concretizes them: cut to the narrowest connected vector's width (floats
    // promote; nothing connected: floats).
    std::optional<std::vector<std::string>> dynamic(const std::string& id, std::initializer_list<int> slots,
                                                    std::string& why) {
        std::vector<std::string> v;
        int concrete = 0;
        for (const int s : slots) {
            auto x = input(id, s, why);
            if (!x)
                return std::nullopt;
            if (fed(id, s))
                if (const int w = width(*x); w > 1)
                    concrete = concrete ? std::min(concrete, w) : w;
            v.push_back(*x);
        }
        for (auto& x : v)
            x = fit(x, std::max(concrete, 1));
        return v;
    }
    static std::string text_of(const JsonValue& o, const char* key) {
        const JsonValue* v = o.find(key);
        return v && v->is_string() ? v->as_string() : "";
    }
    static std::string texture_guid(const std::string& serialized) {
        if (serialized.empty())
            return "";
        const auto t = parse_json(serialized);
        const JsonValue* tex = t.ok() ? t.value->find("texture") : nullptr;
        return tex ? text_of(*tex, "guid") : "";
    }
    // A texture parameter (made once), its texture `guid` (the default) and the Unity reference naming it.
    std::string texture_param(const std::string& name, const std::string& guid, const std::string& ref) {
        if (!_b.has_param(name)) {
            _b.params.push_back({name, "texture", {}, "", true});
            _s.textures.push_back({name, guid, false});
            if (!ref.empty())
                _s.ref_of[name] = ref;
        }
        return name;
    }
    const JsonValue* object(const std::string& id) const {
        const auto it = _objects.find(id);
        return it == _objects.end() ? nullptr : it->second;
    }
    const JsonValue* slot_of(const std::string& node, int slot) const {
        const JsonValue* n = object(node);
        if (!n || !n->find("m_Slots"))
            return nullptr;
        for (const auto& ref : n->find("m_Slots")->items())
            if (const auto* s = object(text_of(ref, "m_Id")); s && s->int_at("m_Id", -999) == slot)
                return s;
        return nullptr;
    }
    // A slot's or property's value as JSON text: a number, or 2 to 4 of them (colours from gamma, as Unity shows them).
    static std::string literal(const JsonValue* v, bool colour) {
        if (!v)
            return "0";
        if (v->is_number())
            return GraphBuilder::number(v->as_number());
        if (v->is_bool())
            return v->as_bool() ? "1" : "0";
        if (!v->is_object())
            return "0";
        std::vector<f32> c;
        for (const char* k : {"x", "y", "z", "w"})
            if (const auto* e = v->find(k); e && e->is_number())
                c.push_back(f32(e->as_number()));
        if (c.empty())
            for (const char* k : {"r", "g", "b", "a"})
                if (const auto* e = v->find(k); e && e->is_number())
                    c.push_back(f32(e->as_number()));
        if (colour)
            for (size_t i = 0; i < std::min<size_t>(3, c.size()); ++i)
                c[i] = unity_linear(c[i]);
        if (c.empty())
            return "0";
        if (c.size() == 1)
            return GraphBuilder::number(c[0]);
        return GraphBuilder::vector(c);
    }
    // A property of the graph as a parameter of kin's (by its reference name, so the material's value finds it).
    std::optional<std::string> property(const JsonValue& p, std::string& why) {
        const std::string type = text_of(p, "m_Type");
        const std::string ref = !text_of(p, "m_OverrideReferenceName").empty() ? text_of(p, "m_OverrideReferenceName")
                                                                               : text_of(p, "m_DefaultReferenceName");
        std::string name = ref;
        for (auto& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                c = '_';
        if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])))
            name = "p_" + name;
        if (_b.has_param(name))
            return GraphBuilder::quote(name);
        const JsonValue* v = p.find("m_Value");
        if (type.find("Texture2DShaderProperty") != std::string::npos)
            return GraphBuilder::quote(
                texture_param(name, v ? texture_guid(text_of(*v, "m_SerializedTexture")) : "", ref));
        GraphBuilder::Param param{name, "float", {}};
        const auto read = [&](std::initializer_list<const char*> keys, bool colour) {
            int i = 0;
            for (const char* k : keys) {
                if (const auto* e = v ? v->find(k) : nullptr; e && e->is_number())
                    param.value[size_t(i)] = colour && i < 3 ? unity_linear(f32(e->as_number())) : f32(e->as_number());
                ++i;
            }
        };
        if (type.find("ColorShaderProperty") != std::string::npos) {
            param.type = "color";
            param.value = {1, 1, 1, 1};
            read({"r", "g", "b", "a"}, true);
        } else if (type.find("Vector2ShaderProperty") != std::string::npos) {
            param.type = "vec2";
            read({"x", "y"}, false);
        } else if (type.find("Vector3ShaderProperty") != std::string::npos) {
            param.type = "vec3";
            read({"x", "y", "z"}, false);
        } else if (type.find("Vector4ShaderProperty") != std::string::npos) {
            param.type = "vec4";
            read({"x", "y", "z", "w"}, false);
        } else if (type.find("Vector1ShaderProperty") != std::string::npos ||
                   type.find("BooleanShaderProperty") != std::string::npos) {
            param.value[0] = !v ? 0.0f : v->is_bool() ? (v->as_bool() ? 1.0f : 0.0f) : f32(v->as_number());
        } else {
            why = "a " + type.substr(type.rfind('.') + 1) + " property";
            return std::nullopt;
        }
        _b.params.push_back(param);
        _s.ref_of[name] = ref;
        return GraphBuilder::quote(name);
    }
    // A sub-graph's node: the sub-graph inlined, its properties fed by this node's slots.
    std::optional<std::string> sub_graph(const std::string& id, const JsonValue& n, int out, std::string& why) {
        auto child = _children.find(id);
        if (child == _children.end()) {
            if (_depth >= 16) {
                why = "sub-graphs 16 deep";
                return std::nullopt;
            }
            const std::string guid = [&] {
                const auto t = parse_json(text_of(n, "m_SerializedSubGraph"));
                const JsonValue* sg = t.ok() ? t.value->find("subGraph") : nullptr;
                return sg ? text_of(*sg, "guid") : std::string();
            }();
            const auto file = guid.empty() ? std::nullopt : GuidIndex::find(_s.project, guid);
            if (!file || !fs::exists(*file)) {
                why = "its sub-graph (" + guid + ") is not in the project";
                return std::nullopt;
            }
            auto objects = std::make_unique<std::vector<JsonValue>>(shader_graph_objects(read_file(*file)));
            std::map<std::string, Binding> bindings;
            const JsonValue* guids = n.find("m_PropertyGuids");
            const JsonValue* ids = n.find("m_PropertyIds");
            if (guids && ids && guids->is_array() && ids->is_array())
                for (size_t i = 0; i < std::min(guids->items().size(), ids->items().size()); ++i)
                    bindings[guids->items()[i].as_string()] = {this, id, int(ids->items()[i].as_number())};
            auto g = std::make_unique<ShaderGraph>(*objects, _b, _x, _s, _depth + 1, std::move(bindings));
            if (!g->graph) {
                why = "its sub-graph (" + file->filename().string() + ") is of Shader Graph's format before 10";
                return std::nullopt;
            }
            _s.files.push_back(std::move(objects));
            child = _children.emplace(id, std::move(g)).first;
        }
        ShaderGraph& g = *child->second;
        const JsonValue* output = g.graph->find("m_OutputNode");
        if (!output) {
            why = "its sub-graph has no output";
            return std::nullopt;
        }
        std::string inner;
        auto v = g.input(text_of(*output, "m_Id"), out, inner);
        if (!v)
            why = (n.find("m_Name") ? text_of(n, "m_Name") : std::string("a sub-graph")) + " > " + inner;
        return v;
    }

    std::optional<std::string> glsl(const std::string& expr, const char* result,
                                    std::vector<std::pair<std::string, std::string>> inputs) {
        return add("glsl", std::move(inputs),
                   {{"glsl", GraphBuilder::quote(expr)}, {"result", GraphBuilder::quote(result)}});
    }

    std::optional<std::string> translate(const std::string& id, int out, std::string& why) {
        const JsonValue* n = object(id);
        if (!n) {
            why = "no node " + id;
            return std::nullopt;
        }
        const std::string full = text_of(*n, "m_Type");
        const std::string kind = full.substr(full.rfind('.') + 1);
        const std::string label = text_of(*n, "m_Name").empty() ? kind : text_of(*n, "m_Name");
        const auto in = [&](int slot) { return input(id, slot, why); };
        // Several inputs, else none (why says which failed).
        const auto all = [&](std::initializer_list<int> slots) -> std::optional<std::vector<std::string>> {
            std::vector<std::string> v;
            for (const int s : slots) {
                auto x = in(s);
                if (!x)
                    return std::nullopt;
                v.push_back(*x);
            }
            return v;
        };
        static const std::map<std::string, const char*> binaries{
            {"AddNode", "add"},           {"SubtractNode", "subtract"}, {"MultiplyNode", "multiply"},
            {"DivideNode", "divide"},     {"MinimumNode", "min"},       {"MaximumNode", "max"},
            {"PowerNode", "power"},       {"DotProductNode", "dot"},    {"CrossProductNode", "cross"},
            {"DistanceNode", "distance"}, {"ModuloNode", "mod"}};
        static const std::map<std::string, const char*> unaries{
            {"OneMinusNode", "one_minus"}, {"SaturateNode", "saturate"},
            {"AbsoluteNode", "abs"},       {"NegateNode", "negate"},
            {"FractionNode", "fract"},     {"FloorNode", "floor"},
            {"CeilingNode", "ceil"},       {"NormalizeNode", "normalize"},
            {"SquareRootNode", "sqrt"},    {"SineNode", "sin"},
            {"CosineNode", "cos"},         {"TangentNode", "tan"},
            {"LengthNode", "length"},      {"SignNode", "sign"}};
        if (const auto b = binaries.find(kind); b != binaries.end()) {
            const auto v = kind == "CrossProductNode" ? all({0, 1}) : dynamic(id, {0, 1}, why);
            return v ? std::optional{add(b->second, {{"a", (*v)[0]}, {"b", (*v)[1]}})} : std::nullopt;
        }
        if (const auto u = unaries.find(kind); u != unaries.end()) {
            const auto x = dynamic(id, {0}, why);
            return x ? std::optional{add(u->second, {{"x", x->front()}})} : std::nullopt;
        }
        if (kind == "LerpNode") {
            const auto v = dynamic(id, {0, 1, 2}, why);
            return v ? std::optional{add("mix", {{"a", (*v)[0]}, {"b", (*v)[1]}, {"t", (*v)[2]}})} : std::nullopt;
        }
        if (kind == "StepNode") {
            const auto v = dynamic(id, {0, 1}, why);
            return v ? std::optional{add("step", {{"edge", (*v)[0]}, {"x", (*v)[1]}})} : std::nullopt;
        }
        if (kind == "SmoothstepNode") {
            const auto v = dynamic(id, {0, 1, 2}, why);
            return v ? std::optional{add("smoothstep", {{"edge0", (*v)[0]}, {"edge1", (*v)[1]}, {"x", (*v)[2]}})}
                     : std::nullopt;
        }
        if (kind == "ClampNode") {
            const auto v = dynamic(id, {0, 1, 2}, why);
            return v ? std::optional{add("clamp", {{"x", (*v)[0]}, {"min", (*v)[1]}, {"max", (*v)[2]}})} : std::nullopt;
        }
        if (kind == "RemapNode") {
            auto v = all({0, 1, 2});
            if (!v)
                return std::nullopt;
            if (!fed(id, 0))
                (*v)[0] = fit((*v)[0], 1);
            const auto part = [&](const std::string& r, int k) {
                return r.front() == '"' ? _b.member(r, std::string(1, "xy"[k])) : component(r, k);
            };
            return add("remap", {{"x", (*v)[0]},
                                 {"in_min", part((*v)[1], 0)},
                                 {"in_max", part((*v)[1], 1)},
                                 {"out_min", part((*v)[2], 0)},
                                 {"out_max", part((*v)[2], 1)}});
        }
        if (kind == "SplitNode") {
            const auto x = in(0);
            if (!x)
                return std::nullopt;
            const int k = std::clamp(out - 1, 0, 3);
            return x->front() == '"' ? _b.member(*x, std::string(1, "xyzw"[k])) : component(*x, k);
        }
        if (kind == "CombineNode") {
            const auto v = all({0, 1, 2, 3});
            if (!v)
                return std::nullopt;
            std::vector<std::pair<std::string, std::string>> parts{{"x", (*v)[0]}, {"y", (*v)[1]}};
            if (out == 4 || out == 5)
                parts.push_back({"z", (*v)[2]});
            if (out == 4)
                parts.push_back({"w", (*v)[3]});
            return add("combine", parts);
        }
        if (kind == "Vector1Node")
            return in(1);
        if (kind == "Vector2Node" || kind == "Vector3Node" || kind == "Vector4Node") {
            std::vector<std::pair<std::string, std::string>> parts;
            for (int k = 0; k < kind[6] - '0'; ++k) {
                const auto c = in(k + 1);
                if (!c)
                    return std::nullopt;
                parts.push_back({std::string(1, "xyzw"[k]), *c});
            }
            return add("combine", parts);
        }
        if (kind == "ColorNode") {
            const auto* c = n->find("m_Color");
            return literal(c ? c->find("color") : nullptr, true);
        }
        if (kind == "ConstantNode") {
            static const f64 constants[] = {3.14159265358979, 6.28318530717959, 1.61803398874989, 2.71828182845905,
                                            1.41421356237310};
            return GraphBuilder::number(constants[std::clamp<i64>(n->int_at("m_constant", 0), 0, 4)]);
        }
        if (kind == "SliderNode") {
            const auto* v = n->find("m_Value");
            return GraphBuilder::number(v && v->find("x") ? v->find("x")->as_number() : 0.0);
        }
        if (kind == "PropertyNode") {
            const auto* ref = n->find("m_Property");
            const JsonValue* p = ref ? object(text_of(*ref, "m_Id")) : nullptr;
            if (!p) {
                why = label + ": its property is missing";
                return std::nullopt;
            }
            if (const JsonValue* g = p->find("m_Guid"); g && !_bindings.empty()) {
                const auto b = _bindings.find(text_of(*g, "m_GuidSerialized"));
                if (b != _bindings.end())
                    return b->second.caller->input(b->second.node, b->second.slot, why);
            }
            return property(*p, why);
        }
        if (kind == "SampleTexture2DNode") {
            const auto t = in(1);
            if (!t)
                return std::nullopt;
            if (t->size() < 3 || t->front() != '"') {
                why = label + ": its texture is not a texture";
                return std::nullopt;
            }
            const std::string param = t->substr(1, t->size() - 2);
            const bool normal = n->int_at("m_TextureType", 0) == 1;
            if (normal)
                for (auto& e : _s.textures)
                    if (e.param == param)
                        e.normal = true;
            const auto uv = in(2);
            if (!uv)
                return std::nullopt;
            const std::string sample = add("texture", {{"uv", *uv}}, {{"texture", GraphBuilder::quote(param)}});
            const std::string v = normal ? add("unpack_normal", {{"texel", sample}}) : sample;
            static const std::map<int, std::string> pins{{4, "x"}, {5, "y"}, {6, "z"}, {7, "w"}};
            if (const auto p = pins.find(out); p != pins.end())
                return normal && out == 7 ? std::optional<std::string>{"1"} : std::optional{_b.member(v, p->second)};
            return v;
        }
        if (kind == "Texture2DAssetNode") {
            const JsonValue* t = n->find("m_Texture");
            const std::string guid = t ? texture_guid(text_of(*t, "m_SerializedTexture")) : "";
            if (guid.empty()) {
                why = label + ": no texture";
                return std::nullopt;
            }
            return GraphBuilder::quote(texture_param("t_" + guid.substr(0, 8), guid, ""));
        }
        if (kind == "UVNode") {
            if (n->int_at("m_OutputChannel", 0) != 0) {
                why = label + ": a second UV set (kin's meshes have one)";
                return std::nullopt;
            }
            return GraphBuilder::quote("uv");
        }
        if (kind == "TilingAndOffsetNode") {
            const auto v = all({0, 1, 2});
            return v ? std::optional{add("add",
                                         {{"a", add("multiply", {{"a", (*v)[0]}, {"b", (*v)[1]}})}, {"b", (*v)[2]}})}
                     : std::nullopt;
        }
        if (kind == "TimeNode") {
            if (out == 0)
                return GraphBuilder::quote("time");
            if (out == 1 || out == 2)
                return add(out == 1 ? "sin" : "cos", {{"x", GraphBuilder::quote("time")}});
            why = label + ": its delta time (kin's graphs have the time only)";
            return std::nullopt;
        }
        if (kind == "PositionNode" || kind == "NormalVectorNode" || kind == "ViewDirectionNode") {
            const i64 space = n->int_at("m_Space", 2);
            if (space != 2 && space != 4) { // world, absolute world
                why = label + ": in " +
                      (space == 0   ? "object"
                       : space == 1 ? "view"
                                    : "tangent") +
                      " space (kin's graphs have the world's)";
                return std::nullopt;
            }
            return GraphBuilder::quote(kind == "PositionNode"       ? "position"
                                       : kind == "NormalVectorNode" ? "normal"
                                                                    : "view");
        }
        if (kind == "VertexColorNode")
            return GraphBuilder::quote("vertex_color");
        if (kind == "FresnelNode" || kind == "FresnelEffectNode") {
            const auto power = in(2);
            return power ? std::optional{add("fresnel", {{"power", *power}})} : std::nullopt;
        }
        if (kind == "NoiseNode" || kind == "GradientNoiseNode" || kind == "VoronoiNode") {
            const auto uv = in(0), scale = in(kind == "VoronoiNode" ? 2 : 1);
            if (!uv || !scale)
                return std::nullopt;
            _x.note(label + ": Unity's noise as kin's (" +
                    std::string(kind == "NoiseNode"           ? "value"
                                : kind == "GradientNoiseNode" ? "gradient"
                                                              : "cellular") +
                    "): alike, not the same pattern");
            return add(kind == "NoiseNode"           ? "noise_value"
                       : kind == "GradientNoiseNode" ? "noise_gradient"
                                                     : "noise_cellular",
                       {{"p", *uv}, {"scale", *scale}});
        }
        if (kind == "BranchNode") { // the predicate is 0 or 1
            const auto p = in(0);
            const auto v = p ? dynamic(id, {1, 2}, why) : std::nullopt;
            return v ? std::optional{add("mix", {{"a", (*v)[1]}, {"b", (*v)[0]}, {"t", *p}})} : std::nullopt;
        }
        if (kind == "ComparisonNode") {
            static const char* ops[] = {"==", "!=", "<", "<=", ">", ">="};
            const auto v = all({0, 1});
            return v ? glsl(std::string("float(a ") + ops[std::clamp<i64>(n->int_at("m_ComparisonType", 0), 0, 5)] +
                                " b)",
                            "float", {{"a", (*v)[0]}, {"b", (*v)[1]}})
                     : std::nullopt;
        }
        if (kind == "KeywordNode") { // a boolean keyword: its On or Off slot by the material's keywords
            const auto* ref = n->find("m_Keyword");
            const JsonValue* k = ref ? object(text_of(*ref, "m_Id")) : nullptr;
            if (!k || k->int_at("m_KeywordType", 0) != 0) {
                why = label + ": an enum keyword";
                return std::nullopt;
            }
            const std::string name = !text_of(*k, "m_OverrideReferenceName").empty()
                                         ? text_of(*k, "m_OverrideReferenceName")
                                         : text_of(*k, "m_DefaultReferenceName");
            const bool on = _s.keywords ? _s.keywords->contains(name) : k->int_at("m_Value", 0) != 0;
            if (on)
                _s.keywords_on.insert(name);
            return in(on ? 1 : 2);
        }
        if (kind == "TransformNode") {
            const JsonValue* c = n->find("m_Conversion");
            const i64 from = c ? c->int_at("from", 2) : 2, to = c ? c->int_at("to", 2) : 2;
            const auto x = in(0);
            if (!x)
                return std::nullopt;
            const bool world = [](i64 s) { return s == 2 || s == 4; }(from) && (to == 2 || to == 4);
            if (world || from == to)
                return x;
            if (from == 3 && (to == 2 || to == 4) && n->int_at("m_ConversionType", 0) != 0)
                return add("tangent_to_world", {{"normal", *x}});
            why = label + ": from " + std::to_string(from) + " to " + std::to_string(to) +
                  " space (kin's graphs have the world's, and tangent to world)";
            return std::nullopt;
        }
        if (kind == "HueNode") { // a turn about grey (not Unity's HSV shift: alike)
            const auto v = all({0, 1});
            if (!v)
                return std::nullopt;
            _x.note(label + ": its hue shift as a turn about grey: alike, not the same colours");
            const bool degrees = n->int_at("m_HueMode", 0) == 0;
            return glsl(
                std::string("c * cos(a) + cross(vec3(0.57735), c) * sin(a) + vec3(0.57735) * dot(vec3(0.57735), "
                            "c) * (1.0 - cos(a))"),
                "vec3",
                {{"c", (*v)[0]},
                 {"a", add("multiply",
                           {{"a", (*v)[1]}, {"b", GraphBuilder::number(degrees ? 0.0174532925 : 6.28318531)}})}});
        }
        if (kind == "ExponentialNode") {
            const auto x = in(0);
            if (!x)
                return std::nullopt;
            return n->int_at("m_ExponentialBase", 0) == 1 ? glsl("exp2(x)", "float", {{"x", *x}})
                                                          : std::optional{add("exp", {{"x", *x}})};
        }
        if (kind == "NormalBlendNode") {
            const auto v = all({0, 1});
            if (!v)
                return std::nullopt;
            return n->int_at("m_BlendMode", 0) == 0
                       ? glsl("normalize(vec3(a.xy + b.xy, a.z * b.z))", "vec3", {{"a", (*v)[0]}, {"b", (*v)[1]}})
                       : glsl("normalize((a + vec3(0.0, 0.0, 1.0)) * dot(a + vec3(0.0, 0.0, 1.0), b * vec3(-1.0, -1.0, "
                              "1.0)) / (a.z + 1.0) - b * vec3(-1.0, -1.0, 1.0))",
                              "vec3", {{"a", (*v)[0]}, {"b", (*v)[1]}});
        }
        if (kind == "IsFrontFaceNode")
            return glsl("gl_FrontFacing ? 1.0 : 0.0", "float", {});
        if (kind == "TriangleWaveNode") {
            const auto x = in(0);
            return x ? glsl("2.0 * abs(2.0 * (x - floor(0.5 + x))) - 1.0", "float", {{"x", *x}}) : std::nullopt;
        }
        if (kind == "SubGraphNode")
            return sub_graph(id, *n, out, why);
        if (kind == "RedirectNodeData" || kind == "PreviewNode")
            return in(0);
        why = label + ": a " + kind + " (kin has no node for it)";
        return std::nullopt;
    }
    static std::string component(const std::string& v, int k) { return GraphBuilder::component(v, k); }

    GraphBuilder& _b;
    material_import::Context& _x;
    GraphShared& _s;
    int _depth = 0;
    std::map<std::string, Binding> _bindings;
    std::map<std::string, const JsonValue*> _objects;
    std::map<std::pair<std::string, int>, std::pair<std::string, int>> _edges;
    std::map<std::string, std::string> _done;
    std::set<std::string> _visiting;
    std::map<std::string, std::unique_ptr<ShaderGraph>> _children;
};

} // namespace

Result3D<ImportedMaterial3D> import_unity_graph(const std::filesystem::path& source,
                                                const MaterialImportOptions3D& options) {
    auto result = import_unity_material(source, options);
    if (!result)
        return result;
    try {
        const UnityMaterial m = read_unity_material(read_file(source));
        ImportedMaterial3D& imported = *result.value;
        material_import::Context x(imported, options, source.stem().string());
        const fs::path project = fs::absolute(options.project).lexically_normal();
        const auto file = GuidIndex::find(project, m.shader_guid);
        if (!file || Context::extension(*file) != ".shadergraph") {
            x.finish();
            return result; // not a Shader Graph's material: its Tier 1 import
        }
        std::string graph_text;
        try {
            graph_text = read_file(*file);
        } catch (const std::exception&) {
            x.note("its Shader Graph (" + file->filename().string() + ") is missing: its Tier 1 import only");
            x.finish();
            return result;
        }
        const auto objects = shader_graph_objects(graph_text);
        GraphBuilder b;
        b.name = file->stem().string();
        GraphShared shared;
        shared.keywords = &m.keywords;
        shared.project = project;
        ShaderGraph g(objects, b, x, shared);
        if (!g.graph) {
            x.note("its Shader Graph (" + file->filename().string() +
                   ") is of the format before Shader Graph 10: its Tier 1 import only");
            x.finish();
            return result;
        }
        std::erase_if(imported.notes, [](const std::string& n) { return n.rfind("a Shader Graph", 0) == 0; });
        // The blocks.
        static const std::vector<std::pair<const char*, const char*>> mapping{
            {"SurfaceDescription.BaseColor", "base_color"}, {"SurfaceDescription.Metallic", "metallic"},
            {"SurfaceDescription.Smoothness", "roughness"}, {"SurfaceDescription.Emission", "emission"},
            {"SurfaceDescription.Occlusion", "occlusion"},  {"SurfaceDescription.NormalTS", "normal"},
            {"SurfaceDescription.Alpha", "alpha"}};
        for (const auto& [descriptor, target] : mapping) {
            const auto it = g.blocks.find(descriptor);
            if (it == g.blocks.end())
                continue;
            std::string why;
            auto v = g.input(it->second, 0, why);
            if (!v) {
                x.note(std::string(descriptor).substr(19) + ": " + why + ": its Tier 1 value kept");
                continue;
            }
            std::string to = target;
            if (to == "roughness")
                v = b.add("one_minus", {{"x", *v}});
            if (to == "normal")
                v = b.add("tangent_to_world", {{"normal", *v}});
            b.outputs[to] = *v;
        }
        for (const auto& [descriptor, id] : g.blocks)
            if (descriptor.rfind("VertexDescription.", 0) == 0 && g.fed(id, 0))
                x.note(descriptor.substr(18) + ": the vertex stage (kin's graphs shade surfaces): left out");
        if (b.outputs.empty()) {
            x.note("none of its graph's blocks could be imported: its Tier 1 import only");
            x.finish();
            return result;
        }
        // The textures in kin's five slots (as graph data), sRGB but for normal maps.
        const auto texture = [&](const GraphBuilder::Param& p) {
            return *std::ranges::find(shared.textures, p.name, &GraphShared::Texture::param);
        };
        if (!b.assign_slots([&](const GraphBuilder::Param& p) { return texture(p).normal; })) {
            x.note("its graph samples more than five textures (kin's material has five map slots): its Tier 1 import "
                   "only");
            x.finish();
            return result;
        }
        // Each texture's: the material's, else the graph's default; its colour space as its import settings say.
        const auto texture_file = [&](const std::string& name) -> std::optional<fs::path> {
            const auto t = texture(*std::ranges::find(b.params, name, &GraphBuilder::Param::name));
            std::string guid = t.guid;
            const std::string ref = shared.ref_of.contains(name) ? shared.ref_of.at(name) : std::string();
            if (const UnityTexEnv* env = ref.empty() ? nullptr : m.texture(ref))
                guid = env->guid;
            return guid.empty() ? std::nullopt : GuidIndex::find(project, guid);
        };
        // The graph's colour spaces (one for every material of it): its default texture's import setting, else
        // its name's (masks, normals, metallic and smoothness, occlusion linear), else sRGB but for normal maps.
        for (auto& p : b.params)
            if (p.type == "texture") {
                const auto t = texture(p);
                const auto file = t.guid.empty() ? std::nullopt : GuidIndex::find(project, t.guid);
                if (const auto srgb = file ? unity_srgb(*file) : std::nullopt)
                    p.srgb = *srgb;
                else {
                    std::string name = p.name;
                    std::ranges::transform(name, name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                    p.srgb = !t.normal;
                    for (const char* data : {"mask", "normal", "bump", "metal", "smooth", "rough", "occlusion", "_ao",
                                             "height", "mrl", "_mr", "noise"})
                        if (name.find(data) != std::string::npos)
                            p.srgb = false;
                }
            }
        const std::string text = b.write();
        const auto compiled = compile_material_graph3d(text);
        if (!compiled.ok()) {
            x.note("the graph did not compile in kin (" + compiled.errors.front().node + ": " +
                   compiled.errors.front().message + "): its Tier 1 import only");
            x.finish();
            return result;
        }
        for (const auto& w : compiled.warnings)
            x.note(w.node + ": " + w.message);
        // Its file: a graph of the material's keywords (those its Keyword nodes read, on) by them.
        std::string file_name = b.name;
        for (const auto& k : shared.keywords_on)
            file_name += "_" + k;
        imported.graph = x.out / (file_name + ".kingraph");
        if (options.write) {
            fs::create_directories(x.out);
            std::ofstream out(imported.graph, std::ios::binary);
            out << text;
        }
        auto& p = imported.asset.base.material;
        p.graph = imported.graph.lexically_relative(x.content);
        // The graph reads the mesh's UV as its own nodes tile it: Tier 1's tiling (the material's UV transform, which
        // kin's "uv" input goes through) would tile it twice.
        p.uv_scale = Vec2f{1, 1};
        p.uv_offset = Vec2f{0, 0};
        // The target: its surface, alpha clip and sides; Unlit.
        if (g.target) {
            const bool transparent = g.target->int_at("m_SurfaceType", 0) == 1;
            const i64 mode = g.target->int_at("m_AlphaMode", 0);
            p.blend = transparent                               ? (mode == 2 ? Blend3D::Additive : Blend3D::Alpha)
                      : g.target->bool_at("m_AlphaClip", false) ? Blend3D::Mask
                                                                : Blend3D::Opaque;
            p.double_sided = g.target->bool_at("m_TwoSided", false);
        }
        p.unlit = g.unlit;
        if (const auto clip = g.blocks.find("SurfaceDescription.AlphaClipThreshold"); clip != g.blocks.end()) {
            std::string why;
            if (const auto v = g.input(clip->second, 0, why); v && v->front() != '[') {
                f32 cutoff = v->front() == '"' ? -1.0f : f32(std::strtod(v->c_str(), nullptr));
                if (v->front() == '"') { // a property: the material's value, else its default
                    const std::string name = v->substr(1, v->size() - 2);
                    const auto param = std::ranges::find(b.params, name, &GraphBuilder::Param::name);
                    const auto ref = shared.ref_of.contains(name) ? shared.ref_of.at(name) : std::string();
                    if (param != b.params.end() && param->type == "float")
                        cutoff = m.floats.contains(ref) ? f32(m.floats.at(ref)) : param->value[0];
                }
                if (cutoff >= 0.0f)
                    p.alpha_cutoff = std::clamp(cutoff, 0.0f, 1.0f);
            }
        }
        // The material's values, by reference name; its textures.
        for (const auto& param : b.params) {
            const auto ref = shared.ref_of.contains(param.name) ? shared.ref_of.at(param.name) : std::string();
            MaterialGraphValue3D v;
            if (param.type == "texture") {
                const auto tex = texture_file(param.name);
                const auto path = tex && fs::exists(*tex) ? x.readable(*tex, param.name) : std::nullopt;
                if (!path) {
                    const bool said = std::ranges::any_of(imported.notes, [&](const std::string& n) {
                        return !ref.empty() && n.rfind(ref + ":", 0) == 0;
                    });
                    if (!said) // Tier 1 tells of the material's own textures
                        x.note(param.name + ": its texture is not in the project: left out");
                    continue;
                }
                if (const auto srgb = unity_srgb(*tex); srgb && *srgb != param.srgb)
                    x.note(param.name + ": its texture is " + (*srgb ? "sRGB" : "linear") + " where its graph reads " +
                           (param.srgb ? "sRGB" : "linear") + " (the graph's default texture's)");
                // Repeated, filtered and mipmapped, as Unity's default sampling is (a graph's UVs often run
                // past 0..1); a map without a sampler would take Material3D's legacy clamp.
                v.texture = MaterialTexture3D{.asset = *path, .sampler = TextureSampler3D{}};
            } else if (param.type == "color") {
                const auto c = ref.empty() ? std::nullopt : m.color(ref);
                if (!c)
                    continue;
                v.count = 4;
                v.value = {unity_linear(c->at(0)), unity_linear(c->at(1)), unity_linear(c->at(2)), c->at(3)};
            } else if (param.type == "float") {
                if (ref.empty() || !m.floats.contains(ref))
                    continue;
                v.count = 1;
                v.value[0] = f32(m.floats.at(ref));
            } else {
                const auto c = ref.empty() ? std::nullopt : m.color(ref); // vectors are saved with the colours
                if (!c)
                    continue;
                v.count = param.type == "vec2" ? 2 : param.type == "vec3" ? 3 : 4;
                v.value = *c;
            }
            p.params[param.name] = v;
        }
        x.finish();
    } catch (const std::exception& error) {
        result.value.reset();
        result.diagnostics.push_back(source.string() + ": " + error.what());
    }
    return result;
}

} // namespace kin
