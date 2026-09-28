#pragma once

#include <kin/renderer/sprite.hpp>

namespace kin::ui2 {

struct UiNineSlice {
    Sprite sprite;
    f32 left = 0.0f;
    f32 top = 0.0f;
    f32 right = 0.0f;
    f32 bottom = 0.0f;
};

// Convenience: construct a UiNineSlice with all four margins equal.
// Avoids the verbose aggregate `{std::move(sprite), m, m, m, m}` at call sites.
inline UiNineSlice uniform_nine_slice(Sprite sprite, f32 margin) {
    return {std::move(sprite), margin, margin, margin, margin};
}

} // namespace kin::ui2
