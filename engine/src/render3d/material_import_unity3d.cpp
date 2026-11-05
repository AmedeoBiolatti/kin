// Unity's materials (.mat) as kin's: the built-in pipeline's Standard, URP's Lit (and Simple Lit, Baked Lit, Unlit),
// HDRP's Lit, read by their properties (kin/render3d/material_import3d.hpp).
#include "material_import3d_impl.hpp"

#include <array>
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
        const auto base = m.color(hdrp ? "_BaseColor" : first({"_BaseColor", "_Color"})).value_or({1, 1, 1, 1});
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
