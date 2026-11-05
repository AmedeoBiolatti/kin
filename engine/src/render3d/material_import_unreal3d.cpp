// Unreal's materials as kin's, from what tools/import_material/unreal_export.py writes inside Unreal's editor
// (kin.unreal-material/1: blend mode, shading model, parameters, and the expressions feeding each output).
#include "material_import3d_impl.hpp"

#include <array>
#include <cstring>
#include <kin/core/json_value.hpp>
#include <map>
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
