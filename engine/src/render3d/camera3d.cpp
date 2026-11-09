#include <algorithm>
#include <cmath>
#include <kin/render3d/camera3d.hpp>
#include <vector>

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
    const f32 frame = frame_aspect(aspect);
    Mat4f p;
    if (projection == Projection3D::Orthographic) {
        const f32 h = height * 0.5f;
        const f32 w = h * frame;
        // An orthographic view needs a far plane; "no limit" is a long way.
        const f32 depth = std::isinf(z_far) ? z_near + 10000.0f : z_far;
        p = Mat4f::orthographic(-w, w, -h, h, z_near, depth);
    } else {
        p = Mat4f::perspective(fov, frame, z_near, z_far);
    }
    if (!windowed() || !(window.w > 0.0f && window.h > 0.0f)) {
        return p;
    }
    // The window's part of clip space stretched over all of it: x' = (x - cx w) / hx, the same for y (which goes up).
    const f32 hx = window.w, hy = window.h;
    const f32 cx = 2.0f * window.x + window.w - 1.0f, cy = 1.0f - 2.0f * window.y - window.h;
    Mat4f crop;
    crop.at(0, 0) = 1.0f / hx;
    crop.at(0, 3) = -cx / hx;
    crop.at(1, 1) = 1.0f / hy;
    crop.at(1, 3) = -cy / hy;
    return crop * p;
}

std::optional<ScreenPoint> Camera3D::project(Vec3f world, Rectf rect) const {
    const f32 aspect = rect.h > 0.0f ? rect.w / rect.h : 1.0f;
    return kin::project(world, view_projection(aspect), rect);
}

Ray3 Camera3D::ray(Vec2f point, Rectf rect) const {
    const f32 aspect = rect.h > 0.0f ? rect.w / rect.h : 1.0f;
    return screen_ray(point, view_projection(aspect).inverse(), rect);
}

Vec4f plane_through(Vec3f point, Vec3f normal) {
    const Vec3f n = normalize(normal);
    return {n.x, n.y, n.z, -dot(n, point)};
}

PlanarView3D mirror_view(const Camera3D& camera, Vec4f plane, f32 aspect, f32 offset) {
    const f32 scale = length(plane.xyz());
    Vec3f n = scale > 0.0f ? plane.xyz() / scale : Vec3f{0.0f, 1.0f, 0.0f};
    f32 w = scale > 0.0f ? plane.w / scale : 0.0f;
    // The camera's side is the front.
    if (dot(n, camera.position) + w < 0.0f) {
        n = -n;
        w = -w;
    }
    const auto reflect = [&](Vec3f v) { return v - n * (2.0f * dot(n, v)); };
    PlanarView3D out;
    out.camera = camera;
    out.camera.position = camera.position - n * (2.0f * (dot(n, camera.position) + w));
    // A reflection turns the picture inside out; a camera can't, so this one sees the reflection's picture flipped left
    // to right. The projector says where each point of the mirror is in that picture, so nothing reads it the wrong
    // way.
    out.camera.rotation = Quatf::look_rotation(reflect(camera.forward()), reflect(camera.up()));
    out.camera.clip_plane = {n.x, n.y, n.z, w - offset};
    out.projector = out.camera.view_projection(aspect);
    return out;
}

Quatf portal_rotation(PortalPose3D entry, PortalPose3D exit) {
    return (exit.rotation * Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 180.0f) * entry.rotation.conjugate()).normalized();
}

Vec3f portal_point(PortalPose3D entry, PortalPose3D exit, Vec3f point) {
    return exit.position + portal_rotation(entry, exit).rotate(point - entry.position);
}

