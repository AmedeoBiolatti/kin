#pragma once
#include <kin/core/types.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <span>
#include <stdexcept>

namespace kin::experimental {
// XImage-style opaque TrueColor pixels. RGBA source alpha is intentionally
// ignored: the rasterizer has already composited the window framebuffer.
class PixelPacker {
public:
    struct Format {
        u32 red, green, blue;
        int bits;
        bool lsb_first;
    };
    explicit PixelPacker(Format format) : _format(format) {
        if (format.bits != 16 && format.bits != 24 && format.bits != 32)
            throw std::invalid_argument("X11 host requires 16, 24 or 32-bit pixels");
        const u32 masks[] = {format.red, format.green, format.blue};
        if ((format.red & format.green) || (format.red & format.blue) || (format.green & format.blue))
            throw std::invalid_argument("X11 host requires disjoint RGB masks");
        for (int c = 0; c < 3; ++c) {
            if (!masks[c] || (format.bits < 32 && (masks[c] >> format.bits)))
                throw std::invalid_argument("X11 host requires valid RGB masks");
            const auto shift = std::countr_zero(masks[c]);
            const auto maximum = masks[c] >> shift;
            if (maximum & (maximum + 1)) throw std::invalid_argument("X11 host requires contiguous RGB masks");
        }
        if (format.bits == 32 && format.green == 0xff00) {
            if (format.red == 0xff0000 && format.blue == 0xff)
                _layout = format.lsb_first ? Layout::Bgrx : Layout::Xrgb;
            if (format.red == 0xff && format.blue == 0xff0000)
                _layout = format.lsb_first ? Layout::Rgbx : Layout::Xbgr;
        }
        if (_layout != Layout::Generic) return;
        for (int c = 0; c < 3; ++c) {
            const auto shift = std::countr_zero(masks[c]);
            const auto maximum = masks[c] >> shift;
            for (unsigned i = 0; i < 256; ++i)
                _channels[c][i] = u32((u64(i) * maximum + 127) / 255) << shift;
        }
    }

    void pack(std::span<const u8> source, Vec2i size, std::span<u8> target, Vec2i output, int pitch) const {
        const int bytes = _format.bits / 8;
        if (size.x <= 0 || size.y <= 0 || output.x <= 0 || output.y <= 0 || pitch <= 0 ||
            std::size_t(size.x) * size.y > source.size() / 4 ||
            std::size_t(output.x) > std::size_t(pitch) / bytes ||
            std::size_t(output.y) > target.size() / std::size_t(pitch))
            throw std::invalid_argument("X11 host: invalid framebuffer storage");
        const Vec2i copied{std::min(size.x, output.x), std::min(size.y, output.y)};
        // Select once per image, rather than testing format/byte order per byte.
        switch (_layout) {
        case Layout::Bgrx: fixed<Layout::Bgrx>(source.data(), size.x, target.data(), pitch, copied); return;
        case Layout::Rgbx: fixed<Layout::Rgbx>(source.data(), size.x, target.data(), pitch, copied); return;
        case Layout::Xrgb: fixed<Layout::Xrgb>(source.data(), size.x, target.data(), pitch, copied); return;
        case Layout::Xbgr: fixed<Layout::Xbgr>(source.data(), size.x, target.data(), pitch, copied); return;
        case Layout::Generic: break;
        }
        for (int y = 0; y < copied.y; ++y) {
            auto* row = target.data() + std::size_t(y) * pitch;
            for (int x = 0; x < copied.x; ++x) {
                const auto* p = source.data() + (std::size_t(y) * size.x + x) * 4;
                const u32 value = _channels[0][p[0]] | _channels[1][p[1]] | _channels[2][p[2]];
                for (int b = 0; b < bytes; ++b)
                    row[x * bytes + b] = u8(value >> ((_format.lsb_first ? b : bytes - b - 1) * 8));
            }
        }
    }

private:
    enum class Layout { Generic, Bgrx, Rgbx, Xrgb, Xbgr };
    template<Layout layout>
    static void fixed(const u8* source, int source_width, u8* target, int pitch, Vec2i size) {
        for (int y = 0; y < size.y; ++y) {
            const auto* src = source + std::size_t(y) * source_width * 4;
            auto* dst = target + std::size_t(y) * pitch;
            for (int x = 0; x < size.x; ++x) {
                u32 value;
                std::memcpy(&value, src + std::size_t(x) * 4, 4);
                if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
                if constexpr (layout == Layout::Bgrx) value = std::byteswap(value) >> 8;
                if constexpr (layout == Layout::Rgbx) value &= 0xffffff;
                if constexpr (layout == Layout::Xrgb) value <<= 8;
                if constexpr (layout == Layout::Xbgr) value = std::byteswap(value) & 0xffffff00;
                if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
                std::memcpy(dst + std::size_t(x) * 4, &value, 4);
            }
        }
    }
    Format _format;
    Layout _layout = Layout::Generic;
    std::array<std::array<u32, 256>, 3> _channels{};
};
}
