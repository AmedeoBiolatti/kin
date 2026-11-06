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

// A texture's colour adjustments as Unreal applies them building it (not in its source file): in linear colour (an
// sRGB texture's decoded, a legacy gamma's by the power 2.2), its HSV value times `brightness` then to the power
// `brightness_curve`, saturation raised by `vibrance` (most where least saturated) then times `saturation`, hue plus
// `hue` degrees, RGB to the power `rgb_curve`, alpha remapped to `min_alpha` to `max_alpha`; then encoded as kin
// reads it (sRGB's curve for a colour texture).
struct ImageAdjust {
    f32 brightness = 1, brightness_curve = 1, vibrance = 0, saturation = 1, rgb_curve = 1, hue = 0;
    f32 min_alpha = 0, max_alpha = 1;
    bool srgb = true, legacy_gamma = false;
    bool identity() const {
        return brightness == 1 && brightness_curve == 1 && vibrance == 0 && saturation == 1 && rgb_curve == 1 &&
               hue == 0 && min_alpha == 0 && max_alpha == 1 && !(srgb && legacy_gamma);
    }
};
void adjust_image(Image& image, const ImageAdjust& adjust);

// The picture as 8-bit RGBA (straight alpha), or none, with why in `why`; an OpenEXR's channels each fitted into 0 to
// 1, a Radiance HDR's colour logarithmically, over its least and largest (its `range`: how to read a texel back).
std::optional<Image> decode_extra_image(std::span<const u8> bytes, std::string* why = nullptr,
                                        ImageRange* range = nullptr);

} // namespace kin::material_import
