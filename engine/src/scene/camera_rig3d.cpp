#include <algorithm>
#include <cmath>
#include <kin/scene/camera_rig3d.hpp>
#include <stdexcept>

namespace kin {
namespace {
bool finite(Vec3f p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool same_scale(Vec3f a, Vec3f b) {
    const auto close = [](f32 x, f32 y) {
        return std::abs(x - y) <= 1e-5f * std::max({1.f, std::abs(x), std::abs(y)});
    };
    return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z);
}
f32 wrapped(f32 angle) {
    return std::remainder(angle, 360.f);
}
f32 blend_angle(f32 a, f32 b, f32 t) {
    return a + wrapped(b - a) * t;
}
} // namespace
CameraRig3D::CameraRig3D(CameraRig3DSettings settings) {
    if (!configure(settings))
        throw std::invalid_argument("invalid camera rig settings");
    if (!set_angles(settings.initial_yaw, settings.initial_pitch))
        throw std::invalid_argument("invalid camera rig initial angles");
}
Status3D CameraRig3D::configure(CameraRig3DSettings settings) {
    if (!finite(settings.target_offset) || !std::isfinite(settings.fov) || settings.fov <= 0 || settings.fov >= 179 ||
        !std::isfinite(settings.z_near) || settings.z_near <= 0 || std::isnan(settings.z_far) ||
        settings.z_far <= settings.z_near || !std::isfinite(settings.pitch_min) || !std::isfinite(settings.pitch_max) ||
        settings.pitch_min < -89 || settings.pitch_max > 89 || settings.pitch_min > settings.pitch_max ||
        !std::isfinite(settings.initial_yaw) || !std::isfinite(settings.initial_pitch) ||
        !std::isfinite(settings.teleport_distance) || settings.teleport_distance <= 0)
        return status3d(false, {"CameraRig3D::configure: invalid settings"});
    if (!_arm.configure(settings.arm))
        return status3d(false, {"CameraRig3D::configure: invalid settings"});
    _fixed_arm = _arm;
    _settings = settings;
    _pitch = std::clamp(_pitch, settings.pitch_min, settings.pitch_max);
    reset();
    return {};
}
void CameraRig3D::follow(SceneEntity3D target) {
    if (target != _target) {
        _target = target;
        reset();
    }
}
void CameraRig3D::reset() {
    _have = false;
    _arm.reset();
    _fixed_arm.reset();
}
Status3D CameraRig3D::set_angles(f32 yaw, f32 pitch) {
    if (!std::isfinite(yaw) || !std::isfinite(pitch))
        return status3d(false, {"CameraRig3D::set_angles: invalid settings"});
    _yaw = wrapped(yaw);
    _pitch = std::clamp(pitch, _settings.pitch_min, _settings.pitch_max);
    return {};
}
Status3D CameraRig3D::orbit(Vec2f degrees) {
    if (!std::isfinite(degrees.x) || !std::isfinite(degrees.y))
        return status3d(false, {"CameraRig3D::orbit: non-finite angle"});
    return set_angles(wrapped(_yaw - wrapped(degrees.x)),
                      std::clamp(_pitch + degrees.y, _settings.pitch_min, _settings.pitch_max));
}
bool CameraRig3D::observe(EcsWorld& world) {
    auto entity = _target.resolve(world);
    if (!entity) {
        reset();
        return false;
    }
    const auto* transform = entity.get<WorldTransform3D>();
    if (!transform) {
        reset();
        return false;
    }
    const auto& m = transform->matrix;
    for (auto value : m.m)
        if (!std::isfinite(value)) {
            reset();
            return false;
        }
    Pose3D pose;
    pose.position = {m.m[12], m.m[13], m.m[14]};
    pose.yaw = _yaw;
    pose.pitch = _pitch;
    // Physics bindings use positive scale. Reject degenerate/mirrored/sheared
    // matrices rather than inventing an interpolated TRS decomposition.
    const Vec3f x{m.m[0], m.m[1], m.m[2]}, y{m.m[4], m.m[5], m.m[6]}, z{m.m[8], m.m[9], m.m[10]};
    pose.scale = {length(x), length(y), length(z)};
    if (!finite(pose.scale) || pose.scale.x < 1e-6f || pose.scale.y < 1e-6f || pose.scale.z < 1e-6f ||
        dot(cross(x, y), z) <= 0 || std::abs(dot(x, y) / (pose.scale.x * pose.scale.y)) > .001f ||
        std::abs(dot(x, z) / (pose.scale.x * pose.scale.z)) > .001f ||
        std::abs(dot(y, z) / (pose.scale.y * pose.scale.z)) > .001f) {
        reset();
        return false;
    }
    pose.rotation = Quatf::look_rotation(-z / pose.scale.z, y / pose.scale.y);
    const bool snap = !_have || length(pose.position - _current.position) > _settings.teleport_distance ||
                      !same_scale(pose.scale, _current.scale);
    _previous = snap ? pose : _current;
    _current = pose;
    _have = true;
    if (snap) {
        _arm.reset();
        _fixed_arm.reset();
    }
    return true;
}
std::optional<CameraRig3DFrame> CameraRig3D::sample(const SceneQueries3D& queries, f32 alpha, f32 seconds,
                                                    SpringArm3D& arm) {
    if (!_have)
        return {};
    if (!queries.resolve(_target)) {
        reset();
        return {};
    }
    if (!std::isfinite(alpha) || !std::isfinite(seconds) || seconds < 0)
        return {};
    const auto t = std::clamp(alpha, 0.f, 1.f);
    const auto position = lerp(_previous.position, _current.position, t);
    const auto rotation = slerp(_previous.rotation, _current.rotation, t);
    const auto view = Quatf::yaw_pitch(blend_angle(_previous.yaw, _current.yaw, t),
                                       _previous.pitch + (_current.pitch - _previous.pitch) * t);
    auto solved = arm.frame(
        position + _settings.target_offset, view, seconds,
        [&](const Shape3D& shape, Vec3f from, Vec3f delta) -> std::optional<ShapeHit3D> {
            const auto hit = queries.shape_cast(
                shape, from, {}, delta,
                {.mask = _settings.mask, .ignore = _target, .sensors = _settings.sensors, .initial_overlap = true});
            return hit ? std::optional<ShapeHit3D>{hit->physics} : std::nullopt;
        });
    if (!solved)
        return {};
    CameraRig3DFrame result;
    result.target = _target;
    result.target_transform = Mat4f::trs(position, rotation, _current.scale);
    result.arm = *solved;
    result.camera.position = solved->position;
    result.camera.rotation = view;
    result.camera.fov = _settings.fov;
    result.camera.z_near = _settings.z_near;
    result.camera.z_far = _settings.z_far;
    return result;
}
std::optional<CameraRig3DFrame> CameraRig3D::frame(const SceneQueries3D& queries, f32 alpha, f32 seconds) {
    return sample(queries, alpha, seconds, _arm);
}
std::optional<CameraRig3DFrame> CameraRig3D::fixed_frame(const SceneQueries3D& queries, f32 seconds) {
    return sample(queries, 1, seconds, _fixed_arm);
}
} // namespace kin
