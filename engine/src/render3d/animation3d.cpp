#include <kin/render3d/animation3d.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace kin {

namespace {

// The key at or before `time` (the index k with times[k] <= time < times[k + 1]), for a track with at least two keys and
// a time inside it.
std::size_t key_before(const std::vector<f32>& times, f32 time) {
    const auto it = std::upper_bound(times.begin(), times.end(), time);
    return static_cast<std::size_t>(it - times.begin()) - 1;
}

Quatf negated(Quatf q) { return {-q.x, -q.y, -q.z, -q.w}; }

Vec3f mix(Vec3f a, Vec3f b, f32 t) { return a + (b - a) * t; }

// glTF's cubic spline between the keys k and k + 1 (spec, appendix C): the Hermite basis, the tangents scaled by the time
// between the keys. `at(key, part)` gives part 0 the in-tangent, 1 the value, 2 the out-tangent.
template <typename T, typename Add, typename Scale>
T cubic(const Track3D<T>& track, std::size_t k, f32 t, const Add& add, const Scale& scale) {
    const f32 dt = track.times[k + 1] - track.times[k];
    const f32 s = dt > 0.0f ? (t - track.times[k]) / dt : 0.0f;
    const f32 s2 = s * s, s3 = s2 * s;
    const T& v0 = track.values[k * 3 + 1];
    const T& b0 = track.values[k * 3 + 2]; // out-tangent of the key before
    const T& a1 = track.values[(k + 1) * 3];   // in-tangent of the key after
    const T& v1 = track.values[(k + 1) * 3 + 1];
    return add(add(scale(v0, 2.0f * s3 - 3.0f * s2 + 1.0f), scale(b0, (s3 - 2.0f * s2 + s) * dt)),
               add(scale(v1, -2.0f * s3 + 3.0f * s2), scale(a1, (s3 - s2) * dt)));
}

// A mask's weight for joint `j`: all 1 when it is empty, 0 past its end.
f32 mask_weight(const PoseMask& mask, std::size_t j) {
    return mask.weights.empty() ? 1.0f : j < mask.weights.size() ? mask.weights[j] : 0.0f;
}

template <typename T>
bool usable(const Track3D<T>& track) {
    return !track.times.empty() && track.valid();
}

} // namespace

f32 AnimClip3D::key_duration() const {
    f32 d = 0.0f;
    for (const JointChannels& c : channels) {
        for (const auto* times : {&c.translation.times, &c.rotation.times, &c.scale.times}) {
            if (!times->empty()) {
                d = std::max(d, times->back());
            }
        }
    }
    return d;
}

Vec3f sample_track(const Track3D<Vec3f>& track, f32 time, Vec3f fallback) {
    if (!usable(track)) {
        return fallback;
    }
    const bool spline = track.interpolation == Interpolation3D::CubicSpline;
    const std::size_t last = track.times.size() - 1;
    const auto value_at = [&](std::size_t k) { return track.values[spline ? k * 3 + 1 : k]; };
    if (track.times.size() == 1 || !(time > track.times.front())) {
        return value_at(0);
    }
    if (time >= track.times[last]) {
        return value_at(last);
    }
    const std::size_t k = key_before(track.times, time);
    switch (track.interpolation) {
    case Interpolation3D::Step: return track.values[k];
    case Interpolation3D::Linear: {
        const f32 dt = track.times[k + 1] - track.times[k];
        return mix(track.values[k], track.values[k + 1], dt > 0.0f ? (time - track.times[k]) / dt : 0.0f);
    }
    case Interpolation3D::CubicSpline:
        return cubic(track, k, time, [](Vec3f a, Vec3f b) { return a + b; }, [](Vec3f a, f32 s) { return a * s; });
    }
    return fallback;
}

