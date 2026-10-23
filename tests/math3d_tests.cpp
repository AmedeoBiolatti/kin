#include <kin/core/math3d.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace {

using kin::Mat4f;
using kin::Quatf;
using kin::Rectf;
using kin::Vec2f;
using kin::Vec3f;
using kin::Vec4f;

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
bool near(Vec3f a, Vec3f b, float eps = 1e-4f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }
bool near(Vec2f a, Vec2f b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps); }
bool near(const Mat4f& a, const Mat4f& b, float eps = 1e-4f) {
    for (int i = 0; i < 16; ++i) {
        if (!near(a.m[i], b.m[i], eps)) {
            return false;
        }
    }
    return true;
}

std::uint64_t splitmix(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
float random(std::uint64_t& state, float lo, float hi) {
    return lo + (hi - lo) * static_cast<float>(splitmix(state) >> 40) / static_cast<float>(1ull << 24);
}
Vec3f random_vec(std::uint64_t& state, float lo, float hi) {
    return {random(state, lo, hi), random(state, lo, hi), random(state, lo, hi)};
}

void test_vectors() {
    constexpr Vec3f a{1.0f, 2.0f, 3.0f};
    constexpr Vec3f b{-2.0f, 0.5f, 4.0f};
    static_assert(a + b == Vec3f{-1.0f, 2.5f, 7.0f});
    static_assert(a - b == Vec3f{3.0f, 1.5f, -1.0f});
    static_assert(a * 2.0f == Vec3f{2.0f, 4.0f, 6.0f});
    static_assert(kin::dot(a, b) == 11.0f);
    // Right-handed: x cross y is z.
    static_assert(kin::cross(Vec3f{1, 0, 0}, Vec3f{0, 1, 0}) == Vec3f{0, 0, 1});
    static_assert(kin::cross(Vec3f{0, 1, 0}, Vec3f{0, 0, 1}) == Vec3f{1, 0, 0});
    assert(near(kin::length(Vec3f{3.0f, 4.0f, 12.0f}), 13.0f));
    assert(near(kin::normalize(Vec3f{0.0f, -5.0f, 0.0f}), Vec3f{0.0f, -1.0f, 0.0f}));
    assert(kin::normalize(Vec3f{}) == Vec3f{});
    assert(near(kin::lerp(a, b, 0.25f), Vec3f{0.25f, 1.625f, 3.25f}));
}

void test_quaternions() {
    // A positive angle turns anticlockwise looking down the axis: forward
    // (-Z) turned 90 degrees about +Y faces -X (left).
    const Quatf left = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 90.0f);
    assert(near(left.rotate({0.0f, 0.0f, -1.0f}), {-1.0f, 0.0f, 0.0f}));
    assert(near(Quatf::axis_angle({1.0f, 0.0f, 0.0f}, 90.0f).rotate({0.0f, 1.0f, 0.0f}), {0.0f, 0.0f, 1.0f}));
    assert(near(Quatf::axis_angle({0.0f, 0.0f, 2.0f}, 90.0f).rotate({1.0f, 0.0f, 0.0f}), {0.0f, 1.0f, 0.0f}));

    // Looking up: pitch about the turned +X raises forward toward +Y.
    const Vec3f up_left = Quatf::yaw_pitch(90.0f, 45.0f).rotate({0.0f, 0.0f, -1.0f});
    assert(near(up_left, {-std::sqrt(0.5f), std::sqrt(0.5f), 0.0f}));

    std::uint64_t seed = 7;
    for (int i = 0; i < 200; ++i) {
        const Quatf q = Quatf::axis_angle(random_vec(seed, -1.0f, 1.0f), random(seed, -360.0f, 360.0f));
        const Quatf r = Quatf::axis_angle(random_vec(seed, -1.0f, 1.0f), random(seed, -360.0f, 360.0f));
        const Vec3f v = random_vec(seed, -10.0f, 10.0f);
        // Products compose as the matrices do: r first.
        assert(near((q * r).rotate(v), q.rotate(r.rotate(v)), 1e-3f));
        assert(near(Mat4f::rotation(q * r), Mat4f::rotation(q) * Mat4f::rotation(r)));
        assert(near(Mat4f::rotation(q).apply(v), q.rotate(v), 1e-3f));
        assert(near(q.conjugate().rotate(q.rotate(v)), v, 1e-3f));
        assert(near(kin::length(q.rotate(v)), kin::length(v), 1e-3f));
        // Slerp ends where it is told to, and turns at a steady speed.
        assert(near(kin::slerp(q, r, 0.0f).rotate(v), q.rotate(v), 1e-3f));
        assert(near(kin::slerp(q, r, 1.0f).rotate(v), r.rotate(v), 1e-3f));
    }
    const Quatf a = Quatf::identity();
    const Quatf b = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 120.0f);
    assert(near(kin::slerp(a, b, 0.5f).rotate({1.0f, 0.0f, 0.0f}),
                Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 60.0f).rotate({1.0f, 0.0f, 0.0f})));
    // The short way round: -q is the same rotation as q.
    const Quatf minus_b{-b.x, -b.y, -b.z, -b.w};
    assert(near(kin::slerp(a, minus_b, 0.5f).rotate({1.0f, 0.0f, 0.0f}),
                Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 60.0f).rotate({1.0f, 0.0f, 0.0f})));
    assert(near(Quatf{0.0f, 0.0f, 0.0f, 2.0f}.normalized().w, 1.0f));

    // look_rotation turns -Z to the way given, +Y as near up as it can, in
    // every branch of the matrix-to-quaternion conversion.
    for (const Vec3f forward : {Vec3f{0, 0, -1}, Vec3f{0, 0, 1}, Vec3f{1, 0, 0}, Vec3f{-1, 0, 0}, Vec3f{0.3f, -0.9f, 0.2f},
                                Vec3f{-0.7f, 0.2f, 0.6f}}) {
        const Quatf look = Quatf::look_rotation(forward);
        const Vec3f f = kin::normalize(forward);
        assert(near(look.rotate({0.0f, 0.0f, -1.0f}), f, 1e-3f));
        const Vec3f up = look.rotate({0.0f, 1.0f, 0.0f});
        assert(near(kin::dot(up, f), 0.0f, 1e-3f) && up.y > 0.0f);
        // The same as look_at's view, inverted.
        const Mat4f view = Mat4f::look_at({1.0f, 2.0f, 3.0f}, Vec3f{1.0f, 2.0f, 3.0f} + forward);
        assert(near(Mat4f::rotation(look).transposed(), Mat4f::translation({}) * view * Mat4f::translation({1.0f, 2.0f, 3.0f}), 1e-3f));
    }
    for (int i = 0; i < 200; ++i) {
        const Vec3f forward = random_vec(seed, -1.0f, 1.0f);
        if (kin::length(forward) < 0.1f || std::abs(kin::normalize(forward).y) > 0.99f) {
            continue;
        }
        assert(near(Quatf::look_rotation(forward).rotate({0.0f, 0.0f, -1.0f}), kin::normalize(forward), 1e-3f));
    }
}

