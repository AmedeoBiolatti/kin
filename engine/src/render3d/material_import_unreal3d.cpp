// Unreal's materials as kin's, from what tools/import_material/unreal_export.py writes inside Unreal's editor
// (kin.unreal-material/1: blend mode, shading model, parameters, and the expressions feeding each output).
#include "material_import3d_impl.hpp"

#include <array>
#include <cstring>
#include <kin/core/json_value.hpp>
#include <kin/render3d/material_graph3d.hpp>
#include <map>
#include <set>
#include <string_view>

namespace kin {
namespace {

using namespace material_import;
namespace fs = std::filesystem;

// What feeds an output, as far as kin reads it: bias + gain x (a texture's channel, its RGB, or nothing), per
// channel. Products of two textures, and expressions kin has no counterpart for, are `why` instead.
struct Term {
    std::array<f32, 4> bias{0, 0, 0, 0}, gain{1, 1, 1, 1};
    std::string texture; // the export's file, relative to it (empty: none)
    int channel = -1;    // -1: RGB(A) as the texture's; 0..3: one channel for every one
    Vec2f tiling{1, 1};  // a TextureCoordinate's
    std::string why;     // not read: why
    bool constant() const { return why.empty() && texture.empty(); }
};

f32 srgb_of(f32 linear) {
    return linear_to_srgb(std::clamp(linear, 0.0f, 1.0f));
}

class Reader {
public:
    explicit Reader(const JsonValue& doc) {
        if (const auto* p = doc.find("parameters")) {
            for (const char* kind : {"scalar", "vector", "texture"})
                if (const auto* group = p->find(kind); group && group->is_object())
                    for (const auto& [name, value] : group->members())
                        _params[std::string(kind) + ":" + name] = &value;
        }
    }

