#pragma once

// Skeletal animation for kin's experimental 3D (docs/3d_plan.md, P1a): clips of keyframed joints (glTF's step, linear and
// cubic-spline interpolation), sampling a clip into a Pose, blending, additive clips, masks, root motion, and a small
// player with crossfades and layers. Plain data and maths on the CPU. Not kin/anim (the engine's general animation of
// registered properties, whose state machines can choose the clips and weights fed to this).
//
//     kin::AnimationPlayer3D player{&skeleton};
//     player.play(&walk);
//     player.crossfade_to(&run, 0.25f);
//     player.update(dt);
//     player.joint_matrices(matrices);
//     scene.draw_skinned(mesh, matrices, transform);

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/render3d/skeleton.hpp>

#include <span>
#include <string>
#include <vector>

namespace kin {

// How a track runs between its keyframes (glTF's sampler interpolation).
enum class Interpolation3D : u8 {
    Step,        // the value of the key before
    Linear,      // lerp (slerp for rotations)
    CubicSpline, // a Hermite spline; each key has an in-tangent, a value and an out-tangent (glTF's cubic spline)
};

// A joint property over time. `times` rise; there is a value for each (for a cubic spline, three: in-tangent, value and
// out-tangent, in that order). No keys: the property is not animated, and the joint keeps its rest value.
template <typename T>
struct Track3D {
    Interpolation3D interpolation = Interpolation3D::Linear;
    std::vector<f32> times;
    std::vector<T> values;

    bool empty() const { return times.empty(); }
    // Whether times and values agree in number (values = times, or three times that for a cubic spline).
    bool valid() const {
        return values.size() == times.size() * (interpolation == Interpolation3D::CubicSpline ? 3u : 1u);
    }
};

// One joint's tracks.
struct JointChannels {
    Track3D<Vec3f> translation;
    Track3D<Quatf> rotation;
    Track3D<Vec3f> scale;
};

// A keyframed animation of one skeleton's joints.
struct AnimClip3D {
    std::string name;
    f32 duration = 0.0f; // seconds: the last key of any track (the loader sets it)
    // Which of the model's skeletons (Model::skeletons) the clip animates.
    u32 skeleton = 0;
    // By joint index; shorter than the skeleton's joints is allowed (the rest are not animated).
    std::vector<JointChannels> channels;

    // The duration from the keys (the latest time of any track).
    f32 key_duration() const;
};

// A track's value at `time`, as glTF's spec has it: before the first key the first value, after the last the last; between
// two keys by the interpolation. Rotations are normalised. `fallback` when the track is empty.
Vec3f sample_track(const Track3D<Vec3f>& track, f32 time, Vec3f fallback);
Quatf sample_track(const Track3D<Quatf>& track, f32 time, Quatf fallback);

// The time to sample a clip at for a player's `time`: wrapped into [0, duration) when `loop` (always 0 for a clip with no
// duration), else held in [0, duration].
f32 clip_time(const AnimClip3D& clip, f32 time, bool loop);

// `clip` at `time` into `out` (sized to the skeleton): animated properties from the keys, the rest from the rest pose.
void sample_clip(const AnimClip3D& clip, const Skeleton& skeleton, f32 time, bool loop, Pose& out);

// --- combining poses ------------------------------------------------------------------------------------------------

// Between `a` (weight 0) and `b` (weight 1): translations and scales lerped, rotations slerped the short way.
Pose blend_poses(const Pose& a, const Pose& b, f32 weight);
// Normalised lerp of rotations: cheaper than slerp, the same path to within rounding for small angles.
Quatf nlerp(Quatf a, Quatf b, f32 t);
// `base` moved toward `layer` joint by joint, each by `mask` times `weight` (mask empty: all 1).
void blend_masked(Pose& base, const Pose& layer, const PoseMask& mask, f32 weight);
// `base` plus what `additive` changes from `reference` (an additive clip's first frame, usually: sample it at time 0), scaled
// by `weight` and `mask`: translations add the difference, rotations are turned by the one from `reference` to `additive`
// (its fraction of it by weight), scales add the difference.
void apply_additive(Pose& base, const Pose& additive, const Pose& reference, const PoseMask& mask, f32 weight);

// --- root motion ----------------------------------------------------------------------------------------------------

// Which parts of the root joint's motion a clip carries as motion of the character instead of a movement of the joint.
struct RootMotionAxes {
    bool x = true, y = false, z = true; // translation (the root's parent space, Y up: a walk moves in x and z)
    bool rotation = false;              // its rotation (a turn)
};

struct RootMotion {
    Vec3f translation{};
    Quatf rotation{};
};

// How far the root joint moved between `from` and `to` seconds of `clip`, in the chosen axes, in the root's parent space.
// With `loop`, a `to` earlier than `from` is taken as the clip having wrapped round its end once: the end's motion
// from `from` plus the start's to `to`; times beyond the duration wrap.
RootMotion extract_root_motion(const AnimClip3D& clip, const Skeleton& skeleton, f32 from, f32 to, bool loop, const RootMotionAxes& axes = {});
// `pose` with the root's motion removed: the chosen axes of its translation put back to the rest pose's (and its rotation to
// the rest rotation), so that the character moves by the extracted motion and not by the root joint.
void remove_root_motion(Pose& pose, const Skeleton& skeleton, const RootMotionAxes& axes = {});

// --- the player -------------------------------------------------------------------------------------------------------

// A point of a one-dimensional blend (a walk at speed 1, a run at speed 4): `clip` plays when the blend's parameter is at
// `position`, and between two neighbouring points their two clips are blended by how far the parameter is between them.
struct BlendPoint3D {
    const AnimClip3D* clip = nullptr;
    f32 position = 0.0f;
};

// A small animation player: a base layer with a clip and a crossfade to the next, and layers above it. Pointers to the
// skeleton and clips are not owned: they must outlive the player (a Model's, usually; its vectors do not move unless the
// Model is copied or changed).
class AnimationPlayer3D {
public:
    explicit AnimationPlayer3D(const Skeleton* skeleton = nullptr);