void test_matrices() {
    constexpr Mat4f identity;
    static_assert(identity == Mat4f::identity());
    static_assert(identity.at(3, 3) == 1.0f && identity.at(0, 1) == 0.0f);
    // Column-major: the translation is in the last column, m[12..14].
    constexpr Mat4f t = Mat4f::translation({1.0f, 2.0f, 3.0f});
    static_assert(t.m[12] == 1.0f && t.m[13] == 2.0f && t.m[14] == 3.0f && t.at(0, 3) == 1.0f);
    static_assert(t.apply(Vec3f{1.0f, 1.0f, 1.0f}) == Vec3f{2.0f, 3.0f, 4.0f});
    static_assert(t.apply_vector(Vec3f{1.0f, 1.0f, 1.0f}) == Vec3f{1.0f, 1.0f, 1.0f});
    static_assert(Mat4f::scaling({2.0f, 3.0f, 4.0f}).determinant() == 24.0f);
    assert(near(Mat4f::rotation_y(90.0f).apply({0.0f, 0.0f, -1.0f}), {-1.0f, 0.0f, 0.0f}));
    assert(near(Mat4f::rotation_x(90.0f).apply({0.0f, 1.0f, 0.0f}), {0.0f, 0.0f, 1.0f}));
    assert(near(Mat4f::rotation_z(90.0f).apply({1.0f, 0.0f, 0.0f}), {0.0f, 1.0f, 0.0f}));

    // m * n: n first.
    const Mat4f moved_then_turned = Mat4f::rotation_z(90.0f) * Mat4f::translation({1.0f, 0.0f, 0.0f});
    assert(near(moved_then_turned.apply({0.0f, 0.0f, 0.0f}), {0.0f, 1.0f, 0.0f}));

    std::uint64_t seed = 11;
    for (int i = 0; i < 200; ++i) {
        const Vec3f pos = random_vec(seed, -50.0f, 50.0f);
        const Quatf rot = Quatf::axis_angle(random_vec(seed, -1.0f, 1.0f), random(seed, -180.0f, 180.0f));
        const Vec3f scale = random_vec(seed, 0.2f, 4.0f);
        const Mat4f m = Mat4f::trs(pos, rot, scale);
        assert(near(m, Mat4f::translation(pos) * Mat4f::rotation(rot) * Mat4f::scaling(scale)));
        assert(near(m.determinant(), scale.x * scale.y * scale.z, 1e-2f));
        assert(near(m * m.inverse(), Mat4f{}, 1e-3f));
        assert(near(m.inverse() * m, Mat4f{}, 1e-3f));
        assert(near(m.transposed().transposed(), m));
        assert(near(m.transposed().at(0, 3), m.at(3, 0)));
        const Vec3f p = random_vec(seed, -10.0f, 10.0f);
        assert(near(m.inverse().apply(m.apply(p)), p, 2e-3f));
    }
    // A projection inverts too.
    const Mat4f proj = Mat4f::perspective(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    assert(near(proj * proj.inverse(), Mat4f{}, 1e-3f));
    // Flat: the identity.
    assert(Mat4f::scaling({1.0f, 0.0f, 1.0f}).inverse() == Mat4f{});
}

void test_look_at() {
    const Vec3f eye{3.0f, 4.0f, 5.0f};
    const Vec3f target{-1.0f, 2.0f, 0.0f};
    const Mat4f view = Mat4f::look_at(eye, target);
    assert(near(view.apply(eye), {0.0f, 0.0f, 0.0f}));
    // The target is straight ahead, down the camera's -Z.
    assert(near(view.apply(target), {0.0f, 0.0f, -kin::length(target - eye)}, 1e-3f));
    // World up stays up (+Y in the camera's space, above the line of sight).
    const Vec3f up = view.apply_vector({0.0f, 1.0f, 0.0f});
    assert(up.y > 0.0f && near(up.x, 0.0f));
    // Rigid: lengths are kept.
    assert(near(view.determinant(), 1.0f));
    // A camera looking down -Z from the origin is the identity.
    assert(near(Mat4f::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}), Mat4f{}));
}

