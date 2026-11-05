#pragma once

// What the importers of other engines' materials share (material_import3d.cpp, material_import_unity3d.cpp): where a
// map is read from (in place inside the content, else copied), the maps made (packed, recombined), decoding with a
// note where it can't be done, the notes, and the .kinmaterials written after it is read back.

#include "image_decode3d.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <kin/assets/image.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <kin/render3d/material_import3d.hpp>
#include <kin/renderer/color.hpp>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

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

} // namespace kin::material_import