    Term term(const JsonValue* n, int depth = 0) const {
        Term t;
        if (!n || n->is_null()) {
            t.why = "nothing";
            return t;
        }
        if (depth > 8) {
            t.why = "too deep";
            return t;
        }
        const std::string cls = n->find("class") ? n->find("class")->as_string() : "";
        const std::string kind = cls.rfind("MaterialExpression", 0) == 0 ? cls.substr(18) : cls;
        const std::string output = n->find("output") ? n->find("output")->as_string() : "";
        const std::string param = n->find("parameter") ? n->find("parameter")->as_string() : "";
        const auto input = [&](size_t i) -> const JsonValue* {
            const auto* inputs = n->find("inputs");
            return inputs && inputs->is_array() && i < inputs->items().size() ? &inputs->items()[i] : nullptr;
        };
        const auto connected = [&](size_t i) { return input(i) && !input(i)->is_null(); };
        const auto constant = [](f32 v) {
            Term c;
            c.bias = {v, v, v, v};
            c.gain = {0, 0, 0, 0};
            return c;
        };
        const auto pick = [&](Term t) { // a vector's output: "R" .. "A" one channel, else as it is
            static const std::string names = "RGBA";
            if (output.size() == 1 && names.find(output) != std::string::npos) {
                const int k = int(names.find(output));
                if (t.texture.empty()) {
                    t.bias.fill(t.bias[k]);
                    t.gain.fill(t.gain[k]);
                } else {
                    t.channel = k;
                }
            }
            return t;
        };
        if (kind == "TextureSample" || kind == "TextureSampleParameter2D") {
            t.texture = n->find("texture") && n->find("texture")->is_string() ? n->find("texture")->as_string() : "";
            if (const auto* v = parameter("texture", param))
                t.texture = v->is_string() ? v->as_string() : "";
            if (t.texture.empty()) {
                t.why = "a texture sample of no texture";
                return t;
            }
            t.bias = {0, 0, 0, 0};
            if (connected(0)) { // its UV
                const auto* uv = input(0);
                const std::string uv_class = uv->find("class") ? uv->find("class")->as_string() : "";
                if (uv_class != "MaterialExpressionTextureCoordinate" || uv->int_at("index", 0) != 0) {
                    t.why = "UVs made by a " + uv_class.substr(std::min<size_t>(18, uv_class.size()));
                    return t;
                }
                t.tiling = {f32(uv->number_at("u_tiling", 1)), f32(uv->number_at("v_tiling", 1))};
            }
            return pick(t);
        }
        if (kind == "Constant" || kind == "ScalarParameter") {
            f32 v = f32(n->number_at("value", 0));
            if (const auto* p = parameter("scalar", param))
                v = f32(p->as_number(v));
            return constant(v);
        }
        if (kind == "Constant3Vector" || kind == "Constant4Vector" || kind == "VectorParameter") {
            const JsonValue* v = n->find("value");
            if (const auto* p = parameter("vector", param))
                v = p;
            Term c = constant(0);
            if (v && v->is_array())
                for (size_t i = 0; i < std::min<size_t>(4, v->items().size()); ++i)
                    c.bias[i] = f32(v->items()[i].as_number());
            return pick(c);
        }
        if (kind == "Multiply" || kind == "Add" || kind == "Subtract") {
            const Term a = connected(0) ? term(input(0), depth + 1) : constant(f32(n->number_at("const_a", 0)));
            const Term b = connected(1) ? term(input(1), depth + 1) : constant(f32(n->number_at("const_b", 1)));
            if (!a.why.empty() || !b.why.empty())
                return a.why.empty() ? b : a;
            if (!a.texture.empty() && !b.texture.empty()) {
                t.why = "a " + kind + " of two textures";
                return t;
            }
            Term out = a.texture.empty() ? b : a;
            const Term& k = a.texture.empty() ? a : b; // the constant one
            for (int i = 0; i < 4; ++i) {
                if (kind == "Multiply") { // (bias + gain x T) x c
                    out.bias[i] =
                        (a.texture.empty() && b.texture.empty() ? a.bias[i] * b.bias[i] : out.bias[i] * k.bias[i]);
                    out.gain[i] *= k.bias[i];
                } else {
                    const f32 sign = kind == "Subtract" && !a.texture.empty() ? -1.0f : 1.0f;
                    if (kind == "Subtract" && a.texture.empty() && !b.texture.empty()) { // c - T
                        out.bias[i] = k.bias[i] - out.bias[i];
                        out.gain[i] = -out.gain[i];
                    } else if (a.texture.empty() && b.texture.empty()) {
                        out.bias[i] = kind == "Subtract" ? a.bias[i] - b.bias[i] : a.bias[i] + b.bias[i];
                    } else {
                        out.bias[i] += sign * k.bias[i];
                    }
                }
            }
            if (a.texture.empty() && b.texture.empty())
                out.gain = {0, 0, 0, 0};
            return out;
        }
        if (kind == "OneMinus") {
            Term in = term(input(0), depth + 1);
            if (in.why.empty())
                for (int i = 0; i < 4; ++i)
                    in.bias[i] = 1.0f - in.bias[i], in.gain[i] = -in.gain[i];
            return in;
        }
        if (kind == "ComponentMask") {
            Term in = term(input(0), depth + 1);
            const auto* mask = n->find("mask");
            int only = -1, count = 0;
            if (mask && mask->is_array())
                for (size_t i = 0; i < mask->items().size(); ++i)
                    if (mask->items()[i].as_bool())
                        only = int(i), ++count;
            if (in.why.empty() && count == 1) {
                if (in.texture.empty()) {
                    in.bias.fill(in.bias[only]);
                    in.gain.fill(in.gain[only]);
                } else {
                    in.channel = in.channel < 0 ? only : in.channel;
                }
            }
            return in;
        }
        if (kind == "LinearInterpolate") { // only where its alpha is a constant 0 or 1: one input or the other
            const Term alpha =
                connected(2) ? term(input(2), depth + 1) : constant(f32(n->number_at("const_alpha", 0.5)));
            if (alpha.constant() && (alpha.bias[0] == 0.0f || alpha.bias[0] == 1.0f)) {
                const bool b = alpha.bias[0] == 1.0f;
                return connected(b ? 1 : 0) ? term(input(b ? 1 : 0), depth + 1)
                                            : constant(f32(n->number_at(b ? "const_b" : "const_a", b ? 1 : 0)));
            }
            t.why = "a Lerp by " + (alpha.why.empty() ? std::string("a varying alpha") : alpha.why);
            return t;
        }
        if (kind == "CollectionParameter") {
            t.why = "the parameter collection's " + param + " (set while the game runs)";
            return t;
        }
        t.why = "a " + kind;
        return t;
    }

private:
    const JsonValue* parameter(const std::string& kind, const std::string& name) const {
        if (name.empty())
            return nullptr;
        const auto it = _params.find(kind + ":" + name);
        return it == _params.end() ? nullptr : it->second;
    }
    std::map<std::string, const JsonValue*> _params;
};

// Whether every texel of `image` is the same (Unreal's placeholder maps: white, black, the flat normal).
std::optional<std::array<u8, 4>> uniform(const Image& image) {
    if (!image.valid())
        return std::nullopt;
    for (size_t i = 4; i < image.rgba.size(); i += 4)
        if (std::memcmp(&image.rgba[i], image.rgba.data(), 4) != 0)
            return std::nullopt;
    return std::array<u8, 4>{image.rgba[0], image.rgba[1], image.rgba[2], image.rgba[3]};
}

} // namespace

Result3D<ImportedMaterial3D> import_unreal_material(const std::filesystem::path& source,
                                                    const MaterialImportOptions3D& options) {
    Result3D<ImportedMaterial3D> result;
    try {
        const std::string text = read_file(source);
        const auto parsed = parse_json(text);
        if (!parsed.ok() || !parsed.value->is_object() || !parsed.value->find("schema") ||
            parsed.value->find("schema")->as_string() != "kin.unreal-material/1")
            throw std::runtime_error("not a kin.unreal-material/1 export (tools/import_material/unreal_export.py)");
        const JsonValue& doc = *parsed.value;
        ImportedMaterial3D imported;
        imported.name = doc.find("name") ? doc.find("name")->as_string() : source.stem().string();
        Context x(imported, options, source.stem().string());
        const Reader reader(doc);
        const fs::path root = fs::absolute(options.project).lexically_normal(); // the export's folder
        const JsonValue* inputs = doc.find("inputs");
        const auto input = [&](const char* key) { return inputs ? inputs->find(key) : nullptr; };
        const auto settings = [&](const std::string& file) -> const JsonValue* {
            const auto* t = doc.find("textures");
            return t ? t->find(file) : nullptr;
        };
        const std::string blend = doc.find("blend_mode") ? doc.find("blend_mode")->as_string() : "BLEND_OPAQUE";
        const std::string model = doc.find("shading_model") ? doc.find("shading_model")->as_string() : "MSM_DEFAULTLIT";
        const bool unlit = model == "MSM_UNLIT";

        // Each output's term; said (once) when kin can't read it.
        std::map<std::string, Term> terms;
        for (const char* key : {"BaseColor", "Metallic", "Roughness", "Specular", "EmissiveColor", "Opacity",
                                "OpacityMask", "Normal", "AmbientOcclusion"}) {
            if (const auto* n = input(key)) {
                terms[key] = reader.term(n);
                if (!terms[key].why.empty())
                    x.note(std::string(key) + ": " + terms[key].why + ": not read");
            }
        }
        for (const char* key : {"WorldPositionOffset", "Refraction", "Anisotropy"})
            if (input(key))
                x.note(std::string(key) + ": not shown by kin");
        const auto readable = [&](const Term& t) { return t.why.empty() && !t.texture.empty(); };
        // A term's texture: its pixels (once), and none for a map of one colour, which is folded into the factor.
        std::map<std::string, std::optional<Image>> images;
        const auto image_of = [&](const std::string& file) -> const std::optional<Image>& {
            auto it = images.find(file);
            if (it == images.end())
                it = images.emplace(file, x.load(root / file, file)).first;
            return it->second;
        };
        const auto fold = [&](Term& t) { // a uniform texture into the bias: (bias + gain x texel)
            if (!readable(t))
                return;
            const auto& image = image_of(t.texture);
            if (!image)
                return;
            if (const auto c = uniform(*image)) {
                for (int i = 0; i < 4; ++i) {
                    const int k = t.channel >= 0 ? t.channel : i;
                    const bool srgb = settings(t.texture) && settings(t.texture)->bool_at("srgb", false) && k < 3;
                    const f32 v = srgb ? srgb_to_linear((*c)[k] / 255.0f) : (*c)[k] / 255.0f;
                    t.bias[i] += t.gain[i] * v;
                    t.gain[i] = 0;
                }
                t.texture.clear();
            }
        };
        for (auto& [key, t] : terms)
            if (key != "Normal")
                fold(t);

        TextureSampler3D sampler; // Unreal's textures are sampled wrapped, filtered, mipmapped by default
        sampler.wrap_u = sampler.wrap_v = Wrap3D::Repeat;
        const auto map = [&](fs::path p) { return MaterialTexture3D{.asset = std::move(p), .sampler = sampler}; };
        const MaterialTexture3D none{};
        auto& p = imported.asset.base.material;
        std::optional<Vec2f> tiling;
        const auto tile = [&](const Term& t, const char* key) {
            if (!readable(t))
                return;
            if (!tiling)
                tiling = t.tiling;
            else if (*tiling != t.tiling)
                x.note(std::string(key) + "'s UV tiling differs from the others': kin reads every map with one");
        };

        // Base colour (Unreal's colours are linear): the gain's, times the map; an unlit material's is its emissive.
        Term base = unlit ? terms["EmissiveColor"] : terms["BaseColor"];
        if (!terms.contains(unlit ? "EmissiveColor" : "BaseColor"))
            base = Term{.bias = {0, 0, 0, 1}, .gain = {0, 0, 0, 0}}; // Unreal's unconnected base colour: black
        f32 alpha = 1.0f;
        const bool masked = blend == "BLEND_MASKED", translucent = blend == "BLEND_TRANSLUCENT" ||
                                                                   blend == "BLEND_ADDITIVE" ||
                                                                   blend == "BLEND_ALPHACOMPOSITE";
        const Term* opacity = masked        ? (terms.contains("OpacityMask") ? &terms["OpacityMask"] : nullptr)
                              : translucent ? (terms.contains("Opacity") ? &terms["Opacity"] : nullptr)
                                            : nullptr;
        if (opacity && opacity->why.empty()) {
            if (opacity->texture.empty())
                alpha = opacity->bias[0];
            else if (opacity->texture == base.texture && opacity->channel == 3 && opacity->bias[0] == 0)
                alpha = opacity->gain[0]; // the base map's alpha, as kin's
            else
                x.note("opacity from another map than the base colour's alpha: kin reads the base map's");
        }
        if (base.why.empty()) {
            const auto c = base.texture.empty() ? base.bias : base.gain;
            p.color = Color{byte(srgb_of(c[0])), byte(srgb_of(c[1])), byte(srgb_of(c[2])), byte(alpha)};
            if (!base.texture.empty() && (base.bias[0] != 0 || base.bias[1] != 0 || base.bias[2] != 0))
                x.note("base colour: an offset added to its map: left out");
            const auto path = readable(base) ? x.readable(root / base.texture, "base colour") : std::nullopt;
            p.texture = path ? map(*path) : none;
            tile(base, "base colour");
        } else {
            p.color = Color{255, 255, 255, byte(alpha)};
            p.texture = none;
        }

        // Metallic, roughness, occlusion: factors, or one map of kin's (G roughness, B metallic) as the export's
        // when it is laid out so (Unreal's ORM maps: R occlusion, G roughness, B metallic), else made.
        const auto scalar_of = [&](const char* key, f32 fallback) {
            const auto it = terms.find(key);
            return it == terms.end() || !it->second.why.empty()
                       ? Term{.bias = {fallback, fallback, fallback, fallback}, .gain = {0, 0, 0, 0}}
                       : it->second;
        };
        const Term metal = scalar_of("Metallic", 0.0f), rough = scalar_of("Roughness", 0.5f),
                   ao = scalar_of("AmbientOcclusion", 1.0f);
        const bool metal_mapped = readable(metal), rough_mapped = readable(rough);
        p.metallic = std::clamp(metal_mapped ? metal.gain[0] : metal.bias[0], 0.0f, 1.0f);
        p.roughness = std::clamp(rough_mapped ? rough.gain[0] : rough.bias[0], 0.0f, 1.0f);
        p.metallic_roughness = none;
        if (metal_mapped || rough_mapped) {
            const bool same = (!metal_mapped || !rough_mapped || metal.texture == rough.texture);
            const bool as_kin = same && (!metal_mapped || (metal.channel == 2 && metal.bias[0] == 0)) &&
                                (!rough_mapped || (rough.channel == 1 && rough.bias[0] == 0));
            const std::string file = metal_mapped ? metal.texture : rough.texture;
            if (as_kin && (metal_mapped && rough_mapped)) {
                if (const auto path = x.readable(root / file, "metallic and roughness"))
                    p.metallic_roughness = map(*path);
            } else {
                const auto metal_image = metal_mapped ? image_of(metal.texture) : std::nullopt;
                const auto rough_image = rough_mapped ? image_of(rough.texture) : std::nullopt;
                if ((!metal_mapped || metal_image) && (!rough_mapped || rough_image)) {
                    const Image& a = metal_image ? *metal_image : *rough_image;
                    const Vec2i size{std::max(a.size.x, rough_image ? rough_image->size.x : 0),
                                     std::max(a.size.y, rough_image ? rough_image->size.y : 0)};
                    Image packed{size, std::vector<u8>(size_t(size.x) * size.y * 4, 255)};
                    const auto value = [&](const Term& t, const Image& im, int xx, int y) {
                        const f32 v = channel(im, texel(im, size, xx, y), t.channel < 0 ? 0 : t.channel) / 255.0f;
                        return t.bias[0] + t.gain[0] * v;
                    };
                    for (int y = 0; y < size.y; ++y)
                        for (int xx = 0; xx < size.x; ++xx) {
                            u8* o = &packed.rgba[(size_t(y) * size.x + xx) * 4];
                            o[1] = rough_image ? byte(value(rough, *rough_image, xx, y)) : 255;
                            o[2] = metal_image ? byte(value(metal, *metal_image, xx, y)) : 255;
                        }
                    if (rough_mapped)
                        p.roughness = 1.0f;
                    if (metal_mapped)
                        p.metallic = 1.0f;
                    p.metallic_roughness = map(x.made("_mr", packed));
                }
            }
            tile(metal_mapped ? metal : rough, "metallic and roughness");
        }
        p.occlusion = none;
        p.occlusion_strength = 1.0f;
        if (readable(ao)) {
            if (ao.channel <= 0 && ao.bias[0] == 0 && ao.gain[0] == 1) {
                if (const auto path = x.readable(root / ao.texture, "ambient occlusion"))
                    p.occlusion = map(*path);
            } else if (const auto& image = image_of(ao.texture)) {
                Image made = *image;
                for (size_t i = 0; i < size_t(made.size.x) * made.size.y; ++i)
                    made.rgba[i * 4] =
                        byte(ao.bias[0] + ao.gain[0] * channel(made, i, ao.channel < 0 ? 0 : ao.channel) / 255.0f);
                p.occlusion = map(x.made("_ao", made));
            }
            tile(ao, "ambient occlusion");
        }
        if (terms.contains("Specular") && terms["Specular"].why.empty() &&
            (!terms["Specular"].texture.empty() || std::abs(terms["Specular"].bias[0] - 0.5f) > 1e-3f))
            x.note("Specular is not 0.5: kin's dielectrics reflect 4% (0.5)");

        // Normal map: Unreal's are DirectX's (green down) unless the texture flips green on import, whose source (as
        // exported) is then as kin's.
        p.normal = none;
        p.normal_scale = 1.0f;
        if (terms.contains("Normal") && readable(terms["Normal"])) {
            const Term& n = terms["Normal"];
            const auto* s = settings(n.texture);
            const bool flipped_on_import = s && s->bool_at("flip_green_channel", false);
            const auto& image = image_of(n.texture);
            const auto one = image ? uniform(*image) : std::nullopt;
            const bool flat =
                one && std::abs((*one)[0] - 128) <= 2 && std::abs((*one)[1] - 128) <= 2 && (*one)[2] >= 250;
            if (flat) {
                // the flat normal (a placeholder): none
            } else if (flipped_on_import) {
                if (const auto path = x.readable(root / n.texture, "normal"))
                    p.normal = map(*path);
            } else if (image) {
                Image made = *image;
                for (size_t i = 0; i < size_t(made.size.x) * made.size.y; ++i)
                    made.rgba[i * 4 + 1] = u8(255 - made.rgba[i * 4 + 1]);
                p.normal = map(x.made("_normal", made));
            }
            tile(n, "normal");
            if (n.gain[0] != 1.0f || n.bias[0] != 0.0f)
                x.note("normal: scaled or offset in the graph: imported as the map");
        }

        // Emission (linear): its gain times its map, or its colour.
        p.emission_mode = EmissionMode3D::Independent;
        p.emission_color = LinearRgb{1, 1, 1};
        p.emissive = 0.0f;
        p.emissive_texture = none;
        if (!unlit && terms.contains("EmissiveColor") && terms["EmissiveColor"].why.empty()) {
            const Term& e = terms["EmissiveColor"];
            const auto c = e.texture.empty() ? e.bias : e.gain;
            if (c[0] > 0 || c[1] > 0 || c[2] > 0) {
                p.emission_color = LinearRgb{c[0], c[1], c[2]};
                p.emissive = 1.0f;
                if (!e.texture.empty())
                    if (const auto path = x.readable(root / e.texture, "emissive"))
                        p.emissive_texture = map(*path);
                tile(e, "emissive");
            }
        }
        if (tiling && *tiling != Vec2f{1, 1})
            p.uv_scale = *tiling;

        // Blend mode, faces, the clip value.
        p.blend = masked                      ? Blend3D::Mask
                  : blend == "BLEND_ADDITIVE" ? Blend3D::Additive
                  : translucent               ? Blend3D::Alpha
                                              : Blend3D::Opaque;
        if (blend == "BLEND_MODULATE")
            x.note("modulate blending: kin blends by alpha (or adds)");
        if (blend == "BLEND_ALPHACOMPOSITE")
            x.note("alpha composite (premultiplied) blending: kin blends by straight alpha");
        p.alpha_cutoff = f32(std::clamp(doc.number_at("opacity_mask_clip_value", 0.3333), 0.0, 1.0));
        p.double_sided = doc.bool_at("two_sided", false);
        p.unlit = unlit;
        if (!unlit && model != "MSM_DEFAULTLIT" && model != "MSM_DEFAULT_LIT")
            x.note("shading model " + model + ": lit as kin's default");

        x.finish();
        result.value = std::move(imported);
    } catch (const std::exception& error) {
        result.diagnostics.push_back(source.string() + ": " + error.what());
    }
    return result;
}

} // namespace kin

