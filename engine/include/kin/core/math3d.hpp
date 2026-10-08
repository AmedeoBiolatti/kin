#pragma once

// 3D vectors, quaternions and 4x4 matrices for kin's experimental 3D
// rendering (docs/3d_plan.md).
//
// Conventions: right-handed, +Y up, -Z forward (glTF's); a camera looks down
// its -Z. Angles are in degrees, as Affine2's are; a positive angle turns
// anticlockwise looking down the axis toward the origin (the right-hand rule).
// Mat4f is column-major (m[column * 4 + row]), as GLSL reads it, and maps
// column vectors: p' = M p. Projections give SDL_GPU's clip space (NDC +Y up,
// depth 0 to 1) with reverse Z: the near plane at depth 1, the far plane (or
// infinity) at 0, so depth tests use "greater".

#include <kin/core/types.hpp>

#include <cmath>
#include <limits>
#include <optional>

namespace kin {

namespace math3d_detail {
constexpr f32 pi = 3.14159265358979323846f;
constexpr f32 radians(f32 degrees) { return degrees * pi / 180.0f; }
} // namespace math3d_detail

struct Vec3f {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;

    friend constexpr Vec3f operator+(Vec3f a, Vec3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vec3f operator-(Vec3f a, Vec3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr Vec3f operator-(Vec3f a) { return {-a.x, -a.y, -a.z}; }
    friend constexpr Vec3f operator*(Vec3f a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
    friend constexpr Vec3f operator*(f32 s, Vec3f a) { return a * s; }
    friend constexpr Vec3f operator/(Vec3f a, f32 s) { return {a.x / s, a.y / s, a.z / s}; }
    // Component by component (a colour times a colour, a size times a scale).
    friend constexpr Vec3f operator*(Vec3f a, Vec3f b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
    constexpr Vec3f& operator+=(Vec3f b) { return *this = *this + b; }
    constexpr Vec3f& operator-=(Vec3f b) { return *this = *this - b; }
    constexpr Vec3f& operator*=(f32 s) { return *this = *this * s; }
    friend constexpr bool operator==(Vec3f, Vec3f) = default;
};

constexpr f32 dot(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3f cross(Vec3f a, Vec3f b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
constexpr f32 length_squared(Vec3f a) { return dot(a, a); }
inline f32 length(Vec3f a) { return std::sqrt(dot(a, a)); }
// `a` at length 1; the zero vector stays zero.
inline Vec3f normalize(Vec3f a) {
    const f32 n = length(a);
    return n > 0.0f ? a / n : Vec3f{};
}
constexpr Vec3f lerp(Vec3f a, Vec3f b, f32 t) { return a + (b - a) * t; }

struct Vec4f {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 0.0f;

    constexpr Vec3f xyz() const { return {x, y, z}; }
    friend constexpr bool operator==(Vec4f, Vec4f) = default;
};

// A rotation, as a unit quaternion (x, y, z the axis times sin(angle / 2),
// w cos(angle / 2)).
struct Quatf {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 1.0f;

    static constexpr Quatf identity() { return {}; }
    // `degrees` about `axis` (any length but zero).
    static Quatf axis_angle(Vec3f axis, f32 degrees) {
        const Vec3f n = normalize(axis);
        const f32 half = math3d_detail::radians(degrees) * 0.5f;
        const f32 s = std::sin(half);
        return {n.x * s, n.y * s, n.z * s, std::cos(half)};
    }
    // Turned `yaw` about +Y, then tilted `pitch` about its own +X: a camera's
    // heading and how far it looks up (positive) or down.
    static Quatf yaw_pitch(f32 yaw, f32 pitch) {
        return axis_angle({0.0f, 1.0f, 0.0f}, yaw) * axis_angle({1.0f, 0.0f, 0.0f}, pitch);
    }
    // The rotation that turns -Z to `forward` with +Y as near `up` as it can
    // be (a camera looking along `forward`). `up` must not be along `forward`.
    static Quatf look_rotation(Vec3f forward, Vec3f up = {0.0f, 1.0f, 0.0f}) {
        const Vec3f f = normalize(forward);
        const Vec3f s = normalize(cross(f, up));
        const Vec3f u = cross(s, f);
        // The rotation matrix's columns are s, u and -f.
        const f32 m00 = s.x, m11 = u.y, m22 = -f.z;
        const f32 trace = m00 + m11 + m22;
        Quatf q;
        if (trace > 0.0f) {
            const f32 r = std::sqrt(1.0f + trace) * 2.0f;
            q = {(u.z + f.y) / r, (-f.x - s.z) / r, (s.y - u.x) / r, 0.25f * r};
        } else if (m00 > m11 && m00 > m22) {
            const f32 r = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            q = {0.25f * r, (u.x + s.y) / r, (-f.x + s.z) / r, (u.z + f.y) / r};
        } else if (m11 > m22) {
            const f32 r = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            q = {(u.x + s.y) / r, 0.25f * r, (-f.y + u.z) / r, (-f.x - s.z) / r};
        } else {
            const f32 r = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
            q = {(-f.x + s.z) / r, (-f.y + u.z) / r, 0.25f * r, (s.y - u.x) / r};
        }
        return q.normalized();
    }

    // q * r: r first, then q (r is the more local rotation), as matrices.
    friend constexpr Quatf operator*(Quatf q, Quatf r) {
        return {q.w * r.x + q.x * r.w + q.y * r.z - q.z * r.y,
                q.w * r.y - q.x * r.z + q.y * r.w + q.z * r.x,
                q.w * r.z + q.x * r.y - q.y * r.x + q.z * r.w,
                q.w * r.w - q.x * r.x - q.y * r.y - q.z * r.z};
    }
    // The opposite rotation (for a unit quaternion, its inverse).
    constexpr Quatf conjugate() const { return {-x, -y, -z, w}; }
    Quatf normalized() const {
        const f32 n = std::sqrt(x * x + y * y + z * z + w * w);
        return n > 0.0f ? Quatf{x / n, y / n, z / n, w / n} : Quatf{};
    }
    constexpr Vec3f rotate(Vec3f v) const {
        // v + 2w (u x v) + 2 u x (u x v), u the vector part.
        const Vec3f u{x, y, z};
        const Vec3f t = cross(u, v) * 2.0f;
        return v + t * w + cross(u, t);
    }
    friend constexpr bool operator==(Quatf, Quatf) = default;
};

// Between `a` (t = 0) and `b` (t = 1) at a steady angular speed, the short way round.
inline Quatf slerp(Quatf a, Quatf b, f32 t) {
    f32 d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    f32 wa = 1.0f - t;
    f32 wb = t;
    if (d < 0.9995f) {
        const f32 angle = std::acos(d);
        const f32 s = std::sin(angle);
        wa = std::sin(wa * angle) / s;
        wb = std::sin(wb * angle) / s;
    }
    return Quatf{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb}.normalized();
}

struct Mat4f {
    // Column-major: m[column * 4 + row]. The identity by default.
    f32 m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    constexpr f32& at(int row, int column) { return m[column * 4 + row]; }
    constexpr f32 at(int row, int column) const { return m[column * 4 + row]; }

    static constexpr Mat4f identity() { return {}; }
    static constexpr Mat4f translation(Vec3f t) {
        Mat4f r;
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }
    static constexpr Mat4f scaling(Vec3f s) {
        Mat4f r;
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        return r;
    }
    static constexpr Mat4f rotation(Quatf q) {
        const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat4f r;
        r.m[0] = 1.0f - 2.0f * (yy + zz);
        r.m[1] = 2.0f * (xy + wz);
        r.m[2] = 2.0f * (xz - wy);
        r.m[4] = 2.0f * (xy - wz);
        r.m[5] = 1.0f - 2.0f * (xx + zz);
        r.m[6] = 2.0f * (yz + wx);
        r.m[8] = 2.0f * (xz + wy);
        r.m[9] = 2.0f * (yz - wx);
        r.m[10] = 1.0f - 2.0f * (xx + yy);
        return r;
    }
    static Mat4f rotation_x(f32 degrees) { return rotation(Quatf::axis_angle({1.0f, 0.0f, 0.0f}, degrees)); }
    static Mat4f rotation_y(f32 degrees) { return rotation(Quatf::axis_angle({0.0f, 1.0f, 0.0f}, degrees)); }
    static Mat4f rotation_z(f32 degrees) { return rotation(Quatf::axis_angle({0.0f, 0.0f, 1.0f}, degrees)); }
    // Scale, then rotate, then move to `pos`: how a node is placed in its parent.
    static constexpr Mat4f trs(Vec3f pos, Quatf rot, Vec3f scale = {1.0f, 1.0f, 1.0f}) {
        Mat4f r = rotation(rot);
        for (int row = 0; row < 3; ++row) {
            r.at(row, 0) *= scale.x;
            r.at(row, 1) *= scale.y;
            r.at(row, 2) *= scale.z;
        }
        r.m[12] = pos.x;
        r.m[13] = pos.y;
        r.m[14] = pos.z;
        return r;
    }

    // The view matrix of a camera at `eye` looking at `target`, `up` roughly
    // up (not along the line of sight): world to the camera's space, where it
    // looks down -Z with +Y up.
    static Mat4f look_at(Vec3f eye, Vec3f target, Vec3f up = {0.0f, 1.0f, 0.0f}) {
        const Vec3f f = normalize(target - eye);
        const Vec3f s = normalize(cross(f, up));
        const Vec3f u = cross(s, f);
        Mat4f r;
        r.at(0, 0) = s.x;
        r.at(0, 1) = s.y;
        r.at(0, 2) = s.z;
        r.at(1, 0) = u.x;
        r.at(1, 1) = u.y;
        r.at(1, 2) = u.z;
        r.at(2, 0) = -f.x;
        r.at(2, 1) = -f.y;
        r.at(2, 2) = -f.z;
        r.at(0, 3) = -dot(s, eye);
        r.at(1, 3) = -dot(u, eye);
        r.at(2, 3) = dot(f, eye);
        return r;
    }
    // A perspective projection, `fov_y` degrees top to bottom, `aspect` width
    // over height, reverse Z: depth 1 at `z_near`, 0 at `z_far`. A `z_far` of
    // infinity (the default) never clips the distance.
    static Mat4f perspective(f32 fov_y, f32 aspect, f32 z_near,
                             f32 z_far = std::numeric_limits<f32>::infinity()) {
        const f32 f = 1.0f / std::tan(math3d_detail::radians(fov_y) * 0.5f);
        Mat4f r;
        r.at(0, 0) = f / aspect;
        r.at(1, 1) = f;
        // depth = (a z + b) / -z: 1 at z = -z_near, 0 at z = -z_far.
        const bool infinite = std::isinf(z_far);
        r.at(2, 2) = infinite ? 0.0f : z_near / (z_far - z_near);
        r.at(2, 3) = infinite ? z_near : z_near * z_far / (z_far - z_near);
        r.at(3, 2) = -1.0f;
        r.at(3, 3) = 0.0f;
        return r;
    }
    // An orthographic projection of the box [left, right] x [bottom, top] in
    // the camera's space, `z_near` to `z_far` in front of it, reverse Z.
    static constexpr Mat4f orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 z_near, f32 z_far) {
        Mat4f r;
        r.at(0, 0) = 2.0f / (right - left);
        r.at(1, 1) = 2.0f / (top - bottom);
        r.at(0, 3) = -(right + left) / (right - left);
        r.at(1, 3) = -(top + bottom) / (top - bottom);
        // depth = a z + b: 1 at z = -z_near, 0 at z = -z_far.
        r.at(2, 2) = 1.0f / (z_far - z_near);
        r.at(2, 3) = z_far / (z_far - z_near);
        return r;
    }

    // m * n: n first, then m (n is the more local transform).
    friend constexpr Mat4f operator*(const Mat4f& a, const Mat4f& b) {
        Mat4f r;
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                f32 sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += a.at(row, k) * b.at(k, column);
                }
                r.at(row, column) = sum;
            }
        }
        return r;
    }
    constexpr Vec4f operator*(Vec4f v) const {
        return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w,
                m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
                m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
                m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w};
    }
    // A point (w = 1) through an affine transform (no projection).
    constexpr Vec3f apply(Vec3f p) const { return (*this * Vec4f{p.x, p.y, p.z, 1.0f}).xyz(); }
    // A direction (w = 0): the linear part only.
    constexpr Vec3f apply_vector(Vec3f v) const { return (*this * Vec4f{v.x, v.y, v.z, 0.0f}).xyz(); }
    // A point through any transform, projections included, divided by w.
    constexpr Vec3f apply_projective(Vec3f p) const {
        const Vec4f c = *this * Vec4f{p.x, p.y, p.z, 1.0f};
        return {c.x / c.w, c.y / c.w, c.z / c.w};
    }

    constexpr Mat4f transposed() const {
        Mat4f r;
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                r.at(row, column) = at(column, row);
            }
        }
        return r;
    }
    constexpr f32 determinant() const {
        const f32 s0 = m[0] * m[5] - m[4] * m[1], s1 = m[0] * m[6] - m[4] * m[2];
        const f32 s2 = m[0] * m[7] - m[4] * m[3], s3 = m[1] * m[6] - m[5] * m[2];
        const f32 s4 = m[1] * m[7] - m[5] * m[3], s5 = m[2] * m[7] - m[6] * m[3];
        const f32 c5 = m[10] * m[15] - m[14] * m[11], c4 = m[9] * m[15] - m[13] * m[11];
        const f32 c3 = m[9] * m[14] - m[13] * m[10], c2 = m[8] * m[15] - m[12] * m[11];
        const f32 c1 = m[8] * m[14] - m[12] * m[10], c0 = m[8] * m[13] - m[12] * m[9];
        return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    }
    // The identity when the matrix squashes space flat.
    constexpr Mat4f inverse() const {
        const f32 s0 = m[0] * m[5] - m[4] * m[1], s1 = m[0] * m[6] - m[4] * m[2];
        const f32 s2 = m[0] * m[7] - m[4] * m[3], s3 = m[1] * m[6] - m[5] * m[2];
        const f32 s4 = m[1] * m[7] - m[5] * m[3], s5 = m[2] * m[7] - m[6] * m[3];
        const f32 c5 = m[10] * m[15] - m[14] * m[11], c4 = m[9] * m[15] - m[13] * m[11];
        const f32 c3 = m[9] * m[14] - m[13] * m[10], c2 = m[8] * m[15] - m[12] * m[11];
        const f32 c1 = m[8] * m[14] - m[12] * m[10], c0 = m[8] * m[13] - m[12] * m[9];
        const f32 det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
        if (det == 0.0f) {
            return {};
        }
        const f32 inv = 1.0f / det;
        Mat4f r;
        r.m[0] = (m[5] * c5 - m[6] * c4 + m[7] * c3) * inv;
        r.m[4] = (-m[4] * c5 + m[6] * c2 - m[7] * c1) * inv;
        r.m[8] = (m[4] * c4 - m[5] * c2 + m[7] * c0) * inv;
        r.m[12] = (-m[4] * c3 + m[5] * c1 - m[6] * c0) * inv;
        r.m[1] = (-m[1] * c5 + m[2] * c4 - m[3] * c3) * inv;
        r.m[5] = (m[0] * c5 - m[2] * c2 + m[3] * c1) * inv;
        r.m[9] = (-m[0] * c4 + m[1] * c2 - m[3] * c0) * inv;
        r.m[13] = (m[0] * c3 - m[1] * c1 + m[2] * c0) * inv;
        r.m[2] = (m[13] * s5 - m[14] * s4 + m[15] * s3) * inv;
        r.m[6] = (-m[12] * s5 + m[14] * s2 - m[15] * s1) * inv;
        r.m[10] = (m[12] * s4 - m[13] * s2 + m[15] * s0) * inv;
        r.m[14] = (-m[12] * s3 + m[13] * s1 - m[14] * s0) * inv;
        r.m[3] = (-m[9] * s5 + m[10] * s4 - m[11] * s3) * inv;
        r.m[7] = (m[8] * s5 - m[10] * s2 + m[11] * s1) * inv;
        r.m[11] = (-m[8] * s4 + m[9] * s2 - m[11] * s0) * inv;
        r.m[15] = (m[8] * s3 - m[9] * s1 + m[10] * s0) * inv;
        return r;
    }