PlanarView3D portal_view(const Camera3D& camera, PortalPose3D entry, PortalPose3D exit, f32 aspect, f32 offset) {
    const Quatf turn = portal_rotation(entry, exit);
    PlanarView3D out;
    out.camera = camera;
    out.camera.position = portal_point(entry, exit, camera.position);
    out.camera.rotation = (turn * camera.rotation).normalized();
    const Vec3f n = normalize(exit.rotation.rotate({0.0f, 0.0f, 1.0f}));
    out.camera.clip_plane = {n.x, n.y, n.z, -dot(n, exit.position) - offset};
    // A point of the entry's surface is read where its counterpart on the exit's shows.
    const Mat4f through =
        Mat4f::translation(exit.position) * Mat4f::rotation(turn) * Mat4f::translation(-entry.position);
    out.projector = out.camera.view_projection(aspect) * through;
    return out;
}

namespace {
// `p` in the portal's own space: x across, y up, z out of its front.
Vec3f local_to(PortalPose3D portal, Vec3f p) {
    return portal.rotation.conjugate().rotate(p - portal.position);
}
bool in_opening(PortalPose3D portal, Vec3f local) {
    return (portal.size.x <= 0.0f || std::abs(local.x) <= portal.size.x * 0.5f) &&
           (portal.size.y <= 0.0f || std::abs(local.y) <= portal.size.y * 0.5f);
}
} // namespace

std::optional<f32> portal_crossing(PortalPose3D entry, Vec3f from, Vec3f to) {
    const f32 a = local_to(entry, from).z, b = local_to(entry, to).z;
    if (!(a >= 0.0f && b < 0.0f)) {
        return std::nullopt;
    }
    const f32 t = a / (a - b);
    if (!in_opening(entry, local_to(entry, from + (to - from) * t))) {
        return std::nullopt;
    }
    return t;
}

std::optional<Mat4f> portal_straddle(PortalPose3D entry, PortalPose3D exit, Vec3f centre, f32 radius, f32 offset) {
    const Vec3f local = local_to(entry, centre);
    // Only what could go through: its centre within the opening, its bounds no larger than the opening (not the wall
    // the portal is in, nor the floor under it).
    const bool fits =
        entry.size.x <= 0.0f || entry.size.y <= 0.0f || radius <= 0.5f * std::hypot(entry.size.x, entry.size.y);
    if (!(std::abs(local.z) < radius + std::max(offset, 0.0f)) || !in_opening(entry, local) || !fits) {
        return std::nullopt;
    }
    return Mat4f::translation(exit.position) * Mat4f::rotation(portal_rotation(entry, exit)) *
           Mat4f::translation(-entry.position);
}

std::optional<PortalRay3D> ray_through_portal(const Ray3& ray, PortalPose3D entry, PortalPose3D exit) {
    const Vec3f origin = local_to(entry, ray.origin);
    const Vec3f direction = entry.rotation.conjugate().rotate(ray.direction);
    if (!(origin.z >= 0.0f && direction.z < 0.0f)) {
        return std::nullopt;
    }
    const f32 t = origin.z / -direction.z;
    const Vec3f hit = origin + direction * t;
    if (!in_opening(entry, hit)) {
        return std::nullopt;
    }
    return PortalRay3D{{portal_point(entry, exit, ray.at(t)), portal_rotation(entry, exit).rotate(ray.direction)}, t};
}

Mat4f portal_surface_transform(PortalPose3D entry, f32 depth) {
    return Mat4f::trs(entry.position + entry.rotation.rotate({0.0f, 0.0f, -depth * 0.5f}), entry.rotation,
                      {entry.size.x, entry.size.y, depth});
}

std::vector<PlanarView3D> portal_chain(const Camera3D& camera, PortalPose3D entry, PortalPose3D exit, f32 aspect,
                                       u32 depth, f32 offset) {
    std::vector<PlanarView3D> views;
    views.reserve(depth);
    const Camera3D* from = &camera;
    for (u32 k = 0; k < depth; ++k) {
        views.push_back(portal_view(*from, entry, exit, aspect, offset));
        from = &views.back().camera;
    }
    return views;
}

