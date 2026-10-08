#pragma once

// A camera for kin's experimental 3D (docs/3d_plan.md): where it is, which
// way it looks (down its own -Z, +Y up), and how it projects.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>

#include <limits>
#include <optional>

namespace kin {

enum class Projection3D : u8 {
    Perspective,
    Orthographic, // parallel: sizes don't shrink with distance (pre-rendered sheets' look)
};

struct Camera3D {
    Vec3f position{0.0f, 0.0f, 5.0f};
    Quatf rotation{};
    Projection3D projection = Projection3D::Perspective;
    f32 fov = 60.0f;     // perspective: degrees, top to bottom of the view
    f32 height = 10.0f;  // orthographic: world units, top to bottom of the view
    f32 z_near = 0.1f;   // nothing nearer is drawn
    f32 z_far = std::numeric_limits<f32>::infinity(); // nor further (infinity: no limit)

    // At `eye`, looking at `target`.
    static Camera3D look_at(Vec3f eye, Vec3f target, f32 fov = 60.0f, Vec3f up = {0.0f, 1.0f, 0.0f});
    // `distance` from `target`, looking at it: from `yaw` degrees round +Y
    // (0: from +Z, looking toward -Z) and `pitch` degrees above it.
    static Camera3D orbit(Vec3f target, f32 yaw, f32 pitch, f32 distance, f32 fov = 60.0f);

    Vec3f forward() const { return rotation.rotate({0.0f, 0.0f, -1.0f}); }
    Vec3f right() const { return rotation.rotate({1.0f, 0.0f, 0.0f}); }
    Vec3f up() const { return rotation.rotate({0.0f, 1.0f, 0.0f}); }

    // World to the camera's space.
    Mat4f view() const;
    // The camera's space to clip space for a view `aspect` wide over high.
    Mat4f projection_matrix(f32 aspect) const;
    Mat4f view_projection(f32 aspect) const { return projection_matrix(aspect) * view(); }

    // Where `world` shows in a view laid into `rect` (2D draw coordinates),
    // nothing when it is behind the camera.
    std::optional<ScreenPoint> project(Vec3f world, Rectf rect) const;
    // The ray from the camera through `point` of a view laid into `rect`.
    Ray3 ray(Vec2f point, Rectf rect) const;
};

// Where `ray` meets the horizontal plane at height `y`, nothing when it runs
// along it or away from it.
std::optional<Vec3f> hit_plane_y(const Ray3& ray, f32 y = 0.0f);

} // namespace kin
