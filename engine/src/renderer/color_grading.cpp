#include <kin/renderer/color_grading.hpp>

#include <kin/assets/content.hpp>
#include <kin/assets/image.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <exception>

namespace kin {

namespace {

constexpr i32 max_lut_size = 64; // a strip 4096 wide

std::optional<ColorLut> fail(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
    return std::nullopt;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// Up to `count` numbers from `line`; how many were read.
std::size_t numbers(std::string_view line, f32* out, std::size_t count) {
    std::size_t n = 0;
    while (n < count) {
        line = trim(line);
        if (line.empty()) {
            break;
        }
        const char* end = line.data() + line.size();
        const auto [next, ec] = std::from_chars(line.data(), end, out[n]);
        if (ec != std::errc{}) {
            break;
        }
        ++n;
        line.remove_prefix(static_cast<std::size_t>(next - line.data()));
    }
    return n;
}

} // namespace

ColorLut ColorLut::neutral(i32 size) {
    ColorLut lut;
    lut._size = std::clamp(size, 2, max_lut_size);
    const i32 n = lut._size;
    lut._strip.resize(static_cast<std::size_t>(n * n * n) * 4u);
    const auto level = [n](i32 i) { return static_cast<u8>(std::lround(static_cast<f32>(i) * 255.0f / static_cast<f32>(n - 1))); };
    for (i32 b = 0; b < n; ++b) {
        for (i32 g = 0; g < n; ++g) {
            for (i32 r = 0; r < n; ++r) {
                u8* p = lut._strip.data() + (static_cast<std::size_t>(g) * static_cast<std::size_t>(n * n) +
                                             static_cast<std::size_t>(b * n + r)) * 4u;
                p[0] = level(r);
                p[1] = level(g);
                p[2] = level(b);
                p[3] = 255;
            }
        }
    }
    return lut;
}

std::optional<ColorLut> ColorLut::from_image(std::span<const u8> rgba, Vec2i image_size, std::string* error) {
    const i32 w = image_size.x, h = image_size.y;
    if (w <= 0 || h <= 0 || rgba.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u) {
        return fail(error, "LUT image: no pixels");
    }
    ColorLut lut;
    if (w == h * h) { // a strip
        lut._size = h;
        lut._strip.assign(rgba.begin(), rgba.begin() + static_cast<std::ptrdiff_t>(w) * h * 4);
    } else if (w == h) { // a square grid of tiles
        const auto n = static_cast<i32>(std::lround(std::cbrt(static_cast<f64>(w) * w)));
        const auto tiles = static_cast<i32>(std::lround(std::sqrt(static_cast<f64>(n))));
        if (n * n * n != w * w || tiles * tiles != n || tiles * n != w) {
            return fail(error, "LUT image: a square image must be size x size tiles of size x size");
        }
        lut._size = n;
        lut._strip.resize(static_cast<std::size_t>(n * n * n) * 4u);
        for (i32 b = 0; b < n; ++b) {
            const i32 tx = (b % tiles) * n, ty = (b / tiles) * n;
            for (i32 g = 0; g < n; ++g) {
                const u8* from = rgba.data() + (static_cast<std::size_t>(ty + g) * static_cast<std::size_t>(w) + static_cast<std::size_t>(tx)) * 4u;
                u8* to = lut._strip.data() + (static_cast<std::size_t>(g) * static_cast<std::size_t>(n * n) + static_cast<std::size_t>(b * n)) * 4u;
                std::copy(from, from + static_cast<std::ptrdiff_t>(n) * 4, to);
            }
        }
    } else {
        return fail(error, "LUT image: neither a strip (size² x size) nor a square grid");
    }
    if (lut._size < 2 || lut._size > max_lut_size) {
        return fail(error, "LUT image: size " + std::to_string(lut._size) + " outside 2 to 64");
    }
    for (std::size_t i = 3; i < lut._strip.size(); i += 4) {
        lut._strip[i] = 255;
    }
    return lut;
}

std::optional<ColorLut> ColorLut::parse_cube(std::string_view text, std::string* error) {
    i32 size = 0;
    f32 lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {1.0f, 1.0f, 1.0f};
    std::vector<f32> values;
    std::size_t line_number = 0;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        ++line_number;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto keyword = [&](std::string_view word) {
            return line.starts_with(word) && (line.size() == word.size() || line[word.size()] == ' ' || line[word.size()] == '\t');
        };
        if (keyword("TITLE") || keyword("LUT_1D_INPUT_RANGE") || keyword("LUT_3D_INPUT_RANGE")) {
            continue;
        }
        if (keyword("LUT_1D_SIZE")) {
            return fail(error, ".cube: 1D LUTs are not read");
        }
        if (keyword("LUT_3D_SIZE")) {
            f32 n = 0.0f;
            if (numbers(line.substr(11), &n, 1) != 1) {
                return fail(error, ".cube line " + std::to_string(line_number) + ": LUT_3D_SIZE without a size");
            }
            size = static_cast<i32>(n);
            continue;
        }
        if (keyword("DOMAIN_MIN") || keyword("DOMAIN_MAX")) {
            if (numbers(line.substr(10), keyword("DOMAIN_MIN") ? lo : hi, 3) != 3) {
                return fail(error, ".cube line " + std::to_string(line_number) + ": a domain needs three numbers");
            }
            continue;
        }
        f32 rgb[3];
        if (numbers(line, rgb, 3) != 3) {
            return fail(error, ".cube line " + std::to_string(line_number) + ": expected \"r g b\"");
        }
        values.insert(values.end(), rgb, rgb + 3);
    }
    if (size < 2 || size > max_lut_size) {
        return fail(error, ".cube: LUT_3D_SIZE missing or outside 2 to 64");
    }
    const auto count = static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    if (values.size() != count * 3u) {
        return fail(error, ".cube: " + std::to_string(values.size() / 3) + " entries for a size " + std::to_string(size) +
                               " LUT (" + std::to_string(count) + " expected)");
    }
    ColorLut lut;
    lut._size = size;
    lut._strip.resize(count * 4u);
    std::size_t i = 0;
    for (i32 b = 0; b < size; ++b) { // red fastest, then green, then blue
        for (i32 g = 0; g < size; ++g) {
            for (i32 r = 0; r < size; ++r, i += 3) {
                u8* p = lut._strip.data() + (static_cast<std::size_t>(g) * static_cast<std::size_t>(size * size) + static_cast<std::size_t>(b * size + r)) * 4u;
                for (std::size_t c = 0; c < 3; ++c) {
                    const f32 span = hi[c] - lo[c];
                    const f32 v = span != 0.0f ? (values[i + c] - lo[c]) / span : values[i + c];
                    p[c] = static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                p[3] = 255;
            }
        }
    }
    return lut;
}

std::optional<ColorLut> ColorLut::load(const std::filesystem::path& path, std::string* error) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".cube") {
        const std::optional<std::string> text = read_content_file(path);
        if (!text) {
            return fail(error, "cannot open " + path.string());
        }
        return parse_cube(*text, error);
    }
    try {
        const Image image = load_image(path);
        return from_image(image.rgba, image.size, error);
    } catch (const std::exception& e) {
        return fail(error, e.what());
    }
}

