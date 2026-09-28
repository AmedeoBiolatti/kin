#include "iso_draw.hpp"

#include <algorithm>
#include <cmath>

namespace isometric_demo {

kin::Color shade(kin::Color color, kin::i32 delta) {
    return kin::Color::rgba(
        static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(color.r) + delta, 0, 255)),
        static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(color.g) + delta, 0, 255)),
        static_cast<kin::u8>(std::clamp(static_cast<kin::i32>(color.b) + delta, 0, 255)),
        color.a
    );
}

void draw_diamond(kin::Renderer2D& renderer,
                  kin::Vec2f center,
                  kin::f32 w,
                  kin::f32 h,
                  kin::Color fill,
                  kin::Color edge) {
    const kin::i32 rows = std::max(1, static_cast<kin::i32>(std::ceil(h)));
    const kin::f32 half_w = w * 0.5f;
    const kin::f32 half_h = h * 0.5f;
    for (kin::i32 i = 0; i <= rows; ++i) {
        const kin::f32 dy = -half_h + static_cast<kin::f32>(i);
        const kin::f32 t = 1.0f - std::abs(dy) / std::max(1.0f, half_h);
        const kin::f32 span = std::max(0.0f, half_w * t);
        renderer.draw_line({center.x - span, center.y + dy}, {center.x + span, center.y + dy}, fill);
    }
    renderer.draw_line({center.x, center.y - half_h}, {center.x + half_w, center.y}, edge);
    renderer.draw_line({center.x + half_w, center.y}, {center.x, center.y + half_h}, shade(edge, -24));
    renderer.draw_line({center.x, center.y + half_h}, {center.x - half_w, center.y}, shade(edge, -36));
    renderer.draw_line({center.x - half_w, center.y}, {center.x, center.y - half_h}, shade(edge, 24));
}

void fill_convex_quad_body(kin::Renderer2D& renderer,
                           std::array<kin::Vec2f, 4> points,
                           kin::Color fill) {
    kin::f32 min_y = points[0].y;
    kin::f32 max_y = points[0].y;
    for (kin::Vec2f p : points) {
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
    }

    const kin::i32 y0 = static_cast<kin::i32>(std::floor(min_y));
    const kin::i32 y1 = static_cast<kin::i32>(std::ceil(max_y));
    for (kin::i32 y = y0; y <= y1; ++y) {
        std::array<kin::f32, 4> xs{};
        kin::i32 count = 0;
        const kin::f32 scan_y = static_cast<kin::f32>(y);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const kin::Vec2f a = points[i];
            const kin::Vec2f b = points[(i + 1) % points.size()];
            if ((scan_y >= std::min(a.y, b.y)) && (scan_y < std::max(a.y, b.y)) && a.y != b.y) {
                const kin::f32 t = (scan_y - a.y) / (b.y - a.y);
                xs[static_cast<std::size_t>(count++)] = a.x + (b.x - a.x) * t;
            }
        }
        if (count >= 2) {
            std::sort(xs.begin(), xs.begin() + count);
            renderer.draw_line({xs[0], scan_y}, {xs[static_cast<std::size_t>(count - 1)], scan_y}, fill);
        }
    }
}

void fill_convex_quad(kin::Renderer2D& renderer,
                      std::array<kin::Vec2f, 4> points,
                      kin::Color fill,
                      kin::Color edge) {
    fill_convex_quad_body(renderer, points, fill);

    for (std::size_t i = 0; i < points.size(); ++i) {
        renderer.draw_line(points[i], points[(i + 1) % points.size()], edge);
    }
}

