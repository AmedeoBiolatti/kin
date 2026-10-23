#include <kin/assets/texture_file.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace kin {
namespace {

// Longer sides than any GPU samples. It also keeps every byte count below in range.
constexpr u32 MaxSide = 1u << 15;

constexpr std::array<u8, 12> Ktx2Identifier{0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

// Little-endian, as both formats store them; the caller has checked the range.
u32 read_u32(std::span<const u8> bytes, std::size_t at) {
    return static_cast<u32>(bytes[at]) | static_cast<u32>(bytes[at + 1]) << 8 | static_cast<u32>(bytes[at + 2]) << 16 |
           static_cast<u32>(bytes[at + 3]) << 24;
}

u64 read_u64(std::span<const u8> bytes, std::size_t at) {
    return static_cast<u64>(read_u32(bytes, at)) | static_cast<u64>(read_u32(bytes, at + 4)) << 32;
}

std::optional<CompressedImage> fail(std::string* error, std::string why) {
    if (error) {
        *error = std::move(why);
    }
    return std::nullopt;
}

std::string size_text(u32 width, u32 height) {
    return std::to_string(width) + " x " + std::to_string(height);
}

// Levels in a whole chain, down to 1 x 1 (the longer side's).
u32 full_chain(Vec2i size) {
    u32 levels = 1;
    while ((std::max(size.x, size.y) >> levels) > 0) {
        ++levels;
    }
    return levels;
}

Vec2i level_size(Vec2i size, u32 level) {
    return {std::max(1, size.x >> level), std::max(1, size.y >> level)};
}

bool starts_with(std::span<const u8> bytes, std::span<const u8> magic) {
    return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
}

constexpr std::array<u8, 4> DdsMagic{'D', 'D', 'S', ' '};

std::optional<TextureFormat> vk_texture_format(u32 vk_format) {
    switch (vk_format) {
    // BC1 without alpha has no format of its own in SDL_GPU: a block's
    // punch-through texels (encoders use them for black) read as transparent
    // black, which an opaque material never looks at.
    case 131: // VK_FORMAT_BC1_RGB_UNORM_BLOCK
    case 133: return TextureFormat::Bc1RgbaUnorm;
    case 132: // VK_FORMAT_BC1_RGB_SRGB_BLOCK
    case 134: return TextureFormat::Bc1RgbaSrgb;
    case 137: return TextureFormat::Bc3RgbaUnorm;
    case 138: return TextureFormat::Bc3RgbaSrgb;
    case 139: return TextureFormat::Bc4RUnorm;
    case 141: return TextureFormat::Bc5RgUnorm;
    case 145: return TextureFormat::Bc7RgbaUnorm;
    case 146: return TextureFormat::Bc7RgbaSrgb;
    case 157: return TextureFormat::Astc4x4RgbaUnorm;
    case 158: return TextureFormat::Astc4x4RgbaSrgb;
    default: return std::nullopt;
    }
}

std::optional<TextureFormat> dxgi_texture_format(u32 dxgi_format) {
    switch (dxgi_format) {
    case 71: return TextureFormat::Bc1RgbaUnorm; // DXGI_FORMAT_BC1_UNORM
    case 72: return TextureFormat::Bc1RgbaSrgb;
    case 77: return TextureFormat::Bc3RgbaUnorm;
    case 78: return TextureFormat::Bc3RgbaSrgb;
    case 80: return TextureFormat::Bc4RUnorm;
    case 83: return TextureFormat::Bc5RgUnorm;
    case 98: return TextureFormat::Bc7RgbaUnorm;
    case 99: return TextureFormat::Bc7RgbaSrgb;
    default: return std::nullopt;
    }
}

// The checks both formats share once their headers are read; `what` names the format in messages.
std::optional<std::string> check_size(const char* what, u32 width, u32 height, u32 levels) {
    if (width == 0 || height == 0 || width > MaxSide || height > MaxSide) {
        return std::string{what} + ": size " + size_text(width, height) + " is outside 1 to " + std::to_string(MaxSide);
    }
    if (levels > full_chain({static_cast<i32>(width), static_cast<i32>(height)})) {
        return std::string{what} + ": " + std::to_string(levels) + " levels, more than a " + size_text(width, height) +
               " texture has";
    }
    return std::nullopt;
}

} // namespace

bool CompressedImage::valid() const {
    if (!texture_format_compressed(format) || size.x <= 0 || size.y <= 0 || size.x > static_cast<i32>(MaxSide) ||
        size.y > static_cast<i32>(MaxSide) || levels.empty() || levels.size() > full_chain(size)) {
        return false;
    }
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const Vec2i expected = level_size(size, static_cast<u32>(i));
        if (levels[i].size != expected || levels[i].bytes.size() != texture_level_bytes(format, expected)) {
            return false;
        }
    }
    return true;
}

