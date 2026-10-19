// Compressed texture files (kin/assets/texture_file.hpp) read on the CPU: KTX2
// and DDS made in memory, the formats' block sizes, and files that must be
// refused (with a reason) without reading outside their bytes.
#include <kin/assets/texture_file.hpp>

#include "texture_file_fixtures.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

using kin::TextureFormat;
using kin::u8;
using kin::u32;
using kin::Vec2i;
using namespace texture_file_fixtures;

static_assert(!kin::texture_format_compressed(TextureFormat::Rgba8) && !kin::texture_format_compressed(TextureFormat::R32Float));
static_assert(kin::texture_format_compressed(TextureFormat::Bc1RgbaUnorm) &&
              kin::texture_format_compressed(TextureFormat::Astc4x4RgbaSrgb));
static_assert(kin::texture_format_block_size(TextureFormat::Bc1RgbaSrgb).bytes == 8 &&
              kin::texture_format_block_size(TextureFormat::Bc4RUnorm).bytes == 8 &&
              kin::texture_format_block_size(TextureFormat::Bc5RgUnorm).bytes == 16 &&
              kin::texture_format_block_size(TextureFormat::Bc7RgbaUnorm).width == 4 &&
              kin::texture_format_block_size(TextureFormat::Astc4x4RgbaUnorm).height == 4);
static_assert(kin::texture_format_block_size(TextureFormat::R16Uint).bytes == 2 &&
              kin::texture_format_block_size(TextureFormat::Rgba8).width == 1);
static_assert(kin::texture_format_bytes(TextureFormat::Bc7RgbaSrgb) == 0 && kin::texture_format_bytes(TextureFormat::Rgba8) == 4);
static_assert(kin::texture_format_srgb(TextureFormat::Bc7RgbaSrgb) && !kin::texture_format_srgb(TextureFormat::Bc7RgbaUnorm) &&
              !kin::texture_format_srgb(TextureFormat::Rgba8) && !kin::texture_format_srgb(TextureFormat::Bc5RgUnorm));
// Edge blocks count whole: 5 x 3 is 2 x 1 blocks; a 1 x 1 level is one block.
static_assert(kin::texture_level_bytes(TextureFormat::Bc7RgbaUnorm, {5, 3}) == 32);
static_assert(kin::texture_level_bytes(TextureFormat::Bc1RgbaUnorm, {1, 1}) == 8);
static_assert(kin::texture_level_bytes(TextureFormat::Rgba8, {3, 2}) == 24);
static_assert(kin::texture_level_bytes(TextureFormat::Bc1RgbaUnorm, {0, 4}) == 0);

const kin::u16 Red = rgb565(31, 0, 0);
const kin::u16 Olive = rgb565(20, 40, 6);

// An 8 x 8 BC1 picture of two levels: olive, then red.
std::vector<u8> two_level_ktx2(Ktx2Fields fields = {}) {
    return ktx2_file(fields, {bc1_level(8, 8, Olive), bc1_level(4, 4, Red)});
}

// Refused, with `reason` in the error.
void refused(const std::vector<u8>& file, std::string_view reason) {
    std::string error;
    const auto image = kin::decode_compressed_image(file, &error);
    if (image || error.find(reason) == std::string::npos) {
        std::cerr << "expected a refusal naming '" << reason << "', got " << (image ? "an image" : "'" + error + "'") << '\n';
        assert(false);
    }
}

void test_ktx2_bc1_levels() {
    const std::vector<u8> file = two_level_ktx2();
    assert(kin::is_compressed_image(file));
    std::string error = "stale";
    const auto image = kin::decode_ktx2(file, &error);
    assert(image && error.empty());
    assert(image->format == TextureFormat::Bc1RgbaUnorm && !image->srgb());
    assert((image->size == Vec2i{8, 8}) && image->levels.size() == 2);
    assert((image->levels[0].size == Vec2i{8, 8}) && image->levels[0].bytes.size() == 32);
    assert((image->levels[1].size == Vec2i{4, 4}) && image->levels[1].bytes.size() == 8);
    // Each level's own blocks, read from where the index puts them (the smallest is stored first).
    assert(image->levels[0].bytes == bc1_level(8, 8, Olive));
    assert(image->levels[1].bytes == bc1_level(4, 4, Red));
    assert(image->valid());

    // The sRGB variant, BC1 without alpha (read as BC1 with it), and a lone level (levelCount 0).
    assert(kin::decode_ktx2(two_level_ktx2({.vk_format = 134}))->format == TextureFormat::Bc1RgbaSrgb);
    assert(kin::decode_ktx2(two_level_ktx2({.vk_format = 134}))->srgb());
    assert(kin::decode_ktx2(two_level_ktx2({.vk_format = 131}))->format == TextureFormat::Bc1RgbaUnorm);
    std::vector<u8> lone = ktx2_file({}, {bc1_level(8, 8, 0)});
    put32(lone, 40, 0);
    const auto one = kin::decode_ktx2(lone);
    assert(one && one->levels.size() == 1 && one->valid());

    // Every format kin reads, at an odd size: whole edge blocks.
    const std::pair<u32, TextureFormat> formats[] = {
        {137, TextureFormat::Bc3RgbaUnorm}, {138, TextureFormat::Bc3RgbaSrgb},   {139, TextureFormat::Bc4RUnorm},
        {141, TextureFormat::Bc5RgUnorm},   {145, TextureFormat::Bc7RgbaUnorm},  {146, TextureFormat::Bc7RgbaSrgb},
        {157, TextureFormat::Astc4x4RgbaUnorm}, {158, TextureFormat::Astc4x4RgbaSrgb}};
    for (const auto& [vk, format] : formats) {
        const std::vector<u8> level0(kin::texture_level_bytes(format, {6, 5}), 0x11);
        const std::vector<u8> level1(kin::texture_level_bytes(format, {3, 2}), 0x22);
        const std::vector<u8> level2(kin::texture_level_bytes(format, {1, 1}), 0x33);
        const auto odd = kin::decode_ktx2(ktx2_file({.vk_format = vk, .width = 6, .height = 5}, {level0, level1, level2}));
        assert(odd && odd->format == format && odd->levels.size() == 3 && odd->valid());
        assert((odd->levels[2].size == Vec2i{1, 1}) && odd->levels[2].bytes == level2);
    }
}