void test_perspective() {
    const float n = 0.5f, f = 200.0f;
    const Mat4f proj = Mat4f::perspective(90.0f, 2.0f, n, f);
    // Reverse Z: depth 1 at the near plane, 0 at the far one, falling between.
    assert(near(proj.apply_projective({0.0f, 0.0f, -n}).z, 1.0f));
    assert(near(proj.apply_projective({0.0f, 0.0f, -f}).z, 0.0f));
    const float d10 = proj.apply_projective({0.0f, 0.0f, -10.0f}).z;
    const float d20 = proj.apply_projective({0.0f, 0.0f, -20.0f}).z;
    assert(d10 > d20 && d20 > 0.0f && d10 < 1.0f);
    // 90 degrees top to bottom: a point 45 degrees up is at the top edge
    // (NDC +Y up); twice as wide as high.
    assert(near(proj.apply_projective({0.0f, 10.0f, -10.0f}).y, 1.0f));
    assert(near(proj.apply_projective({20.0f, 0.0f, -10.0f}).x, 1.0f));
    assert(near(proj.apply_projective({-20.0f, -10.0f, -10.0f}).x, -1.0f));

    // Infinite: depth 1 at the near plane, toward 0 far away, never below.
    const Mat4f infinite = Mat4f::perspective(60.0f, 1.0f, n);
    assert(near(infinite.apply_projective({0.0f, 0.0f, -n}).z, 1.0f));
    const float far_depth = infinite.apply_projective({0.0f, 0.0f, -1e6f}).z;
    assert(far_depth > 0.0f && far_depth < 1e-5f);
}

