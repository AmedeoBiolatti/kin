#pragma once

#include <kin/core/types.hpp>

#include <vector>

namespace kin {

// The perimeter of `rect` with corners of `radius` (clamped to half its
// shorter side): clockwise, a corner at a time from the top right, each arc in
// max(8, ceil(2 * radius)) segments; the four corners alone when square.
// Fills `out` (cleared first) so callers can reuse a buffer across frames.
// Shared by the SDL and SDL_GPU backends.
void rounded_rect_loop(Rectf rect, f32 radius, std::vector<Vec2f>& out);

} // namespace kin