    friend constexpr bool operator==(const Mat4f&, const Mat4f&) = default;
};

// A point on screen: where a world point shows in `viewport` (2D draw
// coordinates: x right, y down, as Renderer2D draws) and its depth there
// (1 at the near plane, 0 at the far one). Nothing for a point behind the
// camera.
struct ScreenPoint {
    Vec2f pos{};
    f32 depth = 0.0f;
};
inline std::optional<ScreenPoint> project(Vec3f world, const Mat4f& view_projection, Rectf viewport) {
    const Vec4f c = view_projection * Vec4f{world.x, world.y, world.z, 1.0f};
    if (c.w <= 0.0f) {
        return std::nullopt;
    }
    const f32 x = c.x / c.w, y = c.y / c.w;
    return ScreenPoint{{viewport.x + (x + 1.0f) * 0.5f * viewport.w, viewport.y + (1.0f - y) * 0.5f * viewport.h},
                       c.z / c.w};
}
// The world point that shows at `pos` in `viewport` at `depth`: project()'s
// inverse, given the inverse of its view-projection matrix.
inline Vec3f unproject(Vec2f pos, f32 depth, const Mat4f& inverse_view_projection, Rectf viewport) {
    const f32 x = (pos.x - viewport.x) / viewport.w * 2.0f - 1.0f;
    const f32 y = 1.0f - (pos.y - viewport.y) / viewport.h * 2.0f;
    return inverse_view_projection.apply_projective({x, y, depth});
}

// A half-line from `origin` along `direction` (length 1).
struct Ray3 {
    Vec3f origin{};
    Vec3f direction{0.0f, 0.0f, -1.0f};

    constexpr Vec3f at(f32 t) const { return origin + direction * t; }
};
// The ray through `pos` in `viewport`, from the near plane away from the
// camera: what a click there points at.
inline Ray3 screen_ray(Vec2f pos, const Mat4f& inverse_view_projection, Rectf viewport) {
    // Depth 0.5 is a finite point in front of the near plane even with an
    // infinite far plane (where depth 0 is at infinity).
    const Vec3f start = unproject(pos, 1.0f, inverse_view_projection, viewport);
    const Vec3f further = unproject(pos, 0.5f, inverse_view_projection, viewport);
    return {start, normalize(further - start)};
}

} // namespace kin
