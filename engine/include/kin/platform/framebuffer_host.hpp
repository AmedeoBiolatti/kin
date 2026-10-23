#pragma once
#include <kin/core/types.hpp>
#include <span>

namespace kin {
// Borrowed RGBA8 pixels: the host must consume them before returning. Window
// coordinates may differ from pixel coordinates on high-density displays.
class FramebufferHost {
public:
    virtual ~FramebufferHost() = default;
    virtual Vec2i pixel_size() const = 0;
    virtual Vec2i window_size() const = 0;
    virtual void present_rgba(std::span<const u8> pixels, Vec2i size) = 0;
};
}