void test_ktx2_refusals() {
    const std::vector<u8> good = two_level_ktx2();
    refused({good.begin(), good.begin() + 80 + 24}, "truncated level index");
    refused({good.begin(), good.begin() + 60}, "truncated header");
    // Level 1 says 16 bytes where a 4 x 4 BC1 level is 8.
    std::vector<u8> wrong = good;
    put64(wrong, 80 + 24 + 8, 16);
    put64(wrong, 80 + 24 + 16, 16);
    refused(wrong, "level 1 is 16 bytes, not the 8");
    std::vector<u8> uncompressed = good;
    put64(uncompressed, 80 + 16, 0);
    refused(uncompressed, "uncompressedByteLength");
    std::vector<u8> outside = good;
    put64(outside, 80, good.size() - 8); // level 0's 32 bytes would run 24 past the end
    refused(outside, "level 0 lies outside the file");
    put64(outside, 80, ~0ull - 4); // an offset that would wrap
    refused(outside, "level 0 lies outside the file");

    refused(two_level_ktx2({.scheme = 1}), "supercompressed (BasisLZ): needs a transcoder");
    refused(two_level_ktx2({.scheme = 2}), "supercompressed (Zstandard): needs a transcoder");
    refused(two_level_ktx2({.scheme = 3}), "supercompressed (ZLIB)");
    refused(two_level_ktx2({.vk_format = 0}), "needs a transcoder");
    refused(two_level_ktx2({.vk_format = 37}), "vkFormat 37 is not a format kin reads");
    refused(two_level_ktx2({.depth = 4}), "a 3D texture");
    refused(two_level_ktx2({.layers = 2}), "an array texture");
    refused(two_level_ktx2({.faces = 6}), "a cube map");
    refused(two_level_ktx2({.height = 0}), "a 1D texture");
    refused(two_level_ktx2({.width = 0}), "size 0 x 8");
    refused(two_level_ktx2({.width = 1u << 16}), "is outside 1 to 32768");
    refused(two_level_ktx2({.type_size = 4}), "typeSize 4");
    // More levels than 8 x 8 has (4: 8, 4, 2, 1), and a count so large it would be a huge index.
    refused(two_level_ktx2({.level_count = 5}), "5 levels, more than a 8 x 8 texture has");
    refused(two_level_ktx2({.level_count = 0xFFFFFFFFu}), "levels, more than");
    refused({good.begin(), good.begin() + 11}, "neither a KTX2 nor a DDS file");
    std::string error;
    assert(!kin::decode_ktx2(std::vector<u8>{1, 2, 3}, &error) && error == "not a KTX2 file");
    assert(!kin::decode_ktx2({}, nullptr)); // no error out-parameter: fine
}

// Every truncation of a good file is refused, and random damage never reads
// outside the bytes (the copies below would show it under a sanitizer) or yields
// an image that is not valid().
void test_ktx2_damage() {
    const std::vector<u8> good = two_level_ktx2();
    for (std::size_t size = 0; size < good.size(); ++size) {
        // Past the last level's end the padding is not needed; before it the file is short.
        const std::vector<u8> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(size));
        const auto image = kin::decode_compressed_image(cut);
        assert(!image || image->valid());
    }
    std::mt19937 random{1234};
    int decoded = 0;
    for (int round = 0; round < 20000; ++round) {
        std::vector<u8> damaged = good;
        const int flips = 1 + static_cast<int>(random() % 4);
        for (int i = 0; i < flips; ++i) {
            damaged[random() % 128] = static_cast<u8>(random()); // the header and the level index
        }
        std::string error;
        const auto image = kin::decode_compressed_image(damaged, &error);
        assert(image ? image->valid() && error.empty() : !error.empty());
        decoded += image ? 1 : 0;
    }
    std::cerr << "KTX2 damage: " << decoded << " of 20000 damaged files still read (valid)\n";
}

