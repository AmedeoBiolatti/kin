#pragma once

// Pictures the material import reads that kin's images don't (image_decode3d.cpp): TIFF, Photoshop's PSD, OpenEXR and
// Radiance HDR.

#include <array>
#include <kin/assets/image.hpp>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace kin::material_import {

// What an 8-bit texel stands for where the file's values are not 0 to 1 (an OpenEXR's: displacements, HDR): each
// channel's value = bias + scale x texel (0 to 1); with `log` (a Radiance HDR's light, a sun 100 times its sky),
// R, G and B are 2 to the power of that (bias + scale x texel), its alpha as before.
struct ImageRange {
    std::array<f32, 4> bias{0, 0, 0, 0}, scale{1, 1, 1, 1};
    bool log = false;
    bool identity() const {
        return !log && bias == std::array<f32, 4>{0, 0, 0, 0} && scale == std::array<f32, 4>{1, 1, 1, 1};
    }
};

// The picture as 8-bit RGBA (straight alpha), or none, with why in `why`; an OpenEXR's channels each fitted into 0 to
// 1, a Radiance HDR's colour logarithmically, over its least and largest (its `range`: how to read a texel back).
std::optional<Image> decode_extra_image(std::span<const u8> bytes, std::string* why = nullptr,
                                        ImageRange* range = nullptr);

} // namespace kin::material_import