Quatf sample_track(const Track3D<Quatf>& track, f32 time, Quatf fallback) {
    if (!usable(track)) {
        return fallback;
    }
    const bool spline = track.interpolation == Interpolation3D::CubicSpline;
    const std::size_t last = track.times.size() - 1;
    const auto value_at = [&](std::size_t k) { return track.values[spline ? k * 3 + 1 : k].normalized(); };
    if (track.times.size() == 1 || !(time > track.times.front())) {
        return value_at(0);
    }
    if (time >= track.times[last]) {
        return value_at(last);
    }
    const std::size_t k = key_before(track.times, time);
    switch (track.interpolation) {
    case Interpolation3D::Step: return track.values[k].normalized();
    case Interpolation3D::Linear: {
        const f32 dt = track.times[k + 1] - track.times[k];
        return slerp(track.values[k].normalized(), track.values[k + 1].normalized(), dt > 0.0f ? (time - track.times[k]) / dt : 0.0f);
    }
    case Interpolation3D::CubicSpline: {
        // The same Hermite sum over the four components, then normalised (glTF: the result of the spline is).
        const auto add = [](Quatf a, Quatf b) { return Quatf{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; };
        const auto scale = [](Quatf a, f32 s) { return Quatf{a.x * s, a.y * s, a.z * s, a.w * s}; };
        return cubic(track, k, time, add, scale).normalized();
    }
    }
    return fallback;
}

f32 clip_time(const AnimClip3D& clip, f32 time, bool loop) {
    if (!(clip.duration > 0.0f) || !std::isfinite(time)) {
        return 0.0f;
    }
    if (loop) {
        f32 t = std::fmod(time, clip.duration);
        if (t < 0.0f) {
            t += clip.duration;
        }
        return t >= clip.duration ? 0.0f : t;
    }
    return std::clamp(time, 0.0f, clip.duration);
}

void sample_clip(const AnimClip3D& clip, const Skeleton& skeleton, f32 time, bool loop, Pose& out) {
    out.joints = skeleton.rest;
    const f32 t = clip_time(clip, time, loop);
    const std::size_t n = std::min<std::size_t>(clip.channels.size(), out.joints.size());
    for (std::size_t j = 0; j < n; ++j) {
        const JointChannels& c = clip.channels[j];
        JointPose& p = out.joints[j];
        p.translation = sample_track(c.translation, t, p.translation);
        p.rotation = sample_track(c.rotation, t, p.rotation);
        p.scale = sample_track(c.scale, t, p.scale);
    }
}

// --- combining -------------------------------------------------------------------------------------------------------

Quatf nlerp(Quatf a, Quatf b, f32 t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) {
        b = negated(b); // the short way round
    }
    return Quatf{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}.normalized();
}

Pose blend_poses(const Pose& a, const Pose& b, f32 weight) {
    const std::size_t n = std::min(a.joints.size(), b.joints.size());
    Pose out;
    out.joints.resize(n);
    for (std::size_t j = 0; j < n; ++j) {
        out.joints[j].translation = mix(a.joints[j].translation, b.joints[j].translation, weight);
        out.joints[j].rotation = slerp(a.joints[j].rotation, b.joints[j].rotation, weight);
        out.joints[j].scale = mix(a.joints[j].scale, b.joints[j].scale, weight);
    }
    return out;
}

void blend_masked(Pose& base, const Pose& layer, const PoseMask& mask, f32 weight) {
    const std::size_t n = std::min(base.joints.size(), layer.joints.size());
    for (std::size_t j = 0; j < n; ++j) {
        const f32 w = std::clamp(mask_weight(mask, j) * weight, 0.0f, 1.0f);
        if (w <= 0.0f) {
            continue;
        }
        JointPose& b = base.joints[j];
        const JointPose& l = layer.joints[j];
        b.translation = mix(b.translation, l.translation, w);
        b.rotation = slerp(b.rotation, l.rotation, w);
        b.scale = mix(b.scale, l.scale, w);
    }
}

