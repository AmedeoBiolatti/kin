#pragma once

#include <kin/renderer/texture.hpp>

#include <utility>

namespace kin {

// Bilinear (Linear) vs nearest-neighbour (Nearest) sampling. The blur primitive
// (post_blur.hpp) requires Linear so its downsample/upsample chain averages.
enum class ScaleMode {
    Nearest,
    Linear,
    // Linear, plus mipmaps (smaller copies made on the GPU): textures drawn
    // smaller than they are stay smooth instead of shimmering, and read
    // faster. Costs a third more memory; updates remake the copies. SDL_GPU,
    // RGBA8 textures (not render targets); elsewhere Linear.
    Mipmapped,
};

// A drawable + sampleable offscreen surface: a Texture whose backing SDL texture
// was created with TARGET access. Reuses Texture/ITextureBackend wholesale, so the
// result can be drawn with the existing draw_texture(texture(), ...) path — no new
// draw call is needed.
//
// Alpha convention (see post_blur.hpp): the render-target / blur path uses
// PREMULTIPLIED alpha so down/upsample averaging stays correct with translucent
// inputs. Compositing a target back out uses the premultiplied blend mode.
class RenderTarget {
public:
    RenderTarget() = default;

    bool valid() const { return _texture.valid(); }
    explicit operator bool() const { return valid(); }
    Vec2i size() const { return _texture.size(); }

    // The sampleable view; pass to draw_texture(rt.texture(), ...) after drawing.
    const Texture& texture() const { return _texture; }

private:
    friend class SdlRenderer2DBackend; // constructs via the private ctor below
    friend class GpuRenderer2DBackend;
    explicit RenderTarget(Texture texture) : _texture(std::move(texture)) {}

    Texture _texture;
};

} // namespace kin
