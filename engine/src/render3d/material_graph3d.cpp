// Material graphs into surface shaders (kin/render3d/material_graph3d.hpp).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <kin/core/json_value.hpp>
#include <kin/render3d/material_graph3d.hpp>
#include <set>
#include <sstream>

namespace kin {
namespace {

using Type = GraphType3D;

const char* glsl_type(Type t) {
    switch (t) {
    case Type::Float:
        return "float";
    case Type::Vec2:
        return "vec2";
    case Type::Vec3:
        return "vec3";
    case Type::Vec4:
        return "vec4";
    case Type::Texture:
        return "sampler2D";
    }
    return "float";
}
int width(Type t) {
    return t == Type::Texture ? 0 : int(t);
}
Type of_width(int n) {
    return n <= 1 ? Type::Float : n == 2 ? Type::Vec2 : n == 3 ? Type::Vec3 : Type::Vec4;
}

std::string number(double v) {
    char text[32];
    std::snprintf(text, sizeof text, "%.9g", v);
    std::string s = text;
    if (s.find_first_of(".eEn") == std::string::npos)
        s += ".0";
    return s;
}

struct Value {
    std::string expr;
    Type type = Type::Float;
};

// `v` as type `to`: a float widens to any; a vector narrows to its first components, or widens with 0 (1 the 4th).
std::string cast(const Value& v, Type to) {
    const int from = width(v.type), n = width(to);
    if (from == n)
        return v.expr;
    if (from == 1)
        return std::string(glsl_type(to)) + "(" + v.expr + ")";
    if (n < from)
        return "(" + v.expr + ")." + std::string("xyzw").substr(0, size_t(n));
    if (n == 1)
        return "(" + v.expr + ").x";
    std::string out = std::string(glsl_type(to)) + "(" + v.expr;
    for (int i = from; i < n; ++i)
        out += i == 3 ? ", 1.0" : ", 0.0";
    return out + ")";
}

// --- the node types ---

enum class In : u8 { Widest, Float, Vec2, Vec3, Vec4, Any, MixT };
struct InputSpec {
    std::string name;
    In kind = In::Widest;
    std::string fallback; // a reference or a value (JSON text) used when the input isn't given; empty: required
};
class Compiler;
struct NodeSpec {
    std::string type, summary;
    std::vector<InputSpec> inputs;
    bool texture = false; // takes a "texture" field (a texture parameter)
    // The node's expression and type from its inputs (cast as their kinds say; Widest ones to `widest`).
    std::function<Value(Compiler&, const std::vector<Value>&, Type widest, const std::string& sampler, bool srgb)> emit;
};

Value make(std::string expr, Type t) {
    return {std::move(expr), t};
}

const std::vector<NodeSpec>& specs() {
    static const std::vector<NodeSpec> all = [] {
        std::vector<NodeSpec> s;
        const auto binary = [&](const char* name, const char* summary, const char* op, bool function) {
            s.push_back({name,
                         summary,
                         {{"a"}, {"b"}},
                         false,
                         [op, function](Compiler&, const std::vector<Value>& in, Type w, const std::string&, bool) {
                             return make(function ? std::string(op) + "(" + in[0].expr + ", " + in[1].expr + ")"
                                                  : "(" + in[0].expr + " " + op + " " + in[1].expr + ")",
                                         w);
                         }});
        };
        binary("add", "a + b", "+", false);
        binary("subtract", "a - b", "-", false);
        binary("multiply", "a x b", "*", false);
        binary("divide", "a / b", "/", false);
        binary("min", "the smaller", "min", true);
        binary("max", "the larger", "max", true);
        binary("power", "a to the b", "pow", true);
        binary("mod", "a modulo b", "mod", true);
        s.push_back({"step",
                     "0 below the edge, 1 from it",
                     {{"edge"}, {"x"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type w, const std::string&, bool) {
                         return make("step(" + in[0].expr + ", " + in[1].expr + ")", w);
                     }});
        const auto unary = [&](const char* name, const char* summary, std::string before, std::string after) {
            s.push_back({name,
                         summary,
                         {{"x"}},
                         false,
                         [before, after](Compiler&, const std::vector<Value>& in, Type w, const std::string&, bool) {
                             return make(before + in[0].expr + after, w);
                         }});
        };
        unary("abs", "|x|", "abs(", ")");
        unary("floor", "rounded down", "floor(", ")");
        unary("ceil", "rounded up", "ceil(", ")");
        unary("fract", "x - floor(x)", "fract(", ")");
        unary("sqrt", "the square root (of 0 at least)", "sqrt(max(", ", 0.0))");
        unary("sin", "the sine (radians)", "sin(", ")");
        unary("cos", "the cosine (radians)", "cos(", ")");
        unary("tan", "the tangent (radians)", "tan(", ")");
        unary("exp", "e to the x", "exp(", ")");
        unary("log", "the natural logarithm (of a tiny positive at least)", "log(max(", ", 1e-20))");
        unary("sign", "-1, 0 or 1", "sign(", ")");
        unary("negate", "-x", "(-", ")");
        unary("one_minus", "1 - x", "(1.0 - ", ")");
        unary("saturate", "x within [0, 1]", "clamp(", ", 0.0, 1.0)");
        unary("normalize", "x made unit", "normalize(", ")");
        s.push_back({"length",
                     "the vector's length",
                     {{"x", In::Any}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         return make("length(" + in[0].expr + ")", Type::Float);
                     }});
        s.push_back({"dot",
                     "a . b",
                     {{"a"}, {"b"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         return make("dot(" + in[0].expr + ", " + in[1].expr + ")", Type::Float);
                     }});
        s.push_back({"distance",
                     "|a - b|",
                     {{"a"}, {"b"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         return make("distance(" + in[0].expr + ", " + in[1].expr + ")", Type::Float);
                     }});
        s.push_back({"cross",
                     "a x b (3D)",
                     {{"a", In::Vec3}, {"b", In::Vec3}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         return make("cross(" + in[0].expr + ", " + in[1].expr + ")", Type::Vec3);
                     }});
        s.push_back({"reflect",
                     "the direction mirrored about the normal",
                     {{"direction", In::Vec3}, {"normal", In::Vec3}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         return make("reflect(" + in[0].expr + ", " + in[1].expr + ")", Type::Vec3);
                     }});
        s.push_back({"mix",
                     "a to b by t (Unreal's Lerp)",
                     {{"a"}, {"b"}, {"t", In::MixT}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type w, const std::string&, bool) {
                         return make("mix(" + in[0].expr + ", " + in[1].expr + ", " + in[2].expr + ")", w);
                     }});
        s.push_back({"clamp",
                     "x within [min, max]",
                     {{"x"}, {"min", In::Widest, "0"}, {"max", In::Widest, "1"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type w, const std::string&, bool) {
                         return make("clamp(" + in[0].expr + ", " + in[1].expr + ", " + in[2].expr + ")", w);
                     }});
        s.push_back({"smoothstep",
                     "0 to 1 smoothly from edge0 to edge1",
                     {{"edge0", In::Widest, "0"}, {"edge1", In::Widest, "1"}, {"x"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type w, const std::string&, bool) {
                         return make("smoothstep(" + in[0].expr + ", " + in[1].expr + ", " + in[2].expr + ")", w);
                     }});
        s.push_back({"remap",
                     "x from [in_min, in_max] to [out_min, out_max]",
                     {{"x"},
                      {"in_min", In::Widest, "0"},
                      {"in_max", In::Widest, "1"},
                      {"out_min", In::Widest, "0"},
                      {"out_max", In::Widest, "1"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type w, const std::string&, bool) {
                         return make("(" + in[3].expr + " + (" + in[0].expr + " - " + in[1].expr + ") / (" +
                                         in[2].expr + " - " + in[1].expr + ") * (" + in[4].expr + " - " + in[3].expr +
                                         "))",
                                     w);
                     }});
        s.push_back({"combine",
                     "a vector of x, y (z, w)",
                     {{"x", In::Float}, {"y", In::Float}, {"z", In::Float, "-"}, {"w", In::Float, "-"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         std::string parts;
                         int n = 0;
                         for (const auto& v : in)
                             if (!v.expr.empty())
                                 parts += (n++ ? ", " : "") + v.expr;
                         return make(std::string(glsl_type(of_width(n))) + "(" + parts + ")", of_width(n));
                     }});
        // Textures.
        s.push_back({"texture",
                     "a texture parameter's texel (vec4)",
                     {{"uv", In::Vec2, "\"uv\""}},
                     true,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string& sampler, bool srgb) {
                         return make("kin_graph_sample(" + sampler + ", " + in[0].expr + ", " +
                                         (srgb ? "true" : "false") + ")",
                                     Type::Vec4);
                     }});
        s.push_back({"normal_map",
                     "a normal map's texel as the world normal",
                     {{"uv", In::Vec2, "\"uv\""}, {"strength", In::Float, "1"}},
                     true,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string& sampler, bool) {
                         return make("kin_graph_tangent_to_world(kin_graph_unpack_normal(texture(" + sampler + ", " +
                                         in[0].expr + "), " + in[1].expr + "), " + in[0].expr +
                                         ", kin_graph_geometry_normal())",
                                     Type::Vec3);
                     }});
        s.push_back({"tangent_to_world",
                     "a tangent-space normal (+y up) in world space",
                     {{"normal", In::Vec3}, {"uv", In::Vec2, "\"uv\""}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         return make("kin_graph_tangent_to_world(normalize(" + in[0].expr + "), " + in[1].expr +
                                         ", kin_graph_geometry_normal())",
                                     Type::Vec3);
                     }});
        s.push_back({"triplanar",
                     "a texture projected along the world's axes",
                     {{"scale", In::Float, "1"},
                      {"sharpness", In::Float, "4"},
                      {"position", In::Vec3, "\"position\""},
                      {"normal", In::Vec3, "\"normal\""}},
                     true,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string& sampler, bool srgb) {
                         return make("kin_graph_triplanar(" + sampler + ", " + in[2].expr + ", " + in[3].expr + ", " +
                                         in[0].expr + ", " + in[1].expr + ", " + (srgb ? "true" : "false") + ")",
                                     Type::Vec4);
                     }});
        // Procedural.
        const auto noise = [&](const char* name, const char* summary, const char* fn) {
            s.push_back({name,
                         summary,
                         {{"p", In::Vec3, "\"uv\""}, {"scale", In::Float, "1"}},
                         false,
                         [fn](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                             return make(std::string(fn) + "(" + in[0].expr + " * " + in[1].expr + ")", Type::Float);
                         }});
        };
        noise("noise_value", "value noise in [0, 1]", "kin_graph_value_noise");
        noise("noise_gradient", "gradient (Perlin) noise in [0, 1]", "kin_graph_gradient_noise");
        noise("noise_cellular", "cellular (Worley) noise: the distance to the nearest point",
              "kin_graph_cellular_noise");
        s.push_back({"fbm",
                     "gradient noise summed over octaves",
                     {{"p", In::Vec3, "\"uv\""},
                      {"scale", In::Float, "1"},
                      {"octaves", In::Float, "4"},
                      {"lacunarity", In::Float, "2"},
                      {"gain", In::Float, "0.5"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         return make("kin_graph_fbm(" + in[0].expr + " * " + in[1].expr + ", int(" + in[2].expr +
                                         "), " + in[3].expr + ", " + in[4].expr + ")",
                                     Type::Float);
                     }});
        s.push_back({"checker",
                     "0 and 1 in squares",
                     {{"uv", In::Vec2, "\"uv\""}, {"scale", In::Float, "8"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         return make("kin_graph_checker(" + in[0].expr + ", " + in[1].expr + ")", Type::Float);
                     }});
        // Effects.
        s.push_back({"fresnel",
                     "1 at grazing angles, 0 facing the view",
                     {{"power", In::Float, "5"}, {"normal", In::Vec3, "\"surface.normal\""}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         return make("pow(1.0 - clamp(dot(normalize(" + in[1].expr + "), kin_view()), 0.0, 1.0), " +
                                         in[0].expr + ")",
                                     Type::Float);
                     }});
        s.push_back({"uv_panner",
                     "the UV moving with time",
                     {{"uv", In::Vec2, "\"uv\""}, {"speed", In::Vec2, "[0.1, 0]"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         return make("(" + in[0].expr + " + " + in[1].expr + " * frame.time.x)", Type::Vec2);
                     }});
        s.push_back({"uv_rotate",
                     "the UV turned about a centre (radians)",
                     {{"uv", In::Vec2, "\"uv\""}, {"angle", In::Float, "0"}, {"center", In::Vec2, "[0.5, 0.5]"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         const std::string d = "(" + in[0].expr + " - " + in[2].expr + ")";
                         return make("(mat2(cos(" + in[1].expr + "), sin(" + in[1].expr + "), -sin(" + in[1].expr +
                                         "), cos(" + in[1].expr + ")) * " + d + " + " + in[2].expr + ")",
                                     Type::Vec2);
                     }});
        return s;
    }();
    return all;
}

const NodeSpec* spec(const std::string& type) {
    for (const auto& s : specs())
        if (s.type == type)
            return &s;
    return nullptr;
}

// The built-in inputs.
struct BuiltIn {
    const char* name;
    const char* expr;
    Type type;
};
constexpr BuiltIn built_ins[] = {
    {"uv", "kin_uv()", Type::Vec2},
    {"position", "kin_position()", Type::Vec3},
    {"normal", "kin_graph_geometry_normal()", Type::Vec3},
    {"view", "kin_view()", Type::Vec3},
    {"vertex_color", "vColor", Type::Vec4},
    {"time", "frame.time.x", Type::Float},
    {"screen", "(gl_FragCoord.xy * frame.occlusion.zw)", Type::Vec2},
    {"camera_distance", "length(frame.eye.xyz - vWorld)", Type::Float},
};
struct Field {
    const char* name;
    Type type;
};
constexpr Field surface_fields[] = {{"base_color", Type::Vec3}, {"alpha", Type::Float},     {"normal", Type::Vec3},
                                    {"metallic", Type::Float},  {"roughness", Type::Float}, {"occlusion", Type::Float},
                                    {"emission", Type::Vec3}};

bool identifier(const std::string& s) {
    if (s.empty() || s.size() > 64 || std::isdigit(static_cast<unsigned char>(s[0])))
        return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

class Compiler {
public:
    CompiledMaterialGraph3D out;

    void compile(std::string_view text) {
        const auto parsed = parse_json(text);
        if (!parsed.ok() || !parsed.value->is_object()) {
            error("", "not JSON: " + (parsed.ok() ? std::string("not an object") : parsed.error));
            return;
        }
        const JsonValue& g = *parsed.value;
        if (!g.find("schema") || g.find("schema")->as_string() != "kin.material-graph/1") {
            error("", "not a kin.material-graph/1 graph");
            return;
        }
        out.name = g.find("name") ? g.find("name")->as_string() : "";
        parameters(g.find("parameters"));
        if (const auto* nodes = g.find("nodes"); nodes && nodes->is_array()) {
            for (const auto& n : nodes->items()) {
                const std::string id = n.find("id") ? n.find("id")->as_string() : "";
                if (!identifier(id))
                    error(id, "a node's id must be a name (letters, digits, _)");
                else if (_nodes.contains(id) || _params.contains(id) || built_in(id))
                    error(id, "the id is taken (by a node, a parameter or a built-in input)");
                else
                    _nodes[id] = &n;
            }
        } else if (g.find("nodes")) {
            error("", "'nodes' must be an array");
        }
        // The outputs, each as kin's surface takes it.
        static const std::map<std::string, std::pair<Type, std::string>> fields{
            {"base_color", {Type::Vec3, "max(%, vec3(0.0))"}},
            {"alpha", {Type::Float, "clamp(%, 0.0, 1.0)"}},
            {"normal", {Type::Vec3, "normalize(%)"}},
            {"metallic", {Type::Float, "clamp(%, 0.0, 1.0)"}},
            {"roughness", {Type::Float, "clamp(%, 0.045, 1.0)"}},
            {"occlusion", {Type::Float, "clamp(%, 0.0, 1.0)"}},
            {"emission", {Type::Vec3, "max(%, vec3(0.0))"}}};
        std::vector<std::string> writes;
        if (const auto* outputs = g.find("outputs"); outputs && outputs->is_object()) {
            for (const auto& [name, ref] : outputs->members()) {
                const auto f = fields.find(name);
                if (f == fields.end()) {
                    error(name, "not an output (base_color, alpha, normal, metallic, roughness, occlusion, emission)");
                    continue;
                }
                const Value v = value(ref, name);
                if (v.expr.empty())
                    continue;
                if (v.type == Type::Texture) {
                    error(name, "a texture is not a value: sample it (a texture node)");
                    continue;
                }
                std::string w = f->second.second;
                w.replace(w.find('%'), 1, cast(v, f->second.first));
                writes.push_back("    s." + name + " = " + w + ";");
            }
        } else {
            error("", "'outputs' must be an object");
        }
        if (!out.ok())
            return;
        std::ostringstream glsl;
        glsl << "// Generated by kin's material graph compiler (kin.material-graph/1)" << (out.name.empty() ? "" : ": ")
             << out.name << ". Do not edit: edit the graph.\n#include \"kin_graph.inc.glsl\"\n\n"
             << "void kin_surface(inout KinSurface s) {\n";
        for (const auto& line : _prologue)
            glsl << line << '\n';
        for (const auto& line : _statements)
            glsl << line << '\n';
        for (const auto& line : writes)
            glsl << line << '\n';
        glsl << "}\n";
        out.surface = glsl.str();
    }

    void error(const std::string& node, std::string message) { out.errors.push_back({node, std::move(message)}); }

private:
    static const BuiltIn* built_in(const std::string& name) {
        for (const auto& b : built_ins)
            if (name == b.name)
                return &b;
        return nullptr;
    }

    void parameters(const JsonValue* list) {
        if (!list)
            return;
        if (!list->is_array()) {
            error("", "'parameters' must be an array");
            return;
        }
        u32 used = 0; // floats of shader_params taken
        std::array<bool, 5> slots{};
        for (const auto& p : list->items()) {
            MaterialGraphParameter3D param;
            param.name = p.find("name") ? p.find("name")->as_string() : "";
            const std::string type = p.find("type") ? p.find("type")->as_string() : "float";
            if (!identifier(param.name) || built_in(param.name)) {
                error(param.name, "a parameter's name must be a name, and not a built-in input's");
                continue;
            }
            if (_params.contains(param.name)) {
                error(param.name, "two parameters of one name");
                continue;
            }
            if (type == "texture") {
                static const std::map<std::string, MaterialMap3D> slot_names{
                    {"base", MaterialMap3D::BaseColor},
                    {"metallic_roughness", MaterialMap3D::MetallicRoughness},
                    {"normal", MaterialMap3D::Normal},
                    {"occlusion", MaterialMap3D::Occlusion},
                    {"emissive", MaterialMap3D::Emissive}};
                const std::string slot = p.find("slot") ? p.find("slot")->as_string() : "";
                const auto it = slot_names.find(slot);
                if (it == slot_names.end()) {
                    error(param.name,
                          "a texture needs a slot: base, metallic_roughness, normal, occlusion or emissive");
                    continue;
                }
                if (slots[u32(it->second)]) {
                    error(param.name, "two textures in the " + slot + " slot");
                    continue;
                }
                slots[u32(it->second)] = true;
                param.type = Type::Texture;
                param.slot = it->second;
                param.size = 0;
                const std::string space = p.find("color_space")                    ? p.find("color_space")->as_string()
                                          : (slot == "base" || slot == "emissive") ? "srgb"
                                                                                   : "linear";
                param.srgb = space == "srgb";
                param.data = p.find("role") && p.find("role")->as_string() == "data";
                // The slot's stock reading undone where the texture isn't its map.
                if (param.data)
                    switch (param.slot) {
                    case MaterialMap3D::BaseColor:
                        _prologue.push_back("    s.base_color = vColor.rgb;\n    s.alpha = vColor.a;");
                        break;
                    case MaterialMap3D::MetallicRoughness:
                        _prologue.push_back("    s.metallic = clamp(material.pbr.x, 0.0, 1.0);\n"
                                            "    s.roughness = clamp(material.pbr.y, 0.045, 1.0);");
                        break;
                    case MaterialMap3D::Normal:
                        _prologue.push_back("    s.normal = kin_graph_geometry_normal();");
                        break;
                    case MaterialMap3D::Occlusion:
                        _prologue.push_back("    s.occlusion = 1.0;");
                        break;
                    case MaterialMap3D::Emissive:
                        _prologue.push_back("    s.emission = (material.emission.w > 0.5 ? material.emission.rgb : "
                                            "s.base_color) * material.params.x;");
                        break;
                    default:
                        break;
                    }
            } else {
                const std::map<std::string, std::pair<Type, u32>> types{{"float", {Type::Float, 1}},
                                                                        {"vec2", {Type::Vec2, 2}},
                                                                        {"vec3", {Type::Vec3, 3}},
                                                                        {"vec4", {Type::Vec4, 4}},
                                                                        {"color", {Type::Vec4, 4}}};
                const auto t = types.find(type);
                if (t == types.end()) {
                    error(param.name, "unknown parameter type " + type);
                    continue;
                }
                param.type = t->second.first;
                param.size = t->second.second;
                // vec3 and vec4 start a vec4 of their own, vec2 an even float: each read in one access.
                const u32 align = param.size >= 3 ? 4 : param.size;
                used = (used + align - 1) / align * align;
                if (used + param.size > 16) {
                    error(param.name, "the parameters fill more than Material3D::shader_params' 16 floats");
                    continue;
                }
                param.offset = used;
                used += param.size;
                if (const auto* d = p.find("default")) {
                    if (d->is_number())
                        param.value[0] = f32(d->as_number());
                    else if (d->is_array())
                        for (size_t i = 0; i < std::min<size_t>(4, d->items().size()); ++i)
                            param.value[i] = f32(d->items()[i].as_number());
                }
                const u32 vec = param.offset / 4, first = param.offset % 4;
                std::string expr = "kin_surface_param(" + std::to_string(vec) + ")";
                if (param.size < 4)
                    expr += "." + std::string("xyzw").substr(first, param.size);
                _prologue.push_back("    const " + std::string(glsl_type(param.type)) + " p_" + param.name + " = " +
                                    expr + ";");
            }
            _params[param.name] = out.parameters.size();
            out.parameters.push_back(param);
        }
    }

    // A reference or value, for `who` (the node or output asking).
    Value value(const JsonValue& ref, const std::string& who) {
        if (ref.is_number())
            return make(number(ref.as_number()), Type::Float);
        if (ref.is_array()) {
            const size_t n = ref.items().size();
            if (n < 2 || n > 4) {
                error(who, "a vector value has 2 to 4 numbers");
                return {};
            }
            std::string e = std::string(glsl_type(of_width(int(n)))) + "(";
            for (size_t i = 0; i < n; ++i)
                e += (i ? ", " : "") + number(ref.items()[i].as_number());
            return make(e + ")", of_width(int(n)));
        }
        if (!ref.is_string()) {
            error(who, "an input is a reference (a string) or a value (a number or an array)");
            return {};
        }
        const std::string text = ref.as_string();
        const size_t dot = text.find('.');
        const std::string base = text.substr(0, dot), member = dot == std::string::npos ? "" : text.substr(dot + 1);
        Value v;
        if (base == "surface") {
            for (const auto& f : surface_fields)
                if (member == f.name)
                    return make("s." + member, f.type);
            error(who, "surface has no field '" + member + "'");
            return {};
        } else if (const auto* b = built_in(base)) {
            v = make(b->expr, b->type);
        } else if (const auto p = _params.find(base); p != _params.end()) {
            const auto& param = out.parameters[p->second];
            v = param.type == Type::Texture ? make(base, Type::Texture) : make("p_" + base, param.type);
        } else if (_nodes.contains(base)) {
            v = node(base);
            if (v.expr.empty())
                return {};
        } else {
            error(who, "'" + base + "' is no node, parameter or built-in input");
            return {};
        }
        if (member.empty())
            return v;
        // A swizzle: 1 to 4 of xyzw or rgba, within the value's width.
        if (member.size() > 4 || v.type == Type::Texture) {
            error(who, "'" + text + "': not a swizzle");
            return {};
        }
        const bool rgba = member.find_first_of("rgba") != std::string::npos;
        const std::string set = rgba ? "rgba" : "xyzw";
        for (const char c : member) {
            const size_t k = set.find(c);
            if (k == std::string::npos || int(k) >= width(v.type)) {
                error(who, "'" + text + "': not a swizzle of a " + glsl_type(v.type));
                return {};
            }
        }
        return make("(" + v.expr + ")." + member, of_width(int(member.size())));
    }

    // A node's value: its statement written (once), after its inputs'.
    Value node(const std::string& id) {
        if (const auto done = _values.find(id); done != _values.end())
            return done->second;
        if (_visiting.contains(id)) {
            error(id, "the node feeds itself (a cycle)");
            return {};
        }
        _visiting.insert(id);
        const JsonValue& n = *_nodes.at(id);
        const std::string type = n.find("type") ? n.find("type")->as_string() : "";
        Value result;
        if (type == "constant") {
            const auto* v = n.find("value");
            result = v ? value(*v, id) : Value{};
            if (!v)
                error(id, "a constant needs a 'value'");
        } else if (type == "glsl") {
            result = raw(n, id);
        } else if (const NodeSpec* sp = spec(type)) {
            result = apply(*sp, n, id);
        } else {
            error(id, "unknown node type '" + type + "'");
        }
        _visiting.erase(id);
        if (result.expr.empty())
            return {};
        const std::string name = "n_" + id;
        _statements.push_back("    const " + std::string(glsl_type(result.type)) + " " + name + " = " + result.expr +
                              ";");
        return _values[id] = make(name, result.type);
    }

    Value apply(const NodeSpec& sp, const JsonValue& n, const std::string& id) {
        const JsonValue* given = n.find("inputs");
        if (given && !given->is_object()) {
            error(id, "'inputs' must be an object");
            return {};
        }
        if (given)
            for (const auto& [name, unused] : given->members()) {
                (void)unused;
                if (std::none_of(sp.inputs.begin(), sp.inputs.end(),
                                 [&](const InputSpec& i) { return i.name == name; }))
                    error(id, sp.type + " has no input '" + name + "'");
            }
        std::vector<Value> in;
        for (const auto& i : sp.inputs) {
            const JsonValue* ref = given ? given->find(i.name) : nullptr;
            if (!ref && i.fallback == "-") {
                in.push_back({});
                continue;
            }
            if (!ref && i.fallback.empty()) {
                error(id, sp.type + " needs its input '" + i.name + "'");
                return {};
            }
            Value v;
            if (ref) {
                v = value(*ref, id);
            } else {
                const auto parsed = parse_json(i.fallback);
                v = value(*parsed.value, id);
            }
            if (v.expr.empty())
                return {};
            if (v.type == Type::Texture) {
                error(id, "input '" + i.name + "': a texture is not a value: sample it (a texture node)");
                return {};
            }
            in.push_back(v);
        }
        // The widest of the Widest inputs; the rest cast as their kinds say.
        int w = 1;
        for (size_t k = 0; k < in.size(); ++k)
            if (sp.inputs[k].kind == In::Widest && !in[k].expr.empty())
                w = std::max(w, width(in[k].type));
        const Type widest = of_width(w);
        for (size_t k = 0; k < in.size(); ++k) {
            if (in[k].expr.empty())
                continue;
            switch (sp.inputs[k].kind) {
            case In::Widest:
                in[k] = make(cast(in[k], widest), widest);
                break;
            case In::Float:
                in[k] = make(cast(in[k], Type::Float), Type::Float);
                break;
            case In::Vec2:
                in[k] = make(cast(in[k], Type::Vec2), Type::Vec2);
                break;
            case In::Vec3:
                in[k] = make(cast(in[k], Type::Vec3), Type::Vec3);
                break;
            case In::Vec4:
                in[k] = make(cast(in[k], Type::Vec4), Type::Vec4);
                break;
            case In::MixT:
                if (in[k].type != Type::Float)
                    in[k] = make(cast(in[k], widest), widest);
                break;
            case In::Any:
                break;
            }
        }
        std::string sampler;
        bool srgb = true;
        if (sp.texture) {
            const std::string t = n.find("texture") ? n.find("texture")->as_string() : "";
            const auto p = _params.find(t);
            if (p == _params.end() || out.parameters[p->second].type != Type::Texture) {
                error(id, sp.type + " needs 'texture': a texture parameter's name");
                return {};
            }
            const auto& param = out.parameters[p->second];
            static const char* samplers[] = {"uBase", "uMetalRough", "uNormalMap", "uOcclusion", "uEmissive"};
            sampler = samplers[u32(param.slot)];
            srgb = param.srgb;
        }
        return sp.emit(*this, in, widest, sampler, srgb);
    }

    // A GLSL expression of its own, its inputs by name: {"type": "glsl", "glsl": "a * 2.0 + b", "result": "vec3",
    // "inputs": {"a": ..., "b": ...}}.
    Value raw(const JsonValue& n, const std::string& id) {
        std::string expr = n.find("glsl") ? n.find("glsl")->as_string() : "";
        const std::string result = n.find("result") ? n.find("result")->as_string() : "float";
        static const std::map<std::string, Type> types{
            {"float", Type::Float}, {"vec2", Type::Vec2}, {"vec3", Type::Vec3}, {"vec4", Type::Vec4}};
        if (expr.empty() || !types.contains(result) || expr.find_first_of(";{}") != std::string::npos) {
            error(id, "a glsl node needs 'glsl' (one expression: no ; { }) and 'result' (float, vec2, vec3, vec4)");
            return {};
        }
        std::string body;
        if (const auto* given = n.find("inputs"); given && given->is_object())
            for (const auto& [name, ref] : given->members()) {
                if (!identifier(name)) {
                    error(id, "a glsl input's name must be a name");
                    return {};
                }
                const Value v = value(ref, id);
                if (v.expr.empty())
                    return {};
                // Each input as a local of the expression's own scope (a function would need declaring earlier).
                body += std::string(glsl_type(v.type)) + " " + name + " = " + v.expr + "; ";
            }
        // A block-scoped expression: GLSL has no statement expressions, so the inputs are bound in a lambda-like
        // helper: written as a nested block computing a temporary.
        const std::string temp = "g_" + id;
        _statements.push_back("    " + result + " " + temp + ";\n    { " + body + temp + " = " + result + "(" + expr +
                              "); }");
        return make(temp, types.at(result));
    }

    std::map<std::string, const JsonValue*> _nodes;
    std::map<std::string, size_t> _params;
    std::map<std::string, Value> _values;
    std::set<std::string> _visiting;
    std::vector<std::string> _prologue, _statements;
};

} // namespace

CompiledMaterialGraph3D compile_material_graph3d(std::string_view graph) {
    Compiler c;
    c.compile(graph);
    if (!c.out.ok())
        c.out.surface.clear();
    return std::move(c.out);
}

std::array<f32, 16> material_graph_params3d(const CompiledMaterialGraph3D& graph,
                                            const std::map<std::string, std::array<f32, 4>>& values) {
    std::array<f32, 16> out{};
    for (const auto& p : graph.parameters) {
        if (p.type == GraphType3D::Texture)
            continue;
        const auto it = values.find(p.name);
        const auto& v = it == values.end() ? p.value : it->second;
        for (u32 i = 0; i < p.size; ++i)
            out[p.offset + i] = v[i];
    }
    return out;
}

const std::vector<MaterialGraphNode3D>& material_graph_nodes3d() {
    static const std::vector<MaterialGraphNode3D> list = [] {
        std::vector<MaterialGraphNode3D> l;
        l.push_back({"constant", "a value: a number or 2 to 4 of them ('value')", {}, {}});
        for (const auto& s : specs()) {
            MaterialGraphNode3D n{s.type, s.summary, {}, {}};
            for (const auto& i : s.inputs)
                n.inputs.push_back(i.name);
            l.push_back(std::move(n));
        }
        l.push_back({"glsl", "a GLSL expression of its own ('glsl', 'result', named 'inputs')", {}, {}});
        return l;
    }();
    return list;
}

} // namespace kin