void apply_additive(Pose& base, const Pose& additive, const Pose& reference, const PoseMask& mask, f32 weight) {
    const std::size_t n = std::min({base.joints.size(), additive.joints.size(), reference.joints.size()});
    for (std::size_t j = 0; j < n; ++j) {
        const f32 w = mask_weight(mask, j) * weight;
        if (w == 0.0f) {
            continue;
        }
        JointPose& b = base.joints[j];
        const JointPose& a = additive.joints[j];
        const JointPose& r = reference.joints[j];
        b.translation = b.translation + (a.translation - r.translation) * w;
        // The turn from the reference to the additive key, a fraction w of it, applied after the base's own.
        const Quatf delta = (a.rotation * r.rotation.conjugate()).normalized();
        b.rotation = (slerp(Quatf{}, delta, w) * b.rotation).normalized();
        b.scale = b.scale + (a.scale - r.scale) * w;
    }
}

// --- root motion -----------------------------------------------------------------------------------------------------

namespace {

JointPose root_at(const AnimClip3D& clip, const Skeleton& skeleton, f32 time, bool loop) {
    const i32 root = skeleton.root_joint();
    if (root < 0) {
        return {};
    }
    JointPose p = skeleton.rest[static_cast<std::size_t>(root)];
    if (static_cast<std::size_t>(root) < clip.channels.size()) {
        const JointChannels& c = clip.channels[static_cast<std::size_t>(root)];
        const f32 t = clip_time(clip, time, loop);
        p.translation = sample_track(c.translation, t, p.translation);
        p.rotation = sample_track(c.rotation, t, p.rotation);
    }
    return p;
}

Vec3f masked(Vec3f v, const RootMotionAxes& axes) {
    return {axes.x ? v.x : 0.0f, axes.y ? v.y : 0.0f, axes.z ? v.z : 0.0f};
}

} // namespace

RootMotion extract_root_motion(const AnimClip3D& clip, const Skeleton& skeleton, f32 from, f32 to, bool loop, const RootMotionAxes& axes) {
    RootMotion motion;
    if (skeleton.root_joint() < 0 || !(clip.duration > 0.0f)) {
        return motion;
    }
    const f32 t0 = clip_time(clip, from, loop);
    const f32 t1 = clip_time(clip, to, loop);
    // Wrapped: the end's motion from t0, then the start's to t1. Going backwards within a clip is just negative motion.
    const bool wrapped = loop && to > from && t1 < t0;
    if (wrapped) {
        const JointPose a = root_at(clip, skeleton, t0, false);
        const JointPose end = root_at(clip, skeleton, clip.duration, false);
        const JointPose start = root_at(clip, skeleton, 0.0f, false);
        const JointPose b = root_at(clip, skeleton, t1, false);
        motion.translation = masked((end.translation - a.translation) + (b.translation - start.translation), axes);
        if (axes.rotation) {
            motion.rotation = ((b.rotation * start.rotation.conjugate()) * (end.rotation * a.rotation.conjugate())).normalized();
        }
        return motion;
    }
    const JointPose a = root_at(clip, skeleton, t0, false);
    const JointPose b = root_at(clip, skeleton, t1, false);
    motion.translation = masked(b.translation - a.translation, axes);
    if (axes.rotation) {
        motion.rotation = (b.rotation * a.rotation.conjugate()).normalized();
    }
    return motion;
}

void remove_root_motion(Pose& pose, const Skeleton& skeleton, const RootMotionAxes& axes) {
    const i32 root = skeleton.root_joint();
    if (root < 0 || static_cast<std::size_t>(root) >= pose.joints.size()) {
        return;
    }
    JointPose& p = pose.joints[static_cast<std::size_t>(root)];
    const JointPose& rest = skeleton.rest[static_cast<std::size_t>(root)];
    if (axes.x) p.translation.x = rest.translation.x;
    if (axes.y) p.translation.y = rest.translation.y;
    if (axes.z) p.translation.z = rest.translation.z;
    if (axes.rotation) p.rotation = rest.rotation;
}

