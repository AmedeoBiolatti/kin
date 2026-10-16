#include "camera.hpp"

#include <algorithm>
#include <cmath>

namespace summit {

namespace {

f32 wrap_angle(f32 a) {
    f32 d = std::fmod(a + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

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
        _prev_facing = _facing = p.facing_yaw;
        _spring_arm.reset();
        _swing = 0.0f;
        _have = true;
    } else {
        _prev_head = _head;
        _prev_yaw = _yaw;
        _prev_pitch = _pitch;
        _prev_feet = _feet;
        _prev_facing = _facing;
        _head = head;
        _yaw = p.yaw;
        _pitch = p.pitch;
        _feet = game.feet(player);
        _facing = p.facing_yaw;
    }
    _pitch_min = game.rules().pitch_min;
    _pitch_max = game.rules().pitch_max;
    _ignore = game.character_state(player).body;
    const TraversalState3D ts = game.traversal(player);
    _hanging = p.state == PlayerState::Alive && ts.hanging();
    // Knocked out, getting up or dead: the body is a ragdoll low on the ground (the head is its pelvis).
    _down = p.state != PlayerState::Alive;
}

void CameraRig::set_look_ahead(Vec2f look) {
    _look_ahead = (std::isfinite(look.x) && std::isfinite(look.y)) ? look : Vec2f{};
}

void CameraRig::reset() {
    _have = false;
    _spring_arm.reset();
}

Vec3f CameraRig::person_position(f32 alpha) const {
    return lerp(_prev_feet, _feet, std::clamp(alpha, 0.0f, 1.0f));
}

f32 CameraRig::person_facing(f32 alpha) const {
    return lerp_angle(_prev_facing, _facing, std::clamp(alpha, 0.0f, 1.0f));
}

CameraView CameraRig::frame(const World3D& world, f32 alpha, f32 dt) {
    CameraView v;
    v.fov = settings.fov;
    const f32 t = std::clamp(alpha, 0.0f, 1.0f);
    Vec3f head = lerp(_prev_head, _head, t);
    // The look is the player's hand as of this frame: the sim's angles as the last step left them, and the motion since that
    // the sim has not stepped yet. It is not interpolated between steps: a step takes the motion of the frame it runs in, so
    // that steps are uneven in what they carry (1, 2, 1... frames' worth when frames come faster than steps), and an angle
    // interpolated between them shudders at the step rate. The angle of the latest step plus the leftover is the integral of
    // the hand itself.
    const f32 yaw = wrap_angle(_yaw - _look_ahead.x);
    const f32 pitch = std::clamp(_pitch + _look_ahead.y, _pitch_min, _pitch_max);
    // The hanging view eases to one side (presentation: the player's own look, and so the hands' reach, are the sim's).
    _swing += ((_hanging ? 1.0f : 0.0f) - _swing) * std::min(1.0f, dt * 3.0f);
    // A body on the ground is framed whole: the pivot above it, the view tipped down, the arm longer (presentation only).
    _lie += ((_down ? 1.0f : 0.0f) - _lie) * std::min(1.0f, dt * 2.5f);
    if (!_down) {
        _lie_pick = true;
    } else if (_lie_pick && !settings.first_person) {
        // The camera for a body on the ground: the way round it that is clearest of rock (the body may lie in a chimney or at a
        // wall's foot), chosen once when it goes down so that the view does not hop about.
        _lie_pick = false;
        QueryFilter3D f;
        f.mask = layer_bit(layers3d::Static);
        f.ignore = _ignore;
        const Vec3f pivot = head + Vec3f{0.0f, settings.lie_height, 0.0f};
        f32 best_score = -1e9f;
        for (const f32 off : {0.0f, 45.0f, -45.0f, 90.0f, -90.0f, 135.0f, -135.0f, 180.0f}) {
            const Quatf q = Quatf::yaw_pitch(yaw + off, settings.lie_pitch);
            const Vec3f back = q.rotate({0.0f, 0.0f, 1.0f});
            const auto hit = world.shape_cast(Shape3D::sphere(settings.probe_radius), pivot, {}, back * settings.lie_distance, f);
            const f32 free = hit ? hit->fraction * settings.lie_distance : settings.lie_distance;
            const f32 score = std::min(free, settings.lie_distance) - 0.004f * std::fabs(off);
            if (score > best_score) {
                best_score = score;
                _lie_offset = off;
            }
        }
    }
    const f32 view_yaw = yaw + settings.hang_yaw * _swing + _lie_offset * _lie;
    f32 view_pitch = std::clamp(pitch + settings.hang_pitch * _swing, -85.0f, 85.0f);
    view_pitch += (settings.lie_pitch - view_pitch) * _lie;
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
    if (_lie > 0.0f) head.y += settings.lie_height * _lie;
    const f32 want = settings.distance + (settings.lie_distance - settings.distance) * _lie;
    if (!_spring_arm.configure({.distance=want,.shoulder=settings.shoulder,.probe_radius=settings.probe_radius,
        .margin=settings.margin,.ease_out=settings.ease_out,.shoulder_radius_scale=.6f})) return v;
    QueryFilter3D filter;filter.mask=layer_bit(layers3d::Static);filter.ignore=_ignore;
    const auto solved=_spring_arm.frame(world,head,v.rotation,dt,filter);
    if (!solved) return v;
    v.arm = solved->length;
    v.obstructed = solved->obstructed;
    v.fov = settings.fov + settings.tight_fov_boost * std::clamp((2.4f - v.arm) / 1.6f, 0.0f, 1.0f);
    v.position = solved->position;
    return v;
}

} // namespace summit