// --- Tier 2: Unreal's material graphs as kin's ---

namespace kin {
namespace {

using material_import::GraphBuilder;

// The export's graph read into a kin graph: each expression and output pin as a kin reference (JSON text), made once.
class UnrealGraph {
public:
    UnrealGraph(const JsonValue& doc, GraphBuilder& b, material_import::Context& x)
        : _doc(doc), _b(b), _x(x), _nodes(doc.find("graph") ? doc.find("graph")->find("nodes") : nullptr) {}

    // The textures the graph names (each a parameter of its own): the export's file, and whether it is a normal map.
    struct Texture {
        std::string param, file;
        bool normal = false, srgb = true;
    };
    std::vector<Texture> textures;

    // `node`'s value at `pin`, or none, with why.
    std::optional<std::string> value(const std::string& node, const std::string& pin, std::string& why) {
        const auto key = node + "|" + pin;
        if (const auto it = _done.find(key); it != _done.end())
            return it->second;
        if (_visiting.contains(node)) {
            why = node + " feeds itself";
            return std::nullopt;
        }
        _visiting.insert(node);
        auto v = translate(node, why);
        _visiting.erase(node);
        if (v)
            v = swizzle(*v, pin);
        if (v)
            _done[key] = *v;
        return v;
    }

private:
    std::string swizzle(const std::string& ref, std::string pin) {
        for (auto& c : pin)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        static const std::set<std::string> pins{"r", "g", "b", "a", "rg", "rgb", "rgba",
                                                "x", "y", "z", "w", "xy", "xyz", "xyzw"};
        if (pin.empty() || !pins.contains(pin) || ref.front() != '"')
            return ref;
        for (auto& c : pin)
            c = c == 'r' ? 'x' : c == 'g' ? 'y' : c == 'b' ? 'z' : c == 'a' ? 'w' : c;
        return _b.member(ref, pin);
    }
    const JsonValue* node(const std::string& name) const { return _nodes ? _nodes->find(name) : nullptr; }
    static f32 prop(const JsonValue& n, const char* key, f32 fallback) {
        const auto* p = n.find("props");
        const auto* v = p ? p->find(key) : nullptr;
        return v && v->is_number() ? f32(v->as_number()) : v && v->is_bool() ? (v->as_bool() ? 1.0f : 0.0f) : fallback;
    }
    static std::string text(const JsonValue& n, const char* key) {
        const auto* p = n.find("props");
        const auto* v = p ? p->find(key) : nullptr;
        return v && v->is_string() ? v->as_string() : "";
    }
    static std::array<f32, 4> colour(const JsonValue& n, const char* key) {
        std::array<f32, 4> c{0, 0, 0, 1};
        const auto* p = n.find("props");
        const auto* v = p ? p->find(key) : nullptr;
        if (v && v->is_array())
            for (size_t i = 0; i < std::min<size_t>(4, v->items().size()); ++i)
                c[i] = f32(v->items()[i].as_number());
        return c;
    }
    // An input: the expression connected to it (by its name; a lone input by any name), else `fallback` (JSON text).
    std::optional<std::string> input(const JsonValue& n, std::initializer_list<const char*> names,
                                     std::optional<std::string> fallback, std::string& why) {
        const auto* inputs = n.find("inputs");
        const JsonValue* in = nullptr;
        if (inputs && inputs->is_object()) {
            for (const char* name : names)
                if ((in = inputs->find(name)))
                    break;
            // (Unreal names no single input of some nodes: One Minus's, Clamp's first: "None" in the export.)
            if (!in)
                for (const char* unnamed : {"None", ""})
                    if (std::any_of(names.begin(), names.end(),
                                    [](const char* x) { return !*x || !std::strcmp(x, "None"); }))
                        if ((in = inputs->find(unnamed)))
                            break;
        }
        if (!in) {
            if (!fallback)
                why = "an input not connected";
            return fallback;
        }
        const std::string from = in->find("node") ? in->find("node")->as_string() : "";
        const std::string pin = in->find("output") ? in->find("output")->as_string() : "";
        return value(from, pin, why);
    }
    std::string param(const std::string& name, const char* type, std::array<f32, 4> value) {
        if (!_b.has_param(name))
            _b.params.push_back({name, type, value});
        return GraphBuilder::quote(name);
    }

