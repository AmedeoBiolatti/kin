// Unreal's materials as kin's, from what tools/import_material/unreal_export.py writes inside Unreal's editor
// (kin.unreal-material/1: blend mode, shading model, parameters, and the expressions feeding each output).
#include "material_import3d_impl.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <kin/core/json_value.hpp>
#include <kin/render3d/material_graph3d.hpp>
#include <map>
#include <memory>
#include <numbers>
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
    std::string approx;  // read, but approximately: how
    bool constant() const { return why.empty() && texture.empty(); }
    Term& approximately(const std::string& how) {
        if (approx.find(how) == std::string::npos)
            approx += (approx.empty() ? "" : "; ") + how;
        return *this;
    }
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
                if (uv_class != "MaterialExpressionTextureCoordinate" || uv->int_at("index", 0) != 0)
                    t.approximately("a map at UVs made by a " + uv_class.substr(std::min<size_t>(18, uv_class.size())) +
                                    ": read at the mesh's");
                else
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
            if (!a.why.empty() && !b.why.empty())
                return a;
            if (!a.why.empty() || !b.why.empty()) { // the side kin reads, alone
                Term one = a.why.empty() ? a : b;
                return one.approximately("a " + kind + " by " + (a.why.empty() ? b.why : a.why) + ": left out");
            }
            if (!a.texture.empty() && !b.texture.empty()) {
                Term first = a;
                return first.approximately("a " + kind + " of two maps: the first's");
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
            const Term a = connected(0) ? term(input(0), depth + 1) : constant(f32(n->number_at("const_a", 0)));
            const Term b = connected(1) ? term(input(1), depth + 1) : constant(f32(n->number_at("const_b", 1)));
            // A constant alpha, a map on one side at most: (1 - alpha) A + alpha B, exactly.
            if (alpha.constant() && a.why.empty() && b.why.empty() && (a.texture.empty() || b.texture.empty())) {
                const f32 k = alpha.bias[0];
                Term out = a.texture.empty() ? b : a;
                for (int i = 0; i < 4; ++i) {
                    out.bias[i] = (1 - k) * a.bias[i] + k * b.bias[i];
                    out.gain[i] = a.texture.empty() ? k * b.gain[i] : (1 - k) * a.gain[i];
                }
                if (out.texture.empty())
                    out.gain = {0, 0, 0, 0};
                out.approx = a.approx + (a.approx.empty() || b.approx.empty() ? "" : "; ") + b.approx;
                return out;
            }
            // A varying alpha: the side with a map (A's first), else A.
            const bool take_b = !(a.why.empty() && !a.texture.empty()) && b.why.empty() && !b.texture.empty();
            Term side = take_b ? b : a;
            if (!side.why.empty())
                return side;
            return side.approximately("a Lerp by " + (alpha.why.empty() ? std::string("a varying alpha") : alpha.why) +
                                      ": its " + (take_b ? "B" : "A"));
        }
        if (kind == "CollectionParameter") {
            if (const auto* v = n->find("value")) { // its default (a game may set it while it runs)
                Term c = constant(0);
                if (v->is_number())
                    c = constant(f32(v->as_number()));
                else
                    for (size_t i = 0; i < std::min<size_t>(4, v->items().size()); ++i)
                        c.bias[i] = f32(v->items()[i].as_number());
                return pick(c).approximately("the parameter collection's " + param + ": its default");
            }
            t.why = "the parameter collection's " + param + " (set while the game runs)";
            return t;
        }
        // Another expression: its first input as it is (a guess, said), where it has one.
        for (size_t i = 0; i < 4; ++i)
            if (connected(i)) {
                Term through = term(input(i), depth + 1);
                if (through.why.empty())
                    return through.approximately("a " + kind + ": its first input as it is");
                break;
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
                else if (!terms[key].approx.empty())
                    x.note(std::string(key) + ": read approximately (" + terms[key].approx + ")");
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
    // A material function's graph, inlined where `call` (a MaterialFunctionCall of `caller`'s) calls it.
    UnrealGraph(UnrealGraph& caller, const JsonValue& call, const JsonValue& function)
        : _doc(caller._doc), _b(caller._b), _x(caller._x), _nodes(function.find("nodes")), _root(&caller.root()),
          _caller(&caller), _call(&call), _depth(caller._depth + 1) {}

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
        if (_visiting.contains(key)) {
            why = node + " feeds itself";
            return std::nullopt;
        }
        _visiting.insert(key);
        const JsonValue* n = this->node(node);
        const bool call =
            n && n->find("class") && n->find("class")->as_string() == "MaterialExpressionMaterialFunctionCall";
        auto v = call ? function_call(node, *n, pin, why) : translate(node, why);
        _visiting.erase(key);
        if (v && !call)
            v = swizzle(*v, pin);
        if (v)
            _done[key] = *v;
        return v;
    }

