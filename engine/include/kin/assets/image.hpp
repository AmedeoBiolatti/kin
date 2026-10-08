#pragma once

#include <kin/core/types.hpp>

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace kin {

struct Image {
    Vec2i size{};
    std::vector<u8> rgba;

    bool valid() const {
        return size.x > 0 && size.y > 0 && rgba.size() == static_cast<std::size_t>(size.x * size.y * 4);
    }
};

Image load_image(const std::filesystem::path& path);
// An image from a file's bytes in memory (PNG, JPEG, ...: what SDL_image
// reads), such as one inside a model file; `name` is for the log. Invalid
// (with the reason logged) when it can't be read.
Image decode_image(std::span<const u8> bytes, std::string_view name = "image");

// Write an RGBA image to a PNG file (creates parent directories). Returns false on
// failure. Useful for tooling and for generating procedural texture assets.
bool save_image(const Image& image, const std::filesystem::path& path);

} // namespace kin