    std::optional<std::string> translate(const std::string& name, std::string& why) {
        const JsonValue* found = node(name);
        if (!found) {
            why = "no expression " + name;
            return std::nullopt;
        }
        const JsonValue& n = *found;
        const std::string cls = n.find("class") ? n.find("class")->as_string() : "";
        const std::string kind = cls.rfind("MaterialExpression", 0) == 0 ? cls.substr(18) : cls;
        const auto num = GraphBuilder::number;
        const auto in = [&](std::initializer_list<const char*> names, std::optional<std::string> fallback = {}) {
            return input(n, names, std::move(fallback), why);
        };
        const auto binary = [&](const char* type, const char* a, const char* b, f32 ka,
                                f32 kb) -> std::optional<std::string> {
            const auto x = in({a}, num(prop(n, "const_a", ka))), y = in({b}, num(prop(n, "const_b", kb)));
            if (!x || !y)
                return std::nullopt;
            return _b.add(type, {{"a", *x}, {"b", *y}});
        };
        const auto unary = [&](const char* type) -> std::optional<std::string> {
            const auto x = in({"", "None", "Input"});
            return x ? std::optional{_b.add(type, {{"x", *x}})} : std::nullopt;
        };
        if (kind == "Constant")
            return num(prop(n, "r", 0));
        if (kind == "Constant2Vector")
            return "[" + num(prop(n, "r", 0)) + ", " + num(prop(n, "g", 0)) + "]";
        if (kind == "Constant3Vector" || kind == "Constant4Vector") {
            const auto c = colour(n, "constant");
            return GraphBuilder::vector({c.data(), kind == "Constant3Vector" ? 3u : 4u});
        }
        if (kind == "ScalarParameter")
            return param(text(n, "parameter_name"), "float", {prop(n, "default_value", 0), 0, 0, 0});
        if (kind == "VectorParameter")
            return param(text(n, "parameter_name"), "color", colour(n, "default_value"));
        if (kind == "TextureSample" || kind == "TextureSampleParameter2D") {
            std::string file = text(n, "texture");
            std::string pname = text(n, "parameter_name");
            if (const auto* p = _doc.find("parameters"); p && !pname.empty())
                if (const auto* t = p->find("texture"); t && t->find(pname) && t->find(pname)->is_string())
                    file = t->find(pname)->as_string();
            if (file.empty()) {
                why = name + ": a texture sample of no texture";
                return std::nullopt;
            }
            if (pname.empty())
                pname = "t_" + std::filesystem::path(file).stem().string();
            for (auto& c : pname)
                if (!std::isalnum(static_cast<unsigned char>(c)))
                    c = '_';
            const bool normal = text(n, "sampler_type").find("NORMAL") != std::string::npos;
            const auto* settings = _doc.find("textures") ? _doc.find("textures")->find(file) : nullptr;
            if (std::none_of(textures.begin(), textures.end(), [&](const Texture& t) { return t.param == pname; }))
                textures.push_back({pname, file, normal, !normal && settings && settings->bool_at("srgb", true)});
            if (!_b.has_param(pname))
                _b.params.push_back({pname, "texture", {}, "", false}); // its slot is chosen when all are known
            const auto uv = in({"UVs", "Coordinates"}, GraphBuilder::quote("uv"));
            if (!uv)
                return std::nullopt;
            const std::string sample = _b.add("texture", {{"uv", *uv}}, {{"texture", GraphBuilder::quote(pname)}});
            if (!normal)
                return sample;
            // Unreal unpacks a normal map's texel; in its tangent space (DirectX's: green down): kin's source turned so
            // where the texture flips it on import.
            const std::string unpacked = _b.add("unpack_normal", {{"texel", sample}});
            const bool flipped = settings && settings->bool_at("flip_green_channel", false);
            return flipped ? _b.add("multiply", {{"a", unpacked}, {"b", "[1, -1, 1]"}}) : unpacked;
        }
        if (kind == "TextureCoordinate") {
            if (prop(n, "coordinate_index", 0) != 0) {
                why = name + ": a second UV set (kin's meshes have one)";
                return std::nullopt;
            }
            const f32 u = prop(n, "u_tiling", 1), v = prop(n, "v_tiling", 1);
            return u == 1 && v == 1 ? GraphBuilder::quote("uv")
                                    : _b.add("multiply", {{"a", GraphBuilder::quote("uv")},
                                                          {"b", "[" + num(u) + ", " + num(v) + "]"}});
        }
        if (kind == "Multiply")
            return binary("multiply", "A", "B", 0, 1);
        if (kind == "Add")
            return binary("add", "A", "B", 0, 1);
        if (kind == "Subtract")
            return binary("subtract", "A", "B", 1, 1);
        if (kind == "Divide")
            return binary("divide", "A", "B", 1, 1);
        if (kind == "Min")
            return binary("min", "A", "B", 0, 1);
        if (kind == "Max")
            return binary("max", "A", "B", 0, 1);
        if (kind == "Dot" || kind == "Cross" || kind == "Distance") {
            const auto a = in({"A"}), b = in({"B"});
            if (!a || !b)
                return std::nullopt;
            return _b.add(kind == "Dot" ? "dot" : kind == "Cross" ? "cross" : "distance", {{"a", *a}, {"b", *b}});
        }
        if (kind == "Power") {
            const auto base = in({"Base"}), e = in({"Exponent", "Exp"}, num(prop(n, "const_exponent", 2)));
            if (!base || !e)
                return std::nullopt;
            return _b.add("power", {{"a", *base}, {"b", *e}});
        }
        if (kind == "Clamp") {
            const auto x = in({"", "None", "Input"}), lo = in({"Min"}, num(prop(n, "min_default", 0))),
                       hi = in({"Max"}, num(prop(n, "max_default", 1)));
            if (!x || !lo || !hi)
                return std::nullopt;
            return _b.add("clamp", {{"x", *x}, {"min", *lo}, {"max", *hi}});
        }
        if (kind == "SmoothStep") { // UE 5's: 0 to 1 smoothly from Min to Max
            const auto lo = in({"Min"}, num(prop(n, "const_min", 0))), hi = in({"Max"}, num(prop(n, "const_max", 1))),
                       x = in({"Value"}, num(prop(n, "const_value", 0)));
            if (!lo || !hi || !x)
                return std::nullopt;
            return _b.add("smoothstep", {{"edge0", *lo}, {"edge1", *hi}, {"x", *x}});
        }
        if (kind == "Step") { // UE 5's: 1 where X >= Y
            const auto y = in({"Y"}, num(prop(n, "const_y", 0))), x = in({"X"}, num(prop(n, "const_x", 0)));
            if (!y || !x)
                return std::nullopt;
            return _b.add("step", {{"edge", *y}, {"x", *x}});
        }
        if (kind == "LinearInterpolate") {
            const auto a = in({"A"}, num(prop(n, "const_a", 0))), b = in({"B"}, num(prop(n, "const_b", 1))),
                       t = in({"Alpha"}, num(prop(n, "const_alpha", 0.5)));
            if (!a || !b || !t)
                return std::nullopt;
            return _b.add("mix", {{"a", *a}, {"b", *b}, {"t", *t}});
        }
        static const std::map<std::string, const char*> unaries{
            {"OneMinus", "one_minus"}, {"Abs", "abs"},         {"Floor", "floor"},       {"Ceil", "ceil"},
            {"Frac", "fract"},         {"SquareRoot", "sqrt"}, {"Saturate", "saturate"}, {"Normalize", "normalize"}};
        if (const auto u = unaries.find(kind); u != unaries.end())
            return unary(u->second);
        if (kind == "Sine" || kind == "Cosine") { // sin(x * 2 pi / period), or of x with no period
            const auto x = in({"", "None", "Input"});
            if (!x)
                return std::nullopt;
            const f32 period = prop(n, "period", 1);
            const std::string arg = period > 0 ? _b.add("multiply", {{"a", *x}, {"b", num(6.283185307 / period)}}) : *x;
            return _b.add(kind == "Sine" ? "sin" : "cos", {{"x", arg}});
        }
        if (kind == "ComponentMask") {
            const auto x = in({"", "None", "Input"});
            if (!x)
                return std::nullopt;
            std::string m;
            for (const auto& [c, k] : {std::pair{"r", 'x'}, {"g", 'y'}, {"b", 'z'}, {"a", 'w'}})
                if (prop(n, c, 0))
                    m += k;
            if (m.empty() || x->front() != '"') {
                why = name + ": a mask of nothing";
                return std::nullopt;
            }
            return _b.member(*x, m);
        }
        if (kind == "AppendVector") {
            const auto a = in({"A"}), b = in({"B"});
            if (!a || !b)
                return std::nullopt;
            return _b.add("append", {{"a", *a}, {"b", *b}});
        }
        if (kind == "Time") {
            const f32 period = prop(n, "period", 0);
            return period > 0 ? _b.add("mod", {{"a", GraphBuilder::quote("time")}, {"b", num(period)}})
                              : GraphBuilder::quote("time");
        }
        if (kind == "WorldPosition") {
            _x.note("World Position: Unreal's centimetres, kin's metres x 100 (the patterns keep their scale)");
            return _b.add("multiply", {{"a", GraphBuilder::quote("position")}, {"b", "100"}});
        }
        if (kind == "VertexColor")
            return GraphBuilder::quote("vertex_color");
        if (kind == "CameraVectorWS")
            return GraphBuilder::quote("view");
        if (kind == "PixelNormalWS" || kind == "VertexNormalWS")
            return GraphBuilder::quote(kind == "PixelNormalWS" ? "surface.normal" : "normal");
        if (kind == "Fresnel") { // base + (1 - base) x (1 - n.v)^exponent
            const auto e = in({"ExponentIn"}, num(prop(n, "exponent", 5)));
            const auto base = in({"BaseReflectFractionIn"}, num(prop(n, "base_reflect_fraction", 0.04f)));
            if (!e || !base)
                return std::nullopt;
            const std::string f = _b.add("fresnel", {{"power", *e}});
            return _b.add("mix", {{"a", *base}, {"b", "1"}, {"t", f}});
        }
        if (kind == "Panner") { // the coordinate plus the speed times the time
            const auto uv = in({"Coordinate"}, GraphBuilder::quote("uv"));
            const auto t = in({"Time"}, GraphBuilder::quote("time"));
            const auto speed =
                in({"Speed"}, "[" + num(prop(n, "speed_x", 0)) + ", " + num(prop(n, "speed_y", 0)) + "]");
            if (!uv || !t || !speed)
                return std::nullopt;
            return _b.add("add", {{"a", *uv}, {"b", _b.add("multiply", {{"a", *speed}, {"b", *t}})}});
        }
        if (kind == "Desaturation") {
            const auto x = in({"", "None", "Input"}), f = in({"Fraction"}, num(prop(n, "const_fraction", 1)));
            if (!x || !f)
                return std::nullopt;
            const auto l = colour(n, "luminance_factors");
            const std::string lum =
                (l[0] + l[1] + l[2]) > 0 ? GraphBuilder::vector({l.data(), 3}) : "[0.3, 0.59, 0.11]";
            const std::string grey = _b.add("dot", {{"a", *x}, {"b", lum}});
            return _b.add("mix", {{"a", *x}, {"b", grey}, {"t", *f}});
        }
        if (kind == "Reroute" || kind == "NamedRerouteDeclaration" || kind == "NamedRerouteUsage")
            return in({"", "None", "Input"});
        if (kind == "StaticSwitchParameter" || kind == "StaticSwitch") {
            const bool on = prop(n, "default_value", 0) != 0;
            _x.note(name + ": a static switch, its default (" + std::string(on ? "true" : "false") + ") taken");
            return in({on ? "True" : "False", on ? "A" : "B"});
        }
        why = name + ": a " + kind + " (kin has no node for it)";
        return std::nullopt;
    }