    void set_skeleton(const Skeleton* skeleton);
    const Skeleton* skeleton() const { return _skeleton; }

    // --- the base layer
    // Starts `clip` at `time` at once (no fade), replacing what played.
    void play(const AnimClip3D* clip, bool loop = true, f32 time = 0.0f);
    // Fades to `clip` over `seconds` (the old one keeps playing as it fades out; a fade begun in the middle of another
    // fades from the picture of the last update, held still, so a quick sequence of fades does not jump). Zero seconds: play().
    void crossfade_to(const AnimClip3D* clip, f32 seconds, bool loop = true, f32 time = 0.0f);
    // A one-dimensional blend as the base: the clips of `points` (sorted by position here; a null clip is dropped) blended by
    // `parameter` (clamped to the first and last position), the two clips either side of it. Their phases are kept together:
    // the base's time() is then a phase from 0 to 1, advanced by speed / (the blended duration), so a walk and a run step
    // in time whatever their lengths. `phase` is where it starts (a foot-matched start: the phase of the clip it follows).
    void play_blend(std::span<const BlendPoint3D> points, f32 parameter, bool loop = true, f32 phase = 0.0f);
    // The same as a fade from what plays now (a clip, a blend, or a fade in progress), as crossfade_to is.
    void crossfade_to_blend(std::span<const BlendPoint3D> points, f32 parameter, f32 seconds, bool loop = true, f32 phase = 0.0f);
    // Moves the base blend's parameter (the speed of a character): takes effect at the next update. Nothing for a plain clip.
    void set_blend_parameter(f32 parameter);
    f32 blend_parameter() const { return _base.parameter; }
    bool blending() const { return !_base.blend.empty(); }
    void set_speed(f32 speed) { _base.speed = speed; }
    f32 speed() const { return _base.speed; }
    // The base clip's time (a blend's: its phase, 0 to 1) and the clip (null: none, or a blend), and the one it is fading from
    // (null when not fading, or when that is a blend).
    f32 time() const { return _base.time; }
    void set_time(f32 time) { _base.time = time; }
    const AnimClip3D* clip() const { return _base.clip; }
    const AnimClip3D* fading_from() const { return _fading ? _from.clip : nullptr; }
    // The fade's progress, 0 to 1 (1 when not fading).
    f32 fade_progress() const { return _fading ? (_fade_seconds > 0.0f ? _fade_elapsed / _fade_seconds : 1.0f) : 1.0f; }
    bool finished() const; // the base clip (or blend) does not loop and its time has reached its end

    // --- layers above the base, in order (index 0 first)
    // A layer plays `clip` over the pose below it, joint by joint by `mask` times `weight`; additive layers add the clip's
    // change from its first frame instead. A layer with no clip does nothing.
    struct Layer {
        const AnimClip3D* clip = nullptr;
        f32 time = 0.0f;
        f32 speed = 1.0f;
        f32 weight = 1.0f;
        bool loop = true;
        bool additive = false;
        PoseMask mask; // empty: all joints
    };
    // The layer `index` (made as needed), to set its fields.
    Layer& layer(u32 index);
    u32 layer_count() const { return static_cast<u32>(_layers.size()); }
    void clear_layers() { _layers.clear(); }

    // --- root motion
    // With it on, the base layer's root joint is held at rest in the chosen axes, and the motion it would have made is
    // reported by root_motion() after each update.
    void set_root_motion(bool on, const RootMotionAxes& axes = {}) {
        _root_motion = on;
        _axes = axes;
    }
    // What the last update() moved the root by (zero when off).
    const RootMotion& root_motion() const { return _delta; }

    // Advances every clip by `dt` seconds (times its speed) and makes the pose.
    void update(f32 dt);
    // The pose of the last update (the rest pose before the first).
    const Pose& pose() const { return _pose; }
    // The matrices of that pose (compute_joint_matrices).
    void joint_matrices(std::vector<Mat4f>& out) const;
    void joint_matrices(std::span<Mat4f> out) const;

private:
    struct Playing {
        const AnimClip3D* clip = nullptr;
        f32 time = 0.0f; // seconds; for a blend its phase (0 to 1)
        f32 speed = 1.0f;
        bool loop = true;
        std::vector<BlendPoint3D> blend; // a 1D blend (then `clip` is null), sorted by position
        f32 parameter = 0.0f;
    };
    static void advance(Playing& p, f32 dt);
    void sample(const Playing& p, Pose& out) const;
    static void set_blend(Playing& p, std::span<const BlendPoint3D> points, f32 parameter);

    const Skeleton* _skeleton = nullptr;
    Playing _base;
    Playing _from;
    bool _fading = false;
    f32 _fade_elapsed = 0.0f;
    f32 _fade_seconds = 0.0f;
    // A fade begun while another runs: the picture at that moment, faded out in place of the clips.
    Pose _frozen;
    bool _from_frozen = false;
    std::vector<Layer> _layers;
    bool _root_motion = false;
    RootMotionAxes _axes;
    RootMotion _delta;
    Pose _pose;
    Pose _scratch, _scratch2, _reference;
};

} // namespace kin
