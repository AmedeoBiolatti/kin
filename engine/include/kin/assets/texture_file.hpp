#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/texture.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kin {

// A texture as the GPU stores it: block-compressed texels with their mip chain,
// read from a KTX2 or DDS file (decode_compressed_image) and made into a texture
// by Renderer2D::create_texture_compressed. kin has no encoder: the files come
// from a tool (docs/assets.md).
struct CompressedImage {
    // One mip level: its size in texels and its blocks, row by row from the top,
    // tightly packed (texture_level_bytes of them).
    struct Level {
        Vec2i size{};
        std::vector<u8> bytes;
    };

    TextureFormat format = TextureFormat::Bc1RgbaUnorm;
    Vec2i size{};              // of level 0
    std::vector<Level> levels; // level 0 (the largest) first, each half the last

    // True when the format holds sRGB colour.
    bool srgb() const { return texture_format_srgb(format); }
    // True when the format is compressed and every level has the size and the
    // bytes its place in the chain gives: what create_texture_compressed takes.
    bool valid() const;
};

// True when `bytes` start as a KTX2 or a DDS file does (whether they can be read
// is decode_compressed_image's answer).
bool is_compressed_image(std::span<const u8> bytes);

// A KTX2 file (the Khronos container) holding one 2D texture of a compressed
// TextureFormat, with its levels as stored (levelCount 0: one level). Empty, with
// the reason in `error`, for anything else: supercompressed files (Basis
// Universal, Zstandard, ZLIB) need a transcoder kin does not ship; 1D, 3D, array
// and cube-map textures, other formats, and a level index or level that does not
// fit the file or its size are refused. Never reads outside `bytes`.
std::optional<CompressedImage> decode_ktx2(std::span<const u8> bytes, std::string* error = nullptr);
// As decode_ktx2, for a DDS file of a BC format (a DX10 header, or the older DXT1,
// DXT5, ATI1/BC4U, ATI2/BC5U ones).
std::optional<CompressedImage> decode_dds(std::span<const u8> bytes, std::string* error = nullptr);
// decode_ktx2 or decode_dds, by the file's first bytes.
std::optional<CompressedImage> decode_compressed_image(std::span<const u8> bytes, std::string* error = nullptr);

} // namespace kin
