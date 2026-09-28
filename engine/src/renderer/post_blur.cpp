#include <kin/renderer/post_blur.hpp>

#include <kin/renderer/color.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <cmath>
#include <utility>
#include <vector>

namespace kin {

// Progressive bilinear dual-filter blur: downsample by halves, then upsample back
// up the recorded size chain. Each step draws a single full quad onto a
// transparent-cleared target, so with premultiplied targets the draw is a plain
// bilinear-filtered copy (dst = src + 0) — no additive/weighted blending needed.
// Runs entirely in premultiplied-alpha space (see render_target.hpp).
PooledTarget blur(Renderer2D& r, const RenderTarget& src, BlurParams params) {
    if (!src.valid() || !r.capabilities().render_targets) {
        return {};
    }

    const i32 passes = std::max(1, params.passes);
    const f32 spread = params.radius <= 0.0f ? 1.0f : params.radius;

    const auto whole = [](Vec2i size) {
        return Rectf{0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)};
    };

    std::vector<Vec2i> sizes;
    sizes.reserve(static_cast<std::size_t>(passes) + 1u);
    sizes.push_back(src.size());

    // Downsample chain. Only the current level is kept alive; moving forward
    // releases the previous level back to the pool for the upsample pass to reuse.
    PooledTarget current;
    const Texture* read = &src.texture();
    Vec2i cur = src.size();
    for (i32 i = 0; i < passes; ++i) {
        const Vec2i half{
            std::max(1, static_cast<i32>(std::lround(static_cast<f32>(cur.x) * 0.5f / spread))),
            std::max(1, static_cast<i32>(std::lround(static_cast<f32>(cur.y) * 0.5f / spread))),
        };
        if (half == cur) {
            break; // reached the 1x1 floor
        }
        PooledTarget next = r.acquire_render_target(half, ScaleMode::Linear);
        if (!next.valid()) {
            return {};
        }
        {
            auto bind = r.scoped_render_target(next.target());
            r.clear(colors::transparent);
            r.draw_texture(*read, whole(cur), whole(half));
        }
        sizes.push_back(half);
        current = std::move(next);
        read = &current.texture();
        cur = half;
    }

    if (!current.valid()) {
        return {}; // src too small to downsample (e.g. already 1x1)
    }

    // Upsample chain back up the recorded sizes; the last result is full-size.
    for (std::size_t target_index = sizes.size() - 1u; target_index-- > 0u;) {
        const Vec2i target_size = sizes[target_index];
        PooledTarget up = r.acquire_render_target(target_size, ScaleMode::Linear);
        if (!up.valid()) {
            return {};
        }
        {
            auto bind = r.scoped_render_target(up.target());
            r.clear(colors::transparent);
            r.draw_texture(current.texture(), whole(sizes[target_index + 1u]), whole(target_size));
        }
        current = std::move(up);
    }

    return current;
}

} // namespace kin
