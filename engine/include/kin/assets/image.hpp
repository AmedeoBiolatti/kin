#pragma once

#include <kin/core/types.hpp>

#include <filesystem>
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

// Write an RGBA image to a PNG file (creates parent directories). Returns false on
// failure. Useful for tooling and for generating procedural texture assets.
bool save_image(const Image& image, const std::filesystem::path& path);

} // namespace kin
