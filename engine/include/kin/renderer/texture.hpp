#pragma once

#include <kin/core/types.hpp>

#include <memory>

namespace kin {

namespace gpu {
class GpuRenderer3D;
}

// Texel formats. Rgba8 is the only one every backend has. R16Uint, Rg16Uint and
// R32Float hold data for material shaders (see draw_shader_surface) on backends
// with capabilities().data_textures; shaders read them with texelFetch, since
// they are never filtered, and draw_texture refuses them. In GLSL, R16Uint and
// Rg16Uint are `usampler2D`, R32Float a `sampler2D`.
//
// The rest are block-compressed: each block of 4 x 4 texels is 8 or 16 bytes,
// decoded by the GPU as it samples, so a texture takes a quarter to an eighth of
// the memory RGBA8 does. They come already compressed, with their own mip chain,
// from KTX2 or DDS files (kin/assets/texture_file.hpp, made with
// Renderer2D::create_texture_compressed; kin has no encoder), and are drawn and
// sampled like Rgba8. The Srgb ones hold colour, treated as
// create_texture_from_rgba's colour is (decoded to linear light where the
// pipeline works in it); the Unorm ones the numbers they store (normal maps,
// masks), as create_texture's Rgba8 data.
enum class TextureFormat : u8 {
    Rgba8,    // 4 bytes: red, green, blue, alpha
    R16Uint,  // 2 bytes: one unsigned 16-bit integer
    Rg16Uint, // 4 bytes: two unsigned 16-bit integers
    R32Float, // 4 bytes: one 32-bit float
    Bc1RgbaUnorm,     // 8 bytes a block: colour with 1-bit alpha (DXT1)
    Bc1RgbaSrgb,
    Bc3RgbaUnorm,     // 16 bytes a block: colour with smooth alpha (DXT5)
    Bc3RgbaSrgb,
    Bc4RUnorm,        // 8 bytes a block: one channel (roughness, a mask)
    Bc5RgUnorm,       // 16 bytes a block: two channels (a normal map's x and y)
    Bc7RgbaUnorm,     // 16 bytes a block: colour and alpha, the best of the BC formats
    Bc7RgbaSrgb,
    Astc4x4RgbaUnorm, // 16 bytes a block: phones' and tablets' format (few desktop GPUs sample it)
    Astc4x4RgbaSrgb,
};

// True for the block-compressed formats.
constexpr bool texture_format_compressed(TextureFormat format) {
    return format >= TextureFormat::Bc1RgbaUnorm;
}

// Bytes of one texel: 0 for the block-compressed formats, which have no bytes of
// their own (see texture_format_block_size).
constexpr u32 texture_format_bytes(TextureFormat format) {
    return format == TextureFormat::R16Uint ? 2u : texture_format_compressed(format) ? 0u : 4u;
}

// A format's unit of storage: `width` x `height` texels in `bytes`. 1 x 1 and
// texture_format_bytes() for the uncompressed formats.
struct TextureBlock {
    u32 width = 1;
    u32 height = 1;
    u32 bytes = 4;
};

constexpr TextureBlock texture_format_block_size(TextureFormat format) {
    switch (format) {
    case TextureFormat::Bc1RgbaUnorm:
    case TextureFormat::Bc1RgbaSrgb:
    case TextureFormat::Bc4RUnorm: return {4, 4, 8};
    case TextureFormat::Bc3RgbaUnorm:
    case TextureFormat::Bc3RgbaSrgb:
    case TextureFormat::Bc5RgUnorm:
    case TextureFormat::Bc7RgbaUnorm:
    case TextureFormat::Bc7RgbaSrgb:
    case TextureFormat::Astc4x4RgbaUnorm:
    case TextureFormat::Astc4x4RgbaSrgb: return {4, 4, 16};
    default: return {1, 1, texture_format_bytes(format)};
    }
}

// The bytes of a level of `size` texels in `format`: whole blocks, those at the
// right and bottom edges partly outside it. 0 for an empty size.
constexpr u64 texture_level_bytes(TextureFormat format, Vec2i size) {
    if (size.x <= 0 || size.y <= 0) {
        return 0;
    }
    const TextureBlock block = texture_format_block_size(format);
    return static_cast<u64>((static_cast<u32>(size.x) + block.width - 1) / block.width) *
           ((static_cast<u32>(size.y) + block.height - 1) / block.height) * block.bytes;
}

// True for the compressed formats that hold sRGB colour (decoded to linear light
// as sampled). False for Rgba8, whose texels are colour or data by how the
// texture was made (create_texture_from_rgba: colour; create_texture: data).
constexpr bool texture_format_srgb(TextureFormat format) {
    return format == TextureFormat::Bc1RgbaSrgb || format == TextureFormat::Bc3RgbaSrgb ||
           format == TextureFormat::Bc7RgbaSrgb || format == TextureFormat::Astc4x4RgbaSrgb;
}

class ITextureBackend {
public:
    // Cheap, RTTI-free downcast discriminator. Each renderer backend tags its own
    // texture wrapper so the per-sprite draw path can compare a tag + static_cast
    // instead of paying dynamic_cast on every textured draw. Unknown stays the
    // default so unrelated backends (tests/fakes) never match a real backend's cast.
    enum class Kind { Unknown, Sdl, Gpu };

    explicit ITextureBackend(Kind kind = Kind::Unknown) : _kind(kind) {}
    virtual ~ITextureBackend() = default;
    virtual Vec2i size() const = 0;
    virtual TextureFormat format() const { return TextureFormat::Rgba8; }

    Kind kind() const { return _kind; }

private:
    Kind _kind = Kind::Unknown;
};

class Texture {
public:
    Texture() = default;
    explicit Texture(std::shared_ptr<ITextureBackend> backend);

    bool valid() const { return _backend != nullptr; }
    explicit operator bool() const { return valid(); }
    Vec2i size() const;
    TextureFormat format() const;
    // True for handles to the same texture.
    bool operator==(const Texture&) const = default;

private:
    friend class Renderer2D;
    friend class RenderQueue;
    friend class SdlRenderer2DBackend;
    friend class GpuRenderer2DBackend;
    friend class gpu::GpuRenderer3D; // binds materials' textures
    friend class Assets3D; // weak, renderer-scoped texture sharing

    const std::shared_ptr<ITextureBackend>& backend() const { return _backend; }

    std::shared_ptr<ITextureBackend> _backend;
};

} // namespace kin
