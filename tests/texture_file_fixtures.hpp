#pragma once
// Compressed texture files made in memory (kin/assets/texture_file.hpp): KTX2
// and DDS containers around solid-colour BC1 and ASTC blocks, which take no
// encoder to write.

#include <kin/core/types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <string_view>
#include <vector>

namespace texture_file_fixtures {

using kin::u8;
using kin::u16;
using kin::u32;
using kin::u64;

inline void put32(std::vector<u8>& out, std::size_t at, u32 value) {
    for (int i = 0; i < 4; ++i) out[at + static_cast<std::size_t>(i)] = static_cast<u8>(value >> (8 * i));
}

inline void put64(std::vector<u8>& out, std::size_t at, u64 value) {
    put32(out, at, static_cast<u32>(value));
    put32(out, at + 4, static_cast<u32>(value >> 32));
}

// An RGB565 colour from its 5-, 6- and 5-bit parts, and the 8-bit colour a BC1
// decoder makes of it (each part's high bits repeated into the low ones).
constexpr u16 rgb565(u32 r5, u32 g6, u32 b5) { return static_cast<u16>(r5 << 11 | g6 << 5 | b5); }
constexpr std::array<u8, 3> rgb565_expanded(u16 colour) {
    const u32 r = colour >> 11, g = (colour >> 5) & 63, b = colour & 31;
    return {static_cast<u8>(r << 3 | r >> 2), static_cast<u8>(g << 2 | g >> 4), static_cast<u8>(b << 3 | b >> 2)};
}

// One colour in a BC1 block: both endpoints that colour, every index 0 (the first).
inline std::vector<u8> bc1_level(u32 width, u32 height, u16 colour) {
    const std::size_t blocks = static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4);
    std::vector<u8> out(blocks * 8, 0);
    for (std::size_t i = 0; i < blocks; ++i) {
        out[i * 8] = out[i * 8 + 2] = static_cast<u8>(colour);
        out[i * 8 + 1] = out[i * 8 + 3] = static_cast<u8>(colour >> 8);
    }
    return out;
}

// One colour in an ASTC block: the LDR "void extent" block, its colour four
// UNORM16 values (an 8-bit value v as v * 257, which the sRGB formats read back
// as v).
inline std::vector<u8> astc_level(u32 width, u32 height, std::array<u8, 4> rgba) {
    const std::size_t blocks = static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4);
    std::vector<u8> out(blocks * 16, 0);
    for (std::size_t i = 0; i < blocks; ++i) {
        put64(out, i * 16, 0xFFFFFFFFFFFFFDFCull);
        for (int c = 0; c < 4; ++c) {
            const u32 v = rgba[static_cast<std::size_t>(c)] * 257u;
            out[i * 16 + 8 + static_cast<std::size_t>(c) * 2] = static_cast<u8>(v);
            out[i * 16 + 9 + static_cast<std::size_t>(c) * 2] = static_cast<u8>(v >> 8);
        }
    }
    return out;
}

// A KTX2 header's fields; level_count 0 is written as levels.size().
struct Ktx2Fields {
    u32 vk_format = 133; // VK_FORMAT_BC1_RGBA_UNORM_BLOCK
    u32 type_size = 1;
    u32 width = 8, height = 8, depth = 0;
    u32 layers = 0, faces = 1;
    u32 level_count = 0;
    u32 scheme = 0;
};

// A KTX2 file of `levels` (level 0 first), stored the way the format lays them
// out: the smallest first, each at a multiple of 16 bytes.
inline std::vector<u8> ktx2_file(const Ktx2Fields& f, const std::vector<std::vector<u8>>& levels) {
    static constexpr u8 identifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    const std::size_t index_end = 80 + 24 * levels.size();
    std::size_t data = (index_end + 15) / 16 * 16;
    std::vector<std::size_t> offsets(levels.size());
    for (std::size_t i = levels.size(); i-- > 0;) {
        offsets[i] = data;
        data = (data + levels[i].size() + 15) / 16 * 16;
    }
    std::vector<u8> out(data, 0);
    std::copy(std::begin(identifier), std::end(identifier), out.begin());
    const u32 header[] = {f.vk_format, f.type_size, f.width, f.height, f.depth, f.layers, f.faces,
                          f.level_count ? f.level_count : static_cast<u32>(levels.size()), f.scheme};
    for (std::size_t i = 0; i < 9; ++i) put32(out, 12 + 4 * i, header[i]);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        put64(out, 80 + 24 * i, offsets[i]);
        put64(out, 88 + 24 * i, levels[i].size());
        put64(out, 96 + 24 * i, levels[i].size());
        std::copy(levels[i].begin(), levels[i].end(), out.begin() + static_cast<std::ptrdiff_t>(offsets[i]));
    }
    return out;
}

// A DDS file of `levels` (level 0 first, one after another): with a DX10 header
// of `dxgi_format` when `four_cc` is "DX10", else the older header of that FourCC.
inline std::vector<u8> dds_file(std::string_view four_cc, u32 dxgi_format, u32 width, u32 height,
                                const std::vector<std::vector<u8>>& levels, u32 caps2 = 0, u32 misc = 0) {
    const bool dx10 = four_cc == "DX10";
    std::vector<u8> out(dx10 ? 148 : 128, 0);
    out[0] = 'D'; out[1] = 'D'; out[2] = 'S'; out[3] = ' ';
    put32(out, 4, 124);
    put32(out, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000); // caps, height, width, pixel format, mip count
    put32(out, 12, height);
    put32(out, 16, width);
    put32(out, 28, static_cast<u32>(levels.size()));
    put32(out, 76, 32);
    put32(out, 80, 0x4); // DDPF_FOURCC
    for (std::size_t i = 0; i < 4; ++i) out[84 + i] = static_cast<u8>(four_cc[i]);
    put32(out, 108, 0x1000 | 0x400000 | 0x8); // texture, mipmap, complex
    put32(out, 112, caps2);
    if (dx10) {
        put32(out, 128, dxgi_format);
        put32(out, 132, 3); // TEXTURE2D
        put32(out, 136, misc);
        put32(out, 140, 1);
    }
    for (const auto& level : levels) out.insert(out.end(), level.begin(), level.end());
    return out;
}

} // namespace texture_file_fixtures
