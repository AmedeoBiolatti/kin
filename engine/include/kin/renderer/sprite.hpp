#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/texture.hpp>

namespace kin {

struct Sprite {
    Texture texture;
    Rectf source{};

    bool valid() const {
        return texture.valid() && source.w > 0.0f && source.h > 0.0f;
    }
};

inline Sprite full_sprite(Texture texture) {
    const Vec2i size = texture.size();
    return Sprite{
        .texture = std::move(texture),
        .source = {0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)},
    };
}

} // namespace kin