void test_orthographic() {
    constexpr Mat4f proj = Mat4f::orthographic(-4.0f, 4.0f, -2.0f, 2.0f, 1.0f, 11.0f);
    static_assert(proj.apply_projective(Vec3f{-4.0f, -2.0f, -1.0f}) == Vec3f{-1.0f, -1.0f, 1.0f});
    static_assert(proj.apply_projective(Vec3f{4.0f, 2.0f, -11.0f}) == Vec3f{1.0f, 1.0f, 0.0f});
    assert(near(proj.apply_projective({0.0f, 0.0f, -6.0f}).z, 0.5f));
    // Off centre.
    const Mat4f shifted = Mat4f::orthographic(0.0f, 10.0f, 0.0f, 5.0f, 0.0f, 1.0f);
    assert(near(shifted.apply_projective({5.0f, 2.5f, 0.0f}), {0.0f, 0.0f, 1.0f}));
}

void test_project_and_rays() {
    const Rectf viewport{100.0f, 50.0f, 640.0f, 360.0f};
    const Mat4f view = Mat4f::look_at({0.0f, 2.0f, 8.0f}, {0.0f, 0.0f, 0.0f});
    const Mat4f proj = Mat4f::perspective(60.0f, viewport.w / viewport.h, 0.1f);
    const Mat4f view_proj = proj * view;
    const Mat4f inverse = view_proj.inverse();

    // What the camera looks at shows at the viewport's centre.
    const auto centre = kin::project({0.0f, 0.0f, 0.0f}, view_proj, viewport);
    assert(centre && near(centre->pos, {420.0f, 230.0f}));
    // Up in the world is up on screen (2D y down: smaller).
    const auto above = kin::project({0.0f, 1.0f, 0.0f}, view_proj, viewport);
    assert(above && above->pos.y < centre->pos.y && near(above->pos.x, 420.0f));
    // +X is to the right.
    const auto right = kin::project({1.0f, 0.0f, 0.0f}, view_proj, viewport);
    assert(right && right->pos.x > centre->pos.x);
    // Nearer is deeper (reverse Z).
    const auto nearer = kin::project({0.0f, 1.0f, 4.0f}, view_proj, viewport);
    assert(nearer && nearer->depth > centre->depth);
    // Behind the camera: nothing.
    assert(!kin::project({0.0f, 2.0f, 20.0f}, view_proj, viewport));

    std::uint64_t seed = 23;
    for (int i = 0; i < 200; ++i) {
        const Vec3f p = random_vec(seed, -3.0f, 3.0f);
        const auto shown = kin::project(p, view_proj, viewport);
        assert(shown);
        assert(near(kin::unproject(shown->pos, shown->depth, inverse, viewport), p, 2e-2f));
        // The ray through where it shows passes through it.
        const kin::Ray3 ray = kin::screen_ray(shown->pos, inverse, viewport);
        assert(near(kin::length(ray.direction), 1.0f));
        const float t = kin::dot(p - ray.origin, ray.direction);
        assert(t > 0.0f && near(ray.at(t), p, 2e-2f));
    }

    // The centre's ray points the way the camera looks, from near the eye.
    const kin::Ray3 ray = kin::screen_ray({420.0f, 230.0f}, inverse, viewport);
    assert(near(ray.direction, kin::normalize(Vec3f{0.0f, -2.0f, -8.0f})));
    assert(kin::length(ray.origin - Vec3f{0.0f, 2.0f, 8.0f}) < 0.2f);

    // Orthographic rays are parallel, each from its own point.
    const Mat4f ortho = Mat4f::orthographic(-8.0f, 8.0f, -4.5f, 4.5f, 0.1f, 50.0f) * view;
    const Mat4f ortho_inverse = ortho.inverse();
    const kin::Ray3 a = kin::screen_ray({120.0f, 60.0f}, ortho_inverse, viewport);
    const kin::Ray3 b = kin::screen_ray({700.0f, 400.0f}, ortho_inverse, viewport);
    assert(near(a.direction, b.direction) && !near(a.origin, b.origin));
}

} // namespace

int main() {
    test_vectors();
    test_quaternions();
    test_matrices();
    test_look_at();
    test_perspective();
    test_orthographic();
    test_project_and_rays();
    return 0;
}
