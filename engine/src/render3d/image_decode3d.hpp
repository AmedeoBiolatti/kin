#pragma once

// Pictures the material import reads that kin's images don't (image_decode3d.cpp): TIFF and Photoshop's PSD.

#include <kin/assets/image.hpp>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace kin::material_import {

// The picture as 8-bit RGBA (straight alpha), or none, with why in `why`.
std::optional<Image> decode_extra_image(std::span<const u8> bytes, std::string* why = nullptr);

} // namespace kin::material_import