// --- the player ------------------------------------------------------------------------------------------------------

AnimationPlayer3D::AnimationPlayer3D(const Skeleton* skeleton) {
    set_skeleton(skeleton);
}

void AnimationPlayer3D::set_skeleton(const Skeleton* skeleton) {
    // Rebinding must also revoke a completed token and stop an already
    // cancelling fade, whose borrowed clip may belong to the old model.
    _action.clip = nullptr; _action.weight = 0;
    _action.status = _action.token ? ActionStatus3D::Cancelled : ActionStatus3D::None;
    _skeleton = skeleton;
    _pose = skeleton ? skeleton->rest_pose() : Pose{};
}

namespace {

// A blend's two clips either side of the parameter, how far toward the second, and the duration blended between theirs (the
// speed of its phase). Both clips are the same one at either end of the points and for a single point.
struct BlendPair {
    const AnimClip3D* a = nullptr;
    const AnimClip3D* b = nullptr;
    f32 weight = 0.0f;
    f32 duration = 0.0f;
};

template <typename Points>
BlendPair blend_pair(const Points& points, f32 parameter) {
    BlendPair pair;
    if (points.empty()) {
        return pair;
    }
    if (parameter <= points.front().position || points.size() == 1) {
        pair.a = pair.b = points.front().clip;
    } else if (parameter >= points.back().position) {
        pair.a = pair.b = points.back().clip;
    } else {
        std::size_t hi = 1;
        while (hi + 1 < points.size() && points[hi].position < parameter) {
            ++hi;
        }
        const auto& lo = points[hi - 1];
        const auto& up = points[hi];
        pair.a = lo.clip;
        pair.b = up.clip;
        const f32 span = up.position - lo.position;
        pair.weight = span > 0.0f ? std::clamp((parameter - lo.position) / span, 0.0f, 1.0f) : 0.0f;
    }
    pair.duration = pair.a->duration + (pair.b->duration - pair.a->duration) * pair.weight;
    return pair;
}

} // namespace

void AnimationPlayer3D::set_blend(Playing& p, std::span<const BlendPoint3D> points, f32 parameter) {
    p.clip = nullptr;
    p.blend.clear();
    for (const BlendPoint3D& point : points) {
        if (point.clip) {
            p.blend.push_back(point);
        }
    }
    std::stable_sort(p.blend.begin(), p.blend.end(), [](const BlendPoint3D& x, const BlendPoint3D& y) { return x.position < y.position; });
    p.parameter = parameter;
}

void AnimationPlayer3D::play(const AnimClip3D* clip, bool loop, f32 time) {
    _base = Playing{clip, time, _base.speed, loop, {}, 0.0f};
    _fading = false;
    _from_frozen = false;
}

void AnimationPlayer3D::play_blend(std::span<const BlendPoint3D> points, f32 parameter, bool loop, f32 phase) {
    _base = Playing{nullptr, phase, _base.speed, loop, {}, 0.0f};
    set_blend(_base, points, parameter);
    _fading = false;
    _from_frozen = false;
}

void AnimationPlayer3D::crossfade_to(const AnimClip3D* clip, f32 seconds, bool loop, f32 time) {
    if (!(seconds > 0.0f) || ((!_base.clip && _base.blend.empty()) && !_fading)) {
        play(clip, loop, time);
        return;
    }
    if (_fading) {
        // A fade in the middle of another: the picture now, held, fades out, so nothing jumps.
        _frozen = _pose;
        _from_frozen = true;
    } else {
        _from = _base;
        _from_frozen = false;
    }
    _base = Playing{clip, time, _base.speed, loop, {}, 0.0f};
    _fading = true;
    _fade_elapsed = 0.0f;
    _fade_seconds = seconds;
}