Color ColorLut::apply(Color color) const {
    if (_size < 2) {
        return color;
    }
    const i32 n = _size;
    const auto at = [&](i32 r, i32 g, i32 b, std::size_t c) {
        return static_cast<f32>(_strip[(static_cast<std::size_t>(g) * static_cast<std::size_t>(n * n) +
                                        static_cast<std::size_t>(b * n + r)) * 4u + c]);
    };
    const auto split = [n](u8 v, i32& i0, i32& i1, f32& f) {
        const f32 x = static_cast<f32>(v) / 255.0f * static_cast<f32>(n - 1);
        i0 = std::min(static_cast<i32>(x), n - 1);
        i1 = std::min(i0 + 1, n - 1);
        f = x - static_cast<f32>(i0);
    };
    i32 r0, r1, g0, g1, b0, b1;
    f32 fr, fg, fb;
    split(color.r, r0, r1, fr);
    split(color.g, g0, g1, fg);
    split(color.b, b0, b1, fb);
    u8 out[3];
    for (std::size_t c = 0; c < 3; ++c) {
        const auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
        const f32 lo = lerp(lerp(at(r0, g0, b0, c), at(r1, g0, b0, c), fr), lerp(at(r0, g1, b0, c), at(r1, g1, b0, c), fr), fg);
        const f32 hi = lerp(lerp(at(r0, g0, b1, c), at(r1, g0, b1, c), fr), lerp(at(r0, g1, b1, c), at(r1, g1, b1, c), fr), fg);
        out[c] = static_cast<u8>(std::clamp(lerp(lo, hi, fb), 0.0f, 255.0f) + 0.5f);
    }
    return {out[0], out[1], out[2], color.a};
}

bool ColorLut::save_png(const std::filesystem::path& path) const {
    return _size >= 2 && save_image(Image{.size = {_size * _size, _size}, .rgba = _strip}, path);
}

} // namespace kin
