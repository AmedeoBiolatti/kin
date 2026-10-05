#pragma once

// A 2D affine transform: a 2x2 linear part and a translation. Angles are in
// degrees, clockwise on screen (y down), like draw_texture's rotation.

#include <kin/core/types.hpp>

#include <algorithm>
#include <cmath>

namespace kin {

struct Affine2 {
    // x' = a·x + c·y + tx
    // y' = b·x + d·y + ty
    f32 a = 1.0f;
    f32 b = 0.0f;
    f32 c = 0.0f;
    f32 d = 1.0f;
    f32 tx = 0.0f;
    f32 ty = 0.0f;

    static constexpr Affine2 identity() { return {}; }
    static constexpr Affine2 translation(Vec2f t) { return {1.0f, 0.0f, 0.0f, 1.0f, t.x, t.y}; }
    static constexpr Affine2 scaling(Vec2f s) { return {s.x, 0.0f, 0.0f, s.y, 0.0f, 0.0f}; }
    static Affine2 rotation(f32 degrees) {
        constexpr f32 pi = 3.14159265358979323846f;
        const f32 radians = degrees * pi / 180.0f;
        const f32 cos = std::cos(radians);
        const f32 sin = std::sin(radians);
        return {cos, sin, -sin, cos, 0.0f, 0.0f};
    }
    // Scale, then rotate, then move to `pos`: how Transform2D places a child.
    static Affine2 trs(Vec2f pos, f32 degrees, Vec2f scale = {1.0f, 1.0f}) {
        Affine2 m = degrees == 0.0f ? Affine2{} : rotation(degrees);
        m.a *= scale.x;
        m.b *= scale.x;
        m.c *= scale.y;
        m.d *= scale.y;
        m.tx = pos.x;
        m.ty = pos.y;
        return m;
    }

    constexpr Vec2f apply(Vec2f p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    // A direction or size: the linear part only.
    constexpr Vec2f apply_vector(Vec2f v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }

    constexpr f32 determinant() const { return a * d - b * c; }
    constexpr bool is_identity() const { return *this == Affine2{}; }
    constexpr bool is_translation() const { return a == 1.0f && b == 0.0f && c == 0.0f && d == 1.0f; }
    // Rotation and uniform scale only (no mirroring, no shear): rectangles stay rectangles.
    constexpr bool is_similarity() const {
        const f32 eps = 1e-5f * (a * a + b * b);
        return a * a + b * b > 0.0f && std::abs(a - d) <= eps + 1e-6f && std::abs(b + c) <= eps + 1e-6f;
    }
    // For a similarity: its scale and its angle in degrees.
    f32 uniform_scale() const { return std::sqrt(a * a + b * b); }
    f32 rotation_degrees() const {
        constexpr f32 pi = 3.14159265358979323846f;
        return std::atan2(b, a) * 180.0f / pi;
    }

    // The identity when the transform squashes the plane flat.
    constexpr Affine2 inverse() const {
        const f32 det = determinant();
        if (det == 0.0f) {
            return {};
        }
        const f32 ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
        return {ia, ib, ic, id, -(ia * tx + ic * ty), -(ib * tx + id * ty)};
    }

    // m * n: n first, then m (n is the more local transform).
    friend constexpr Affine2 operator*(const Affine2& m, const Affine2& n) {
        return {m.a * n.a + m.c * n.b,
                m.b * n.a + m.d * n.b,
                m.a * n.c + m.c * n.d,
                m.b * n.c + m.d * n.d,
                m.a * n.tx + m.c * n.ty + m.tx,
                m.b * n.tx + m.d * n.ty + m.ty};
    }
    friend constexpr bool operator==(const Affine2&, const Affine2&) = default;
};

// The axis-aligned box around `rect` once transformed.
inline Rectf transformed_bounds(const Affine2& m, Rectf rect) {
    if (m.is_translation()) {
        return {rect.x + m.tx, rect.y + m.ty, rect.w, rect.h};
    }
    const Vec2f p[4] = {m.apply({rect.x, rect.y}), m.apply({rect.x + rect.w, rect.y}),
                        m.apply({rect.x + rect.w, rect.y + rect.h}), m.apply({rect.x, rect.y + rect.h})};
    f32 x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;
    for (const Vec2f& q : p) {
        x0 = std::min(x0, q.x);
        x1 = std::max(x1, q.x);
        y0 = std::min(y0, q.y);
        y1 = std::max(y1, q.y);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

} // namespace kin
