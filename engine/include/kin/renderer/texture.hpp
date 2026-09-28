#pragma once

#include <kin/core/types.hpp>

#include <memory>

namespace kin {

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

private:
    friend class Renderer2D;
    friend class SdlRenderer2DBackend;
    friend class GpuRenderer2DBackend;

    const std::shared_ptr<ITextureBackend>& backend() const { return _backend; }

    std::shared_ptr<ITextureBackend> _backend;
};

} // namespace kin
