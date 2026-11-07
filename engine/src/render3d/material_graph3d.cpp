// Material graphs into surface shaders (kin/render3d/material_graph3d.hpp).
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/render3d/material_graph3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/shader_compiler.hpp>
#include <set>
#include <sstream>

extern "C" {
char* kin_zlib_inflate(const char* bytes, int size, int expected, int* out_size);
void kin_import_free(void* pixels);
unsigned char* kin_zlib_deflate(const unsigned char* bytes, int size, int* out_size);
void kin_deflate_free(void* bytes);
}

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
        for (const bool into_world : {true, false})
            s.push_back({into_world ? "object_to_world" : "world_to_object",
                         into_world ? "a point (w 1) or a direction (w 0) in the object's space, in the world's (at a "
                                      "vertex only: the offset)"
                                    : "a point (w 1) or a direction (w 0) in the world, in the object's space (at a "
                                      "vertex only: the offset)",
                         {{"x", In::Vec3}, {"w", In::Float, "1"}},
                         false,
                         [into_world](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                             return make(
                                 std::string(into_world ? "kin_graph_object_to_world(" : "kin_graph_world_to_object(") +
                                     in[0].expr + ", " + in[1].expr + ")",
                                 Type::Vec3);
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
        s.push_back({"append",
                     "a vector of a's components then b's (Unreal's AppendVector)",
                     {{"a", In::Any}, {"b", In::Any}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         const int n = width(in[0].type) + width(in[1].type);
                         if (n > 4)
                             return Value{}; // (more than 4 components: the caller says so)
                         return make(std::string(glsl_type(of_width(n))) + "(" + in[0].expr + ", " + in[1].expr + ")",
                                     of_width(n));
                     }});
        // Textures.
        s.push_back({"unpack_normal",
                     "a normal map's texel as a tangent-space normal (+y up), its slope times strength",
                     {{"texel", In::Vec4}, {"strength", In::Float, "1"}},
                     false,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string&, bool) {
                         return make("kin_graph_unpack_normal(" + in[0].expr + ", " + in[1].expr + ")", Type::Vec3);
                     }});
        s.push_back({"texture",
                     "a texture parameter's texel (vec4)",
                     {{"uv", In::Vec2, "\"uv\""}},
                     true,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string& sampler, bool srgb) {
                         return make("kin_graph_sample(" + sampler + ", " + in[0].expr + ", " +
                                         (srgb ? "true" : "false") + ")",
                                     Type::Vec4);
                     }});
        s.push_back({"texture_equirect",
                     "an equirectangular texture parameter's texel in a world direction (vec4; the environment's "
                     "layout: the centre +x, the top +y, right toward +z)",
                     {{"direction", In::Vec3}},
                     true,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string& sampler, bool srgb) {
                         return make("kin_graph_sample_equirect(" + sampler + ", " + in[0].expr + ", " +
                                         (srgb ? "true" : "false") + ")",
                                     Type::Vec4);
                     }});
        s.push_back({"normal_map",
                     "a normal map's texel as the world normal",
                     {{"uv", In::Vec2, "\"uv\""}, {"strength", In::Float, "1"}},
                     true,
                     [](Compiler&, const std::vector<Value>& in, Type, const std::string& sampler, bool) {
                         return make("kin_graph_tangent_to_world(kin_graph_unpack_normal(kin_graph_sample(" + sampler +
                                         ", " + in[0].expr + ", false), " + in[1].expr + "), " + in[0].expr +
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
    {"object_position", "", Type::Vec3}, // (at a vertex only: vertex_input's)
};
struct Field {
    const char* name;
    Type type;
};
constexpr Field surface_fields[] = {{"base_color", Type::Vec3}, {"alpha", Type::Float},     {"normal", Type::Vec3},
                                    {"metallic", Type::Float},  {"roughness", Type::Float}, {"occlusion", Type::Float},
                                    {"emission", Type::Vec3},   {"backlight", Type::Vec3}};

bool identifier(const std::string& s) {
    if (s.empty() || s.size() > 64 || std::isdigit(static_cast<unsigned char>(s[0])))
        return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

class Compiler {
public:
    CompiledMaterialGraph3D out;
    // The cut-out passes' compile (kin_surface_alpha): their inputs only, the alpha output only. The vertex variants'
    // (kin_surface_offset, `vertex`): a vertex's inputs only, the offset output only.
    explicit Compiler(bool mask = false, bool vertex = false) : _mask(mask || vertex), _vertex(vertex) {}
    std::string alpha_function;  // the mask compile's
    std::string offset_function; // the vertex compile's

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
            {"emission", {Type::Vec3, "max(%, vec3(0.0))"}},
            {"backlight", {Type::Vec3, "max(%, vec3(0.0))"}}};
        std::vector<std::string> writes;
        if (_vertex) { // only the offset, as kin_surface_offset returns it (kin_surface_vertex.inc.glsl)
            const auto* outputs = g.find("outputs");
            const auto* ref = outputs ? outputs->find("offset") : nullptr;
            if (!ref)
                return;
            const Value v = value(*ref, "offset");
            if (v.expr.empty() || !out.ok())
                return;
            std::ostringstream fn;
            fn << "vec3 kin_surface_offset() {\n";
            for (const auto& line : _prologue)
                fn << line << '\n';
            for (const auto& line : _statements)
                fn << line << '\n';
            fn << "    return " << cast(v, Type::Vec3) << ";\n}\n";
            offset_function = fn.str();
            // (A glsl node's frame values at a vertex are its block's; the time the vertex's, as the velocity's
            // previous position takes another.)
            for (const auto& [from, to] : std::initializer_list<std::pair<const char*, const char*>>{
                     {"frame.time.x", "kin_v_time"},
                     {"frame.eye", "kin_vs.eye"},
                     {"frame.cascade_forward", "kin_vs.forward"}})
                for (size_t at; (at = offset_function.find(from)) != std::string::npos;)
                    offset_function.replace(at, std::strlen(from), to);
            for (const char* fragment_only : {"frame.", "vWorld", "vColor", "vUV", "gl_FragCoord", "dFd", "fwidth"})
                if (offset_function.find(fragment_only) != std::string::npos) {
                    error("offset", std::string("reads what a vertex does not have (") + fragment_only + ")");
                    offset_function.clear();
                    return;
                }
            return;
        }
        if (_mask) { // only the alpha, as kin_surface_alpha returns it
            const auto* outputs = g.find("outputs");
            const auto* ref = outputs ? outputs->find("alpha") : nullptr;
            if (!ref)
                return;
            const Value v = value(*ref, "alpha");
            if (v.expr.empty() || !out.ok())
                return;
            std::ostringstream fn;
            fn << "float kin_surface_alpha(vec2 uv, vec3 position, float vertex_alpha) {\n";
            for (const auto& line : _prologue)
                fn << line << '\n';
            for (const auto& line : _statements)
                fn << line << '\n';
            fn << "    return clamp(" << cast(v, Type::Float) << ", 0.0, 1.0);\n}\n";
            alpha_function = fn.str();
            // (Time in the cut-out passes is the cut-out block's.)
            for (size_t at; (at = alpha_function.find("frame.time.x")) != std::string::npos;)
                alpha_function.replace(at, 12, "material.cutoff.z");
            return;
        }
        if (const auto* outputs = g.find("outputs"); outputs && outputs->is_object()) {
            for (const auto& [name, ref] : outputs->members()) {
                if (name == "offset")
                    continue; // (the vertex variants', compiled below)
                const auto f = fields.find(name);
                if (f == fields.end()) {
                    error(name, "not an output (base_color, alpha, normal, metallic, roughness, occlusion, emission, "
                                "backlight, offset)");
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
        // The alpha again, for the cut-out passes, where they have what it needs.
        std::string cutout;
        if (g.find("outputs") && g.find("outputs")->find("alpha")) {
            Compiler mask(true);
            mask.compile(text);
            if (mask.out.ok() && !mask.alpha_function.empty()) {
                cutout = mask.alpha_function;
                out.cutout = true;
            } else if (!mask.out.errors.empty()) {
                const auto& e = mask.out.errors.front();
                out.warnings.push_back({e.node, e.message + ": the shadow, occluder, pick and pre-passes cut this "
                                                            "material by its base map's alpha"});
            }
        }
        // The offset, for the vertex variants (World Position Offset): what a vertex has.
        std::string offset;
        if (g.find("outputs") && g.find("outputs")->find("offset")) {
            Compiler vertex(false, true);
            vertex.compile(text);
            if (vertex.out.ok() && !vertex.offset_function.empty()) {
                offset = vertex.offset_function;
                out.offset = true;
            } else if (!vertex.out.errors.empty()) {
                const auto& e = vertex.out.errors.front();
                out.warnings.push_back({e.node, e.message + ": the offset is not drawn (a vertex cannot compute it)"});
            }
        }
        std::ostringstream glsl;
        glsl << "// Generated by kin's material graph compiler (kin.material-graph/1)" << (out.name.empty() ? "" : ": ")
             << out.name << ". Do not edit: edit the graph.\n";
        if (!offset.empty())
            glsl << "#if defined(KIN_SURFACE_VERTEX)\n" << offset;
        if (!cutout.empty())
            glsl << (offset.empty() ? "#ifdef KIN_SURFACE_MASK\n" : "#elif defined(KIN_SURFACE_MASK)\n") << cutout;
        if (!offset.empty() || !cutout.empty())
            glsl << "#else\n";
        glsl << "#include \"kin_graph.inc.glsl\"\n\n"
             << "void kin_surface(inout KinSurface s) {\n";
        for (const auto& line : _prologue)
            glsl << line << '\n';
        for (const auto& line : _statements)
            glsl << line << '\n';
        for (const auto& line : writes)
            glsl << line << '\n';
        glsl << "}\n";
        if (!offset.empty() || !cutout.empty())
            glsl << "#endif\n";
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
    // A built-in input's value at a vertex (kin_surface_vertex.inc.glsl), or none.
    static std::optional<Value> vertex_input(const std::string& name) {
        if (name == "uv")
            return make("kin_uv()", Type::Vec2);
        if (name == "position")
            return make("kin_position()", Type::Vec3);
        if (name == "normal")
            return make("kin_graph_geometry_normal()", Type::Vec3);
        if (name == "view")
            return make("kin_view()", Type::Vec3);
        if (name == "vertex_color")
            return make("kin_v_color", Type::Vec4);
        if (name == "time")
            return make("kin_v_time", Type::Float);
        if (name == "camera_distance")
            return make("length(kin_vs.eye.xyz - kin_v_world)", Type::Float);
        if (name == "object_position")
            return make("kin_object_position()", Type::Vec3);
        return std::nullopt;
    }
    // A built-in input's value in the cut-out passes (kin_surface_alpha's arguments and the cut-out block), or none.
    static std::optional<Value> mask_input(const std::string& name) {
        if (name == "uv")
            return make("uv", Type::Vec2);
        if (name == "position")
            return make("position", Type::Vec3);
        if (name == "vertex_color")
            return make("vec4(1.0, 1.0, 1.0, vertex_alpha)", Type::Vec4);
        if (name == "time")
            return make("material.cutoff.z", Type::Float);
        return std::nullopt;
    }

    void parameters(const JsonValue* list) {
        if (!list)
            return;
        if (!list->is_array()) {
            error("", "'parameters' must be an array");
            return;
        }
        std::array<bool, surface_param_count3d> taken{}; // the floats of shader_params taken
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
                if (slot == "layers") { // a layer of the graph maps (Material3D::graph_maps)
                    const i64 layer = p.find("layer") ? p.find("layer")->as_int(-1) : -1;
                    if (layer < 0 || layer >= i64(max_graph_map_layers3d)) {
                        error(param.name, "a texture in the layers needs 'layer': 0 to " +
                                              std::to_string(max_graph_map_layers3d - 1));
                        continue;
                    }
                    if (std::ranges::any_of(out.parameters, [&](const auto& q) { return q.layer == u32(layer); })) {
                        error(param.name, "two textures in layer " + std::to_string(layer));
                        continue;
                    }
                    param.layer = u32(layer);
                    param.slot = MaterialMap3D::Count;
                } else if (it == slot_names.end()) {
                    error(param.name,
                          "a texture needs a slot: base, metallic_roughness, normal, occlusion, emissive or layers");
                    continue;
                } else if (slots[u32(it->second)]) {
                    error(param.name, "two textures in the " + slot + " slot");
                    continue;
                } else {
                    slots[u32(it->second)] = true;
                    param.slot = it->second;
                }
                param.type = Type::Texture;
                param.size = 0;
                const std::string space = p.find("color_space")                    ? p.find("color_space")->as_string()
                                          : (slot == "base" || slot == "emissive") ? "srgb"
                                                                                   : "linear";
                param.srgb = space == "srgb";
                param.data = p.find("role") && p.find("role")->as_string() == "data";
                // The slot's stock reading undone where the texture isn't its map.
                if (param.data && !_mask)
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
                // vec3 and vec4 start a vec4 of their own, vec2 an even float: each read in one access. The first
                // place free so (a float in a vec3's fourth, or after a lone float), in the order they are declared.
                const u32 align = param.size >= 3 ? 4 : param.size;
                u32 at = surface_param_count3d;
                for (u32 o = 0; o + param.size <= surface_param_count3d && at == surface_param_count3d; o += align) {
                    bool free = true;
                    for (u32 k = o; k < o + param.size; ++k)
                        free = free && !taken[k];
                    if (free)
                        at = o;
                }
                if (at == surface_param_count3d) {
                    error(param.name, "the parameters fill more than Material3D::shader_params' " +
                                          std::to_string(surface_param_count3d) + " floats");
                    continue;
                }
                param.offset = at;
                for (u32 k = at; k < at + param.size; ++k)
                    taken[k] = true;
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
            if (_vertex) {
                error(who, "surface." + member + " is not at a vertex");
                return {};
            }
            if (_mask) {
                if (member == "alpha")
                    return make("(vertex_alpha * texture(uBase, uv).a)", Type::Float);
                error(who, "surface." + member + " is not in the cut-out passes");
                return {};
            }
            for (const auto& f : surface_fields)
                if (member == f.name)
                    return make("s." + member, f.type);
            error(who, "surface has no field '" + member + "'");
            return {};
        } else if (const auto* b = built_in(base)) {
            if (_vertex) {
                const auto m = vertex_input(base);
                if (!m) {
                    error(who, "'" + base + "' is not at a vertex");
                    return {};
                }
                v = *m;
            } else if (_mask) {
                const auto m = mask_input(base);
                if (!m) {
                    error(who, "'" + base + "' is not in the cut-out passes");
                    return {};
                }
                v = *m;
            } else {
                if (!*b->expr) {
                    error(who, "'" + base + "' is only at a vertex (in the offset)");
                    return {};
                }
                v = make(b->expr, b->type);
            }
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
        if (!_vertex && (type == "object_to_world" || type == "world_to_object")) {
            error(id, "a " + type +
                          " node is only at a vertex (in the offset: the fragments don't have the object's "
                          "placing)");
            _visiting.erase(id);
            return {};
        }
        if (_vertex && (type == "normal_map" || type == "tangent_to_world" || type == "fresnel")) {
            error(id, "a " + type + " node is not at a vertex");
            _visiting.erase(id);
            return {};
        }
        if (_mask && !_vertex &&
            (type == "normal_map" || type == "tangent_to_world" || type == "triplanar" || type == "fresnel" ||
             type == "glsl" || type == "unpack_normal")) {
            error(id, "a " + type + " node is not in the cut-out passes");
            _visiting.erase(id);
            return {};
        }
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
            if (param.layered()) { // (kin_graph.inc.glsl's overloads take the array and the layer)
                if (_mask && !_vertex) {
                    error(id, "a texture in the graph maps' layers is not in the cut-out passes");
                    return {};
                }
                sampler = "uGraphMaps, " + std::to_string(param.layer) + ".0";
            } else {
                sampler = samplers[u32(param.slot)];
            }
            srgb = param.srgb;
        }
        Value made = sp.emit(*this, in, widest, sampler, srgb);
        if (made.expr.empty())
            error(id, sp.type + ": its inputs make no value (" +
                          (sp.type == "append" ? std::string("more than 4 components") : std::string("types")) + ")");
        return made;
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

    bool _mask = false;   // the cut-outs' compile, or the vertex's (both: no surface, a restricted set of inputs)
    bool _vertex = false; // the vertex's
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

std::array<f32, surface_param_count3d>
material_graph_params3d(const CompiledMaterialGraph3D& graph, const std::map<std::string, std::array<f32, 4>>& values) {
    std::array<f32, surface_param_count3d> out{};
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

namespace {
const char* type_name(GraphType3D t) {
    switch (t) {
    case GraphType3D::Float:
        return "float";
    case GraphType3D::Vec2:
        return "vec2";
    case GraphType3D::Vec3:
        return "vec3";
    case GraphType3D::Vec4:
        return "vec4";
    case GraphType3D::Texture:
        return "texture";
    }
    return "float";
}
constexpr const char* slot_names[] = {"base", "metallic_roughness", "normal", "occlusion", "emissive"};
} // namespace

std::string write_material_graph_layout3d(const CompiledMaterialGraph3D& graph) {
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object().field("schema", "kin.material-graph-layout/1").field("name", graph.name);
    json.key("parameters").begin_array();
    for (const auto& p : graph.parameters) {
        json.begin_object().field("name", p.name).field("type", type_name(p.type));
        if (p.type == GraphType3D::Texture) {
            if (p.layered())
                json.field("slot", "layers").field("layer", p.layer);
            else
                json.field("slot", slot_names[u32(p.slot)]);
            json.field("srgb", p.srgb).field("data", p.data);
        } else {
            json.field("offset", p.offset).field("size", p.size);
            json.key("value").begin_array();
            for (u32 i = 0; i < p.size; ++i)
                json.value(f64(p.value[i]));
            json.end_array();
        }
        json.end_object();
    }
    json.end_array().end_object();
    return out.str();
}

std::optional<CompiledMaterialGraph3D> read_material_graph_layout3d(std::string_view text, std::string* why) {
    const auto fail = [&](std::string w) -> std::optional<CompiledMaterialGraph3D> {
        if (why)
            *why = std::move(w);
        return std::nullopt;
    };
    const auto parsed = parse_json(text);
    if (!parsed.ok() || !parsed.value->find("schema") ||
        parsed.value->find("schema")->as_string() != "kin.material-graph-layout/1")
        return fail("not a kin.material-graph-layout/1 layout");
    CompiledMaterialGraph3D g;
    g.name = parsed.value->find("name") ? parsed.value->find("name")->as_string() : "";
    const auto* list = parsed.value->find("parameters");
    if (!list || !list->is_array())
        return fail("a layout without parameters");
    for (const auto& item : list->items()) {
        MaterialGraphParameter3D p;
        p.name = item.find("name") ? item.find("name")->as_string() : "";
        const std::string type = item.find("type") ? item.find("type")->as_string() : "";
        bool known = false;
        for (const auto t :
             {GraphType3D::Float, GraphType3D::Vec2, GraphType3D::Vec3, GraphType3D::Vec4, GraphType3D::Texture})
            if (type == type_name(t))
                p.type = t, known = true;
        if (!known || p.name.empty())
            return fail("a parameter of unknown type or no name");
        if (p.type == GraphType3D::Texture) {
            const std::string slot = item.find("slot") ? item.find("slot")->as_string() : "";
            const auto* at = std::find(std::begin(slot_names), std::end(slot_names), slot);
            if (slot == "layers") {
                const i64 layer = item.int_at("layer", -1);
                if (layer < 0 || layer >= i64(max_graph_map_layers3d))
                    return fail("a texture in the layers without its layer");
                p.layer = u32(layer);
                p.slot = MaterialMap3D::Count;
            } else if (at == std::end(slot_names))
                return fail("a texture of unknown slot");
            else
                p.slot = MaterialMap3D(at - std::begin(slot_names));
            p.srgb = item.bool_at("srgb", true);
            p.data = item.bool_at("data", false);
            p.size = 0;
        } else {
            p.offset = u32(item.int_at("offset", 0));
            p.size = u32(item.int_at("size", 1));
            if (p.offset + p.size > surface_param_count3d || p.size < 1 || p.size > 4)
                return fail("a parameter beyond shader_params");
            if (const auto* v = item.find("value"); v && v->is_array())
                for (size_t i = 0; i < std::min<size_t>(4, v->items().size()); ++i)
                    p.value[i] = f32(v->items()[i].as_number());
        }
        g.parameters.push_back(p);
    }
    return g;
}

// KINGRAPH, the version (4), the payload's size and its FNV-1a hash, then the payload deflated (zlib): the number of
// variants, the layout's bytes, then each variant's, each after its length (little-endian u32). Version 3 is the same
// with 16 floats of parameters (the merged draws' variants read them 4 vec4s a copy: not used); version 2 without
// the number (8 variants); version 1 that payload as it is.
static u32 fnv1a(std::span<const u8> bytes) {
    u32 h = 2166136261u;
    for (const u8 b : bytes)
        h = (h ^ b) * 16777619u;
    return h;
}

std::vector<u8> pack_cooked_material_graph3d(const CookedMaterialGraph3D& cooked) {
    std::vector<u8> payload;
    const auto u32le = [](std::vector<u8>& to, u32 v) {
        for (int i = 0; i < 4; ++i)
            to.push_back(u8(v >> (8 * i)));
    };
    u32le(payload, surface_variant_count3d);
    const std::string layout = write_material_graph_layout3d(cooked.graph);
    u32le(payload, u32(layout.size()));
    payload.insert(payload.end(), layout.begin(), layout.end());
    for (const auto& v : cooked.spirv) {
        u32le(payload, u32(v.size()));
        payload.insert(payload.end(), v.begin(), v.end());
    }
    std::vector<u8> out{'K', 'I', 'N', 'G', 'R', 'A', 'P', 'H'};
    int size = 0;
    unsigned char* deflated = kin_zlib_deflate(payload.data(), int(payload.size()), &size);
    if (!deflated) // (only out of memory)
        return {};
    u32le(out, 4);
    u32le(out, u32(payload.size()));
    u32le(out, fnv1a(payload));
    out.insert(out.end(), deflated, deflated + size);
    kin_deflate_free(deflated);
    return out;
}

std::optional<CookedMaterialGraph3D> unpack_cooked_material_graph3d(std::span<const u8> bytes, std::string* why) {
    const auto fail = [&](std::string w) -> std::optional<CookedMaterialGraph3D> {
        if (why)
            *why = std::move(w);
        return std::nullopt;
    };
    const auto u32_at = [](std::span<const u8> b, size_t at) {
        return u32(b[at]) | u32(b[at + 1]) << 8 | u32(b[at + 2]) << 16 | u32(b[at + 3]) << 24;
    };
    if (bytes.size() < 16 || std::memcmp(bytes.data(), "KINGRAPH", 8) != 0)
        return fail("not a cooked material graph");
    const u32 version = u32_at(bytes, 8);
    std::vector<u8> inflated;
    std::span<const u8> payload;
    if (version == 1) {
        payload = bytes.subspan(12);
    } else if (version >= 2 && version <= 4) {
        if (bytes.size() < 20)
            return fail("a cooked material graph cut short");
        const u32 expected = u32_at(bytes, 12), hash = u32_at(bytes, 16);
        if (expected > 256u * 1024 * 1024)
            return fail("a cooked material graph too large");
        int size = 0;
        char* out = kin_zlib_inflate(reinterpret_cast<const char*>(bytes.data() + 20), int(bytes.size() - 20),
                                     int(expected), &size);
        if (!out || u32(size) != expected || fnv1a({reinterpret_cast<const u8*>(out), size_t(size)}) != hash) {
            if (out)
                kin_import_free(out);
            return fail("a cooked material graph cut short (it does not inflate)");
        }
        inflated.assign(out, out + size);
        kin_import_free(out);
        payload = inflated;
    } else {
        return fail("a cooked material graph of another version");
    }
    size_t at = 0;
    const auto next = [&]() -> std::optional<u32> {
        if (at + 4 > payload.size())
            return std::nullopt;
        const u32 v = u32_at(payload, at);
        at += 4;
        return v;
    };
    // Versions 1 and 2 hold the 8 variants every surface needs; 3 and 4 say how many (the merged draws' too).
    const auto count = version >= 3 ? next() : std::optional<u32>{surface_required_variants3d};
    if (!count || *count < surface_required_variants3d || *count > surface_variant_count3d)
        return fail("a cooked material graph of an unknown number of variants");
    const auto layout_size = next();
    if (!layout_size || at + *layout_size > payload.size())
        return fail("a cooked material graph cut short");
    CookedMaterialGraph3D cooked;
    std::string layout_why;
    auto layout =
        read_material_graph_layout3d({reinterpret_cast<const char*>(payload.data() + at), *layout_size}, &layout_why);
    if (!layout)
        return fail(layout_why);
    cooked.graph = std::move(*layout);
    at += *layout_size;
    for (u32 i = 0; i < *count; ++i) {
        auto& v = cooked.spirv[i];
        const auto n = next();
        if (n && *n == 0 && i >= surface_required_variants3d)
            continue; // an optional variant it lacks
        if (!n || *n < 20 || at + *n > payload.size() || payload[at] != 3 || payload[at + 1] != 2 ||
            payload[at + 2] != 35 || payload[at + 3] != 7)
            return fail("a cooked material graph whose SPIR-V is cut short or not SPIR-V");
        v.assign(payload.begin() + std::ptrdiff_t(at), payload.begin() + std::ptrdiff_t(at + *n));
        at += *n;
    }
    // Version 3's merged-draw variants read the parameter table at its old stride (16 floats a copy): left out, so
    // those draws go unmerged; its other variants read the start of today's blocks, as before.
    if (version == 3)
        for (u32 i = u32(SurfaceVariant3D::Material); i <= u32(SurfaceVariant3D::MaterialLightsFog); ++i)
            cooked.spirv[i].clear();
    return cooked;
}

std::optional<CookedMaterialGraph3D> cook_material_graph3d(std::string_view text, std::string* why) {
    const auto fail = [&](std::string w) -> std::optional<CookedMaterialGraph3D> {
        if (why)
            *why = std::move(w);
        return std::nullopt;
    };
    CookedMaterialGraph3D cooked;
    cooked.graph = compile_material_graph3d(text);
    if (!cooked.graph.ok()) {
        std::string all;
        for (const auto& e : cooked.graph.errors)
            all += (all.empty() ? "" : "\n") + (e.node.empty() ? std::string() : e.node + ": ") + e.message;
        return fail(all);
    }
    static std::atomic<u32> counter{0};
    const std::string stem =
        "kin-graph-cook-" + std::to_string(counter++) + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&cooked));
    std::string w;
    bool ok = true;
    for (u32 i = 0; i < surface_variant_count3d && ok; ++i) {
        // The cut-outs' variants only for a graph whose alpha they can compute; the vertex ones for one with an offset.
        const bool vertex = i >= surface_vertex_variant3d;
        if ((i >= surface_mask_variant3d && !vertex && !cooked.graph.cutout) || (vertex && !cooked.graph.offset))
            continue;
        const std::filesystem::path file = std::filesystem::temp_directory_path() /
                                           (stem + "-" + std::to_string(i) + (vertex ? ".vert.glsl" : ".frag.glsl"));
        {
            std::ofstream out(file, std::ios::binary);
            out << surface_shader_source(cooked.graph.surface, SurfaceVariant3D(i));
        }
        auto spirv = compile_glsl(file, vertex ? ShaderStage::Vertex : ShaderStage::Fragment, &w,
                                  surface_variant_defines(SurfaceVariant3D(i)), true);
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        ok = spirv.has_value();
        if (ok)
            cooked.spirv[i] = std::move(*spirv);
    }
    if (!ok)
        return fail(w);
    cooked.graph.surface.clear();
    return cooked;
}

} // namespace kin