std::optional<Rectf> planar_on_screen(const Camera3D& camera, f32 aspect, Vec3f centre, Quatf rotation, Vec2f size) {
    const Vec3f n = rotation.rotate({0.0f, 0.0f, 1.0f});
    const bool facing = camera.projection == Projection3D::Orthographic ? dot(n, camera.forward()) < 0.0f
                                                                        : dot(n, camera.position - centre) > 0.0f;
    if (!facing) {
        return std::nullopt;
    }
    const Mat4f vp = camera.view_projection(aspect);
    std::vector<Vec4f> polygon;
    for (const Vec2f c : {Vec2f{-0.5f, -0.5f}, Vec2f{0.5f, -0.5f}, Vec2f{0.5f, 0.5f}, Vec2f{-0.5f, 0.5f}}) {
        const Vec3f p = centre + rotation.rotate({c.x * size.x, c.y * size.y, 0.0f});
        polygon.push_back(vp * Vec4f{p.x, p.y, p.z, 1.0f});
    }
    // Only what is in front of the camera (w > 0): the polygon cut there.
    constexpr f32 eps = 1e-5f;
    std::vector<Vec4f> front;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Vec4f a = polygon[i], b = polygon[(i + 1) % polygon.size()];
        if (a.w > eps) {
            front.push_back(a);
        }
        if ((a.w > eps) != (b.w > eps)) {
            const f32 t = (eps - a.w) / (b.w - a.w);
            front.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, eps});
        }
    }
    if (front.empty()) {
        return std::nullopt;
    }
    f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const Vec4f& c : front) {
        x0 = std::min(x0, c.x / c.w);
        x1 = std::max(x1, c.x / c.w);
        y0 = std::min(y0, c.y / c.w);
        y1 = std::max(y1, c.y / c.w);
    }
    x0 = std::max(x0, -1.0f), y0 = std::max(y0, -1.0f), x1 = std::min(x1, 1.0f), y1 = std::min(y1, 1.0f);
    if (!(x0 < x1 && y0 < y1)) {
        return std::nullopt;
    }
    // 0..1 across and down (clip space goes up).
    return Rectf{0.5f + 0.5f * x0, 0.5f - 0.5f * y1, 0.5f * (x1 - x0), 0.5f * (y1 - y0)};
}

void crop_planar_view(PlanarView3D& view, Rectf window, f32 aspect, bool mirrored, const Mat4f& through) {
    if (mirrored) {
        window.x = 1.0f - window.x - window.w;
    }
    view.camera.window = window;
    view.projector = view.camera.view_projection(aspect) * through;
}

PlanarPicture3D planar_picture(Rectf bounds, Vec2i frame) {
    constexpr int step = 64;
    const auto axis = [&](f32 lo, f32 extent, int pixels, f32& start, f32& share, int& size) {
        pixels = std::max(pixels, 1);
        const f32 margin = 2.0f / f32(pixels);
        const f32 a = std::max(lo - margin, 0.0f), b = std::min(lo + extent + margin, 1.0f);
        size = std::clamp((int(std::ceil((b - a) * f32(pixels))) + step - 1) / step * step, 1, pixels);
        share = f32(size) / f32(pixels);
        start = size == pixels ? 0.0f : std::clamp(0.5f * (a + b) - 0.5f * share, 0.0f, 1.0f - share);
    };
    PlanarPicture3D p;
    axis(bounds.x, bounds.w, frame.x, p.window.x, p.window.w, p.size.x);
    axis(bounds.y, bounds.h, frame.y, p.window.y, p.window.h, p.size.y);
    return p;
}

void planar_view_params(std::span<f32> params, const Mat4f& projector, f32 distortion, f32 strength) {
    if (params.size() < 20) {
        return;
    }
    for (std::size_t i = 0; i < 16; ++i) {
        params[i] = projector.m[i];
    }
    params[16] = distortion;
    params[17] = strength;
    params[18] = 0.0f;
    params[19] = 0.0f;
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