private:
    UnrealGraph& root() { return _root ? *_root : *this; }
    // What feeds `n`'s input `name` (its JSON), or null.
    static const JsonValue* connected(const JsonValue& n, const std::string& name) {
        const auto* inputs = n.find("inputs");
        return inputs && inputs->is_object() ? inputs->find(name) : nullptr;
    }
    std::optional<std::string> source_value(const JsonValue& in, std::string& why) {
        return value(in.find("node") ? in.find("node")->as_string() : "", pin_of(in), why);
    }
    // The output an input reads: its name, but a function call's by its index (the editor's name for such a pin is
    // not always the output's): the call's outputs in order (function_outputs, exported), else the function's
    // outputs by sort priority where those differ, else the name as exported.
    std::string pin_of_input(const JsonValue& in) const {
        const std::string name = in.find("output") ? in.find("output")->as_string() : "";
        const auto* index = in.find("output_index");
        const JsonValue* call = node(in.find("node") ? in.find("node")->as_string() : "");
        if (!index || !index->is_number() || !call || !call->find("class") ||
            call->find("class")->as_string() != "MaterialExpressionMaterialFunctionCall")
            return name;
        const size_t k = size_t(index->as_int(0));
        if (const auto* listed = call->find("props") ? call->find("props")->find("function_outputs") : nullptr;
            listed && listed->is_array())
            return k < listed->items().size() ? listed->items()[k].as_string(name) : name;
        const auto* functions = _doc.find("functions");
        const JsonValue* function = functions ? functions->find(text(*call, "material_function")) : nullptr;
        const auto* outputs = function ? function->find("outputs") : nullptr;
        const auto* nodes = function ? function->find("nodes") : nullptr;
        if (!outputs || !outputs->is_object() || !nodes)
            return name;
        std::vector<std::pair<f64, std::string>> order;
        for (const auto& [output, at] : outputs->members()) {
            const auto* n = nodes->find(at.as_string());
            const auto* props = n ? n->find("props") : nullptr;
            order.emplace_back(props ? props->number_at("sort_priority", 0) : 0, output);
        }
        std::ranges::stable_sort(order, {}, &std::pair<f64, std::string>::first);
        for (size_t i = 1; i < order.size(); ++i)
            if (order[i].first == order[i - 1].first)
                return name; // (tied: the order is not known)
        return k < order.size() ? order[k].second : name;
    }
    // A material function's call: the function's graph inlined (one per call: its inputs are the call's), the value of
    // its output `pin` (its first where the pin is unnamed).
    std::optional<std::string> function_call(const std::string& name, const JsonValue& n, const std::string& pin,
                                             std::string& why) {
        const std::string path = text(n, "material_function");
        const auto* functions = _doc.find("functions");
        const JsonValue* function = functions && !path.empty() ? functions->find(path) : nullptr;
        if (!function || !function->is_object() || function->find("error")) {
            why = name + ": its material function " + (path.empty() ? std::string("(not exported)") : path) +
                  " is not in the export";
            return std::nullopt;
        }
        if (_depth >= 16) {
            why = name + ": material functions 16 deep";
            return std::nullopt;
        }
        // Parallax occlusion mapping (Unreal's function, a Custom HLSL loop kin cannot read): the UVs as they come, no
        // depth offset, unshadowed.
        if (std::filesystem::path(path).filename().string().rfind("ParallaxOcclusionMapping", 0) == 0) {
            _x.note(name + ": Parallax Occlusion Mapping: its UVs as they come (no parallax), no depth offset, no "
                           "self-shadow");
            if (pin.find("Depth") != std::string::npos || pin.find("Offset") != std::string::npos)
                return std::string(
                    pin.find("Offset") != std::string::npos && pin.find("UV") != std::string::npos ? "[0, 0]" : "0");
            if (pin.find("Shadow") != std::string::npos)
                return std::string("1");
            if (const auto* uv = connected(n, "UVs"))
                return source_value(*uv, why);
            return GraphBuilder::quote("uv");
        }
        auto& child = _children[name];
        if (!child)
            child = std::make_unique<UnrealGraph>(*this, n, *function);
        const auto* outputs = function->find("outputs");
        const JsonValue* out = nullptr;
        if (outputs && outputs->is_object()) {
            out = pin.empty() ? nullptr : outputs->find(pin);
            if (!out && !outputs->members().empty())
                out = &outputs->members().begin()->second;
        }
        if (!out || !out->is_string()) {
            why = name + ": " + path + " has no output " + pin;
            return std::nullopt;
        }
        std::string inner;
        auto v = child->value(out->as_string(), "", inner);
        if (!v)
            why = std::filesystem::path(path).filename().string() + " > " + inner;
        return v;
    }
    // A static boolean's value at import (a StaticBool, a StaticBoolParameter by the material's values, a function's
    // static input by its call), or none.
    std::optional<bool> static_bool(const JsonValue& in) {
        return static_bool_of(in.find("node") ? in.find("node")->as_string() : "");
    }
    std::optional<bool> static_bool_of(const std::string& name, int hops = 0) {
        const JsonValue* n = node(name);
        if (!n || hops > 64)
            return std::nullopt;
        const std::string cls = n->find("class") ? n->find("class")->as_string() : "";
        if (cls == "MaterialExpressionNamedRerouteUsage")
            return static_bool_of(text(*n, "declaration"), hops + 1);
        if (cls == "MaterialExpressionReroute" || cls == "MaterialExpressionNamedRerouteDeclaration") {
            const auto* inputs = n->find("inputs");
            if (!inputs || !inputs->is_object() || inputs->members().empty())
                return std::nullopt;
            const auto& first = inputs->members().begin()->second;
            return static_bool_of(first.find("node") ? first.find("node")->as_string() : "", hops + 1);
        }
        if (cls == "MaterialExpressionStaticBool")
            return prop(*n, "value", 0) != 0;
        if (cls == "MaterialExpressionStaticBoolParameter" || cls == "MaterialExpressionStaticSwitchParameter")
            return switch_value(*n);
        if (cls == "MaterialExpressionFunctionInput" && _caller) {
            if (const auto* given = connected(*_call, text(*n, "input_name")))
                return _caller->static_bool(*given);
            if (const auto* preview = connected(*n, "Preview"))
                return static_bool(*preview);
            const auto* p = n->find("props") ? n->find("props")->find("preview_value") : nullptr;
            return p && p->is_array() && !p->items().empty() && p->items()[0].as_number() != 0;
        }
        return std::nullopt;
    }
    // A static switch parameter's value: the material's (an instance's own), else its default.
    bool switch_value(const JsonValue& n) {
        const auto* params = _doc.find("parameters");
        const auto* switches = params ? params->find("switch") : nullptr;
        const std::string name = text(n, "parameter_name");
        const auto* given = switches ? switches->find(name) : nullptr;
        const bool fallback = prop(n, "default_value", 0) != 0;
        const bool on = given && given->is_bool() ? given->as_bool() : fallback;
        if (on != fallback)
            root().switched[name] = on;
        return on;
    }

public:
    // The output an input (or one of the material's outputs) reads: pin_of_input's.
    std::string pin_of(const JsonValue& in) const { return pin_of_input(in); }
    // The static switches set otherwise than their defaults (they make another graph: its file is named by them).
    std::map<std::string, bool> switched;
    // The parameters as the material's values, in the graph (a graph of the material's own): where they don't fit.
    bool fold = false;