void AnimationPlayer3D::crossfade_to_blend(std::span<const BlendPoint3D> points, f32 parameter, f32 seconds, bool loop, f32 phase) {
    if (!(seconds > 0.0f) || ((!_base.clip && _base.blend.empty()) && !_fading)) {
        play_blend(points, parameter, loop, phase);
        return;
    }
    if (_fading) {
        _frozen = _pose;
        _from_frozen = true;
    } else {
        _from = _base;
        _from_frozen = false;
    }
    _base = Playing{nullptr, phase, _base.speed, loop, {}, 0.0f};
    set_blend(_base, points, parameter);
    _fading = true;
    _fade_elapsed = 0.0f;
    _fade_seconds = seconds;
}

void AnimationPlayer3D::set_blend_parameter(f32 parameter) {
    if (!_base.blend.empty()) {
        _base.parameter = parameter;
    }
}

bool AnimationPlayer3D::finished() const {
    if (!_base.blend.empty()) {
        return !_base.loop && _base.time >= 1.0f;
    }
    return _base.clip && !_base.loop && _base.time >= _base.clip->duration;
}

AnimationPlayer3D::Layer& AnimationPlayer3D::layer(u32 index) {
    if (_layers.size() <= index) {
        _layers.resize(index + 1);
    }
    return _layers[index];
}

void AnimationPlayer3D::advance(Playing& p, f32 dt) {
    if (!p.blend.empty()) {
        // A blend's time is a phase: it advances by the time over the duration blended at the parameter now.
        const BlendPair pair = blend_pair(p.blend, p.parameter);
        if (pair.duration > 0.0f) {
            p.time += dt * p.speed / pair.duration;
            p.time = p.loop ? p.time - std::floor(p.time) : std::clamp(p.time, 0.0f, 1.0f);
        } else {
            p.time = 0.0f;
        }
        return;
    }
    if (!p.clip) {
        return;
    }
    p.time += dt * p.speed;
    if (p.clip->duration > 0.0f) {
        if (p.loop) {
            p.time = clip_time(*p.clip, p.time, true);
        } else {
            p.time = std::clamp(p.time, 0.0f, p.clip->duration);
        }
    } else {
        p.time = 0.0f;
    }
}

void AnimationPlayer3D::sample(const Playing& p, Pose& out) const {
    if (!p.blend.empty()) {
        const BlendPair pair = blend_pair(p.blend, p.parameter);
        // Each clip at the same phase of its own length; (for a looped phase of exactly 1 the clip's end, as the wrap leaves 0).
        sample_clip(*pair.a, *_skeleton, p.time * pair.a->duration, p.loop, out);
        if (pair.b != pair.a && pair.weight > 0.0f) {
            Pose second;
            sample_clip(*pair.b, *_skeleton, p.time * pair.b->duration, p.loop, second);
            out = blend_poses(out, second, pair.weight);
        }
    } else if (p.clip) {
        sample_clip(*p.clip, *_skeleton, p.time, p.loop, out);
    } else {
        out = _skeleton->rest_pose();
    }
}

namespace {
std::atomic<u64> next_action_token{1};
ActionToken3D action_token() {
    auto next = next_action_token.load(std::memory_order_relaxed);
    while (next != std::numeric_limits<u64>::max())
        if (next_action_token.compare_exchange_weak(next, next + 1, std::memory_order_relaxed)) return {next};
    return {}; // never recycle an identity
}
}

