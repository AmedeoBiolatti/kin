#pragma once

// What the importers of other engines' materials share (material_import3d.cpp, material_import_unity3d.cpp): where a
// map is read from (in place inside the content, else copied), the maps made (packed, recombined), decoding with a
// note where it can't be done, the notes, and the .kinmaterials written after it is read back.

#include "image_decode3d.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <kin/assets/image.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <kin/render3d/material_import3d.hpp>
#include <kin/renderer/color.hpp>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace kin::material_import {

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

inline u8 byte(f32 v) {
    return u8(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// A channel selector's value at a pixel: 0 red, 1 green, 2 blue, 3 alpha, 4 grayscale (the mean of RGB).
inline u8 channel(const Image& image, size_t pixel, int which) {
    const u8* p = &image.rgba[pixel * 4];
    return which >= 0 && which < 4 ? p[which] : u8((u32(p[0]) + p[1] + p[2] + 1) / 3);
}

// `image`'s texel nearest to (x, y) of a picture of `size`.
inline size_t texel(const Image& image, Vec2i size, int x, int y) {
    return size_t((y * image.size.y / size.y) * image.size.x + x * image.size.x / size.x);
}

class Context {
public:
    Context(ImportedMaterial3D& imported, const MaterialImportOptions3D& options, std::string stem)
        : imported(imported), options(options), content(std::filesystem::absolute(options.content).lexically_normal()),
          out(std::filesystem::absolute(options.out).lexically_normal()), stem(std::move(stem)) {}

    ImportedMaterial3D& imported;
    const MaterialImportOptions3D& options;
    const std::filesystem::path content, out;
    const std::string stem;

    void note(std::string what) {
        auto& notes = imported.notes;
        if (std::find(notes.begin(), notes.end(), what) == notes.end())
            notes.push_back(std::move(what));
    }
    bool inside(const std::filesystem::path& file) const {
        const auto relative = file.lexically_relative(content);
        return !relative.empty() && *relative.begin() != "..";
    }
    // Where kin reads a file: where it is (inside the content), else a copy in `out`/textures.
    std::filesystem::path place(const std::filesystem::path& file) {
        if (inside(file))
            return file.lexically_relative(content);
        const auto copy = out / "textures" / file.filename();
        if (options.write) {
            std::filesystem::create_directories(copy.parent_path());
            std::filesystem::copy_file(file, copy, std::filesystem::copy_options::overwrite_existing);
        }
        imported.written.push_back(copy);
        return copy.lexically_relative(content);
    }
    // A map made by the import, as `out`/textures/<stem><suffix>.png.
    std::filesystem::path made(const std::string& suffix, const Image& image) {
        const auto file = out / "textures" / (stem + suffix + ".png");
        if (options.write && !save_image(image, file))
            throw std::runtime_error("cannot write " + file.string());
        imported.written.push_back(file);
        return file.lexically_relative(content);
    }
    static std::string extension(const std::filesystem::path& file) {
        std::string e = file.extension().string();
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return e;
    }
    // A texture's pixels, for what is made from it; none (said) where they can't be read: a GPU-compressed file (kin
    // uploads those as they are, but does not decode them) or a format kin's images don't read.
    std::optional<Image> load(const std::filesystem::path& file, const std::string& role) {
        if (const auto it = _decoded.find(file); it != _decoded.end())
            return it->second;
        return _decoded[file] = decode(file, role);
    }
    std::optional<Image> decode(const std::filesystem::path& file, const std::string& role) {
        const std::string e = extension(file);
        if (e == ".dds" || e == ".ktx" || e == ".ktx2") {
            note(role + ": " + file.filename().string() +
                 " is GPU-compressed (kin uploads those as they are, but does not decode them to repack): left out, "
                 "its factor kept");
            return std::nullopt;
        }
        Image image;
        std::string why = "not a picture kin reads";
        if (e == ".tif" || e == ".tiff" || e == ".psd") {
            // Formats kin's images (SDL_image) don't read: the import's own decoder.
            const std::string bytes = read_file(file);
            if (auto decoded = decode_extra_image({reinterpret_cast<const u8*>(bytes.data()), bytes.size()}, &why))
                image = std::move(*decoded);
        } else {
            try {
                image = load_image(file);
            } catch (const std::exception& error) {
                why = error.what();
            }
        }
        if (!image.valid()) {
            note(role + ": " + file.filename().string() + " can't be decoded (" + why + "): left out");
            return std::nullopt;
        }
        return image;
    }
    // A map kin reads as it is: in place or copied when kin reads its format, else decoded into a PNG (said when
    // neither can be done: none then).
    std::optional<std::filesystem::path> readable(const std::filesystem::path& file, const std::string& role) {
        const std::string e = extension(file);
        if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" || e == ".dds" || e == ".ktx2")
            return place(file);
        if (const auto image = load(file, role))
            return made("_" + file.stem().string(), *image);
        return std::nullopt;
    }

    std::map<std::filesystem::path, std::optional<Image>> _decoded; // each file decoded once a material

    // The .kinmaterials, read back by kin's parser before it is written.
    void finish() {
        imported.file = out / (stem + ".kinmaterials");
        const std::string text = write_materials3d_asset(imported.asset);
        if (const auto check = parse_materials3d_asset(text, imported.file); !check)
            throw std::runtime_error("the import does not read back: " +
                                     (check.diagnostics.empty() ? std::string("?") : check.diagnostics.front()));
        if (options.write) {
            std::filesystem::create_directories(out);
            std::ofstream file(imported.file, std::ios::binary);
            file << text;
            if (!file)
                throw std::runtime_error("cannot write " + imported.file.string());
        }
    }
};

// A kin.material-graph/1 graph made by an import (the other engines' graphs: Tier 2): parameters, nodes (each a type,
// inputs as JSON text: a reference "\"n3.rgb\"" or a value "0.5", "[1, 0, 0]"; and fields such as "texture"), outputs.
class GraphBuilder {
public:
    std::string name;
    struct Param {
        std::string name, type; // float, vec2, vec3, vec4, color, texture
        std::array<f32, 4> value{};
        std::string slot; // a texture's
        bool srgb = false;
    };
    std::vector<Param> params;
    std::map<std::string, std::string> outputs; // output -> JSON text

    // A node; its id ("n<k>") as a reference's JSON text, with `member` ("rgb", "x") after a dot.
    // Its width (1 to 4) noted as kin's compiler makes it (the widest input; fixed for some types).
    std::string add(const std::string& type, std::vector<std::pair<std::string, std::string>> inputs,
                    std::vector<std::pair<std::string, std::string>> fields = {}) {
        int w = 1;
        for (const auto& [k, v] : inputs)
            if (!(type == "mix" && k == "t"))
                w = std::max(w, width(v));
        static const std::map<std::string, int> fixed{
            {"dot", 1},         {"length", 1},         {"distance", 1},         {"cross", 3},
            {"texture", 4},     {"unpack_normal", 3},  {"tangent_to_world", 3}, {"fresnel", 1},
            {"noise_value", 1}, {"noise_gradient", 1}, {"noise_cellular", 1},   {"fbm", 1},
            {"checker", 1},     {"normal_map", 3},     {"triplanar", 4},        {"reflect", 3},
            {"uv_panner", 2},   {"uv_rotate", 2}};
        if (const auto f = fixed.find(type); f != fixed.end())
            w = f->second;
        if (type == "combine")
            w = int(inputs.size());
        if (type == "remap" && !inputs.empty())
            w = width(inputs.front().second);
        if (type == "append" && inputs.size() == 2)
            w = std::min(4, width(inputs[0].second) + width(inputs[1].second));
        for (const auto& [k, v] : fields)
            if (type == "glsl" && k == "result")
                w = v == "\"float\"" || v.size() < 6 ? 1 : v[4] - '0';
        if (type == "constant")
            for (const auto& [k, v] : fields)
                if (k == "value")
                    w = width(v);
        const std::string id = "n" + std::to_string(_nodes.size());
        _nodes.push_back({id, type, std::move(inputs), std::move(fields)});
        widths[id] = w;
        return quote(id);
    }
    std::map<std::string, int> widths; // the nodes' (by id)

    // A value's width (1 to 4) by its JSON text: a number, an array, a reference (a node, a swizzle of one, a
    // built-in, a parameter).
    int width(const std::string& v) const {
        if (v.empty() || v.front() == '[')
            return v.empty() ? 1 : 1 + int(std::count(v.begin(), v.end(), ','));
        if (v.front() != '"' || v.size() < 3)
            return 1;
        const std::string body = v.substr(1, v.size() - 2);
        if (const size_t dot = body.find('.'); dot != std::string::npos) {
            const std::string m = body.substr(dot + 1);
            if (m.find_first_not_of("xyzw") == std::string::npos)
                return int(m.size());
            static const std::map<std::string, int> fields{{"base_color", 3}, {"alpha", 1},     {"normal", 3},
                                                           {"metallic", 1},   {"roughness", 1}, {"occlusion", 1},
                                                           {"emission", 3}};
            const auto f = fields.find(m);
            return f == fields.end() ? 4 : f->second;
        }
        static const std::map<std::string, int> builtins{{"uv", 2},     {"position", 3},       {"normal", 3},
                                                         {"view", 3},   {"vertex_color", 4},   {"time", 1},
                                                         {"screen", 2}, {"camera_distance", 1}};
        if (const auto it = builtins.find(body); it != builtins.end())
            return it->second;
        if (const auto it = widths.find(body); it != widths.end())
            return it->second;
        for (const auto& p : params)
            if (p.name == body)
                return p.type == "float" ? 1 : p.type == "vec2" ? 2 : p.type == "vec3" ? 3 : 4;
        return 4;
    }
    // `v` cut to its first `n` components (where it is wider).
    std::string fit(const std::string& v, int n) {
        if (n < 1 || width(v) <= n)
            return v;
        if (v.front() == '"')
            return member(v, std::string("xyzw").substr(0, size_t(n)));
        if (n == 1)
            return component(v, 0);
        std::string out = "[";
        for (int k = 0; k < n; ++k)
            out += (k ? ", " : "") + component(v, k);
        return out + "]";
    }
    // A component of a value's JSON text (an array's; a number is its own).
    static std::string component(const std::string& v, int k) {
        if (v.empty() || v.front() != '[')
            return v;
        std::vector<std::string> parts;
        std::string cur;
        for (const char c : v.substr(1, v.size() - 2)) {
            if (c == ',') {
                parts.push_back(cur);
                cur.clear();
            } else if (c != ' ') {
                cur += c;
            }
        }
        parts.push_back(cur);
        return k >= 0 && k < int(parts.size()) ? parts[size_t(k)] : "0";
    }
    static std::string quote(const std::string& s) { return "\"" + s + "\""; }
    // A swizzle `m` (of xyzw) of a reference: composed with one it has ("n3.xyz" then "xy": "n3.xy"); a field
    // ("surface.normal") passed through a node first, as references take one member.
    std::string member(const std::string& ref, const std::string& m) {
        if (ref.size() < 3 || ref.front() != '"')
            return ref;
        const std::string body = ref.substr(1, ref.size() - 2);
        const size_t dot = body.find('.');
        if (dot == std::string::npos)
            return quote(body + "." + m);
        const std::string base = body.substr(0, dot), have = body.substr(dot + 1);
        const bool swizzle = have.find_first_not_of("xyzw") == std::string::npos;
        if (!swizzle)
            return member(add("add", {{"a", ref}, {"b", "0"}}), m);
        std::string composed;
        for (const char c : m) {
            const size_t k = std::string("xyzw").find(c);
            composed += k < have.size() ? have[k] : have.back();
        }
        return quote(base + "." + composed);
    }
    static std::string number(f64 v) {
        char text[32];
        std::snprintf(text, sizeof text, "%.9g", v);
        return text;
    }
    static std::string vector(std::span<const f32> v) {
        std::string out = "[";
        for (size_t i = 0; i < v.size(); ++i)
            out += (i ? ", " : "") + number(v[i]);
        return out + "]";
    }
    // The texture parameters in kin's five map slots: a normal map (by `normal`) in the normal slot, the rest in
    // order. False when there are more than five.
    template <class Normal> bool assign_slots(Normal&& normal) {
        static const char* names[] = {"base", "normal", "metallic_roughness", "occlusion", "emissive"};
        std::array<bool, 5> taken{};
        for (auto& p : params) {
            if (p.type != "texture")
                continue;
            u32 slot = normal(p) && !taken[1] ? 1 : 5;
            for (u32 k = 0; k < 5 && slot == 5; ++k)
                if (!taken[k] && k != 1)
                    slot = k;
            if (slot == 5 && !taken[1])
                slot = 1;
            if (slot == 5)
                return false;
            taken[slot] = true;
            p.slot = names[slot];
        }
        return true;
    }
    bool has_param(const std::string& n) const {
        return std::any_of(params.begin(), params.end(), [&](const Param& p) { return p.name == n; });
    }

    std::string write() const {
        std::string out = "{\"schema\": \"kin.material-graph/1\", \"name\": " + quote(name) + ",\n \"parameters\": [";
        for (size_t i = 0; i < params.size(); ++i) {
            const auto& p = params[i];
            out += std::string(i ? ",\n  " : "\n  ") + "{\"name\": " + quote(p.name) + ", \"type\": " + quote(p.type);
            if (p.type == "texture")
                out += ", \"slot\": " + quote(p.slot) + ", \"color_space\": " + quote(p.srgb ? "srgb" : "linear") +
                       ", \"role\": \"data\"";
            else {
                const size_t n = p.type == "float" ? 1 : p.type == "vec2" ? 2 : p.type == "vec3" ? 3 : 4;
                out += ", \"default\": " + (n == 1 ? number(p.value[0]) : vector({p.value.data(), n}));
            }
            out += "}";
        }
        out += "],\n \"nodes\": [";
        for (size_t i = 0; i < _nodes.size(); ++i) {
            const auto& n = _nodes[i];
            out += std::string(i ? ",\n  " : "\n  ") + "{\"id\": " + quote(n.id) + ", \"type\": " + quote(n.type);
            for (const auto& [k, v] : n.fields)
                out += ", " + quote(k) + ": " + v;
            if (!n.inputs.empty()) {
                out += ", \"inputs\": {";
                for (size_t k = 0; k < n.inputs.size(); ++k)
                    out += (k ? ", " : "") + quote(n.inputs[k].first) + ": " + n.inputs[k].second;
                out += "}";
            }
            out += "}";
        }
        out += "],\n \"outputs\": {";
        size_t k = 0;
        for (const auto& [o, v] : outputs)
            out += std::string(k++ ? ", " : "") + quote(o) + ": " + v;
        return out + "}}\n";
    }

private:
    struct Node {
        std::string id, type;
        std::vector<std::pair<std::string, std::string>> inputs, fields;
    };
    std::vector<Node> _nodes;
};

} // namespace kin::material_import