bool is_compressed_image(std::span<const u8> bytes) {
    return starts_with(bytes, Ktx2Identifier) || starts_with(bytes, DdsMagic);
}

std::optional<CompressedImage> decode_ktx2(std::span<const u8> bytes, std::string* error) {
    // The identifier, the header and the index; the level index follows, 24 bytes a level.
    constexpr std::size_t HeaderBytes = 80;
    constexpr std::size_t LevelIndexBytes = 24;
    if (!starts_with(bytes, Ktx2Identifier)) {
        return fail(error, "not a KTX2 file");
    }
    if (bytes.size() < HeaderBytes) {
        return fail(error, "KTX2: truncated header");
    }
    const u32 vk_format = read_u32(bytes, 12);
    const u32 type_size = read_u32(bytes, 16);
    const u32 width = read_u32(bytes, 20);
    const u32 height = read_u32(bytes, 24);
    const u32 depth = read_u32(bytes, 28);
    const u32 layers = read_u32(bytes, 32);
    const u32 faces = read_u32(bytes, 36);
    const u32 level_count = read_u32(bytes, 40);
    const u32 scheme = read_u32(bytes, 44);
    if (scheme != 0) {
        const char* const names[] = {"", "BasisLZ", "Zstandard", "ZLIB"};
        return fail(error, std::string{"KTX2: supercompressed ("} +
                               (scheme < 4 ? std::string{names[scheme]} : "scheme " + std::to_string(scheme)) +
                               "): needs a transcoder, which kin does not ship");
    }
    if (vk_format == 0) {
        return fail(error, "KTX2: no vkFormat (Basis Universal texels): needs a transcoder, which kin does not ship");
    }
    const std::optional<TextureFormat> format = vk_texture_format(vk_format);
    if (!format) {
        return fail(error, "KTX2: vkFormat " + std::to_string(vk_format) +
                               " is not a format kin reads (BC1, BC3, BC4, BC5, BC7, ASTC 4x4)");
    }
    if (faces != 1) {
        return fail(error, faces == 6 ? std::string{"KTX2: a cube map: only 2D textures are read"}
                                      : "KTX2: faceCount " + std::to_string(faces));
    }
    if (depth > 1) {
        return fail(error, "KTX2: a 3D texture: only 2D textures are read");
    }
    if (layers > 1) {
        return fail(error, "KTX2: an array texture: only 2D textures are read");
    }
    if (width > 0 && height == 0) {
        return fail(error, "KTX2: a 1D texture: only 2D textures are read");
    }
    if (type_size != 1) {
        return fail(error, "KTX2: typeSize " + std::to_string(type_size) + ", not the 1 of a compressed format");
    }
    const u32 levels = std::max(level_count, 1u); // 0: one level, the reader to make the rest (it cannot, compressed)
    if (const auto why = check_size("KTX2", width, height, levels)) {
        return fail(error, *why);
    }
    if ((bytes.size() - HeaderBytes) / LevelIndexBytes < levels) {
        return fail(error, "KTX2: truncated level index");
    }
    CompressedImage image;
    image.format = *format;
    image.size = {static_cast<i32>(width), static_cast<i32>(height)};
    image.levels.resize(levels);
    for (u32 i = 0; i < levels; ++i) {
        const std::size_t at = HeaderBytes + static_cast<std::size_t>(i) * LevelIndexBytes;
        const u64 offset = read_u64(bytes, at);
        const u64 length = read_u64(bytes, at + 8);
        const u64 uncompressed = read_u64(bytes, at + 16);
        const Vec2i size = level_size(image.size, i);
        const u64 expected = texture_level_bytes(*format, size);
        const std::string level = "KTX2: level " + std::to_string(i);
        if (length != expected) {
            return fail(error, level + " is " + std::to_string(length) + " bytes, not the " + std::to_string(expected) +
                                   " of a " + size_text(static_cast<u32>(size.x), static_cast<u32>(size.y)) +
                                   " level of its format");
        }
        if (uncompressed != length) {
            return fail(error, level + ": uncompressedByteLength differs from byteLength");
        }
        if (offset > bytes.size() || length > bytes.size() - offset) {
            return fail(error, level + " lies outside the file");
        }
        image.levels[i].size = size;
        const auto* from = bytes.data() + offset;
        image.levels[i].bytes.assign(from, from + length);
    }
    if (error) {
        error->clear();
    }
    return image;
}