ActionToken3D AnimationPlayer3D::play_action(const AnimClip3D* clip, const ActionPlayback3D& o, ActionStart3D start) {
    if (!_skeleton || !clip || !std::isfinite(clip->duration) || clip->duration <= 0 ||
        !std::isfinite(o.speed) || o.speed <= 0 || !std::isfinite(o.weight) || o.weight <= 0 || o.weight > 1 ||
        !std::isfinite(o.fade_in) || o.fade_in < 0 || !std::isfinite(o.fade_out) || o.fade_out < 0 ||
        o.mask_joint < -1 || o.mask_joint >= i32(_skeleton->joint_count()) ||
        (start == ActionStart3D::IfIdle && action_busy())) return {};
    const auto token = action_token();
    if (!token) return {};
    if (o.mask_joint >= 0) _action.mask.set_subtree(*_skeleton, o.mask_joint, 1, o.include_root);
    else _action.mask.weights.clear();
    _action.clip = clip; _action.options = o; _action.token = token; _action.status = ActionStatus3D::Playing;
    _action.time = _action.elapsed = _action.fade_elapsed = _action.fade_weight = 0;
    _action.weight = o.fade_in > 0 ? 0 : o.weight;
    return token;
}

bool AnimationPlayer3D::action_busy() const {
    return _action.status == ActionStatus3D::Playing || _action.status == ActionStatus3D::Finishing ||
        (_action.status == ActionStatus3D::Cancelled && _action.clip);
}
ActionStatus3D AnimationPlayer3D::action_status(ActionToken3D token) const {
    if (!token) return ActionStatus3D::None;
    return token == _action.token ? _action.status : ActionStatus3D::Superseded;
}
bool AnimationPlayer3D::action_event_valid(ActionToken3D token) const {
    const auto status = action_status(token);
    return status == ActionStatus3D::Playing || status == ActionStatus3D::Finishing || status == ActionStatus3D::Completed;
}
bool AnimationPlayer3D::cancel_action(ActionToken3D token, f32 seconds) {
    if (token != _action.token || !token || _action.status == ActionStatus3D::None || !std::isfinite(seconds) || (seconds < 0 && seconds != -1) || _action.status == ActionStatus3D::Cancelled) return false;
    if (seconds < 0) seconds = _action.options.fade_out;
    _action.status = ActionStatus3D::Cancelled; _action.fade_elapsed = 0; _action.fade_weight = _action.weight;
    _action.options.fade_out = seconds;
    if (seconds <= 0 || _action.weight <= 0) { _action.weight = 0; _action.clip = nullptr; }
    return true;
}
void AnimationPlayer3D::advance_action(f32 dt) {
    if (!_action.clip) return;
    auto& a = _action;
    if (a.status == ActionStatus3D::Playing) {
        const double remaining = std::max(0.0, double(a.clip->duration - a.time) / a.options.speed);
        const f32 playing_dt = f32(std::min(double(dt), remaining));
        a.time = std::min(a.clip->duration, a.time + playing_dt * a.options.speed);
        a.elapsed += playing_dt;
        a.weight = a.options.weight * (a.options.fade_in > 0 ? std::clamp(a.elapsed / a.options.fade_in, 0.f, 1.f) : 1.f);
        if (double(dt) < remaining && a.time < a.clip->duration) return;
        a.time = a.clip->duration; a.status = ActionStatus3D::Finishing;
        a.fade_weight = a.weight; a.fade_elapsed = f32(std::max(0.0, double(dt) - remaining));
    } else a.fade_elapsed += dt;
    if (a.status == ActionStatus3D::Finishing || a.status == ActionStatus3D::Cancelled) {
        const float fraction = a.options.fade_out > 0 ? std::clamp(a.fade_elapsed / a.options.fade_out, 0.f, 1.f) : 1.f;
        a.weight = a.fade_weight * (1 - fraction);
        if (fraction >= 1 || a.fade_weight <= 0) {
            if (a.status == ActionStatus3D::Finishing) a.status = ActionStatus3D::Completed;
            a.clip = nullptr; a.weight = 0;
        }
    }
}

void AnimationPlayer3D::update(f32 dt) {
    AnimationEventBatch3D ignored;
    update(dt, {}, ignored);
}