    const JsonValue& _doc;
    GraphBuilder& _b;
    material_import::Context& _x;
    const JsonValue* _nodes;
    std::map<std::string, std::string> _done;
    std::set<std::string> _visiting;
};

} // namespace

Result3D<ImportedMaterial3D> import_unreal_graph(const std::filesystem::path& source,
                                                 const MaterialImportOptions3D& options) {
    // The Tier 1 import first: what the graph can't express keeps its values.
    auto result = import_unreal_material(source, options);
    if (!result)
        return result;
    try {
        const auto parsed = parse_json(read_file(source));
        const JsonValue& doc = *parsed.value;
        ImportedMaterial3D& imported = *result.value;
        material_import::Context x(imported, options, source.stem().string());
        if (!doc.find("graph")) {
            x.note("the export has no graph (an older unreal_export.py): its Tier 1 import only");
            return result;
        }
        GraphBuilder b;
        const std::string base = doc.find("base") ? doc.find("base")->as_string() : source.stem().string();
        b.name = std::filesystem::path(base).filename().string();
        UnrealGraph g(doc, b, x);
        const std::string model = doc.find("shading_model") ? doc.find("shading_model")->as_string() : "";
        const std::string blend = doc.find("blend_mode") ? doc.find("blend_mode")->as_string() : "";
        const bool unlit = model == "MSM_UNLIT";
        const auto* outputs = doc.find("graph")->find("outputs");
        static const std::vector<std::pair<const char*, const char*>> mapping{
            {"BaseColor", "base_color"},   {"Metallic", "metallic"},          {"Roughness", "roughness"},
            {"EmissiveColor", "emission"}, {"AmbientOcclusion", "occlusion"}, {"Normal", "normal"},
            {"Opacity", "alpha"},          {"OpacityMask", "alpha"}};
        for (const auto& [from, to] : mapping) {
            const auto* o = outputs ? outputs->find(from) : nullptr;
            if (!o)
                continue;
            if ((std::string(from) == "Opacity" && blend == "BLEND_MASKED") ||
                (std::string(from) == "OpacityMask" && blend != "BLEND_MASKED"))
                continue;
            std::string why;
            // (The graph's word on this output replaces the Tier 1 import's.)
            std::erase_if(imported.notes,
                          [&](const std::string& n) { return n.rfind(std::string(from) + ": ", 0) == 0; });
            auto v =
                g.value(o->find("node")->as_string(), o->find("output") ? o->find("output")->as_string() : "", why);
            if (!v) {
                x.note(std::string(from) + ": " + why + ": its Tier 1 value kept");
                continue;
            }
            std::string target = unlit && std::string(from) == "EmissiveColor" ? "base_color" : to;
            if (target == "normal") // Unreal's tangent space (green down) as kin's (+y up), then the world's
                v = b.add("tangent_to_world", {{"normal", b.add("multiply", {{"a", *v}, {"b", "[1, -1, 1]"}})}});
            b.outputs[target] = *v;
        }
        for (const char* other : {"WorldPositionOffset", "Refraction", "Anisotropy", "Specular"})
            if (outputs && outputs->find(other))
                x.note(std::string(other) + ": not in kin's graph outputs");
        // The textures in kin's five slots (as graph data: the graph reads them; kin's own reading of a slot undone).
        const auto texture = [&](const GraphBuilder::Param& p) {
            return *std::ranges::find(g.textures, p.name, &UnrealGraph::Texture::param);
        };
        if (!b.assign_slots([&](const GraphBuilder::Param& p) { return texture(p).normal; })) {
            x.note("its graph samples more than five textures (kin's material has five map slots): its Tier 1 import "
                   "only");
            x.finish();
            return result;
        }
        for (auto& p : b.params)
            if (p.type == "texture")
                p.srgb = texture(p).srgb;
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
        imported.graph = x.out / (b.name + ".kingraph");
        if (options.write) {
            std::filesystem::create_directories(x.out);
            std::ofstream file(imported.graph, std::ios::binary);
            file << text;
        }
        auto& p = imported.asset.base.material;
        p.graph = imported.graph.lexically_relative(x.content);
        // The graph reads the mesh's UV as its own nodes tile it: Tier 1's tiling (the material's UV transform, which
        // kin's "uv" input goes through) would tile it twice.
        p.uv_scale = Vec2f{1, 1};
        p.uv_offset = Vec2f{0, 0};
        // The material's values of the graph's parameters (an instance's own), and its textures.
        const auto* values = doc.find("parameters");
        for (const auto& param : b.params) {
            MaterialGraphValue3D v;
            if (param.type == "texture") {
                const auto t = std::ranges::find(g.textures, param.name, &UnrealGraph::Texture::param);
                const auto path = x.readable(x.options.project / t->file, param.name);
                if (!path)
                    continue;
                v.texture = MaterialTexture3D{.asset = *path};
            } else {
                const auto* group = values ? values->find(param.type == "float" ? "scalar" : "vector") : nullptr;
                const auto* given = group ? group->find(param.name) : nullptr;
                if (!given)
                    continue;
                v.count = param.type == "float" ? 1 : 4;
                if (given->is_number())
                    v.value[0] = f32(given->as_number());
                else
                    for (size_t i = 0; i < std::min<size_t>(4, given->items().size()); ++i)
                        v.value[i] = f32(given->items()[i].as_number());
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