std::optional<CompressedImage> decode_dds(std::span<const u8> bytes, std::string* error) {
    // The magic and DDS_HEADER; DDS_HEADER_DXT10 follows when the FourCC is DX10, then the levels, one after another.
    constexpr std::size_t HeaderBytes = 128;
    constexpr std::size_t Dx10Bytes = 20;
    const auto four_cc = [](const char (&code)[5]) {
        return static_cast<u32>(static_cast<u8>(code[0])) | static_cast<u32>(static_cast<u8>(code[1])) << 8 |
               static_cast<u32>(static_cast<u8>(code[2])) << 16 | static_cast<u32>(static_cast<u8>(code[3])) << 24;
    };
    if (!starts_with(bytes, DdsMagic)) {
        return fail(error, "not a DDS file");
    }
    if (bytes.size() < HeaderBytes || read_u32(bytes, 4) != 124 || read_u32(bytes, 76) != 32) {
        return fail(error, "DDS: truncated or malformed header");
    }
    const u32 flags = read_u32(bytes, 8);
    const u32 height = read_u32(bytes, 12);
    const u32 width = read_u32(bytes, 16);
    const u32 depth = read_u32(bytes, 24);
    const u32 levels = std::max(read_u32(bytes, 28), 1u); // as DirectXTex reads it: 0 is one level
    const u32 pixel_flags = read_u32(bytes, 80);
    const u32 code = read_u32(bytes, 84);
    const u32 caps2 = read_u32(bytes, 112);
    if (!(pixel_flags & 0x4)) { // DDPF_FOURCC
        return fail(error, "DDS: uncompressed texels: only BC formats are read");
    }
    std::optional<TextureFormat> format;
    std::size_t at = HeaderBytes;
    if (code == four_cc("DX10")) {
        if (bytes.size() < HeaderBytes + Dx10Bytes) {
            return fail(error, "DDS: truncated DX10 header");
        }
        const u32 dxgi = read_u32(bytes, 128);
        const u32 dimension = read_u32(bytes, 132);
        if (read_u32(bytes, 136) & 0x4) { // DDS_RESOURCE_MISC_TEXTURECUBE
            return fail(error, "DDS: a cube map: only 2D textures are read");
        }
        if (dimension != 3) { // D3D10_RESOURCE_DIMENSION_TEXTURE2D
            return fail(error, dimension == 4 ? "DDS: a 3D texture: only 2D textures are read" : "DDS: not a 2D texture");
        }
        if (read_u32(bytes, 140) > 1) {
            return fail(error, "DDS: an array texture: only 2D textures are read");
        }
        format = dxgi_texture_format(dxgi);
        if (!format) {
            return fail(error, "DDS: DXGI format " + std::to_string(dxgi) + " is not one kin reads (BC1, BC3, BC4, BC5, BC7)");
        }
        at += Dx10Bytes;
    } else {
        format = code == four_cc("DXT1")                              ? std::optional{TextureFormat::Bc1RgbaUnorm}
               : code == four_cc("DXT5")                              ? std::optional{TextureFormat::Bc3RgbaUnorm}
               : code == four_cc("ATI1") || code == four_cc("BC4U") ? std::optional{TextureFormat::Bc4RUnorm}
               : code == four_cc("ATI2") || code == four_cc("BC5U") ? std::optional{TextureFormat::Bc5RgUnorm}
                                                                      : std::nullopt;
        if (!format) {
            return fail(error, "DDS: FourCC '" + std::string(reinterpret_cast<const char*>(bytes.data() + 84), 4) +
                                   "' is not one kin reads (DX10, DXT1, DXT5, ATI1, ATI2, BC4U, BC5U)");
        }
        if (caps2 & 0x200) { // DDSCAPS2_CUBEMAP
            return fail(error, "DDS: a cube map: only 2D textures are read");
        }
        if ((caps2 & 0x200000) || ((flags & 0x800000) && depth > 1)) { // DDSCAPS2_VOLUME, DDSD_DEPTH
            return fail(error, "DDS: a 3D texture: only 2D textures are read");
        }
    }
    if (const auto why = check_size("DDS", width, height, levels)) {
        return fail(error, *why);
    }
    CompressedImage image;
    image.format = *format;
    image.size = {static_cast<i32>(width), static_cast<i32>(height)};
    image.levels.resize(levels);
    for (u32 i = 0; i < levels; ++i) {
        const Vec2i size = level_size(image.size, i);
        const u64 length = texture_level_bytes(*format, size);
        if (length > bytes.size() - at) {
            return fail(error, "DDS: level " + std::to_string(i) + " lies outside the file");
        }
        image.levels[i].size = size;
        image.levels[i].bytes.assign(bytes.data() + at, bytes.data() + at + length);
        at += static_cast<std::size_t>(length);
    }
    if (error) {
        error->clear();
    }
    return image;
}

std::optional<CompressedImage> decode_compressed_image(std::span<const u8> bytes, std::string* error) {
    if (starts_with(bytes, Ktx2Identifier)) {
        return decode_ktx2(bytes, error);
    }
    if (starts_with(bytes, DdsMagic)) {
        return decode_dds(bytes, error);
    }
    return fail(error, "neither a KTX2 nor a DDS file");
}

} // namespace kin