private:
    // A GLSL expression of its own (kin's glsl node), `width` floats.
    std::string glsl(const std::string& expr, int width, std::vector<std::pair<std::string, std::string>> inputs) {
        static const char* types[] = {"float", "float", "vec2", "vec3", "vec4"};
        return _b.add(
            "glsl", std::move(inputs),
            {{"glsl", GraphBuilder::quote(expr)}, {"result", GraphBuilder::quote(types[std::clamp(width, 1, 4)])}});
    }
    // A world vector of kin's (x, y, z) in Unreal's axes: (x, z, y), Z up (its glTF exporter's mapping, its own
    // inverse).
    std::string unreal_axes(const std::string& ref) { return _b.member(ref, "xzy"); }
    std::string swizzle(const std::string& ref, std::string pin) {
        for (auto& c : pin)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        static const std::set<std::string> pins{"r", "g", "b", "a", "rg", "rgb", "rgba",
                                                "x", "y", "z", "w", "xy", "xyz", "xyzw"};
        if (pin.empty() || !pins.contains(pin))
            return ref;
        for (auto& c : pin)
            c = c == 'r' ? 'x' : c == 'g' ? 'y' : c == 'b' ? 'z' : c == 'a' ? 'w' : c;
        return pick(ref, pin);
    }
    // Components `m` (of xyzw) of a value: a reference's swizzle; a constant's own components (a number's, each one).
    std::string pick(const std::string& ref, const std::string& m) {
        if (ref.empty())
            return ref;
        if (ref.front() == '"')
            return _b.member(ref, m);
        const int w = _b.width(ref);
        std::string out = m.size() == 1 ? "" : "[";
        for (size_t i = 0; i < m.size(); ++i) {
            const int k = int(std::string("xyzw").find(m[i]));
            out += (i ? ", " : "") + (w == 1 ? ref : GraphBuilder::component(ref, std::min(k, w - 1)));
        }
        return m.size() == 1 ? out : out + "]";
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
        return value(from, pin_of(*in), why);
    }
    std::string param(const std::string& name, const char* type, std::array<f32, 4> value) {
        if (root().fold) { // the material's value (its instance's, else the default) in the graph
            const auto* values = _doc.find("parameters");
            const auto* group = values ? values->find(std::string(type) == "float" ? "scalar" : "vector") : nullptr;
            if (const auto* given = group ? group->find(name) : nullptr) {
                if (given->is_number())
                    value[0] = f32(given->as_number());
                else
                    for (size_t i = 0; i < std::min<size_t>(4, given->items().size()); ++i)
                        value[i] = f32(given->items()[i].as_number());
            }
            return std::string(type) == "float" ? GraphBuilder::number(value[0])
                                                : GraphBuilder::vector({value.data(), 4});
        }
        const auto found = std::ranges::find(_b.params, name, &GraphBuilder::Param::source);
        if (found != _b.params.end())
            return GraphBuilder::quote(found->name);
        const std::string safe = _b.safe_name(name);
        _b.params.push_back({safe, type, value, "", false, name});
        return GraphBuilder::quote(safe);
    }

    std::optional<std::string> translate(const std::string& name, std::string& why) {
        const JsonValue* found = node(name);
        if (!found) {
            why = "no expression " + name;
            return std::nullopt;
        }
        const JsonValue& n = *found;
        const std::string cls = n.find("class") ? n.find("class")->as_string() : "";
        std::string kind = cls.rfind("MaterialExpression", 0) == 0 ? cls.substr(18) : cls;
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
            const auto x = in({"", "None", "Input", "VectorInput"}); // (Normalize's is VectorInput)
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
            if (const auto* tex = connected(n, "Tex")) { // a texture object's (a function's texture input)
                const auto t = source_value(*tex, why);
                if (!t)
                    return std::nullopt;
                if (t->size() < 4 || (*t)[1] != '@') {
                    why = name + ": its Tex is not a texture object";
                    return std::nullopt;
                }
                const std::string pname = t->substr(2, t->size() - 3);
                auto& list = root().textures;
                const auto entry = std::ranges::find(list, pname, &Texture::param);
                const bool normal = text(n, "sampler_type").find("NORMAL") != std::string::npos;
                const auto* settings =
                    _doc.find("textures") && entry != list.end() ? _doc.find("textures")->find(entry->file) : nullptr;
                if (entry != list.end() && normal) {
                    entry->normal = true;
                    entry->srgb = false;
                }
                // (Its file now: the UVs' nodes may add textures to the list, which may move.)
                const std::string entry_file = entry != list.end() ? entry->file : std::string();
                const auto uv = in({"UVs", "Coordinates"}, GraphBuilder::quote("uv"));
                if (!uv)
                    return std::nullopt;
                const std::string sample =
                    ranged(_b.add("texture", {{"uv", *uv}}, {{"texture", GraphBuilder::quote(pname)}}), entry_file);
                if (!normal)
                    return sample;
                const std::string unpacked = _b.add("unpack_normal", {{"texel", sample}});
                const bool flipped = settings && settings->bool_at("flip_green_channel", false);
                return flipped ? _b.add("multiply", {{"a", unpacked}, {"b", "[1, -1, 1]"}}) : unpacked;
            }
        }
        if (kind == "TextureObject" || kind == "TextureObjectParameter") {
            // A texture as a value (to a sample's Tex, through functions' texture inputs): "@<parameter>".
            std::string file = text(n, "texture");
            std::string pname = text(n, "parameter_name");
            if (const auto* p = _doc.find("parameters"); p && !pname.empty())
                if (const auto* t = p->find("texture"); t && t->find(pname) && t->find(pname)->is_string())
                    file = t->find(pname)->as_string();
            if (file.empty()) {
                why = name + ": a texture object of no texture";
                return std::nullopt;
            }
            if (pname.empty())
                pname = "t_" + std::filesystem::path(file).stem().string();
            const std::string source = pname;
            if (const auto known = std::ranges::find(_b.params, source, &GraphBuilder::Param::source);
                known != _b.params.end())
                pname = known->name;
            else
                pname = _b.safe_name(source);
            const auto* settings = _doc.find("textures") ? _doc.find("textures")->find(file) : nullptr;
            auto& list = root().textures;
            if (std::none_of(list.begin(), list.end(), [&](const Texture& t) { return t.param == pname; }))
                list.push_back({pname, file, false, settings ? settings->bool_at("srgb", true) : true});
            if (!_b.has_param(pname))
                _b.params.push_back({pname, "texture", {}, "", false, source});
            return GraphBuilder::quote("@" + pname);
        }
        if (kind == "FunctionOutput") { // its one input, by whatever name
            const auto* inputs = n.find("inputs");
            if (!inputs || !inputs->is_object() || inputs->members().empty()) {
                why = name + ": a function output fed by nothing";
                return std::nullopt;
            }
            return source_value(inputs->members().begin()->second, why);
        }
        if (kind == "FunctionInput") {
            // The call's input of its name, else its preview (as Unreal uses it by default): an input or its value,
            // cast to the input's type as Unreal casts it (a vec3 into a Vector2 input: its xy).
            const std::string input_name = text(n, "input_name");
            std::string type = text(n, "input_type");
            for (auto& c : type)
                c = char(std::toupper(static_cast<unsigned char>(c)));
            const int width = type.find("VECTOR2") != std::string::npos   ? 2
                              : type.find("VECTOR3") != std::string::npos ? 3
                              : type.find("VECTOR4") != std::string::npos ? 4
                                                                          : 1;
            const bool value_type = type.find("TEXTURE") == std::string::npos &&
                                    type.find("BOOL") == std::string::npos &&
                                    type.find("ATTRIBUTES") == std::string::npos;
            if (_caller)
                if (const auto* given = connected(*_call, input_name)) {
                    std::string outer;
                    auto v = _caller->source_value(*given, outer);
                    if (!v)
                        why = input_name + ": " + outer;
                    return v && value_type ? std::optional{_b.fit(*v, width)} : v;
                }
            if (const auto* preview = connected(n, "Preview")) {
                auto v = source_value(*preview, why);
                return v && value_type ? std::optional{_b.fit(*v, width)} : v;
            }
            if (type.find("TEXTURE") != std::string::npos) {
                why = name + ": a texture input not given";
                return std::nullopt;
            }
            const auto* p = n.find("props") ? n.find("props")->find("preview_value") : nullptr;
            std::vector<f32> c;
            for (int k = 0; k < width; ++k)
                c.push_back(p && p->is_array() && size_t(k) < p->items().size() ? f32(p->items()[size_t(k)].as_number())
                                                                                : 0.0f);
            return width == 1 ? num(c[0]) : GraphBuilder::vector(c);
        }
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
            const std::string source = pname;
            if (const auto known = std::ranges::find(_b.params, source, &GraphBuilder::Param::source);
                known != _b.params.end())
                pname = known->name;
            else
                pname = _b.safe_name(source);
            const bool normal = text(n, "sampler_type").find("NORMAL") != std::string::npos;
            const auto* settings = _doc.find("textures") ? _doc.find("textures")->find(file) : nullptr;
            auto& list = root().textures;
            if (std::none_of(list.begin(), list.end(), [&](const Texture& t) { return t.param == pname; }))
                list.push_back({pname, file, normal, !normal && settings && settings->bool_at("srgb", true)});
            if (!_b.has_param(pname)) // its slot is chosen when all are known
                _b.params.push_back({pname, "texture", {}, "", false, source});
            const auto uv = in({"UVs", "Coordinates"}, GraphBuilder::quote("uv"));
            if (!uv)
                return std::nullopt;
            const std::string sample =
                ranged(_b.add("texture", {{"uv", *uv}}, {{"texture", GraphBuilder::quote(pname)}}), file);
            if (!normal)
                return sample;
            // Unreal unpacks a normal map's texel; in its tangent space (DirectX's: green down): kin's source turned so
            // where the texture flips it on import.
            const std::string unpacked = _b.add("unpack_normal", {{"texel", sample}});
            const bool flipped = settings && settings->bool_at("flip_green_channel", false);
            return flipped ? _b.add("multiply", {{"a", unpacked}, {"b", "[1, -1, 1]"}}) : unpacked;
        }
        if (kind == "TextureCoordinate") {
            if (prop(n, "coordinate_index", 0) != 0)
                _x.note(name + ": UV set " + num(prop(n, "coordinate_index", 0)) +
                        ": the first (kin's meshes have one)");
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
        if (kind == "DotProduct" || kind == "CrossProduct")
            kind = kind == "DotProduct" ? "Dot" : "Cross"; // (Unreal's class names)
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
            // Unreal's Power clamps its base at 0 (PositiveClampedPow): GLSL's pow of a negative is undefined.
            return _b.add("power", {{"a", _b.add("max", {{"a", *base}, {"b", "0"}})}, {"b", *e}});
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
            {"Frac", "fract"},         {"SquareRoot", "sqrt"}, {"Saturate", "saturate"}, {"Normalize", "normalize"},
            {"Sign", "sign"},          {"Tangent", "tan"}};
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
            if (m.empty()) {
                why = name + ": a mask of nothing";
                return std::nullopt;
            }
            return pick(*x, m);
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
        // Unreal's world vectors in its axes (Z up: kin's y), as its glTF exporter maps them (X, Y, Z: kin's x, z,
        // y), and its centimetres: a pattern on height, or along a side, stays on it.
        if (kind == "WorldPosition") {
            _x.note("World Position: Unreal's axes and centimetres, kin's (x, z, y) metres x 100");
            return _b.add("multiply", {{"a", unreal_axes(GraphBuilder::quote("position"))}, {"b", "100"}});
        }
        if (kind == "VertexColor")
            return GraphBuilder::quote("vertex_color");
        if (kind == "CameraVectorWS")
            return unreal_axes(GraphBuilder::quote("view"));
        // Unreal's world in its axes and centimetres, as World Position: the eye, the view depth.
        if (kind == "CameraPositionWS")
            return glsl("frame.eye.xzy * 100.0", 3, {});
        if (kind == "PixelDepth")
            return glsl("dot(kin_position() - frame.eye.xyz, frame.cascade_forward.xyz) * 100.0", 1, {});
        if (kind == "Round") {
            const auto x = in({"", "None", "Input"});
            return x ? std::optional{glsl("round(x)", _b.width(*x), {{"x", *x}})} : std::nullopt;
        }
        if (kind == "Rotator") { // the coordinate turned about the centre by time x speed (radians)
            const auto c = in({"Coordinate"}, GraphBuilder::quote("uv")), t = in({"Time"}, GraphBuilder::quote("time"));
            if (!c || !t)
                return std::nullopt;
            const std::string centre =
                "[" + num(prop(n, "center_x", 0.5f)) + ", " + num(prop(n, "center_y", 0.5f)) + "]";
            const std::string angle = _b.add("multiply", {{"a", *t}, {"b", num(prop(n, "speed", 0.25f))}});
            return glsl("vec2(dot(vec2(cos(a), -sin(a)), p - c), dot(vec2(sin(a), cos(a)), p - c)) + c", 2,
                        {{"p", _b.fit(*c, 2)}, {"a", angle}, {"c", centre}});
        }
        if (kind == "FeatureLevelSwitch" || kind == "QualitySwitch") {
            // What a desktop GPU compiles (SM6's, SM5's) at the editor's quality (Epic's, High's), else the default.
            const auto* inputs = n.find("inputs");
            for (const char* level : {"SM6", "SM5", "Epic", "High", "Default"})
                if (inputs && inputs->find(level))
                    return in({level});
            return in({"Default"});
        }
        if (kind == "ConstantBiasScale") { // (x + bias) x scale (Unreal's defaults: 1, 0.5)
            const auto x = in({"", "None", "Input"});
            return x ? std::optional{_b.add("multiply",
                                            {{"a", _b.add("add", {{"a", *x}, {"b", num(prop(n, "bias", 1))}})},
                                             {"b", num(prop(n, "scale", 0.5f))}})}
                     : std::nullopt;
        }
        if (kind == "InverseLinearInterpolate") { // where Value is between A and B: (value - a) / (b - a)
            const auto a = in({"A"}, num(prop(n, "const_a", 0))), b = in({"B"}, num(prop(n, "const_b", 1))),
                       v = in({"Value"}, num(prop(n, "const_value", 0)));
            if (!a || !b || !v)
                return std::nullopt;
            const std::string t = _b.add("divide", {{"a", _b.add("subtract", {{"a", *v}, {"b", *a}})},
                                                    {"b", _b.add("subtract", {{"a", *b}, {"b", *a}})}});
            return prop(n, "clamp_result", 0) != 0 ? _b.add("saturate", {{"x", t}}) : t;
        }
        if (kind == "ReflectionVectorWS") // the view reflected about the pixel's normal (in Unreal's axes)
            return unreal_axes(_b.add("reflect", {{"direction", _b.add("negate", {{"x", GraphBuilder::quote("view")}})},
                                                  {"normal", GraphBuilder::quote("surface.normal")}}));
        if (kind == "ScreenPosition") // its ViewportUV: 0 to 1 across the view, from the top left (kin's screen)
            return GraphBuilder::quote("screen");
        if (kind == "PixelNormalWS" || kind == "VertexNormalWS")
            return unreal_axes(GraphBuilder::quote(kind == "PixelNormalWS" ? "surface.normal" : "normal"));
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
        if (kind == "NamedRerouteUsage") // its declaration's
            if (const std::string declaration = text(n, "declaration"); !declaration.empty())
                return value(declaration, "", why);
        if (kind == "Reroute" || kind == "NamedRerouteDeclaration" || kind == "NamedRerouteUsage")
            return in({"", "None", "Input"});
        if (kind == "StaticSwitchParameter" || kind == "StaticSwitch") {
            // Chosen at import as Unreal chooses it at compile time: a parameter by the material's value (an
            // instance's own), a switch by its Value (a static bool), else its default.
            std::optional<bool> on;
            if (kind == "StaticSwitchParameter")
                on = switch_value(n);
            else if (const auto* v = connected(n, "Value"))
                on = static_bool(*v);
            if (!on) {
                on = prop(n, "default_value", 0) != 0;
                _x.note(name + ": a static switch, its default (" + std::string(*on ? "true" : "false") + ") taken");
            }
            return in({*on ? "True" : "False", *on ? "A" : "B"});
        }
        if (kind == "Custom") { // HLSL of one expression, its inputs by name: as GLSL where every word is known
            const std::string code = text(n, "code");
            std::vector<std::pair<std::string, std::string>> inputs;
            if (const auto* given = n.find("inputs"); given && given->is_object())
                for (const auto& [input, from] : given->members()) {
                    if (!from.find("node"))
                        continue;
                    const auto v = source_value(from, why);
                    if (!v)
                        return std::nullopt;
                    inputs.emplace_back(input, *v);
                }
            const auto glsl_code = custom_as_glsl(code, inputs);
            if (!glsl_code) {
                why = name + ": a Custom node's HLSL kin cannot read";
                return std::nullopt;
            }
            const std::string type = text(n, "output_type");
            const int width = type.find("FLOAT2") != std::string::npos   ? 2
                              : type.find("FLOAT3") != std::string::npos ? 3
                              : type.find("FLOAT4") != std::string::npos ? 4
                                                                         : 1;
            static const char* types[] = {"float", "float", "vec2", "vec3", "vec4"};
            _x.note(name + ": a Custom node's HLSL read as GLSL (" + code.substr(0, 60) + ")");
            return glsl(std::string(types[width]) + "(" + *glsl_code + ")", width, std::move(inputs));
        }
        if (kind == "BumpOffset") {
            // The coordinate shifted along the view as the tangent frame sees it, by the height above the reference
            // plane (Unreal's: uv + view_tangent.xy x (height - reference) x ratio).
            const auto uv = in({"Coordinate"}, GraphBuilder::quote("uv"));
            const auto h = in({"Height"});
            const auto ratio = in({"HeightRatioInput"}, num(prop(n, "height_ratio", 0.05f)));
            if (!uv || !h || !ratio)
                return std::nullopt;
            return glsl("p + kin_graph_world_to_uv(kin_view(), kin_uv(), kin_graph_geometry_normal()).xy * "
                        "((h - k) * r)",
                        2,
                        {{"p", _b.fit(*uv, 2)},
                         {"h", _b.fit(*h, 1)},
                         {"r", _b.fit(*ratio, 1)},
                         {"k", num(prop(n, "reference_plane", 0.5f))}});
        }
        if (kind == "Transform") { // a vector from one space to another
            const auto v = in({"", "None", "Input"});
            if (!v)
                return std::nullopt;
            const std::string from = upper(text(n, "transform_source_type")), to = upper(text(n, "transform_type"));
            const auto space = [](const std::string& e, const char* fallback) {
                for (const char* s : {"TANGENT", "LOCAL", "WORLD", "VIEW", "CAMERA", "INSTANCE", "PARTICLE"})
                    if (e.find(s) != std::string::npos)
                        return std::string(s);
                return std::string(fallback);
            };
            const std::string a = space(from, "TANGENT"), b = space(to, "WORLD");
            // (View and camera space are the same for a vector.)
            const auto known = [](std::string& s) {
                if (s == "CAMERA")
                    s = "VIEW";
                if (s == "INSTANCE") // (an instance's space: its object's, as local)
                    s = "LOCAL";
                return s == "TANGENT" || s == "LOCAL" || s == "WORLD" || s == "VIEW";
            };
            std::string from_space = a, to_space = b;
            if (!known(from_space) || !known(to_space)) {
                why = name + ": a Transform from " + a + " to " + b + " space (kin has tangent, local, world, view)";
                return std::nullopt;
            }
            if (from_space == "LOCAL" || to_space == "LOCAL")
                _x.note(name + ": a Transform from or to local space: as world space (kin's graphs do not see the "
                               "object's transform)");
            if (from_space == "VIEW" || to_space == "VIEW")
                _x.note(name + ": a Transform from or to view space: the camera's, as if it did not roll");
            if (from_space == to_space)
                return _b.fit(*v, 3);
            // Into kin's world (kin's axes; Unreal's world vectors are in its own: kin's x, z, y), then out of it.
            const std::map<std::string, std::string> into{
                {"TANGENT", "kin_graph_uv_to_world(v, kin_uv(), kin_graph_geometry_normal())"},
                {"VIEW", "kin_graph_view_to_world(v)"},
                {"WORLD", "v.xzy"},
                {"LOCAL", "v.xzy"}};
            const std::map<std::string, std::string> out_of{
                {"TANGENT", "kin_graph_world_to_uv(@, kin_uv(), kin_graph_geometry_normal())"},
                {"VIEW", "kin_graph_world_to_view(@)"},
                {"WORLD", "(@).xzy"},
                {"LOCAL", "(@).xzy"}};
            std::string expr = out_of.at(to_space);
            expr.replace(expr.find('@'), 1, into.at(from_space));
            return glsl(expr, 3, {{"v", _b.fit(*v, 3)}});
        }
        if (kind == "TransformPosition") { // a position between local and world space: the same in kin's graphs
            const std::string from = upper(text(n, "transform_source_type")), to = upper(text(n, "transform_type"));
            const auto local_or_world = [](const std::string& e) {
                return e.empty() || e.find("LOCAL") != std::string::npos || e.find("INSTANCE") != std::string::npos ||
                       (e.find("WORLD") != std::string::npos && e.find("TRANSLATED") == std::string::npos);
            };
            if (!local_or_world(from) || !local_or_world(to)) {
                why = name + ": a TransformPosition from " + from + " to " + to + " (kin has local and world)";
                return std::nullopt;
            }
            _x.note(name + ": a position from or to local space: as world space (kin's graphs do not see the object's "
                           "transform)");
            return in({"", "None", "Input"});
        }
        if (kind == "ParticleColor") { // a particle's colour: kin's particles give theirs as the vertex colour
            _x.note(name + ": the particle's colour: the vertex colour");
            return GraphBuilder::quote("vertex_color");
        }
        if (kind == "DynamicParameter") { // a particle emitter's: its default
            _x.note(name + ": a dynamic parameter: its default (kin's particles set none)");
            const auto c = colour(n, "default_value");
            return GraphBuilder::vector({c.data(), 4});
        }
        if (kind == "PerInstanceFadeAmount") {
            _x.note(name + ": the instance's fade: none");
            return std::string("0");
        }
        if (kind == "ObjectPositionWS") {
            _x.note(name + ": the object's position: the world's origin (kin's graphs do not see the object's "
                           "transform)");
            return std::string("[0, 0, 0]");
        }
        if (kind == "DepthFade") { // (its fade where the surface nears what is behind it: kin's graphs see no depth)
            _x.note(name + ": Depth Fade: its opacity, without the fade near what is behind (kin's graphs see no "
                           "scene depth)");
            return in({"Opacity", "OpacityInput"}, num(prop(n, "opacity_default", 1)));
        }
        if (kind == "SceneDepth") {
            _x.note(name + ": Scene Depth: 10 km, as if nothing were behind (kin's graphs see no scene depth)");
            return std::string("1000000");
        }
        if (kind == "LandscapeLayerCoords") {
            // A landscape's coordinates (its quads, a metre each at Unreal's default scale) by the world position:
            // a landscape at the origin, unrotated. Scaled, turned and panned as the node's mapping says.
            const std::string type = upper(text(n, "mapping_type"));
            const std::string axes = type.find("XZ") != std::string::npos   ? "p.xy"
                                     : type.find("YZ") != std::string::npos ? "p.zy"
                                                                            : "p.xz";
            f32 scale = prop(n, "mapping_scale", 0);
            if (scale == 0)
                scale = 1;
            const f32 angle = prop(n, "mapping_rotation", 0) * std::numbers::pi_v<f32> / 180;
            _x.note(name + ": landscape coordinates by the world position (a landscape at the origin, 1 m quads)");
            return glsl("mat2(" + num(std::cos(angle)) + ", " + num(-std::sin(angle)) + ", " + num(std::sin(angle)) +
                            ", " + num(std::cos(angle)) + ") * (" + axes + " / " + num(scale) + ") + vec2(" +
                            num(prop(n, "mapping_pan_u", 0)) + ", " + num(prop(n, "mapping_pan_v", 0)) + ")",
                        2, {{"p", GraphBuilder::quote("position")}});
        }
        if (kind == "TextureSampleParameterCube" || kind == "TextureSampleCube") {
            _x.note(name + ": a cube map: the scene's environment in its direction");
            const auto d = in({"UVs", "Coordinates"}, GraphBuilder::quote("view"));
            if (!d)
                return std::nullopt;
            return glsl("vec4(kin_graph_environment(d.xzy), 1.0)", 4, {{"d", _b.fit(*d, 3)}});
        }
        if (kind == "CollectionParameter") {
            const auto* p = n.find("props");
            const auto* v = p ? p->find("collection_value") : nullptr;
            if (!v) {
                why = name + ": the parameter collection's " + text(n, "parameter_name") + " (no default exported)";
                return std::nullopt;
            }
            _x.note(name + ": the parameter collection's " + text(n, "parameter_name") +
                    ": its default (a game may set it while it runs)");
            if (v->is_number())
                return num(f32(v->as_number()));
            const auto c = colour(n, "collection_value");
            return GraphBuilder::vector({c.data(), 4});
        }
        if (kind == "If") { // A > B, A == B (within the threshold), A < B
            const auto a = in({"A"}), b = in({"B"}, num(prop(n, "const_b", 0)));
            const auto gt = in({"A > B"}), lt = in({"A < B"});
            if (!a || !b || !gt || !lt)
                return std::nullopt;
            const auto eq = in({"A == B"}, *gt);
            if (!eq)
                return std::nullopt;
            const int w = std::max({_b.width(*gt), _b.width(*lt), _b.width(*eq)});
            static const char* types[] = {"float", "float", "vec2", "vec3", "vec4"};
            const auto as = [&](const std::string& v) {
                return _b.width(v) == w ? v : _b.width(v) == 1 ? v : _b.fit(v, w);
            };
            const std::string t = types[std::clamp(w, 1, 4)];
            return glsl("abs(a - b) <= " + num(prop(n, "equals_threshold", 0.00001f)) + " ? " + t + "(e) : (a > b ? " +
                            t + "(g) : " + t + "(l))",
                        w,
                        {{"a", _b.fit(*a, 1)}, {"b", _b.fit(*b, 1)}, {"g", as(*gt)}, {"e", as(*eq)}, {"l", as(*lt)}});
        }
        why = name + ": a " + kind + " (kin has no node for it)";
        return std::nullopt;
    }

    // A Custom node's code as one GLSL expression, or none: an expression (or `return` one;) of its inputs, numbers
    // (made floats) and the functions below (HLSL's renamed), a member only after a name or ')'. Anything else (a
    // statement, a loop, a texture, an engine global) is refused.
    static std::optional<std::string> custom_as_glsl(std::string code,
                                                     const std::vector<std::pair<std::string, std::string>>& inputs) {
        const auto trim = [](std::string& t) {
            while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
                t.pop_back();
            size_t i = 0;
            while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])))
                ++i;
            t.erase(0, i);
        };
        trim(code);
        if (code.rfind("return", 0) == 0 && code.size() > 6 && !std::isalnum(static_cast<unsigned char>(code[6]))) {
            code.erase(0, 6);
            trim(code);
            if (code.empty() || code.back() != ';')
                return std::nullopt;
            code.pop_back();
            trim(code);
        }
        if (code.empty() || code.find_first_of(";{}[]#\"'?:") != std::string::npos)
            return std::nullopt;
        static const std::map<std::string, std::string> functions{{"abs", "abs"},
                                                                  {"sin", "sin"},
                                                                  {"cos", "cos"},
                                                                  {"tan", "tan"},
                                                                  {"asin", "asin"},
                                                                  {"acos", "acos"},
                                                                  {"atan", "atan"},
                                                                  {"atan2", "atan"},
                                                                  {"sqrt", "sqrt"},
                                                                  {"rsqrt", "inversesqrt"},
                                                                  {"pow", "pow"},
                                                                  {"exp", "exp"},
                                                                  {"exp2", "exp2"},
                                                                  {"log", "log"},
                                                                  {"log2", "log2"},
                                                                  {"floor", "floor"},
                                                                  {"ceil", "ceil"},
                                                                  {"round", "round"},
                                                                  {"trunc", "trunc"},
                                                                  {"frac", "fract"},
                                                                  {"clamp", "clamp"},
                                                                  {"min", "min"},
                                                                  {"max", "max"},
                                                                  {"lerp", "mix"},
                                                                  {"step", "step"},
                                                                  {"smoothstep", "smoothstep"},
                                                                  {"dot", "dot"},
                                                                  {"cross", "cross"},
                                                                  {"length", "length"},
                                                                  {"distance", "distance"},
                                                                  {"normalize", "normalize"},
                                                                  {"sign", "sign"},
                                                                  {"reflect", "reflect"},
                                                                  {"Mod", "mod"},
                                                                  {"fmod", "mod"},
                                                                  {"float", "float"},
                                                                  {"float2", "vec2"},
                                                                  {"float3", "vec3"},
                                                                  {"float4", "vec4"},
                                                                  {"saturate", "kin_graph_saturate"}};
        std::string out;
        char last = 0; // the last significant character written
        for (size_t i = 0; i < code.size();) {
            const char c = code[i];
            if (std::isspace(static_cast<unsigned char>(c))) {
                out += ' ';
                ++i;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && i + 1 < code.size() && std::isdigit(static_cast<unsigned char>(code[i + 1])))) {
                size_t j = i;
                bool real = false;
                while (j < code.size() && (std::isalnum(static_cast<unsigned char>(code[j])) || code[j] == '.')) {
                    real = real || code[j] == '.' || code[j] == 'e' || code[j] == 'E';
                    ++j;
                }
                std::string number = code.substr(i, j - i);
                if (!number.empty() && (number.back() == 'f' || number.back() == 'F'))
                    number.pop_back();
                if (number.find_first_not_of("0123456789.eE") != std::string::npos)
                    return std::nullopt;
                out += real ? number : number + ".0";
                last = '0';
                i = j;
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t j = i;
                while (j < code.size() && (std::isalnum(static_cast<unsigned char>(code[j])) || code[j] == '_'))
                    ++j;
                const std::string word = code.substr(i, j - i);
                i = j;
                if (last == '.') { // a member: a swizzle
                    if (word.find_first_not_of("xyzwrgba") != std::string::npos || word.size() > 4)
                        return std::nullopt;
                    out += word;
                } else if (std::ranges::any_of(inputs, [&](const auto& in) { return in.first == word; })) {
                    out += word;
                } else if (const auto f = functions.find(word); f != functions.end()) {
                    out += f->second;
                } else {
                    return std::nullopt;
                }
                last = 'a';
                continue;
            }
            if (c == '(' && (code.compare(i, 6, "(uint)") == 0 || code.compare(i, 5, "(int)") == 0)) {
                out += "trunc"; // a cast of what follows (a bracket) to an integer
                i += code[i + 1] == 'u' ? 6 : 5;
                last = 'a';
                continue;
            }
            if (c == '.' && last != 'a' && last != ')')
                return std::nullopt;
            if (std::string("+-*/(),.").find(c) == std::string::npos)
                return std::nullopt;
            out += c;
            last = c;
            ++i;
        }
        return out;
    }

    // A sample of `file` as its values: an 8-bit texel times the range a file decoded from values past 0 to 1 has (an
    // OpenEXR's), plus its least; the sample as it is for another.
    std::string ranged(const std::string& sample, const std::string& file) {
        if (file.empty())
            return sample;
        const auto path = _x.options.project / file;
        (void)_x.load(path, file); // (decoded once: its range known)
        const auto it = _x.ranges.find(path);
        if (it == _x.ranges.end())
            return sample;
        _x.note(std::filesystem::path(file).filename().string() +
                ": its values (past 0 to 1) fitted into 8 bits, read back by its range");
        return _b.add("add", {{"a", _b.add("multiply", {{"a", sample}, {"b", GraphBuilder::vector(it->second.scale)}})},
                              {"b", GraphBuilder::vector(it->second.bias)}});
    }

    static std::string upper(std::string s) {
        for (auto& c : s)
            c = char(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    const JsonValue& _doc;
    GraphBuilder& _b;
    material_import::Context& _x;
    const JsonValue* _nodes;
    UnrealGraph* _root = nullptr;     // a function's: the material's graph (its textures)
    UnrealGraph* _caller = nullptr;   // a function's: the graph calling it
    const JsonValue* _call = nullptr; // and the call (its inputs)
    int _depth = 0;
    std::map<std::string, std::unique_ptr<UnrealGraph>> _children; // the functions inlined, by call
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
        auto graph = std::make_unique<UnrealGraph>(doc, b, x);
        const std::string model = doc.find("shading_model") ? doc.find("shading_model")->as_string() : "";
        const std::string blend = doc.find("blend_mode") ? doc.find("blend_mode")->as_string() : "";
        const bool unlit = model == "MSM_UNLIT";
        const auto* outputs = doc.find("graph")->find("outputs");
        static const std::vector<std::pair<const char*, const char*>> mapping{
            {"BaseColor", "base_color"},   {"Metallic", "metallic"},          {"Roughness", "roughness"},
            {"EmissiveColor", "emission"}, {"AmbientOcclusion", "occlusion"}, {"Normal", "normal"},
            {"Opacity", "alpha"},          {"OpacityMask", "alpha"},          {"WorldPositionOffset", "offset"}};
        const auto build = [&](UnrealGraph& g) {
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
                auto v = g.value(o->find("node")->as_string(), g.pin_of(*o), why);
                if (!v) {
                    x.note(std::string(from) + ": " + why + ": its Tier 1 value kept");
                    continue;
                }
                std::string target = unlit && std::string(from) == "EmissiveColor" ? "base_color" : to;
                if (target == "normal" && !doc.bool_at("tangent_space_normal", true)) // the world's (Unreal's axes)
                    v = b.add("normalize", {{"x", b.member(*v, "xzy")}});
                else if (target == "normal") // Unreal's tangent space (green down) as kin's (+y up), then the world's
                    v = b.add("tangent_to_world", {{"normal", b.add("multiply", {{"a", *v}, {"b", "[1, -1, 1]"}})}});
                else if (target == "offset") // Unreal's centimetres in its axes: kin's metres in its own
                    v = b.add("multiply", {{"a", b.member(b.fit(*v, 3), "xzy")}, {"b", "0.01"}});
                b.outputs[target] = *v;
            }
        };
        build(*graph);
        // Parameters past Material3D::shader_params' floats (as kin's compiler packs them): the material's values in a
        // graph of its own.
        {
            std::array<bool, surface_param_count3d> taken{};
            bool fits = true;
            for (const auto& p : b.params) {
                if (p.type == "texture")
                    continue;
                const u32 size = p.type == "float" ? 1 : p.type == "vec2" ? 2 : p.type == "vec3" ? 3 : 4;
                const u32 align = size >= 3 ? 4 : size;
                u32 at = surface_param_count3d;
                for (u32 o = 0; o + size <= surface_param_count3d && at == surface_param_count3d; o += align) {
                    bool free = true;
                    for (u32 k = o; k < o + size; ++k)
                        free = free && !taken[k];
                    if (free)
                        at = o;
                }
                if (at == surface_param_count3d) {
                    fits = false;
                    break;
                }
                for (u32 k = at; k < at + size; ++k)
                    taken[k] = true;
            }
            if (!fits) {
                x.note("its parameters fill more than Material3D::shader_params' " +
                       std::to_string(surface_param_count3d) +
                       " floats: their values (this material's) in a graph of its own, not changeable as parameters");
                b = GraphBuilder{};
                b.name = std::filesystem::path(base).filename().string() + "__" + source.stem().string();
                graph = std::make_unique<UnrealGraph>(doc, b, x);
                graph->fold = true;
                build(*graph);
            }
        }
        UnrealGraph& g = *graph;
        for (const char* other : {"Refraction", "Anisotropy", "Specular"})
            if (outputs && outputs->find(other))
                x.note(std::string(other) + ": not in kin's graph outputs");
        // The textures in kin's five slots (as graph data: the graph reads them; kin's own reading of a slot undone).
        const auto texture = [&](const GraphBuilder::Param& p) {
            return *std::ranges::find(g.textures, p.name, &UnrealGraph::Texture::param);
        };
        if (!b.assign_slots([&](const GraphBuilder::Param& p) { return texture(p).normal; })) {
            x.note("its graph samples more than 21 textures (kin's five map slots and 16 layers): its Tier 1 import "
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
        // Its file: the base material's name, and the static switches set otherwise than their defaults (another
        // graph each: an instance changing one).
        std::string file_name = b.name;
        for (const auto& [name, on] : g.switched) {
            std::string part = name;
            for (auto& c : part)
                if (!std::isalnum(static_cast<unsigned char>(c)))
                    c = '_';
            file_name += "__" + part + (on ? "_on" : "_off");
        }
        imported.graph = x.out / (file_name + ".kingraph");
        if (options.write) {
            std::filesystem::create_directories(x.out);
            std::ofstream file(imported.graph, std::ios::binary);
            file << text;
        }
        auto& p = imported.asset.base.material;
        p.graph = imported.graph.lexically_relative(x.content);
        // A graph's vertex colour is kin's (the mesh's times the material's colour): where the graph gives the base
        // colour or the alpha, the material's is white or opaque, as Unreal's Vertex Color is the mesh's alone. (Where
        // it does not, the Tier 1 colour stays: the surface starts from it.)
        Color c = p.color.value_or(Color{255, 255, 255, 255});
        if (b.outputs.contains("base_color"))
            c.r = c.g = c.b = 255;
        if (b.outputs.contains("alpha"))
            c.a = 255;
        p.color = c;
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
                // Repeated, filtered and mipmapped, as Unreal's default sampling is (a graph's UVs often run
                // past 0..1); a map without a sampler would take Material3D's legacy clamp.
                v.texture = MaterialTexture3D{.asset = *path, .sampler = TextureSampler3D{}};
            } else {
                const auto* group = values ? values->find(param.type == "float" ? "scalar" : "vector") : nullptr;
                const auto* given = group ? group->find(param.source.empty() ? param.name : param.source) : nullptr;
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