void draw_column_sides(kin::Renderer2D& renderer,
                       kin::Vec2f center,
                       kin::f32 w,
                       kin::f32 h,
                       kin::f32 depth,
                       kin::Color base) {
    const kin::f32 half_w = w * 0.5f;
    const kin::f32 half_h = h * 0.5f;
    const kin::Vec2f left{center.x - half_w, center.y};
    const kin::Vec2f right{center.x + half_w, center.y};
    const kin::Vec2f bottom{center.x, center.y + half_h};
    for (kin::i32 i = 0; i < static_cast<kin::i32>(depth); ++i) {
        const kin::f32 y = static_cast<kin::f32>(i);
        const kin::f32 t = std::clamp(y / std::max(1.0f, depth), 0.0f, 1.0f);
        renderer.draw_line({left.x, left.y + y}, {bottom.x, bottom.y + y}, shade(base, static_cast<kin::i32>(-38 - t * 24.0f)));
        renderer.draw_line({right.x, right.y + y}, {bottom.x, bottom.y + y}, shade(base, static_cast<kin::i32>(-56 - t * 20.0f)));
    }
    renderer.draw_line(left, {left.x, left.y + depth}, shade(base, -64));
    renderer.draw_line(right, {right.x, right.y + depth}, shade(base, -70));
    renderer.draw_line({bottom.x, bottom.y}, {bottom.x, bottom.y + depth}, shade(base, -80));
}

void draw_disk(kin::Renderer2D& renderer, kin::Vec2f center, kin::f32 radius, kin::Color fill, kin::Color edge) {
    const kin::i32 rows = std::max(1, static_cast<kin::i32>(std::ceil(radius * 2.0f)));
    for (kin::i32 i = 0; i <= rows; ++i) {
        const kin::f32 dy = -radius + static_cast<kin::f32>(i);
        const kin::f32 span = std::sqrt(std::max(0.0f, radius * radius - dy * dy));
        renderer.draw_line({center.x - span, center.y + dy}, {center.x + span, center.y + dy}, fill);
    }
    renderer.draw_line({center.x - radius * 0.5f, center.y - radius * 0.82f},
                       {center.x + radius * 0.5f, center.y - radius * 0.82f},
                       edge);
}

void draw_door_on_south_wall(kin::Renderer2D& renderer,
                             kin::Vec2f top_left,
                             kin::Vec2f top_right,
                             kin::Vec2f bottom_left,
                             kin::Vec2f bottom_right,
                             kin::f32 width,
                             kin::f32 height,
                             kin::Color color) {
    const kin::Vec2f top_center{
        (top_left.x + top_right.x) * 0.5f,
        (top_left.y + top_right.y) * 0.5f,
    };
    const kin::Vec2f bottom_center{
        (bottom_left.x + bottom_right.x) * 0.5f,
        (bottom_left.y + bottom_right.y) * 0.5f,
    };
    const kin::Vec2f wall_up{
        top_center.x - bottom_center.x,
        top_center.y - bottom_center.y,
    };
    const kin::f32 wall_h = std::max(1.0f, std::sqrt(wall_up.x * wall_up.x + wall_up.y * wall_up.y));
    const kin::f32 height_t = std::clamp(height / wall_h, 0.0f, 0.82f);
    const kin::Vec2f door_top{
        bottom_center.x + wall_up.x * height_t,
        bottom_center.y + wall_up.y * height_t,
    };
    const kin::Vec2f wall_right{
        bottom_right.x - bottom_left.x,
        bottom_right.y - bottom_left.y,
    };
    const kin::f32 wall_w = std::max(1.0f, std::sqrt(wall_right.x * wall_right.x + wall_right.y * wall_right.y));
    const kin::Vec2f door_half{
        wall_right.x / wall_w * width * 0.5f,
        wall_right.y / wall_w * width * 0.5f,
    };
    fill_convex_quad(renderer,
                     {{
                         {door_top.x - door_half.x, door_top.y - door_half.y},
                         {door_top.x + door_half.x, door_top.y + door_half.y},
                         {bottom_center.x + door_half.x, bottom_center.y + door_half.y},
                         {bottom_center.x - door_half.x, bottom_center.y - door_half.y},
                     }},
                     color,
                     shade(color, -34));
}

} // namespace isometric_demo