void AnimationPlayer3D::update(f32 dt, std::span<const ClipEventTrack3D> tracks, AnimationEventBatch3D& events) {
    events.clear();
    if (!std::isfinite(dt) || dt < 0 || !std::isfinite(_base.speed)) {
        _delta = {};
        return;
    }
    if (!_skeleton) {
        return;
    }
    _delta = {};
    std::array<double, AnimationEventBatch3D::capacity> distances{};
    double total = 0;
    const auto gather = [&](const AnimClip3D* source, double before, double after, double span, bool loop, ActionToken3D token) {
        if (!source || !std::isfinite(before) || !std::isfinite(after) || span <= 0) return;
        if (loop) { const double cycle = std::floor(before); before -= cycle; after -= cycle; }
        else { before = std::clamp(before, 0.0, 1.0); after = std::clamp(after, 0.0, 1.0); }
        const bool forward = after > before;
        for (const auto& track : tracks) {
            if (track.clip != source->name || (token ? track.source == MarkerSource3D::Base : track.source == MarkerSource3D::Action)) continue;
            for (const auto& marker : track.markers) {
                const double phase = marker.phase;
                if (!std::isfinite(phase) || phase < 0 || phase >= 1 || after == before) continue;
                double first = phase, count = 0;
                if (loop) {
                    first += forward ? std::floor(before - phase) + 1 : std::ceil(before - phase) - 1;
                    count = forward ? std::floor(after - phase) - std::floor(before - phase)
                                    : std::ceil(before - phase) - std::ceil(after - phase);
                } else count = forward ? (phase > before && phase <= after) : (phase < before && phase >= after);
                if (count <= 0) continue;
                total += count;
                for (u32 n = 0; n < AnimationEventBatch3D::capacity && double(n) < count; ++n) {
                    const double distance = std::abs(first + (forward ? double(n) : -double(n)) - before) / span;
                    u32 at = 0;
                    while (at < events.count && distances[at] <= distance) ++at;
                    if (at >= AnimationEventBatch3D::capacity) break;
                    const u32 last = std::min(events.count, u32(AnimationEventBatch3D::capacity - 1));
                    for (u32 i = last; i > at; --i) {
                        events.entries[i] = events.entries[i-1]; events.actions[i] = events.actions[i-1]; distances[i] = distances[i-1];
                    }
                    events.entries[at] = &marker.event; events.actions[at] = token; distances[at] = distance;
                    events.count = std::min(events.count + 1, u32(AnimationEventBatch3D::capacity));
                }
            }
        }
    };
    const bool suppress_base = action_busy() && !_action.options.base_events;
    if (!tracks.empty() && !suppress_base) {
        const AnimClip3D* source = _base.clip;
        double duration = source ? source->duration : 0;
        const bool blend = !_base.blend.empty();
        if (blend) { const auto pair = blend_pair(_base.blend, _base.parameter); source = pair.weight >= .5f ? pair.b : pair.a; duration = pair.duration; }
        if (source && std::isfinite(duration) && duration > 0) {
            const double scale = blend ? 1 : duration;
            const double before = double(_base.time) / scale;
            const f32 next = _base.time + dt * _base.speed / f32(duration / scale);
            const double after = std::isfinite(next) ? double(next) / scale : before + double(dt) * _base.speed / duration;
            gather(source, before, after, std::abs(after - before), _base.loop, {});
        }
    }
    if (!tracks.empty() && _action.status == ActionStatus3D::Playing && _action.clip) {
        const double duration = _action.clip->duration;
        const double before = _action.time / duration;
        const f32 next = _action.time + dt * _action.options.speed;
        const double after = std::isfinite(next) ? double(next) / duration : before + double(dt) * _action.options.speed / duration;
        gather(_action.clip, before, after, double(dt) * _action.options.speed / duration, false, _action.token);
    }
    events.dropped = u32(std::min(std::max(0.0, total - events.count), double(std::numeric_limits<u32>::max())));
    const f32 base_before = _base.time;
    const Playing from_before = _from;
    advance(_base, dt);
    if (_fading) {
        advance(_from, dt);
        _fade_elapsed += dt;
    }
    // Root motion of the base layer: the clip's own motion between the times, faded like the pictures are.
    const auto motion_of = [&](const Playing& before, const Playing& after) {
        RootMotion m;
        if (!before.blend.empty()) {
            // A blend: each of its two clips' motion over the same span of phase, mixed as the pictures are.
            const BlendPair pair = blend_pair(before.blend, before.parameter);
            const f32 end = before.loop ? before.time + (pair.duration > 0.0f ? dt * before.speed / pair.duration : 0.0f) : after.time;
            const auto of = [&](const AnimClip3D* clip) {
                return extract_root_motion(*clip, *_skeleton, before.time * clip->duration, end * clip->duration, before.loop, _axes);
            };
            m = of(pair.a);
            if (pair.b != pair.a && pair.weight > 0.0f) {
                const RootMotion other = of(pair.b);
                m.translation = mix(m.translation, other.translation, pair.weight);
                m.rotation = nlerp(m.rotation, other.rotation, pair.weight);
            }
        } else if (before.clip) {
            // A clip that did not wrap or stop at its end moved from `before` to `after`; one that wrapped carries on past
            // the end (extract_root_motion wraps when `to` is earlier and the clip loops).
            const f32 span = dt * before.speed;
            const f32 to = before.loop ? before.time + span : after.time;
            m = extract_root_motion(*before.clip, *_skeleton, before.time, to, before.loop, _axes);
        }
        return m;
    };
    Playing base_start = _base;
    base_start.time = base_before;
    if (_root_motion) {
        _delta = motion_of(base_start, _base);
    }
    sample(_base, _pose);
    if (_fading) {
        const f32 alpha = _fade_seconds > 0.0f ? std::clamp(_fade_elapsed / _fade_seconds, 0.0f, 1.0f) : 1.0f;
        if (_from_frozen) {
            _pose = blend_poses(_frozen, _pose, alpha);
        } else {
            sample(_from, _scratch);
            if (_root_motion) {
                const RootMotion old = motion_of(from_before, _from);
                _delta.translation = mix(old.translation, _delta.translation, alpha);
                _delta.rotation = nlerp(old.rotation, _delta.rotation, alpha);
            }
            _pose = blend_poses(_scratch, _pose, alpha);
        }
        if (alpha >= 1.0f) {
            _fading = false;
            _from_frozen = false;
        }
    }
    if (_root_motion) {
        remove_root_motion(_pose, *_skeleton, _axes);
    }
    for (Layer& layer : _layers) {
        if (!layer.clip) {
            continue;
        }
        Playing p{layer.clip, layer.time, layer.speed, layer.loop, {}, 0.0f};
        advance(p, dt);
        layer.time = p.time;
        sample(p, _scratch);
        if (layer.additive) {
            sample_clip(*layer.clip, *_skeleton, 0.0f, layer.loop, _reference);
            apply_additive(_pose, _scratch, _reference, layer.mask, layer.weight);
        } else {
            blend_masked(_pose, _scratch, layer.mask, layer.weight);
        }
    }
    advance_action(dt);
    if (_action.clip && _action.weight > 0) {
        sample_clip(*_action.clip, *_skeleton, _action.time, false, _scratch);
        if (_action.options.additive) {
            sample_clip(*_action.clip, *_skeleton, 0, false, _reference);
            apply_additive(_pose, _scratch, _reference, _action.mask, _action.weight);
        } else blend_masked(_pose, _scratch, _action.mask, _action.weight);
    }
}

void AnimationPlayer3D::joint_matrices(std::vector<Mat4f>& out) const {
    if (_skeleton) {
        compute_joint_matrices(*_skeleton, _pose, out);
    } else {
        out.clear();
    }
}

void AnimationPlayer3D::joint_matrices(std::span<Mat4f> out) const {
    if (_skeleton) {
        compute_joint_matrices(*_skeleton, _pose, out);
    }
}

} // namespace kin