void test_dds() {
    const auto level0 = bc1_level(8, 8, Olive);
    const auto level1 = bc1_level(4, 4, Red);
    std::string error;
    const auto legacy = kin::decode_dds(dds_file("DXT1", 0, 8, 8, {level0, level1}), &error);
    assert(legacy && error.empty() && legacy->format == TextureFormat::Bc1RgbaUnorm && legacy->levels.size() == 2);
    assert(legacy->levels[0].bytes == level0 && legacy->levels[1].bytes == level1 && legacy->valid());

    const std::vector<u8> bc7_0(64, 0x5A), bc7_1(16, 0xA5);
    const std::vector<u8> dx10 = dds_file("DX10", 99, 8, 8, {bc7_0, bc7_1}); // DXGI_FORMAT_BC7_UNORM_SRGB
    assert(kin::is_compressed_image(dx10));
    const auto bc7 = kin::decode_compressed_image(dx10, &error);
    assert(bc7 && bc7->format == TextureFormat::Bc7RgbaSrgb && bc7->srgb() && bc7->levels[1].bytes == bc7_1);
    assert(kin::decode_dds(dds_file("DX10", 83, 4, 4, {std::vector<u8>(16)}))->format == TextureFormat::Bc5RgUnorm);
    assert(kin::decode_dds(dds_file("ATI1", 0, 4, 4, {std::vector<u8>(8)}))->format == TextureFormat::Bc4RUnorm);
    assert(kin::decode_dds(dds_file("DXT5", 0, 4, 4, {std::vector<u8>(16)}))->format == TextureFormat::Bc3RgbaUnorm);

    std::vector<u8> short_file = dds_file("DXT1", 0, 8, 8, {level0, level1});
    short_file.resize(short_file.size() - 1); // a byte of level 1 missing
    refused(short_file, "DDS: level 1 lies outside the file");
    std::vector<u8> one_level = dds_file("DXT1", 0, 8, 8, {level0});
    put32(one_level, 28, 0); // no mip count: one level
    assert(kin::decode_dds(one_level)->levels.size() == 1);
    refused(dds_file("DX10", 99, 8, 8, {bc7_0, bc7_1}, 0, 0x4), "DDS: a cube map");
    refused(dds_file("DXT1", 0, 8, 8, {level0, level1}, 0x200 | 0xFC00), "DDS: a cube map");
    refused(dds_file("DXT1", 0, 8, 8, {level0, level1}, 0x200000), "DDS: a 3D texture");
    refused(dds_file("DX10", 95, 8, 8, {bc7_0, bc7_1}), "DXGI format 95 is not one kin reads"); // BC6H
    refused(dds_file("DXT3", 0, 8, 8, {bc7_0, bc7_1}), "FourCC 'DXT3' is not one kin reads");
    std::vector<u8> array = dx10;
    put32(array, 140, 6);
    refused(array, "DDS: an array texture");
    std::vector<u8> plain = dds_file("DXT1", 0, 8, 8, {level0, level1});
    put32(plain, 80, 0x40); // DDPF_RGB
    refused(plain, "uncompressed texels");
    refused({dx10.begin(), dx10.begin() + 140}, "DDS: truncated DX10 header");
    refused({dx10.begin(), dx10.begin() + 100}, "DDS: truncated or malformed header");
    std::mt19937 random{99};
    for (int round = 0; round < 20000; ++round) {
        std::vector<u8> damaged = dx10;
        for (int i = 0; i < 3; ++i) damaged[random() % 148] = static_cast<u8>(random());
        const auto image = kin::decode_compressed_image(damaged);
        assert(!image || image->valid());
    }
}

void test_compressed_image_valid() {
    kin::CompressedImage image = *kin::decode_ktx2(two_level_ktx2());
    assert(image.valid());
    kin::CompressedImage short_level = image;
    short_level.levels[1].bytes.pop_back();
    assert(!short_level.valid());
    kin::CompressedImage wrong_size = image;
    wrong_size.levels[1].size = {4, 2};
    assert(!wrong_size.valid());
    kin::CompressedImage uncompressed = image;
    uncompressed.format = TextureFormat::Rgba8;
    assert(!uncompressed.valid());
    kin::CompressedImage empty = image;
    empty.levels.clear();
    assert(!empty.valid() && !kin::CompressedImage{}.valid());
}

} // namespace

int main() {
    test_ktx2_bc1_levels();
    test_ktx2_refusals();
    test_ktx2_damage();
    test_dds();
    test_compressed_image_valid();
    std::cout << "texture file tests passed\n";
    return 0;
}
