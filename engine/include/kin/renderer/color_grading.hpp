#pragma once

// The colour pipeline (Renderer2D::set_color_space) and what is done to the
// image on its way to the display (Renderer2D::set_color_output): exposure,
// tonemapping, grading through a 3D lookup table, dithering.

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

// What colours are blended in.
enum class ColorSpace : u8 {
    Gamma,  // their sRGB values, as stored (kin's default; most 2D engines')
    Linear, // linear light: textures and colours decoded from sRGB, blended,
            // and encoded again for the display. Edges, glows, gradients and
            // translucent overlaps come out as light would.
};

// How light above white comes down to the display's range (Linear).
enum class Tonemap : u8 {
    None,     // clipped at white
    Reinhard, // x / (1 + x) per channel: gentle, a little flat
    Aces,     // the ACES filmic curve (Narkowicz's fit): contrasty, bright colours desaturate
};

// A 3D colour lookup table: each sRGB colour in, the colour out, `size` points
// along each axis, read with trilinear interpolation. Grading tools save them
// as .cube files; a PNG of one (a "LUT strip") can be graded in any image
// editor: export ColorLut::neutral(), grade a screenshot with it pasted in,
// cut it out again.
class ColorLut {
public:
    // Changes nothing. `size` from 2 to 64.
    static ColorLut neutral(i32 size = 32);
    // An image of one, RGBA: a strip size² x size (blue slices left to right,
    // red across each slice, green down), or a square grid of size x size
    // tiles (size³ = side², tiles left to right, then down).
    static std::optional<ColorLut> from_image(std::span<const u8> rgba, Vec2i image_size, std::string* error = nullptr);
    // A .cube file's text (Adobe/Resolve 3D LUT: LUT_3D_SIZE, DOMAIN_MIN/MAX,
    // then size³ "r g b" lines, red fastest).
    static std::optional<ColorLut> parse_cube(std::string_view text, std::string* error = nullptr);
    // A .png (as from_image) or a .cube file.
    static std::optional<ColorLut> load(const std::filesystem::path& path, std::string* error = nullptr);

    i32 size() const { return _size; }
    // RGBA as a strip, size² x size.
    std::span<const u8> strip() const { return _strip; }
    // As the GPU applies it.
    Color apply(Color color) const;
    bool save_png(const std::filesystem::path& path) const;

private:
    i32 _size = 0;
    std::vector<u8> _strip;
};

// What happens to the image on its way to the display (each frame, any time).
struct ColorOutput {
    // Linear only: light is scaled by `exposure`, brought into range by
    // `tonemap`, and encoded to sRGB.
    f32 exposure = 1.0f;
    Tonemap tonemap = Tonemap::None;
    // Then graded: the colour looked up in `lut`, cross-faded towards `lut_to`
    // by `lut_mix` (day to night), applied at `lut_strength`.
    std::shared_ptr<const ColorLut> lut;
    std::shared_ptr<const ColorLut> lut_to;
    f32 lut_mix = 0.0f;
    f32 lut_strength = 1.0f;
    // A noise of half a step in the 8-bit output (Linear), so smooth dark
    // gradients, fog and vignettes do not band.
    bool dither = false;
};

} // namespace kin
