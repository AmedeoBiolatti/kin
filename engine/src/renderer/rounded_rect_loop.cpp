#include "rounded_rect_loop.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace kin {
namespace {

// cos and sin along a quarter turn in `segments` steps, worked out once a
// count: every corner of that radius is this arc, turned and scaled. A
// radius that keeps changing (an animation) would add counts without end,
// so past a few dozen the table starts over.
const std::vector<Vec2f>& quarter_arc(i32 segments) {
    thread_local std::unordered_map<i32, std::vector<Vec2f>> arcs;
    if (arcs.size() >= 64 && !arcs.contains(segments)) {
        arcs.clear();
    }
    auto [found, inserted] = arcs.try_emplace(segments);
    if (inserted) {
        constexpr f32 half_pi = 1.57079632679489661923f;
        found->second.reserve(static_cast<std::size_t>(segments + 1));
        for (i32 i = 0; i <= segments; ++i) {
            const f32 t = (static_cast<f32>(i) / static_cast<f32>(segments)) * half_pi;
            found->second.push_back({std::cos(t), std::sin(t)});
        }
    }
    return found->second;
}

} // namespace

void rounded_rect_loop(Rectf rect, f32 radius, std::vector<Vec2f>& out) {
    out.clear();
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    const f32 r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    if (r <= 0.0f) {
        out.push_back({rect.x, rect.y});
        out.push_back({rect.x + rect.w, rect.y});
        out.push_back({rect.x + rect.w, rect.y + rect.h});
        out.push_back({rect.x, rect.y + rect.h});
        return;
    }
    const i32 segments = std::max(8, static_cast<i32>(std::ceil(r * 2.0f)));
    const std::vector<Vec2f>& arc = quarter_arc(segments);
    const f32 left = rect.x + r;
    const f32 right = rect.x + rect.w - r;
    const f32 top = rect.y + r;
    const f32 bottom = rect.y + rect.h - r;
    out.reserve(static_cast<std::size_t>((segments + 1) * 4));
    // Each corner's quarter turn starts where the last one's ended: from
    // straight up at the top right, through right, down and left. (c, s) is
    // the arc's cos and sin; a corner a quarter turn on is (-s, c).
    for (const Vec2f p : arc) {
        out.push_back({right + p.y * r, top - p.x * r});
    }
    for (const Vec2f p : arc) {
        out.push_back({right + p.x * r, bottom + p.y * r});
    }
    for (const Vec2f p : arc) {
        out.push_back({left - p.y * r, bottom + p.x * r});
    }
    for (const Vec2f p : arc) {
        out.push_back({left - p.x * r, top - p.y * r});
    }
}

} // namespace kin
