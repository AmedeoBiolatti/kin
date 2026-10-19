#include <cmath>
#include <kin/render3d/camera3d.hpp>

namespace kin {

Camera3D Camera3D::look_at(Vec3f eye, Vec3f target, f32 fov, Vec3f up) {
    Camera3D camera;
    camera.position = eye;
    camera.rotation = Quatf::look_rotation(target - eye, up);
    camera.fov = fov;
    return camera;
}

Camera3D Camera3D::orbit(Vec3f target, f32 yaw, f32 pitch, f32 distance, f32 fov) {
    Camera3D camera;
    camera.rotation = Quatf::yaw_pitch(yaw, -pitch);
    camera.position = target - camera.forward() * distance;
    camera.fov = fov;
    return camera;
}

Mat4f Camera3D::view() const {
    // The camera's placement, inverted: rotation transposed, position undone.
    return Mat4f::rotation(rotation.conjugate()) * Mat4f::translation(-position);
}

Mat4f Camera3D::projection_matrix(f32 aspect) const {
    if (projection == Projection3D::Orthographic) {
        const f32 h = height * 0.5f;
        const f32 w = h * aspect;
        // An orthographic view needs a far plane; "no limit" is a long way.
        const f32 depth = std::isinf(z_far) ? z_near + 10000.0f : z_far;
        return Mat4f::orthographic(-w, w, -h, h, z_near, depth);
    }
    return Mat4f::perspective(fov, aspect, z_near, z_far);
}

std::optional<ScreenPoint> Camera3D::project(Vec3f world, Rectf rect) const {
    const f32 aspect = rect.h > 0.0f ? rect.w / rect.h : 1.0f;
    return kin::project(world, view_projection(aspect), rect);
}

Ray3 Camera3D::ray(Vec2f point, Rectf rect) const {
    const f32 aspect = rect.h > 0.0f ? rect.w / rect.h : 1.0f;
    return screen_ray(point, view_projection(aspect).inverse(), rect);
}

std::optional<Vec3f> hit_plane_y(const Ray3& ray, f32 y) {
    if (std::abs(ray.direction.y) < 1e-6f) {
        return std::nullopt;
    }
    const f32 t = (y - ray.origin.y) / ray.direction.y;
    if (t < 0.0f) {
        return std::nullopt;
    }
    return ray.at(t);
}

} // namespace kin
