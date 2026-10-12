#include "camera.hpp"

#include <algorithm>
#include <cmath>

namespace summit {

namespace {

f32 lerp_angle(f32 a, f32 b, f32 t) {
    f32 d = std::fmod(b - a + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return a + (d - 180.0f) * t;
}

} // namespace

void CameraRig::observe(const Game& game, i32 player) {
    const Player& p = game.player(player);
    const Vec3f head = game.head(player);
    // A jump of more than 12 m in one step is a respawn or a teleport: no sweep from the old place.
    if (!_have || length(head - _head) > 12.0f) {
        _prev_head = _head = head;
        _prev_yaw = _yaw = p.yaw;
        _prev_pitch = _pitch = p.pitch;
        _prev_feet = _feet = game.feet(player);
        _arm = -1.0f;
        _swing = 0.0f;
        _have = true;
    } else {
        _prev_head = _head;
        _prev_yaw = _yaw;
        _prev_pitch = _pitch;
        _prev_feet = _feet;
        _head = head;
        _yaw = p.yaw;
        _pitch = p.pitch;
        _feet = game.feet(player);
    }
    _ignore = game.character_state(player).body;
    const TraversalState3D ts = game.traversal(player);
    _hanging = p.state == PlayerState::Alive && ts.hanging();
}

void CameraRig::reset() {
    _have = false;
    _arm = -1.0f;
}

CameraView CameraRig::frame(const World3D& world, f32 alpha, f32 dt) {
    CameraView v;
    v.fov = settings.fov;
    const f32 t = std::clamp(alpha, 0.0f, 1.0f);
    const Vec3f head = lerp(_prev_head, _head, t);
    const f32 yaw = lerp_angle(_prev_yaw, _yaw, t);
    const f32 pitch = _prev_pitch + (_pitch - _prev_pitch) * t;
    // The hanging view eases to one side (presentation: the player's own look, and so the hands' reach, are the sim's).
    _swing += ((_hanging ? 1.0f : 0.0f) - _swing) * std::min(1.0f, dt * 3.0f);
    const f32 view_yaw = yaw + settings.hang_yaw * _swing;
    const f32 view_pitch = std::clamp(pitch + settings.hang_pitch * _swing, -85.0f, 85.0f);
    v.rotation = Quatf::yaw_pitch(view_yaw, view_pitch);
    const Vec3f forward = v.rotation.rotate({0.0f, 0.0f, -1.0f});
    if (settings.first_person) {
        const Vec3f feet = lerp(_prev_feet, _feet, t);
        // The eye is the head's height above the feet while it stands; a ragdoll's head is where its body is.
        const Vec3f eye = (head.y - feet.y > 0.9f) ? Vec3f{feet.x, feet.y + settings.eye_height, feet.z} : head;
        v.position = eye + forward * 0.12f;
        v.arm = 0.0f;
        return v;
    }
    const Vec3f right = v.rotation.rotate({1.0f, 0.0f, 0.0f});
    // The pivot to the shoulder, unless that is inside rock (a cast from the head decides).
    Vec3f pivot = head;
    {
        QueryFilter3D f;
        f.mask = layer_bit(layers3d::Static);
        f.ignore = _ignore;
        const Vec3f side = right * settings.shoulder;
        const auto hit = world.shape_cast(Shape3D::sphere(settings.probe_radius * 0.6f), head, {}, side, f);
        pivot = head + side * (hit ? std::max(0.0f, hit->fraction - 0.05f) : 1.0f);
    }
    // The arm: a sphere cast back along the view.
    const f32 want = settings.distance;
    f32 arm = want;
    bool obstructed = false;
    QueryFilter3D f;
    f.mask = layer_bit(layers3d::Static);
    f.ignore = _ignore;
    const Vec3f back = -forward;
    const auto hit = world.shape_cast(Shape3D::sphere(settings.probe_radius), pivot, {}, back * want, f);
    if (hit) {
        arm = std::max(0.0f, hit->fraction * want - settings.margin);
        obstructed = true;
    }
    // Pulled in at once, eased out.
    if (_arm < 0.0f || arm < _arm) {
        _arm = arm;
    } else {
        _arm = std::min(arm, _arm + settings.ease_out * dt);
    }
    // Never past what the cast just found (an eased arm from before may be longer than the way now allows).
    _arm = std::min(_arm, arm);
    v.arm = _arm;
    v.obstructed = obstructed;
    v.position = pivot + back * _arm;
    return v;
}

} // namespace summit
