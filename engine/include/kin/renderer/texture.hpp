#pragma once

#include <kin/core/types.hpp>

#include <memory>

namespace kin {

// Texel formats. Rgba8 is the only one every backend has and the only one that is
// drawn as an image. The others hold data for material shaders (see
// draw_shader_surface) on backends with capabilities().data_textures; shaders
// read them with texelFetch, since they are never filtered. In GLSL, R16Uint and
// Rg16Uint are `usampler2D`, R32Float a `sampler2D`.
enum class TextureFormat : u8 {
    Rgba8,    // 4 bytes: red, green, blue, alpha
    R16Uint,  // 2 bytes: one unsigned 16-bit integer
    Rg16Uint, // 4 bytes: two unsigned 16-bit integers
    R32Float, // 4 bytes: one 32-bit float
};

constexpr u32 texture_format_bytes(TextureFormat format) {
    return format == TextureFormat::R16Uint ? 2u : 4u;
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

    const std::shared_ptr<ITextureBackend>& backend() const { return _backend; }

    std::shared_ptr<ITextureBackend> _backend;
};

} // namespace kin
