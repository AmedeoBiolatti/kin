#include <stdexcept>
// Renders the reference scenes (docs/3d_plan.md, G0): each camera of each
// scene to a PNG, timed, with what was drawn, into a folder a later run (or
// Cycles' pictures) is compared with (tools/reference/run.py).
//
//   kin_reference <scene.json>... --out DIR [--cache DIR] [--frames N]
//                 [--warmup N] [--repeat N] [--width W --height H]
//                 [--cascades N] [--shadow-distance D]
//                 [--ao] [--ao-radius R] [--ao-intensity I] [--ao-half]
//                 [--contact-shadows] [--contact-distance D] [--contact-steps N]
//                 [--contact-thickness T]
//                 [--taa] [--taa-blend B] [--taa-sharpness S] [--taa-frames N]
//                 [--supersample N] [--samples N]
//                 [--lod] [--lod-error PX] [--lod-bias B] [--lod-force N] [--lod-levels N] [--lod-clusters]
//                 [--lod-gpu | --lod-cpu] [--readback] [--gpu-pass-markers] [--lod-room TRIANGLES] [--lod-occlusion]
//                 [--bake-cache DIR]
//
// --bake-cache DIR keeps the levels of detail and cluster hierarchies the run builds in DIR
// (Renderer3D::set_bake_cache), and reads them back on the next run with the same DIR instead of building them: the
// pictures are the same.
//
// A scene file names its models (in the cache, tools/reference/fetch.py),
// its cameras, lights and view; `procedural: "crowd"` builds Signal
// Breach's deck and 20,000 enemies in code. Times: `render()` on the CPU,
// and the GPU's frame with the view rendered --repeat times in it (divided
// by that: the 3D work then outweighs the rest of the frame and the
// timer's noise), median, p10 and p90 over --frames frames. A scene's
// `lights` may set `shadow_cascades` and `shadow_distance`; --cascades and
// --shadow-distance set them for every scene of a run instead (a label of
// its own, so the default pictures stay comparable). A scene file may have
// `ambient_occlusion: {enabled, radius, intensity, half_resolution}`; --ao turns
// it on for every scene (with --ao-radius, --ao-intensity and --ao-half to
// set the rest), likewise a label of its own. Likewise
// `contact_shadows: {enabled, distance, thickness, intensity, steps}` and
// --contact-shadows (with --contact-distance, --contact-steps, --contact-thickness).
// And `taa: {enabled, blend, sharpness, jitter_scale, jitter_samples}` and --taa (with
// --taa-blend, --taa-sharpness): temporal anti-aliasing, whose history needs frames to
// settle, so --taa-frames N makes the run at least N frames before each picture (the
// timing loop's own frames count; the default is 32). Every frame is a step of time:
// a scene with `"animated": true` (the procedural `motion`) is posed by the frame's
// time, which is 0 at the picture and negative before it (so every run's picture shows
// the same moment, and --repeat is 1 for it: the frames are the animation); a camera's
// `pan` is units a frame it has moved, arriving at its `eye` at the picture. --supersample N
// writes <camera>_ss.png too (--samples N sets the view's MSAA for a whole run): the same picture drawn at N x the size
// with 4x MSAA, with TAA off, and averaged down in linear light (the still picture to judge static parts by). Levels of
// detail (G3a): a scene file may have `lod: {enabled, error_pixels, bias, force_level}` and a model's entry
// `lod_levels: N` (a scene's own `lod_levels` is for its procedural meshes): meshes are then made with N levels.
// --lod-levels N builds N levels for every mesh of a run, and --lod turns the selection on for every scene (with
// --lod-error, --lod-bias and --lod-force), each a label of its own. The levels' build time and memory are printed with
// the load. Clusters (G3b): a model's entry `clusters: true`, or --lod-clusters for every model of a run (not the
// procedural meshes), builds a hierarchy of clusters of about 128 triangles for each of its meshes (which then
// take no levels); with the selection on, each copy is cut through its hierarchy by the error bound
// (--lod-force N is then the depth of the cut, 0 the leaves). The hierarchy's build time, the process's peak
// resident memory so far, and each camera's cut (copies, clusters tested and drawn, bytes uploaded, CPU time) are
// printed. The cut on the GPU (G3c): the default where the device can (Scene3D::debug.gpu_culling); --lod-cpu cuts on
// the CPU, --lod-gpu on the GPU (a device that cannot falls back, and the run says which cut it made). --readback reads
// the GPU cut's counts back for the picture's render (clusters drawn, triangles, clusters dropped) -- never in the
// timed frames, which would wait for the GPU. --lod-room N sets Lod3D::max_cluster_triangles. Occlusion culling (G3d):
// a scene's `lod: {occlusion: true}` (or `{enabled, depth_margin, pixel_margin}`), or --lod-occlusion for every scene
// (it turns the selection on too): clusters behind others are not drawn. With --readback each camera also prints the
// clusters of the first pass, those occluded and those drawn for nothing. `procedural: "hidden"` builds a wall, a
// floor, a few objects in front of it and 64 copies of a 100,000-triangle torus (`clusters: true`) behind it, the scene
// it is judged on. Sky, fog and time of day (P3a): a scene file may have `sky: {enabled, mode: "procedural"|"image",
// turbidity, sun_intensity, intensity, ground_color, sun_size, sun_disc, stars, star_brightness, moon: {enabled,
// direction, size, light, color}, night_color, light_threshold, light_interval}`, `fog: {enabled, density,
// height_density, height_falloff, base_height, start, max_opacity, color, color_from_sky, sun_scatter,
// sun_anisotropy}`, `lights.sky_lights_scene`, and `time_of_day: {hours, day_of_year, latitude, axial_tilt,
// moon_phase}` (applied to the sun, the moon's fill light, the ambient light, the moon of the sky and the fog's
// colour); a camera may say `hours` (its own time of day: tod_sweep). `procedural: "landscape"` builds a 6 km valley: a
// height-field, 2500 cone trees and six red pillars at 50 m to 4 km. --sky, --no-sky, --fog D, --no-fog, --sky-light
// and --time H set or remove them for every scene. Spot lights and local shadows (P3b): `lights.spots: [{position,
// direction | target, color, radius, size, inner, outer, shadow, attach}]`
// (`shadow: true` or `{enabled, resolution, bias, normal_offset, near, distance, softness}`; `attach: {offset: [right,
// up, forward], yaw, pitch}` carries the light with each camera: a flashlight), `lights.points[].shadow_enabled` and
// `.size`, `lights.max_shadowed_spots`, `max_shadowed_points`, `local_shadow_resolution`, `fill_shadow: {enabled,
// strength, cascades, resolution, distance}`, `sun_shadow_strength`, `time_of_day.shadows`. `procedural: "stage"`
// builds a floor, walls (or a closed `room`), and the scene's `stage: {pillars, poles, lamps, trees, benches}` as [[x,
// z], ...] ([x, z, height] for lamps). --spots N / --spot-shadows M / --point-lights N / --point-shadows M put lights
// in front of every scene's camera (the cost tables), --moon-shadow and --moon-cascades N make the fill light cast,
// --local-res N sets their maps' side. Particles and weather (P3c): `weather: {rain, snow, dust, mist: intensity or
// {intensity, max, box, above, fall_speed, wind_influence, flutter, size: [a, b], alpha, color, splash, splash_size,
// soft, soft_distance, lit}, wind: {direction, speed, gust, gust_period, gust_length, turbulence, turbulence_length},
// ground: y, time, seed, apply}` (`apply`: the weather dims the sun and ambient light and thickens the fog) and
// `particles: [{name, seed, simulate, position, rotation, shape, extent, cone_angle, spread, rate, max, burst,
// lifetime, speed, size, gravity, drag, wind, colors, alpha, look: {blend, facing, axis, stretch, soft, soft_distance,
// lit, receives_shadow, casts_shadow, shadow_cutoff, softness, ring, sort, fog}}]` (each system is stepped `simulate`
// seconds at load: the pictures show that moment). --rain I, --snow I,
// --wind S (m/s), --no-weather,
// --no-particles, --soft 0|1, --particles N (a system of N live particles in the middle of the scene: the cost tables).

#include "character_level.hpp"
#include "climb_level.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <kin/assets/bake_cache.hpp>
#include <kin/assets/image.hpp>
#include <kin/core/jobs.hpp>
#include <kin/core/json_value.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/app.hpp>
#include <kin/platform/process.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/taa3d.hpp>
#include <kin/render3d/timeofday3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using kin::Mat4f;
using kin::Vec3f;

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

Vec3f vec3(const kin::JsonValue* v, Vec3f fallback = {}) {
    if (v == nullptr || v->items().size() < 3) {
        return fallback;
    }
    const auto& a = v->items();
    return {static_cast<float>(a[0].as_number()), static_cast<float>(a[1].as_number()),
            static_cast<float>(a[2].as_number())};
}

kin::LinearRgb light_color(const kin::JsonValue* value, kin::LinearRgb fallback = {}) {
    const Vec3f rgb = vec3(value, {fallback.r, fallback.g, fallback.b});
    return {rgb.x, rgb.y, rgb.z};
}

kin::Color colour(const kin::JsonValue* v, kin::Color fallback) {
    if (v == nullptr || v->items().size() < 3) {
        return fallback;
    }
    const auto& a = v->items();
    const auto c = [&](std::size_t i) { return static_cast<kin::u8>(std::clamp<double>(a[i].as_number(), 0, 255)); };
    return kin::Color::rgba(c(0), c(1), c(2), a.size() > 3 ? c(3) : 255);
}

struct Summary {
    double median = 0, p10 = 0, p90 = 0, min = 0;
};

Summary summarize(std::vector<double> v) {
    std::erase_if(v, [](double x) { return !(x > 0.0); });
    if (v.empty()) {
        return {};
    }
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) {
        return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))];
    };
    return {at(0.5), at(0.1), at(0.9), v.front()};
}

std::string json_summary(const Summary& s) {
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, "{\"median\": %.4f, \"p10\": %.4f, \"p90\": %.4f, \"min\": %.4f}", s.median,
                  s.p10, s.p90, s.min);
    return buffer;
}

// [a, b, c, d]
std::string json_list(const std::array<kin::u32, kin::max_shadow_cascades>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out += (i ? ", " : "") + std::to_string(values[i]);
    }
    return out + "]";
}

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

// `animation: {ecs: true, ik: [...]}` (docs/rendering_3d.md, Inverse kinematics): the entity's IK constraints, in
// order. Each:
//   {name, kind: "two_bone" | "look_at" | "foot_placer" | "fabrik", joints: [...], weight, fade,
//    target: [x, y, z] (the world; none: no target), pole_direction: [x, y, z] (model) | pole_position: [x, y, z]
//    (world), max_stretch, min_bend, max_bend, hinge_axis: [x, y, z] (the mid joint's own axis: a hinge), max_twist
//    (degrees), bend_swing (degrees a second), target_rotation: {axis, degrees} (world), fabrik: max_iterations,
//    tolerance, fix_root (the pole is the one above), look_at: weights: [...], max_yaw: [...], max_pitch_up: [...],
//    max_pitch_down: [...] (per joint), smooth, release, aim: [..], up: [..], foot_placer: foot_heights: [...], up,
//    max_tilt, max_pelvis_drop, max_pelvis_raise, smooth, toes: [joint names, "" for none],
//      toe_heights: [...], toe_max_tilt, lock: {height, release_height, release_distance, replant} (foot locking),
//      ground: {floor: y, boxes: [{min: [..], max: [..]}], slope: {point, normal},
//               ramps: [{x_min, x_max, z_start, degrees}] (a ramp rising along +z from z_start, up from the floor at y
//               = 0)}
//      (what the query hits first, in the world)}
std::vector<kin::IKConstraint3D> parse_ik(const kin::JsonValue& list) {
    std::vector<kin::IKConstraint3D> out;
    for (const kin::JsonValue& j : list.items()) {
        kin::IKConstraint3D k;
        k.name = j.string_at("name");
        const std::string kind = j.string_at("kind");
        k.kind = kind == "look_at"       ? kin::IKKind3D::LookAt
                 : kind == "foot_placer" ? kin::IKKind3D::FootPlacer
                 : kind == "fabrik"      ? kin::IKKind3D::Fabrik
                                         : kin::IKKind3D::TwoBone;
        if (const kin::JsonValue* joints = j.find("joints")) {
            for (const kin::JsonValue& n : joints->items()) {
                k.joints.push_back(n.as_string());
            }
        }
        k.weight = static_cast<float>(j.number_at("weight", 1.0));
        k.fade_seconds = static_cast<float>(j.number_at("fade", 0.0));
        if (const kin::JsonValue* t = j.find("target")) {
            k.set_target(vec3(t));
        }
        if (const kin::JsonValue* d = j.find("pole_direction")) {
            k.set_pole_direction(vec3(d));
        } else if (const kin::JsonValue* pp = j.find("pole_position")) {
            k.set_pole_position(vec3(pp));
        }
        k.two_bone.max_stretch = static_cast<float>(j.number_at("max_stretch", 1.0));
        k.two_bone.min_bend_degrees = static_cast<float>(j.number_at("min_bend", 0.0));
        k.two_bone.max_bend_degrees = static_cast<float>(j.number_at("max_bend", 180.0));
        k.two_bone.max_twist_degrees = static_cast<float>(j.number_at("max_twist", 180.0));
        k.two_bone.bend_swing_degrees_per_second = static_cast<float>(j.number_at("bend_swing", 0.0));
        if (const kin::JsonValue* hinge = j.find("hinge_axis")) {
            k.two_bone.hinge = true;
            k.two_bone.hinge_axis = vec3(hinge, {1, 0, 0});
        }
        if (k.kind == kin::IKKind3D::Fabrik) {
            k.fabrik.max_iterations = static_cast<kin::u32>(j.number_at("max_iterations", 16.0));
            k.fabrik.tolerance = static_cast<float>(j.number_at("tolerance", 1e-4));
            k.fabrik.fix_root = j.number_at("fix_root", 1.0) != 0.0;
        }
        if (const kin::JsonValue* r = j.find("target_rotation")) {
            k.set_target_rotation(kin::Quatf::axis_angle(vec3(r->find("axis"), {0, 1, 0}),
                                                         static_cast<float>(r->number_at("degrees", 0.0))),
                                  static_cast<float>(r->number_at("weight", 1.0)));
        }
        const auto numbers = [&](const char* key, std::vector<float>& into) {
            if (const kin::JsonValue* a = j.find(key)) {
                for (const kin::JsonValue& n : a->items()) {
                    into.push_back(static_cast<float>(n.as_number()));
                }
            }
        };
        if (k.kind == kin::IKKind3D::LookAt) {
            std::vector<float> w, yaw, up, down;
            numbers("weights", w);
            numbers("max_yaw", yaw);
            numbers("max_pitch_up", up);
            numbers("max_pitch_down", down);
            for (std::size_t i = 0; i < k.joints.size(); ++i) {
                kin::LookAtJoint3D l;
                l.weight = i < w.size() ? w[i] : 1.0f;
                l.max_yaw_degrees = i < yaw.size() ? yaw[i] : 180.0f;
                l.max_pitch_up_degrees = i < up.size() ? up[i] : 180.0f;
                l.max_pitch_down_degrees = i < down.size() ? down[i] : 180.0f;
                k.look_links.push_back(l);
            }
            k.look_at.smooth_seconds = static_cast<float>(j.number_at("smooth", 0.0));
            k.look_at.release_degrees = static_cast<float>(j.number_at("release", 30.0));
            k.look_at.aim_axis = vec3(j.find("aim"), k.look_at.aim_axis);
            k.look_at.up_axis = vec3(j.find("up"), k.look_at.up_axis);
        }
        if (k.kind == kin::IKKind3D::FootPlacer) {
            numbers("foot_heights", k.foot_heights);
            k.foot.max_tilt_degrees = static_cast<float>(j.number_at("max_tilt", 30.0));
            k.foot.max_pelvis_drop = static_cast<float>(j.number_at("max_pelvis_drop", 0.5));
            k.foot.max_pelvis_raise = static_cast<float>(j.number_at("max_pelvis_raise", 0.0));
            k.foot.smooth_seconds = static_cast<float>(j.number_at("smooth", 0.1));
            if (const kin::JsonValue* toes = j.find("toes")) {
                for (const kin::JsonValue& n : toes->items()) {
                    k.toe_joints.push_back(n.as_string());
                }
            }
            numbers("toe_heights", k.toe_heights);
            k.foot.toe_max_tilt_degrees = static_cast<float>(j.number_at("toe_max_tilt", 40.0));
            if (const kin::JsonValue* lock = j.find("lock")) {
                k.foot.lock_feet = true;
                k.foot.lock_height = static_cast<float>(lock->number_at("height", 0.02));
                k.foot.lock_release_height = static_cast<float>(lock->number_at("release_height", 0.06));
                k.foot.lock_release_distance = static_cast<float>(lock->number_at("release_distance", 0.15));
                k.foot.lock_replant_seconds = static_cast<float>(lock->number_at("replant", 0.15));
            }
            if (const kin::JsonValue* g = j.find("ground")) {
                struct Box {
                    Vec3f lo, hi;
                };
                std::vector<Box> boxes;
                if (const kin::JsonValue* b = g->find("boxes")) {
                    for (const kin::JsonValue& item : b->items()) {
                        boxes.push_back({vec3(item.find("min")), vec3(item.find("max"))});
                    }
                }
                struct Ramp {
                    float x_min, x_max, z_start, tan;
                };
                std::vector<Ramp> ramps;
                if (const kin::JsonValue* r = g->find("ramps")) {
                    for (const kin::JsonValue& item : r->items()) {
                        ramps.push_back(
                            {static_cast<float>(item.number_at("x_min", -1e9)),
                             static_cast<float>(item.number_at("x_max", 1e9)),
                             static_cast<float>(item.number_at("z_start", 0.0)),
                             std::tan(static_cast<float>(item.number_at("degrees", 0.0)) * 3.14159265f / 180.0f)});
                    }
                }
                const bool has_floor = g->find("floor") != nullptr;
                const float floor_y = static_cast<float>(g->number_at("floor", 0.0));
                const bool has_slope = g->find("slope") != nullptr;
                const Vec3f slope_point = has_slope ? vec3(g->find("slope")->find("point")) : Vec3f{};
                const Vec3f slope_normal =
                    has_slope ? normalize(vec3(g->find("slope")->find("normal"), {0, 1, 0})) : Vec3f{0, 1, 0};
                // The first hit along the segment: the nearest of the boxes' faces, the slope plane and the floor.
                k.ground = [boxes, ramps, has_floor, floor_y, has_slope, slope_point,
                            slope_normal](Vec3f from, Vec3f to) -> std::optional<kin::GroundHit3D> {
                    const Vec3f d = to - from;
                    float best = 2.0f;
                    kin::GroundHit3D hit;
                    const auto plane = [&](Vec3f point, Vec3f n) {
                        const float denom = dot(d, n);
                        if (denom >= -1e-9f) {
                            return;
                        }
                        const float t = dot(point - from, n) / denom;
                        if (t >= 0.0f && t < best) {
                            best = t;
                            hit = {from + d * t, n};
                        }
                    };
                    if (has_floor) {
                        plane({0.0f, floor_y, 0.0f}, {0.0f, 1.0f, 0.0f});
                    }
                    if (has_slope) {
                        plane(slope_point, slope_normal);
                    }
                    // A ramp: a plane through (x, 0, z_start) that rises with z, only where the hit lies over it (x in
                    // range, z past the start).
                    for (const Ramp& r : ramps) {
                        const Vec3f n = normalize(Vec3f{0.0f, 1.0f, -r.tan});
                        const float denom = dot(d, n);
                        if (denom < -1e-9f) {
                            const float t = dot(Vec3f{0.0f, 0.0f, r.z_start} - from, n) / denom;
                            const Vec3f p = from + d * t;
                            if (t >= 0.0f && t < best && p.x >= r.x_min && p.x <= r.x_max && p.z >= r.z_start) {
                                best = t;
                                hit = {p, n};
                            }
                        }
                    }
                    for (const Box& b : boxes) {
                        // Slab test; the face it enters through gives the normal.
                        float t0 = 0.0f, t1 = 1.0f;
                        Vec3f n{};
                        bool inside = true;
                        const float lo[3] = {b.lo.x, b.lo.y, b.lo.z}, hi[3] = {b.hi.x, b.hi.y, b.hi.z};
                        const float o[3] = {from.x, from.y, from.z}, dir[3] = {d.x, d.y, d.z};
                        for (int a = 0; a < 3 && inside; ++a) {
                            if (std::fabs(dir[a]) < 1e-9f) {
                                inside = o[a] >= lo[a] && o[a] <= hi[a];
                                continue;
                            }
                            float ta = (lo[a] - o[a]) / dir[a], tb = (hi[a] - o[a]) / dir[a];
                            float sign = -1.0f;
                            if (ta > tb) {
                                std::swap(ta, tb);
                                sign = 1.0f;
                            }
                            if (ta > t0) {
                                t0 = ta;
                                n = {};
                                (a == 0 ? n.x : a == 1 ? n.y : n.z) = sign;
                            }
                            t1 = std::min(t1, tb);
                            inside = t0 <= t1;
                        }
                        if (inside && t0 < best && (n.x != 0.0f || n.y != 0.0f || n.z != 0.0f)) {
                            best = t0;
                            hit = {from + d * t0, n};
                        }
                    }
                    if (best > 1.0f) {
                        return std::nullopt;
                    }
                    return hit;
                };
            }
        }
        out.push_back(std::move(k));
    }
    return out;
}

// A model placed in the scene.
struct Placed {
    kin::Model3D model;
    Mat4f transform = Mat4f::identity();
    // A skinned model's pose (the model's entry `animation: {clip, time, loop, speed}`; docs/rendering_3d.md, Skinned
    // meshes): the clip (a name or an index, the first by default) sampled at `time` seconds when that is given, else
    // at the frame's time (frame N at N / 60 s, 0 at the picture) times `speed`; `loop` (on by default) wraps, else it
    // holds the last key. `root_motion: "remove"` holds the root joint at rest, so the character animates in place.
    bool animated = false;
    std::size_t clip = 0;
    bool fixed_time = false;
    double time = 0.0;
    bool loop = true;
    double speed = 1.0;
    bool remove_root_motion = false;
    // A 1D blend in place of the one clip (`animation: {blend: {parameter, parameter_to, parameter_seconds, points:
    // [{clip, position}]}}`): the clips of the points (indices into the model's) blended by the parameter, which runs
    // from `parameter` to `parameter_to` over `parameter_seconds` of the frames (a constant without `parameter_to`).
    struct BlendPoint {
        std::size_t clip = 0;
        float position = 0.0f;
    };
    std::vector<BlendPoint> blend;
    double parameter = 0.0, parameter_to = 0.0, parameter_seconds = 2.0;
    bool ramp = false;
    // `animation: {ecs: true}`: the model is an entity with SkinnedMeshRenderer3D and Animator3D, stepped by
    // WorldRenderState3D::animate at 60 steps a second (250 of them before the picture, the frame's index after), and
    // drawn by its collect(): what a game does.
    bool ecs = false;
    Vec3f position{};
    float rotation_y = 0.0f, uniform_scale = 1.0f;
    std::shared_ptr<const kin::Model3D> shared;
    flecs::entity entity;
    kin::Animator3D animator; // the entity's at the start
};

// Signal Breach's deck (48 x 32 tiles) and `count` enemies over it, three
// meshes instanced, as kin_draw_bench's shadows mode has them.
struct Crowd {
    kin::Mesh3D tile;
    std::array<kin::Mesh3D, 3> enemy;
    std::vector<kin::MeshInstance3D> tiles;
    std::array<std::vector<kin::MeshInstance3D>, 3> enemies;

    void make(kin::Renderer3D& r3d, int count, const kin::MeshOptions3D& options) {
        tile = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}), options).value_or({});
        enemy = {r3d.create_mesh(kin::meshes::star_prism(5, 0.32f, 0.14f, 0.16f), options).value_or({}),
                 r3d.create_mesh(kin::meshes::cylinder(0.4f, 0.16f, 6), options).value_or({}),
                 r3d.create_mesh(kin::meshes::torus(0.26f, 0.07f, 20, 8), options).value_or({})};
        for (int z = 0; z < 32; ++z) {
            for (int x = 0; x < 48; ++x) {
                tiles.push_back({Mat4f::translation({x + 0.5f, -0.5f, z + 0.5f}), kin::Color::rgb(90, 96, 110)});
            }
        }
        kin::u32 seed = 7;
        const auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 16777216.0f;
        };
        for (int i = 0; i < count; ++i) {
            const Vec3f at{next() * 48.0f, 0.6f + next() * 2.0f, next() * 32.0f};
            enemies[static_cast<std::size_t>(i % 3)].push_back(
                {Mat4f::translation(at) * Mat4f::rotation_y(static_cast<float>(i) * 37.0f),
                 kin::Color::rgb(255, 255, 255)});
        }
    }

    void draw(kin::Scene3D& scene) const {
        (void)scene.draw_instanced(tile, tiles);
        for (std::size_t k = 0; k < 3; ++k) {
            (void)scene.draw_instanced(enemy[k], enemies[k], {.color = kin::Color::rgb(200, 120, 80)});
        }
    }
    // The enemies drifting (`--crowd-move`): each a little from where it was, a pure function of `time` (frames), as a
    // game's moves them.
    void draw_moving(kin::Scene3D& scene, int time, std::array<std::vector<kin::MeshInstance3D>, 3>& moved) const {
        (void)scene.draw_instanced(tile, tiles);
        for (std::size_t k = 0; k < 3; ++k) {
            moved[k] = enemies[k];
            for (std::size_t i = 0; i < moved[k].size(); ++i) {
                const float phase = static_cast<float>(time) * 0.05f + static_cast<float>(i) * 0.37f;
                moved[k][i].transform.m[12] += std::sin(phase) * 0.15f;
                moved[k][i].transform.m[14] += std::cos(phase) * 0.15f;
            }
            (void)scene.draw_instanced(enemy[k], moved[k], {.color = kin::Color::rgb(200, 120, 80)});
        }
    }
};

// A scene made for temporal anti-aliasing (`procedural: "motion"`): high-contrast thin things
// (a fence of posts and rails, a wire, poles at three depths), and four things moving in
// a way that is a pure function of time: a bar orbiting, a cross turning, a block going to
// and fro, a bright pole sliding in front of the fence. Time is in frames; 0 is the picture.
struct Motion {
    kin::Mesh3D floor, wall, post, rail, wire, pole, block, bar, cross_bar, ball;

    void make(kin::Renderer3D& r3d, const kin::MeshOptions3D& options) {
        floor = r3d.create_mesh(kin::meshes::box({16.0f, 0.2f, 12.0f}), options).value_or({});
        wall = r3d.create_mesh(kin::meshes::box({16.0f, 4.0f, 0.2f}), options).value_or({});
        post = r3d.create_mesh(kin::meshes::box({0.035f, 1.2f, 0.035f}), options).value_or({});
        rail = r3d.create_mesh(kin::meshes::box({4.0f, 0.03f, 0.03f}), options).value_or({});
        wire = r3d.create_mesh(kin::meshes::box({6.0f, 0.012f, 0.012f}), options).value_or({});
        pole = r3d.create_mesh(kin::meshes::box({0.02f, 2.4f, 0.02f}), options).value_or({});
        block = r3d.create_mesh(kin::meshes::box({0.5f, 0.5f, 0.5f}), options).value_or({});
        bar = r3d.create_mesh(kin::meshes::box({0.9f, 0.05f, 0.05f}), options).value_or({});
        cross_bar = r3d.create_mesh(kin::meshes::box({1.0f, 0.03f, 0.03f}), options).value_or({});
        ball = r3d.create_mesh(kin::meshes::sphere(0.18f, 24, 12), options).value_or({});
    }

    void draw(kin::Scene3D& scene, int t) const {
        const float time = static_cast<float>(t);
        const kin::Material3D dark{.color = kin::Color::rgb(28, 30, 36), .roughness = 0.9f};
        const kin::Material3D bright{.color = kin::Color::rgb(245, 245, 240), .roughness = 0.6f};
        const kin::Material3D warm{.color = kin::Color::rgb(255, 150, 60), .roughness = 0.5f};
        const kin::Material3D unlit_white{.color = kin::Color::rgb(255, 255, 255), .unlit = true};
        (void)scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}),
                         {.color = kin::Color::rgb(70, 74, 82), .roughness = 0.9f});
        (void)scene.draw(wall, Mat4f::translation({0.0f, 2.0f, -3.0f}), dark);
        // A fence: 60 posts every 6.5 cm, two rails (white against the dark wall).
        for (int i = 0; i < 60; ++i) {
            (void)scene.draw(post, Mat4f::translation({-1.95f + 0.065f * static_cast<float>(i), 0.6f, -1.0f}), bright);
        }
        (void)scene.draw(rail, Mat4f::translation({0.0f, 0.35f, -1.0f}), bright);
        (void)scene.draw(rail, Mat4f::translation({0.0f, 1.0f, -1.0f}), bright);
        // A wire across, and poles at three depths.
        (void)scene.draw(wire, Mat4f::translation({0.0f, 1.9f, -2.0f}) * Mat4f::rotation_z(4.0f), unlit_white);
        for (int i = 0; i < 3; ++i) {
            (void)scene.draw(
                pole,
                Mat4f::translation({2.2f + 0.5f * static_cast<float>(i), 1.2f, -2.5f + 1.2f * static_cast<float>(i)}),
                bright);
        }
        // Moving: a bar orbiting on a circle at 3 degrees a frame, a cross turning at 4, a block going to and fro, a
        // pole sliding.
        const float orbit = time * 3.0f * 3.14159265f / 180.0f;
        (void)scene.draw(bar,
                         Mat4f::translation({-2.6f + 0.6f * std::cos(orbit), 1.0f + 0.6f * std::sin(orbit), -0.4f}) *
                             Mat4f::rotation_z(time * 3.0f + 90.0f),
                         warm);
        const Mat4f turn = Mat4f::translation({-3.6f, 0.9f, -0.4f}) * Mat4f::rotation_y(time * 4.0f);
        (void)scene.draw(cross_bar, turn, warm);
        (void)scene.draw(cross_bar, turn * Mat4f::rotation_y(90.0f), warm);
        const float to_and_fro = 1.5f * std::sin(time * 0.05f);
        (void)scene.draw(block, Mat4f::translation({3.5f + to_and_fro, 0.25f, -0.2f}) * Mat4f::rotation_y(time * 2.0f),
                         warm);
        (void)scene.draw(ball, Mat4f::translation({-0.8f + 1.2f * std::sin(time * 0.04f), 0.5f, 0.6f}), warm);
        (void)scene.draw(pole, Mat4f::translation({0.6f * std::sin(time * 0.06f), 1.2f, 0.2f}), warm);
    }
};

// A valley, for the sky and the fog (`procedural: "landscape"`, P3a): a height-field 6 km long and 6 km wide with the
// valley's walls and mountains at the far end (vertices spaced finer near the camera), 2500 cone trees on its floor and
// slopes, and six red pillars 40 m tall at 50, 200, 500, 1000, 2000 and 4000 m down the valley (their colour against
// the fog tells the distance). The ground is coloured by height and slope: grass, rock, snow.
struct Landscape {
    kin::Mesh3D terrain, tree, pillar;
    std::vector<kin::MeshInstance3D> trees;

    static float smooth(float a, float b, float x) {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    static float height(float x, float z) {
        const float ax = std::abs(x);
        const float rolling = 14.0f * std::sin(x * 0.0045f + 0.7f) * std::cos(z * 0.0031f) +
                              7.0f * std::sin(x * 0.011f - z * 0.0067f) + 3.0f * std::sin(x * 0.031f + z * 0.027f);
        const float walls = 260.0f * std::pow(smooth(120.0f, 1500.0f, ax), 1.4f);
        const float far = 950.0f * smooth(1400.0f, 5600.0f, -z) * (0.65f + 0.35f * std::sin(x * 0.0021f + 1.1f));
        return rolling * (0.3f + 0.7f * smooth(0.0f, 400.0f, ax)) + walls + far;
    }

    void make(kin::Renderer3D& r3d, const kin::JsonValue& doc) {
        constexpr int nx = 255, nz = 400;
        kin::MeshData3D grid;
        grid.vertices.reserve(static_cast<std::size_t>(nx) * nz);
        const auto at = [&](int i, int j) {
            const float s = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(nx - 1);
            const float t = static_cast<float>(j) / static_cast<float>(nz - 1);
            const float x = 3000.0f * (s < 0 ? -1.0f : 1.0f) * std::pow(std::abs(s), 2.5f);
            const float z = 120.0f - 6120.0f * t * t;
            return std::pair<float, float>{x, z};
        };
        for (int j = 0; j < nz; ++j) {
            for (int i = 0; i < nx; ++i) {
                const auto [x, z] = at(i, j);
                const float y = height(x, z);
                const float e = 2.0f;
                const Vec3f n = kin::normalize(
                    {height(x - e, z) - height(x + e, z), 2.0f * e, height(x, z - e) - height(x, z + e)});
                // Grass low and flat, rock steep, snow high.
                const float rock = smooth(0.78f, 0.55f, n.y);
                const float snow = smooth(520.0f, 700.0f, y + 40.0f * (n.y - 0.7f));
                const float grass = 1.0f - rock;
                float r = 0.20f * grass + 0.42f * rock, g = 0.36f * grass + 0.38f * rock,
                      b = 0.14f * grass + 0.33f * rock;
                r += (0.9f - r) * snow;
                g += (0.92f - g) * snow;
                b += (0.95f - b) * snow;
                grid.vertices.push_back(
                    {.position = {x, y, z},
                     .normal = n,
                     .uv = {x * 0.01f, z * 0.01f},
                     .color = kin::Color::rgb(static_cast<kin::u8>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f),
                                              static_cast<kin::u8>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f),
                                              static_cast<kin::u8>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f))});
            }
        }
        for (int j = 0; j + 1 < nz; ++j) {
            for (int i = 0; i + 1 < nx; ++i) {
                const kin::u32 a = static_cast<kin::u32>(j * nx + i), b = a + 1, c = a + static_cast<kin::u32>(nx),
                               d = c + 1;
                // Counter-clockwise seen from above (+y): rows run toward -z (up the picture seen from above), columns
                // toward +x.
                grid.indices.insert(grid.indices.end(), {a, b, c, b, d, c});
            }
        }
        terrain = r3d.create_mesh(grid).value_or({});
        tree = r3d.create_mesh(kin::meshes::cone(3.0f, 12.0f, 10)).value_or({});
        pillar = r3d.create_mesh(kin::meshes::box({8.0f, 40.0f, 8.0f})).value_or({});
        // The trees: a fixed hash puts each (no random numbers to differ between runs), on the valley's floor and
        // lower slopes, not on rock or snow.
        const int count = static_cast<int>(doc.number_at("trees", 2500));
        kin::u32 state = 0x2545F491u;
        const auto random = [&] {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / 16777216.0f;
        };
        for (int i = 0; i < count * 4 && static_cast<int>(trees.size()) < count; ++i) {
            const float x = (random() * 2.0f - 1.0f) * 420.0f;
            const float z = 100.0f - 3200.0f * random() * random();
            const float y = height(x, z);
            const float e = 2.0f;
            const float slope =
                kin::normalize({height(x - e, z) - height(x + e, z), 2.0f * e, height(x, z - e) - height(x, z + e)}).y;
            if (slope < 0.8f || y > 160.0f || (std::abs(x - 12.0f) < 14.0f)) {
                continue; // none on steep ground, high up, or in the lane the pillars stand in
            }
            const float scale = 0.6f + 1.0f * random();
            trees.push_back(
                {.transform = Mat4f::trs({x, y, z}, kin::Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 360.0f * random()),
                                         {scale, scale * (0.9f + 0.4f * random()), scale}),
                 .tint = kin::Color::rgb(static_cast<kin::u8>(40 + 40 * random()),
                                         static_cast<kin::u8>(80 + 50 * random()), 40)});
        }
    }

    void draw(kin::Scene3D& scene) const {
        (void)scene.draw(terrain, Mat4f::identity(), {.color = kin::colors::white, .roughness = 1.0f});
        (void)scene.draw_instanced(tree, trees, {.color = kin::colors::white, .roughness = 0.9f});
        const kin::Material3D red{.color = kin::Color::rgb(200, 30, 30), .roughness = 0.8f};
        for (const float z : {-50.0f, -200.0f, -500.0f, -1000.0f, -2000.0f, -4000.0f}) {
            (void)scene.draw(pillar, Mat4f::translation({12.0f, height(12.0f, z) + 20.0f, z}), red);
        }
    }
};

// Occlusion culling's scene (`procedural: "hidden"`): a floor, a wall 20 m wide and 8 high across the way, 8 tori in
// front of it, and behind it 64 more (8 x 8, turned every which way), each of 100,352 triangles, made with clusters
// when the scene says `clusters: true` (G3d). Seen from in front of the wall, the 64 are hidden; from the side, some
// show past its end; from behind the grid, nothing is hidden by the wall.
struct Hidden {
    kin::Mesh3D floor, wall, torus;
    std::vector<kin::MeshInstance3D> behind, front;
    Vec3f extent{12.0f, 8.0f, 21.0f}; // the scene's bounds: +-x, up to y, to z

    // `doc` may set `grid: [nx, nz]` (8 x 8), `spacing: [dx, dz]` (3, 2.4) and `segments` (224: 100,352 triangles a
    // torus).
    void make(kin::Renderer3D& r3d, kin::MeshOptions3D options, const kin::JsonValue& doc) {
        int nx = 8, nz = 8;
        float dx = 3.0f, dz = 2.4f;
        if (const kin::JsonValue* grid = doc.find("grid"); grid && grid->items().size() == 2) {
            nx = static_cast<int>(grid->items()[0].as_number());
            nz = static_cast<int>(grid->items()[1].as_number());
        }
        if (const kin::JsonValue* spacing = doc.find("spacing"); spacing && spacing->items().size() == 2) {
            dx = static_cast<float>(spacing->items()[0].as_number());
            dz = static_cast<float>(spacing->items()[1].as_number());
        }
        const int segments = static_cast<int>(doc.number_at("segments", 224));
        const float half = dx * static_cast<float>(nx) * 0.5f;
        const float wall_width = std::max(20.0f, 2.0f * half);
        extent = {wall_width * 0.6f + 2.0f, 8.0f, 3.5f + dz * static_cast<float>(nz) + 2.0f};
        floor = r3d.create_mesh(kin::meshes::box({extent.x * 2.0f + 40.0f, 0.2f, extent.z + 60.0f}),
                                kin::MeshOptions3D{.levels = options.levels})
                    .value_or({});
        wall = r3d.create_mesh(kin::meshes::box({wall_width, 8.0f, 0.4f}), kin::MeshOptions3D{.levels = options.levels})
                   .value_or({});
        torus = r3d.create_mesh(kin::meshes::torus(0.8f, 0.3f, segments, segments), options).value_or({});
        for (int j = 0; j < nz; ++j) {
            for (int i = 0; i < nx; ++i) {
                const Vec3f at{-half + 0.5f * dx + dx * static_cast<float>(i), 1.1f, 3.5f + dz * static_cast<float>(j)};
                behind.push_back({Mat4f::translation(at) * Mat4f::rotation_y(static_cast<float>(i * 8 + j) * 29.0f) *
                                      Mat4f::rotation_x(static_cast<float>(i + 3 * j) * 17.0f),
                                  kin::Color::rgb(static_cast<kin::u8>(120 + 16 * (i % 8)),
                                                  static_cast<kin::u8>(200 - 12 * j), 150)});
            }
        }
        for (int i = 0; i < 8; ++i) {
            front.push_back({Mat4f::translation({-9.1f + 2.6f * static_cast<float>(i), 1.1f,
                                                 -4.0f - 1.2f * static_cast<float>(i % 3)}) *
                                 Mat4f::rotation_y(static_cast<float>(i) * 41.0f),
                             kin::Color::rgb(230, 170, static_cast<kin::u8>(60 + 15 * i))});
        }
    }

    void draw(kin::Scene3D& scene) const {
        (void)scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}),
                         {.color = kin::Color::rgb(96, 100, 108), .roughness = 0.9f});
        (void)scene.draw(wall, Mat4f::translation({0.0f, 4.0f, 0.0f}),
                         {.color = kin::Color::rgb(190, 186, 176), .roughness = 0.8f});
        (void)scene.draw_instanced(torus, behind, {.roughness = 0.45f});
        (void)scene.draw_instanced(torus, front, {.roughness = 0.45f});
    }
};

// `procedural: "physics_pile"` (docs/physics_3d.md): boxes and spheres dropped into a walled yard of kin/physics3d, at
// a fixed step from a fixed recipe, drawn as meshes with the physics debug lines over them (the shapes in the colour of
// awake and sleeping, the contacts and their normals). The picture of frame f is the world 150 + f steps in; a frame
// asked for out of order starts the world again, so every frame is the same whatever order the run takes them in.
struct PhysicsPile {
    struct Item {
        kin::Body3D body;
        Vec3f half;
        bool ball = false;
        kin::Color colour;
    };
    static constexpr int lead = 150;
    kin::Mesh3D box_mesh, ball_mesh, debug_mesh;
    std::unique_ptr<kin::World3D> world;
    std::vector<Item> items;
    std::vector<kin::ContactEvent3D> last_events;
    int count = 60;
    bool debug = true;
    int stepped = 0;
    float yard = 3.0f;

    void reset() {
        kin::World3DConfig cfg;
        cfg.single_threaded = true; // one thread: the picture does not depend on a pool
        cfg.persist_events = true;
        world = std::make_unique<kin::World3D>(cfg);
        items.clear();
        last_events.clear();
        stepped = 0;
        const auto wall = [&](Vec3f half, Vec3f at) {
            world->create_body({.shape = kin::Shape3D::box(half),
                                .motion = kin::Body3DMotion::Static,
                                .position = at,
                                .friction = 0.6f});
        };
        wall({yard + 0.3f, 0.25f, yard + 0.3f}, {0.0f, -0.25f, 0.0f});
        wall({0.15f, 0.75f, yard + 0.3f}, {-yard - 0.15f, 0.75f, 0.0f});
        wall({0.15f, 0.75f, yard + 0.3f}, {yard + 0.15f, 0.75f, 0.0f});
        wall({yard + 0.3f, 0.75f, 0.15f}, {0.0f, 0.75f, -yard - 0.15f});
        wall({yard + 0.3f, 0.75f, 0.15f}, {0.0f, 0.75f, yard + 0.15f});
        std::uint32_t seed = 2024u;
        const auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
        };
        for (int i = 0; i < count; ++i) {
            const int column = i % 5, row = (i / 5) % 4, layer = i / 20;
            const Vec3f at{(static_cast<float>(column) - 2.0f) * 1.0f + (next() - 0.5f) * 0.3f,
                           1.2f + static_cast<float>(layer) * 1.05f,
                           (static_cast<float>(row) - 1.5f) * 1.0f + (next() - 0.5f) * 0.3f};
            const kin::Quatf q = kin::Quatf::axis_angle({next() + 0.1f, next(), next()}, next() * 360.0f);
            Item item;
            item.ball = i % 3 == 0;
            item.half =
                item.ball ? Vec3f{0.32f, 0.32f, 0.32f} : Vec3f{0.35f + 0.1f * next(), 0.25f + 0.1f * next(), 0.3f};
            item.colour = item.ball ? kin::Color::rgb(220, 90, 70)
                                    : kin::Color::rgb(static_cast<kin::u8>(90 + 20 * (i % 4)), 150,
                                                      static_cast<kin::u8>(200 - 15 * (i % 5)));
            item.body = world->create_body(
                {.shape = item.ball ? kin::Shape3D::sphere(item.half.x) : kin::Shape3D::box(item.half),
                 .position = at,
                 .rotation = q,
                 .friction = 0.5f,
                 .restitution = 0.15f,
                 .user_id = static_cast<kin::u64>(i)});
            items.push_back(item);
        }
        world->optimize_broad_phase();
    }

    void make(kin::Renderer3D& r3d, const kin::JsonValue& doc) {
        count = static_cast<int>(doc.number_at("count", 60));
        debug = doc.find("debug") == nullptr || doc.bool_at("debug");
        box_mesh = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f})).value_or({});
        ball_mesh = r3d.create_mesh(kin::meshes::sphere(1.0f, 24, 12)).value_or({});
        reset();
    }

    void advance_to(int steps) {
        if (steps < stepped) {
            reset();
        }
        while (stepped < steps) {
            (void)world->step(1.0f / 60.0f);
            last_events = world->drain_events();
            ++stepped;
        }
    }

    void draw(kin::Renderer3D& r3d, kin::Scene3D& scene, int time) {
        advance_to(lead + std::max(0, time));
        const kin::Material3D stone{.color = kin::Color::rgb(150, 146, 140), .roughness = 0.9f};
        (void)scene.draw(
            box_mesh,
            Mat4f::translation({0.0f, -0.25f, 0.0f}) * Mat4f::scaling({2 * yard + 0.6f, 0.5f, 2 * yard + 0.6f}), stone);
        (void)scene.draw(
            box_mesh, Mat4f::translation({-yard - 0.15f, 0.75f, 0.0f}) * Mat4f::scaling({0.3f, 1.5f, 2 * yard + 0.6f}),
            stone);
        (void)scene.draw(
            box_mesh, Mat4f::translation({yard + 0.15f, 0.75f, 0.0f}) * Mat4f::scaling({0.3f, 1.5f, 2 * yard + 0.6f}),
            stone);
        (void)scene.draw(
            box_mesh, Mat4f::translation({0.0f, 0.75f, -yard - 0.15f}) * Mat4f::scaling({2 * yard + 0.6f, 1.5f, 0.3f}),
            stone);
        for (const Item& item : items) {
            const Mat4f m = Mat4f::trs(world->position(item.body), world->rotation(item.body),
                                       item.half * (item.ball ? 1.0f : 2.0f));
            (void)scene.draw(item.ball ? ball_mesh : box_mesh, m, {.color = item.colour, .roughness = 0.5f});
        }
        if (debug) {
            kin::DebugDraw3DOptions look;
            look.point_size = 0.07f; // a metre-scale yard seen from 9 m: crosses and normals that show
            look.normal_length = 0.45f;
            kin::DebugDraw3D lines{look};
            lines.world(*world);
            lines.contacts(last_events);
            debug_mesh = kin::draw_debug_lines(r3d, scene, lines.lines());
        }
    }
};

// `procedural: "physics_character"` (docs/physics_3d.md, "Character controller"): the controller's test level
// (tests/character_level.hpp: ramps, stairs, a hump, a ledge, a tunnel, boxes, platforms, a trigger) with a skinned
// person
// (`made/skinned_biped_walk.glb`) driven through it by the scripted route of the scene file, by the ECS system
// (CharacterController3D, a locomotion Animator3D, the foot placer on the physics floor). The picture of frame f is the
// world `lead + f` fixed steps in; a frame asked for out of order starts the world again, so every frame is the same
// whatever order the run takes them in. The debug lines show every body, the character's capsule and its contacts.
struct PhysicsCharacter {
    kin::Mesh3D box_mesh, ball_mesh, debug_mesh;
    std::shared_ptr<const kin::Model3D> person;
    std::unique_ptr<kin::World3D> world;
    std::unique_ptr<kin::EcsWorld> ecs;
    std::unique_ptr<kin::WorldPhysicsState3D> bodies;
    std::unique_ptr<kin::WorldRenderState3D> render;
    character_level::Level level;
    character_level::Route route;
    flecs::entity hero;
    int lead = 456;
    int stepped = 0;
    bool debug = true;
    bool loaded = false;

    Vec3f feet() const {
        const kin::Transform3D* t = hero.is_valid() ? hero.get<kin::Transform3D>() : nullptr;
        return t != nullptr ? t->position : Vec3f{};
    }

    void reset() {
        kin::World3DConfig cfg;
        cfg.single_threaded = true; // one thread: the picture does not depend on a pool
        world = std::make_unique<kin::World3D>(cfg);
        ecs = std::make_unique<kin::EcsWorld>();
        level = character_level::build(*world);
        world->optimize_broad_phase();
        bodies = std::make_unique<kin::WorldPhysicsState3D>(*ecs, *world);
        render = std::make_unique<kin::WorldRenderState3D>(*ecs);
        kin::CharacterController3D controller;
        controller.desc.height = 1.75f;
        controller.input.forward = {1.0f, 0.0f, 0.0f};
        kin::LocomotionClips3D clips;
        kin::Animator3D animator = kin::locomotion_animator(clips);
        kin::IKConstraint3D feet_ik;
        feet_ik.name = "feet";
        feet_ik.kind = kin::IKKind3D::FootPlacer;
        feet_ik.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
        feet_ik.foot_heights = {0.05f, 0.05f};
        feet_ik.pole = kin::IKConstraint3D::Pole::ModelDirection;
        feet_ik.pole_vector = {0.0f, 0.0f, 1.0f};
        feet_ik.two_bone.max_stretch = 1.1f;
        feet_ik.foot.smooth_seconds = 0.08f;
        feet_ik.fade_seconds = 0.12f;
        animator.add_ik(feet_ik);
        hero = ecs->raw()
                   .entity()
                   .set(kin::Transform3D{.position = route.start})
                   .set(controller)
                   .set(kin::SkinnedMeshRenderer3D{.model = person})
                   .set(static_cast<const kin::Animator3D&>(animator))
                   .set(kin::CharacterAnimationLink3D{});
        stepped = 0;
    }

    void make(kin::Renderer3D& r3d, const kin::JsonValue& doc, const fs::path& cache) {
        debug = doc.find("debug") == nullptr || doc.bool_at("debug");
        lead = static_cast<int>(doc.number_at("lead", 456));
        if (const kin::JsonValue* c = doc.find("character")) {
            route = character_level::load_route(*c);
        }
        box_mesh = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f})).value_or({});
        ball_mesh = r3d.create_mesh(kin::meshes::sphere(1.0f, 32, 16)).value_or({});
        std::string error;
        kin::Model3D model = r3d.load_model(cache / "made" / "skinned_biped_walk.glb", {}).value_or({});
        error = kin::diagnostics3d_text(r3d.diagnostics());
        if (!model) {
            std::fprintf(stderr, "  skinned_biped_walk.glb: %s (python3 tools/reference/make_scenes.py <cache>/made)\n",
                         error.c_str());
            return;
        }
        person = std::make_shared<const kin::Model3D>(std::move(model));
        loaded = true;
        reset();
    }

    // One fixed step of the route: the keys of this frame, the level's platforms, the physics system, the animator.
    void step_once() {
        const int n = stepped;
        kin::CharacterController3D* c = hero.get_mut<kin::CharacterController3D>();
        for (const character_level::Key& k : route.keys) {
            if (k.frame != n) {
                continue;
            }
            if (k.has_move)
                c->input.move = k.move;
            if (k.has_forward)
                c->input.forward = k.forward;
            if (k.has_jump)
                c->input.jump = k.jump;
            if (k.has_crouch)
                c->input.crouch = k.crouch;
            if (k.has_sprint)
                c->input.sprint = k.sprint;
            if (k.has_teleport)
                hero.get_mut<kin::Transform3D>()->position = k.teleport;
        }
        // It faces the way it was told to go (the model faces +Z).
        const Vec3f f = c->input.forward;
        if (f.x != 0.0f || f.z != 0.0f) {
            c->facing = kin::Quatf::axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(f.x, f.z) * 57.29578f);
        }
        level.drive(*world, n, 1.0f / 60.0f);
        bodies->step(1.0f / 60.0f);
        // The person has no crouch clip: while the capsule is crouched the model is squashed to its height.
        const kin::CharacterController3D* done = hero.get<kin::CharacterController3D>();
        hero.get_mut<kin::Transform3D>()->scale.y =
            done->state.height > 0.0f ? done->state.height / done->desc.height : 1.0f;
        render->animate(1.0f / 60.0f);
        ++stepped;
    }

    void advance_to(int steps) {
        if (steps < stepped) {
            reset();
        }
        while (stepped < steps) {
            step_once();
        }
    }

    // `base` is the camera's own frame (its `frame`, else the scene's lead); `time` the offset the harness asks for.
    void draw(kin::Renderer3D& r3d, kin::Scene3D& scene, int base, int time) {
        if (!loaded) {
            return;
        }
        advance_to(base + std::max(0, time));
        using character_level::Kind;
        const auto colour_of = [](Kind k) {
            switch (k) {
            case Kind::Floor:
                return kin::Color::rgb(112, 120, 108);
            case Kind::Ramp:
                return kin::Color::rgb(214, 150, 80);
            case Kind::Plateau:
                return kin::Color::rgb(196, 176, 140);
            case Kind::Hump:
                return kin::Color::rgb(120, 170, 120);
            case Kind::Step:
                return kin::Color::rgb(176, 170, 160);
            case Kind::Ledge:
                return kin::Color::rgb(170, 120, 120);
            case Kind::Wall:
                return kin::Color::rgb(150, 160, 180);
            case Kind::Ceiling:
                return kin::Color::rgb(130, 140, 160);
            case Kind::Box:
                return kin::Color::rgb(220, 190, 90);
            case Kind::HeavyBox:
                return kin::Color::rgb(90, 70, 60);
            case Kind::Platform:
                return kin::Color::rgb(80, 140, 220);
            case Kind::Spinner:
                return kin::Color::rgb(80, 200, 190);
            case Kind::Sensor:
                return kin::Color::rgb(240, 220, 90);
            }
            return kin::Color::rgb(200, 200, 200);
        };
        for (const character_level::Piece& p : level.pieces) {
            Vec3f centre = p.centre;
            kin::Quatf rotation = p.rotation;
            if (p.moving) {
                centre = world->position(p.body);
                rotation = world->rotation(p.body);
            }
            if (p.kind == Kind::Sensor) {
                continue; // drawn by its debug box only: a trigger is not a thing
            }
            const kin::Material3D material{.color = colour_of(p.kind), .roughness = 0.85f};
            if (p.shape == kin::Shape3DKind::Sphere) {
                (void)scene.draw(ball_mesh, Mat4f::trs(centre, rotation, p.half), material);
            } else {
                (void)scene.draw(box_mesh, Mat4f::trs(centre, rotation, p.half * 2.0f), material);
            }
        }
        render->propagate_transforms();
        render->collect(scene);
        if (debug) {
            kin::DebugDraw3DOptions look;
            look.point_size = 0.06f;
            look.normal_length = 0.4f;
            kin::DebugDraw3D lines{look};
            lines.world(*world);
            const kin::Character3D c = bodies->character_of(hero);
            for (const kin::CharacterContact3D& k : world->character_contacts(c)) {
                lines.cross(k.point, k.sensor ? kin::Color::rgb(255, 220, 60) : kin::Color::rgb(255, 60, 60));
                lines.line(k.point, k.point + k.normal * 0.4f,
                           k.walkable ? kin::Color::rgb(60, 255, 120) : kin::Color::rgb(255, 120, 0));
            }
            debug_mesh = kin::draw_debug_lines(r3d, scene, lines.lines());
        }
    }
};

// `procedural: "physics_climb"` (docs/physics_3d.md, "Grab and climb"): the climb level (tests/climb_level.hpp: a 4 m
// wall with a ledge, an overhang, a smooth wall with holds, a platform that moves, a crate on a pedestal, ice, a ledge
// under a low roof) with the skinned person of `physics_character` driven through it by the scripted route of the scene
// file (grab and release by hand, reach, climb, pull up, jump off, drop), through the ECS system (CharacterController3D
// with traversal on, the locomotion Animator3D with the hands' IK on the anchors). The picture of frame f is the world
// `lead + f` fixed steps in; a frame asked for out of order starts the world again, so every frame is the same whatever
// order the run takes them in. The debug lines show every body, the grips (a cross at each anchor, its surface's normal
// and frame, a line to the body), the hang place and, for a hand that has the command and holds nothing, what its probe
// sees.
struct PhysicsClimb {
    kin::Mesh3D box_mesh, debug_mesh;
    std::shared_ptr<const kin::Model3D> person;
    std::unique_ptr<kin::World3D> world;
    std::unique_ptr<kin::EcsWorld> ecs;
    std::unique_ptr<kin::WorldPhysicsState3D> bodies;
    std::unique_ptr<kin::WorldRenderState3D> render;
    climb_level::Level level;
    climb_level::Route route;
    flecs::entity hero;
    int lead = 300;
    int stepped = 0;
    bool debug = true;
    bool loaded = false;
    climb_level::Applied applied;

    Vec3f feet() const {
        const kin::Transform3D* t = hero.is_valid() ? hero.get<kin::Transform3D>() : nullptr;
        return t != nullptr ? t->position : Vec3f{};
    }

    void reset() {
        // What refers to the ECS world and the physics world goes first (a frame earlier than the last one asked for
        // starts again).
        render.reset();
        bodies.reset();
        hero = {};
        ecs.reset();
        world.reset();
        kin::World3DConfig cfg;
        cfg.single_threaded = true; // one thread: the picture does not depend on a pool
        world = std::make_unique<kin::World3D>(cfg);
        ecs = std::make_unique<kin::EcsWorld>();
        level = climb_level::build(*world);
        bodies = std::make_unique<kin::WorldPhysicsState3D>(*ecs, *world);
        render = std::make_unique<kin::WorldRenderState3D>(*ecs);
        kin::CharacterController3D controller;
        controller.desc = climb_level::climber_desc();
        controller.input.forward = {0.0f, 0.0f, -1.0f};
        // The model faces +Z; it starts looking into the wall.
        controller.facing = kin::Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 180.0f);
        kin::LocomotionClips3D clips;
        kin::Animator3D animator = kin::locomotion_animator(clips);
        kin::IKConstraint3D feet_ik;
        feet_ik.name = "feet";
        feet_ik.kind = kin::IKKind3D::FootPlacer;
        feet_ik.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
        feet_ik.foot_heights = {0.05f, 0.05f};
        feet_ik.pole = kin::IKConstraint3D::Pole::ModelDirection;
        feet_ik.pole_vector = {0.0f, 0.0f, 1.0f};
        feet_ik.two_bone.max_stretch = 1.1f;
        feet_ik.foot.smooth_seconds = 0.08f;
        feet_ik.fade_seconds = 0.12f;
        animator.add_ik(feet_ik);
        // The hands: a two-bone arm each, the elbows out, down and back; on the anchors while a limb reaches or holds.
        for (const char* side : {"L", "R"}) {
            kin::IKConstraint3D hand;
            hand.name = std::string("hand_") + side;
            hand.kind = kin::IKKind3D::TwoBone;
            hand.joints = {std::string("shoulder_") + side, std::string("elbow_") + side, std::string("hand_") + side};
            hand.weight = 0.0f;
            hand.fade_seconds = 0.12f;
            hand.pole = kin::IKConstraint3D::Pole::ModelDirection;
            hand.pole_vector = {side[0] == 'L' ? 0.5f : -0.5f, -0.7f, -0.5f};
            hand.two_bone.max_stretch = 1.05f;
            animator.add_ik(hand);
        }
        kin::CharacterAnimationLink3D link;
        link.hands = {"hand_L", "hand_R"};
        link.hand_clearance = 0.05f;
        hero = ecs->raw()
                   .entity()
                   .set(kin::Transform3D{.position = route.start})
                   .set(controller)
                   .set(kin::SkinnedMeshRenderer3D{.model = person})
                   .set(static_cast<const kin::Animator3D&>(animator))
                   .set(link);
        stepped = 0;
        applied = {};
        applied.input.forward = {0.0f, 0.0f, -1.0f};
    }

    void make(kin::Renderer3D& r3d, const kin::JsonValue& doc, const fs::path& cache) {
        debug = doc.find("debug") == nullptr || doc.bool_at("debug");
        lead = static_cast<int>(doc.number_at("lead", 300));
        if (const kin::JsonValue* c = doc.find("character")) {
            route = climb_level::load_route(*c);
        }
        box_mesh = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f})).value_or({});
        std::string error;
        kin::Model3D model = r3d.load_model(cache / "made" / "skinned_biped_walk.glb", {}).value_or({});
        error = kin::diagnostics3d_text(r3d.diagnostics());
        if (!model) {
            std::fprintf(stderr, "  skinned_biped_walk.glb: %s (python3 tools/reference/make_scenes.py <cache>/made)\n",
                         error.c_str());
            return;
        }
        person = std::make_shared<const kin::Model3D>(std::move(model));
        loaded = true;
        reset();
    }

    // One fixed step of the route: the keys of this frame, the level's platform, the physics system, the animator.
    void step_once() {
        const int n = stepped;
        applied = climb_level::apply_keys(applied, route, n);
        kin::CharacterController3D* c = hero.get_mut<kin::CharacterController3D>();
        c->input = applied.input;
        c->traversal = applied.traversal;
        if (applied.teleport) {
            hero.get_mut<kin::Transform3D>()->position = applied.teleport_to;
        }
        if (applied.stamina) {
            world->set_traversal_resource(bodies->character_of(hero), applied.stamina_value);
        }
        // While walking it faces the way it was told to go (the model faces +Z); hanging, the system turns it to the
        // wall.
        const Vec3f f = c->input.forward;
        if (c->state.traversal_mode == kin::TraversalMode3D::Walking && (f.x != 0.0f || f.z != 0.0f)) {
            c->facing = kin::Quatf::axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(f.x, f.z) * 57.29578f);
        }
        level.drive(*world, n, 1.0f / 60.0f);
        bodies->step(1.0f / 60.0f);
        render->animate(1.0f / 60.0f);
        ++stepped;
    }

    void advance_to(int steps) {
        if (steps < stepped) {
            reset();
        }
        while (stepped < steps) {
            step_once();
        }
    }

    void draw(kin::Renderer3D& r3d, kin::Scene3D& scene, int base, int time) {
        if (!loaded) {
            return;
        }
        advance_to(base + std::max(0, time));
        using climb_level::Kind;
        const auto colour_of = [](Kind k) {
            switch (k) {
            case Kind::Floor:
                return kin::Color::rgb(112, 120, 108);
            case Kind::Wall:
                return kin::Color::rgb(186, 160, 130);
            case Kind::Lip:
                return kin::Color::rgb(210, 190, 150);
            case Kind::Roof:
                return kin::Color::rgb(130, 140, 160);
            case Kind::SmoothWall:
                return kin::Color::rgb(150, 156, 170);
            case Kind::Hold:
                return kin::Color::rgb(230, 130, 70);
            case Kind::Platform:
                return kin::Color::rgb(80, 140, 220);
            case Kind::Pedestal:
                return kin::Color::rgb(176, 170, 160);
            case Kind::Crate:
                return kin::Color::rgb(220, 190, 90);
            case Kind::IceWall:
                return kin::Color::rgb(150, 205, 235);
            case Kind::LowRoof:
                return kin::Color::rgb(110, 110, 125);
            case Kind::Ledge:
                return kin::Color::rgb(170, 120, 120);
            }
            return kin::Color::rgb(200, 200, 200);
        };
        for (const climb_level::Piece& p : level.pieces) {
            Vec3f centre = p.centre;
            kin::Quatf rotation = p.rotation;
            if (p.moving) {
                centre = world->position(p.body);
                rotation = world->rotation(p.body);
            }
            const kin::Material3D material{.color = colour_of(p.kind), .roughness = 0.85f};
            (void)scene.draw(box_mesh, Mat4f::trs(centre, rotation, p.half * 2.0f), material);
        }
        render->propagate_transforms();
        render->collect(scene);
        if (debug) {
            kin::DebugDraw3DOptions look;
            look.point_size = 0.05f;
            look.normal_length = 0.3f;
            kin::DebugDraw3D lines{look};
            lines.world(*world);
            const kin::Character3D c = bodies->character_of(hero);
            lines.grips(*world, c);
            // A hand that has the command and holds nothing: what it could take (best one longer).
            const kin::TraversalState3D t = world->traversal(c);
            const kin::CharacterDesc3D desc = climb_level::climber_desc();
            const kin::CharacterState3D s = world->character(c);
            for (kin::u32 h = 0; h < t.limb_count; ++h) {
                if (!applied.traversal.grab[h] || t.grips[h].phase != kin::GripPhase3D::Free) {
                    continue;
                }
                kin::GripProbeDesc3D probe;
                const Vec3f sh = desc.traversal.limbs[h].shoulder;
                const Vec3f facing =
                    t.mode == kin::TraversalMode3D::Walking ? Vec3f{0.0f, 0.0f, -1.0f} : -normalize(t.surface_normal);
                const Vec3f right = normalize(kin::cross(facing, s.up));
                probe.origin = s.position + right * sh.x + s.up * sh.y + facing * sh.z;
                probe.direction = length(applied.traversal.reach) > 0.0f ? applied.traversal.reach : facing;
                probe.up = s.up;
                probe.forward = facing;
                probe.reach = desc.traversal.reach;
                probe.clearance_radius = desc.radius;
                probe.clearance_height = desc.height;
                probe.ignore = s.body;
                lines.candidates(world->probe_grips(probe));
            }
            debug_mesh = kin::draw_debug_lines(r3d, scene, lines.lines());
        }
    }
};

// `procedural: "physics_ragdoll"` (docs/physics_3d.md, "Ragdolls"): five skinned people (`made/skinned_biped_walk.glb`)
// made ragdolls by the ECS system (Ragdoll3D from their skeleton, `auto_ragdoll_desc`), on a recipe of the scene's own:
// one dropped 4 m onto a ramp, one standing whose left arm only is a ragdoll and is knocked, one that blends into a
// ragdoll when struck, lies, and gets up again (placed where the pelvis lies, the clip blended back), one thrown at a
// wall, and one struck by a ball (the contact event starts its ragdoll with the impulse at the bone that was hit). The
// pose is written into each Animator3D after the clips and before the joint matrices, so that the skinned draw is the
// bodies'. The picture of frame f is the world `lead + f` fixed steps in; a frame asked for out of order starts the
// world again, so every frame is the same whatever order the run takes them in. The debug lines show the bodies (the
// capsules of the bones) in the colours of awake and sleeping.
struct PhysicsRagdoll {
    struct Block {
        Vec3f centre, half;
        kin::Quatf rotation;
        kin::Color colour;
    };
    kin::Mesh3D box_mesh, ball_mesh, debug_mesh;
    std::shared_ptr<const kin::Model3D> person;
    std::unique_ptr<kin::World3D> world;
    std::unique_ptr<kin::EcsWorld> ecs;
    std::unique_ptr<kin::WorldPhysicsState3D> bodies;
    std::unique_ptr<kin::WorldRenderState3D> render;
    std::vector<Block> blocks;
    // 0 the faller, 1 the arm, 2 the one that gets up, 3 the thrown, 4 the one hit by the ball.
    std::array<flecs::entity, 5> actors{};
    kin::Body3D ball{};
    bool ball_hit = false;
    int lead = 0;
    int stepped = 0;
    bool debug = true;
    bool loaded = false;

    int bone_of(flecs::entity e, const char* joint) const {
        const kin::RagdollHandle3D r = bodies->ragdoll_of(e);
        const kin::RagdollDesc* d = world->ragdoll_desc(r);
        const kin::SkinnedMeshRenderer3D* sk = e.get<kin::SkinnedMeshRenderer3D>();
        if (d == nullptr || sk == nullptr) {
            return -1;
        }
        const kin::i32 j = sk->model->skeletons[0].find(joint);
        for (std::size_t i = 0; i < d->bones.size(); ++i) {
            if (d->bones[i].joint == j) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void reset() {
        render.reset();
        bodies.reset();
        actors = {};
        ecs.reset();
        world.reset();
        kin::World3DConfig cfg;
        cfg.single_threaded = true; // one thread: the picture does not depend on a pool
        world = std::make_unique<kin::World3D>(cfg);
        ecs = std::make_unique<kin::EcsWorld>();
        blocks.clear();
        const auto add = [&](Vec3f centre, Vec3f half, kin::Quatf rotation, kin::Color colour) {
            blocks.push_back({centre, half, rotation, colour});
            world->create_body({.shape = kin::Shape3D::box(half),
                                .motion = kin::Body3DMotion::Static,
                                .position = centre,
                                .rotation = rotation,
                                .friction = 0.7f});
        };
        add({0.0f, -0.5f, 0.0f}, {14.0f, 0.5f, 8.0f}, {}, kin::Color::rgb(118, 124, 112));
        add({-4.0f, 1.2f, 0.0f}, {2.0f, 0.15f, 2.5f}, kin::Quatf::axis_angle({0, 0, 1}, 22.0f),
            kin::Color::rgb(214, 150, 80));
        add({6.4f, 1.5f, 0.0f}, {0.2f, 1.5f, 3.5f}, {}, kin::Color::rgb(170, 120, 120));
        bodies = std::make_unique<kin::WorldPhysicsState3D>(*ecs, *world);
        render = std::make_unique<kin::WorldRenderState3D>(*ecs);
        const Vec3f at[5] = {
            {-4.2f, 4.0f, 0.0f}, {-1.4f, 0.0f, 0.0f}, {1.6f, 0.0f, 0.0f}, {4.0f, 1.0f, 1.5f}, {-8.0f, 0.0f, 2.5f}};
        const float yaw[5] = {0.0f, 0.0f, 25.0f, 90.0f, 90.0f};
        for (int i = 0; i < 5; ++i) {
            kin::Animator3D animator;
            animator.clip = "idle";
            animator.loop = true;
            actors[static_cast<std::size_t>(i)] =
                ecs->raw()
                    .entity()
                    .set(kin::Transform3D{.position = at[i], .rotation = kin::Quatf::axis_angle({0, 1, 0}, yaw[i])})
                    .set(kin::SkinnedMeshRenderer3D{.model = person})
                    .set(static_cast<const kin::Animator3D&>(animator))
                    .set(kin::Ragdoll3D{});
        }
        bodies->sync_bodies();
        ball = {};
        ball_hit = false;
        stepped = 0;
    }

    void make(kin::Renderer3D& r3d, const kin::JsonValue& doc, const fs::path& cache) {
        debug = doc.find("debug") == nullptr || doc.bool_at("debug");
        lead = static_cast<int>(doc.number_at("lead", 0));
        box_mesh = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f})).value_or({});
        ball_mesh = r3d.create_mesh(kin::meshes::sphere(1.0f, 24, 12)).value_or({});
        std::string error;
        kin::Model3D model = r3d.load_model(cache / "made" / "skinned_biped_walk.glb", {}).value_or({});
        error = kin::diagnostics3d_text(r3d.diagnostics());
        if (!model) {
            std::fprintf(stderr, "  skinned_biped_walk.glb: %s (python3 tools/reference/make_scenes.py <cache>/made)\n",
                         error.c_str());
            return;
        }
        person = std::make_shared<const kin::Model3D>(std::move(model));
        loaded = true;
        reset();
    }

    void step_once() {
        const int n = stepped;
        const auto fall = [&](int i) { return actors[static_cast<std::size_t>(i)]; };
        if (n == 0) {
            // The arm alone is the ragdoll's; the faller lets go at once.
            bodies->set_ragdoll_subtree(fall(1), "shoulder_L", 1.0f);
            bodies->start_ragdoll(fall(0), {.set_velocity = true, .velocity = {1.2f, 0.0f, 0.0f}});
        }
        if (n == 30) {
            bodies->start_ragdoll(fall(1),
                                  {.impulse_bone = bone_of(fall(1), "hand_L"), .impulse = {0.0f, 55.0f, 40.0f}});
        }
        if (n == 20) {
            bodies->start_ragdoll(fall(3),
                                  {.blend_seconds = 0.1f, .set_velocity = true, .velocity = {7.0f, 2.5f, 0.0f}});
        }
        if (n == 60) {
            bodies->start_ragdoll(
                fall(2),
                {.blend_seconds = 0.4f, .impulse_bone = bone_of(fall(2), "chest"), .impulse = {70.0f, 10.0f, -40.0f}});
        }
        if (n == 90) {
            ball = world->create_body({.shape = kin::Shape3D::sphere(0.2f),
                                       .motion = kin::Body3DMotion::Dynamic,
                                       .position = {-12.0f, 1.3f, 2.5f},
                                       .linear_velocity = {16.0f, 0.5f, 0.0f},
                                       .mass = 6.0f,
                                       .friction = 0.5f,
                                       .gravity_factor = 0.3f,
                                       .continuous = true});
        }
        if (n == 330) {
            // Gets up where it lies: the character is put at the pelvis's ground point, facing by it, and the clip is
            // blended in.
            const kin::RagdollPlace3D place = world->ragdoll_get_up_place(bodies->ragdoll_of(fall(2)));
            if (place.found) {
                kin::Transform3D* t = fall(2).get_mut<kin::Transform3D>();
                t->position = place.position;
                t->rotation = place.rotation;
            }
            bodies->ragdoll_get_up(fall(2), 1.0f);
        }
        bodies->step(1.0f / 60.0f);
        // The ball's first contact with the person it was fired at: that ragdoll starts, with the ball's momentum at
        // the bone.
        if (ball.valid() && !ball_hit) {
            const std::vector<kin::Body3D> mine = world->ragdoll_bodies(bodies->ragdoll_of(fall(4)));
            for (const kin::ContactEvent3D& e : bodies->events()) {
                const kin::Body3D other = e.a == ball ? e.b : (e.b == ball ? e.a : kin::Body3D{});
                if (other.valid() && std::find(mine.begin(), mine.end(), other) != mine.end() &&
                    e.kind == kin::ContactKind3D::Begin) {
                    const std::size_t bone =
                        static_cast<std::size_t>(std::find(mine.begin(), mine.end(), other) - mine.begin());
                    bodies->start_ragdoll(fall(4), {.blend_seconds = 0.05f,
                                                    .impulse_bone = static_cast<int>(bone),
                                                    .impulse = world->linear_velocity(ball) * 2.5f,
                                                    .impulse_point = e.point,
                                                    .point_given = true});
                    ball_hit = true;
                    break;
                }
            }
        }
        render->animate(1.0f / 60.0f);
        ++stepped;
    }

    void advance_to(int steps) {
        if (steps < stepped) {
            reset();
        }
        while (stepped < steps) {
            step_once();
        }
    }

    void draw(kin::Renderer3D& r3d, kin::Scene3D& scene, int base, int time) {
        if (!loaded) {
            return;
        }
        advance_to(base + std::max(0, time));
        for (const Block& b : blocks) {
            (void)scene.draw(box_mesh, Mat4f::trs(b.centre, b.rotation, b.half * 2.0f),
                             {.color = b.colour, .roughness = 0.85f});
        }
        if (ball.valid()) {
            (void)scene.draw(ball_mesh, Mat4f::trs(world->position(ball), world->rotation(ball), {0.2f, 0.2f, 0.2f}),
                             {.color = kin::Color::rgb(230, 80, 60), .roughness = 0.4f});
        }
        render->propagate_transforms();
        render->collect(scene);
        if (debug) {
            kin::DebugDraw3DOptions look;
            look.point_size = 0.05f;
            kin::DebugDraw3D lines{look};
            lines.world(*world);
            debug_mesh = kin::draw_debug_lines(r3d, scene, lines.lines());
        }
    }
};

// `procedural: "physics_rope"` (docs/physics_3d.md, "Ropes"): chains of capsules with ball joints (Rope3D, drawn as
// tubes by WorldPhysicsState3D::collect_ropes): a hanging rope from a beam; a bridge of three ropes between two posts
// with a box on it (between low walls); a rope that swings with the skinned person on it (made out from the vertical,
// the person takes hold with both hands, climbs, and lets go: through the ECS system, CharacterController3D with
// traversal on, the hands' IK on the anchors); and a rope that a winch reels in with a crate on its end. The picture of
// frame f is the world `lead + f` fixed steps in (a frame asked for out of order starts the world again).
struct PhysicsRope {
    struct Block {
        Vec3f centre, half;
        kin::Color colour;
        flecs::entity entity; // a dynamic one is drawn where its entity is
    };
    kin::Mesh3D box_mesh, debug_mesh;
    std::shared_ptr<const kin::Model3D> person;
    std::unique_ptr<kin::World3D> world;
    std::unique_ptr<kin::EcsWorld> ecs;
    std::unique_ptr<kin::WorldPhysicsState3D> bodies;
    std::unique_ptr<kin::WorldRenderState3D> render;
    std::vector<Block> blocks;
    flecs::entity hero, crate, bridge_box, winch_rope, swing_rope;
    std::vector<flecs::entity> crates;
    int lead = 0;
    int stepped = 0;
    bool debug = true;
    bool loaded = false;

    Vec3f feet() const {
        const kin::Transform3D* t = hero.is_valid() ? hero.get<kin::Transform3D>() : nullptr;
        return t != nullptr ? t->position : Vec3f{};
    }

    void reset() {
        render.reset();
        bodies.reset();
        hero = crate = bridge_box = winch_rope = swing_rope = {};
        crates.clear();
        ecs.reset();
        world.reset();
        kin::World3DConfig cfg;
        cfg.single_threaded = true;
        world = std::make_unique<kin::World3D>(cfg);
        ecs = std::make_unique<kin::EcsWorld>();
        blocks.clear();
        bodies = std::make_unique<kin::WorldPhysicsState3D>(*ecs, *world);
        render = std::make_unique<kin::WorldRenderState3D>(*ecs);
        flecs::world& w = ecs->raw();
        const auto block = [&](Vec3f centre, Vec3f half, kin::Color colour, bool dynamic = false, float mass = 0.0f) {
            kin::Collider3D c;
            c.kind = kin::Collider3D::Kind::Box;
            c.half_extents = half;
            flecs::entity e = w.entity().set(kin::Transform3D{.position = centre}).set(c);
            if (dynamic) {
                e.set(kin::RigidBody3D{.mass = mass, .friction = 0.5f});
            }
            blocks.push_back({centre, half, colour, dynamic ? e : flecs::entity{}});
            return e;
        };
        block({10.0f, -0.5f, 0.0f}, {22.0f, 0.5f, 10.0f}, kin::Color::rgb(118, 124, 112));
        // The hanging rope's beam, and a rope from it with a ball of weight at its end (a free rope with nothing on
        // it).
        block({0.0f, 6.1f, 0.0f}, {0.4f, 0.1f, 0.4f}, kin::Color::rgb(120, 100, 80));
        // The bridge: two posts and low walls beside it, three ropes between, a box.
        block({-5.0f, 2.0f, 0.0f}, {0.25f, 2.0f, 1.2f}, kin::Color::rgb(130, 110, 90));
        block({1.0f, 2.0f, 0.0f}, {0.25f, 2.0f, 1.2f}, kin::Color::rgb(130, 110, 90));
        for (const float z : {-1.0f, 1.0f}) {
            block({-2.0f, 3.2f, z}, {3.0f, 0.6f, 0.05f}, kin::Color::rgb(150, 130, 100));
        }
        for (int k = 0; k < 3; ++k) {
            kin::Rope3D rope;
            const float z = 0.5f * static_cast<float>(k - 1);
            // From the posts' inner faces (an anchor inside a solid is pushed out of it, and kicks the rope).
            rope.start = kin::RopeAttach3D::at({-4.75f, 3.8f, z});
            rope.end = kin::RopeAttach3D::at({0.75f, 3.8f, z});
            rope.length = 5.9f;
            rope.radius = 0.05f;
            rope.mass_per_metre = 2.0f;
            rope.render = true;
            rope.render_radius = 0.06f;
            w.entity().set(kin::Transform3D{}).set(rope);
        }
        bridge_box = block({-2.0f, 3.3f, 0.0f}, {0.35f, 0.12f, 0.8f}, kin::Color::rgb(220, 190, 90), true, 30.0f);
        // The swing: a rope from a beam 9 m up made 25 degrees out, the person under its end.
        block({8.0f, 9.1f, 0.0f}, {0.4f, 0.1f, 0.4f}, kin::Color::rgb(120, 100, 80));
        {
            kin::Rope3D rope;
            rope.start = kin::RopeAttach3D::at({8.0f, 9.0f, 0.0f});
            rope.end = kin::RopeAttach3D::free();
            rope.length = 6.0f;
            const float a = 25.0f * 3.14159265f / 180.0f;
            rope.hang_direction = {std::sin(a), -std::cos(a), 0.0f};
            rope.render = true;
            rope.linear_damping = 0.0f;
            rope.angular_damping = 0.1f;
            swing_rope = w.entity().set(kin::Transform3D{}).set(rope);
        }
        // The winch: a rope from a gantry 6 m up to a 15 kg crate on the floor; reeled in from frame 120.
        block({16.0f, 6.1f, 0.0f}, {0.4f, 0.1f, 0.4f}, kin::Color::rgb(120, 100, 80));
        crate = block({16.0f, 0.35f, 0.0f}, {0.35f, 0.35f, 0.35f}, kin::Color::rgb(180, 120, 70), true, 15.0f);
        {
            kin::Rope3D rope;
            rope.start = kin::RopeAttach3D::at({16.0f, 6.0f, 0.0f});
            rope.end = kin::RopeAttach3D::on(crate, {0.0f, 0.35f, 0.0f});
            rope.linear_damping = 0.5f;
            rope.render = true;
            winch_rope = w.entity().set(kin::Transform3D{}).set(rope);
        }
        // The person on the swing: the character (traversal on, stamina to spare), the locomotion animator, hands on
        // the anchors.
        bodies->sync_bodies();
        const kin::Rope3D* sr = swing_rope.get<kin::Rope3D>();
        (void)sr;
        kin::CharacterController3D controller;
        controller.desc.traversal.enabled = true;
        controller.desc.traversal.resource.max = 1000.0f;
        controller.input.forward = {0.0f, 0.0f, -1.0f};
        controller.facing = kin::Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 180.0f);
        kin::LocomotionClips3D clips;
        kin::Animator3D animator = kin::locomotion_animator(clips);
        for (const char* side : {"L", "R"}) {
            kin::IKConstraint3D hand;
            hand.name = std::string("hand_") + side;
            hand.kind = kin::IKKind3D::TwoBone;
            hand.joints = {std::string("shoulder_") + side, std::string("elbow_") + side, std::string("hand_") + side};
            hand.weight = 0.0f;
            hand.fade_seconds = 0.12f;
            hand.pole = kin::IKConstraint3D::Pole::ModelDirection;
            hand.pole_vector = {side[0] == 'L' ? 0.5f : -0.5f, -0.7f, -0.5f};
            hand.two_bone.max_stretch = 1.05f;
            animator.add_ik(hand);
        }
        kin::CharacterAnimationLink3D link;
        link.hands = {"hand_L", "hand_R"};
        link.hand_clearance = 0.05f;
        // Under the swing rope's end (x = 8 + 6 sin 25), 1.5 below it, a hand's width in front.
        const float a = 25.0f * 3.14159265f / 180.0f;
        const Vec3f end{8.0f + 6.0f * std::sin(a), 9.0f - 6.0f * std::cos(a), 0.0f};
        hero = w.entity()
                   .set(kin::Transform3D{.position = end + Vec3f{0.0f, -1.5f, 0.45f}})
                   .set(controller)
                   .set(kin::SkinnedMeshRenderer3D{.model = person})
                   .set(static_cast<const kin::Animator3D&>(animator))
                   .set(link);
        stepped = 0;
    }

    void make(kin::Renderer3D& r3d, const kin::JsonValue& doc, const fs::path& cache) {
        debug = doc.find("debug") != nullptr && doc.bool_at("debug");
        lead = static_cast<int>(doc.number_at("lead", 0));
        box_mesh = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f})).value_or({});
        std::string error;
        kin::Model3D model = r3d.load_model(cache / "made" / "skinned_biped_walk.glb", {}).value_or({});
        error = kin::diagnostics3d_text(r3d.diagnostics());
        if (!model) {
            std::fprintf(stderr, "  skinned_biped_walk.glb: %s (python3 tools/reference/make_scenes.py <cache>/made)\n",
                         error.c_str());
            return;
        }
        person = std::make_shared<const kin::Model3D>(std::move(model));
        loaded = true;
        reset();
    }

    void step_once() {
        const int n = stepped;
        kin::CharacterController3D* c = hero.get_mut<kin::CharacterController3D>();
        if (n == 10) {
            c->traversal.grab = {true, true};
            c->traversal.reach = {0.0f, 0.5f, -1.0f};
        }
        if (n == 240) {
            c->input.move = {0.0f, 1.0f}; // up the rope
        }
        if (n == 420) {
            c->input.move = {0.0f, 0.0f};
        }
        if (n == 560) {
            c->traversal.jump_off = true;
        }
        if (n == 562) {
            c->traversal.jump_off = false;
            c->traversal.grab = {false, false};
        }
        if (n == 120) {
            kin::Rope3D* r = winch_rope.get_mut<kin::Rope3D>();
            r->target_length = 2.2f;
            r->reel_speed = 0.8f;
        }
        bodies->step(1.0f / 60.0f);
        render->animate(1.0f / 60.0f);
        ++stepped;
    }

    void advance_to(int steps) {
        if (steps < stepped) {
            reset();
        }
        while (stepped < steps) {
            step_once();
        }
    }

    void draw(kin::Renderer3D& r3d, kin::Scene3D& scene, int base, int time) {
        if (!loaded) {
            return;
        }
        advance_to(base + std::max(0, time));
        for (const Block& b : blocks) {
            // Dynamic ones are drawn where the world has them (their entity's transform).
            Mat4f m = Mat4f::translation(b.centre);
            if (b.entity.is_valid()) {
                const kin::Transform3D* t = b.entity.get<kin::Transform3D>();
                m = Mat4f::trs(t->position, t->rotation);
            }
            (void)scene.draw(box_mesh, m * Mat4f::scaling(b.half * 2.0f), {.color = b.colour, .roughness = 0.85f});
        }
        render->propagate_transforms();
        render->collect(scene);
        bodies->collect_ropes(r3d, scene);
        if (debug) {
            kin::DebugDraw3DOptions look;
            look.point_size = 0.05f;
            kin::DebugDraw3D lines{look};
            lines.world(*world);
            debug_mesh = kin::draw_debug_lines(r3d, scene, lines.lines());
        }
    }
};

// A stage for the light scenes (`procedural: "stage"`, P3b): a floor 24 m by 22, a back wall and a left wall 6 m tall,
// and what the scene file's `stage` places on it: `pillars` [[x, z], ...] (0.6 x 4 x 0.6 m), `poles` [[x, z], ...] (a
// thin 8 cm post 3 m tall: its shadow is the check of where a shadow lands), `lamps` [[x, z, height], ...] (a post and
// a glowing bulb that casts nothing; the light itself is the scene's own spot), `trees` [[x, z], ...] (two crossed
// quads, a leafy alpha texture, cut-out: the cut-out caster), `benches` [[x, z], ...] (a slab on two blocks).
// Everything stands on the floor at y = 0.
struct Stage {
    kin::Mesh3D floor, wall_back, wall_side, pillar, pole, post, bulb, quad, slab, block;
    kin::Texture leaf;
    std::vector<Vec3f> pillars, poles, lamps, trees, benches;
    bool walls = true, floor_on = true, room = false; // `room`: the right and front walls and a ceiling at 6 m too

    static kin::MeshData3D square(float half) {
        kin::MeshData3D m;
        m.vertices = {{{-half, 0, 0}, {0, 0, 1}, {0, 1}},
                      {{half, 0, 0}, {0, 0, 1}, {1, 1}},
                      {{half, 2.0f * half, 0}, {0, 0, 1}, {1, 0}},
                      {{-half, 2.0f * half, 0}, {0, 0, 1}, {0, 0}}};
        m.indices = {0, 1, 2, 0, 2, 3};
        return m;
    }

    void make(kin::Renderer3D& r3d, kin::Renderer2D& renderer, const kin::JsonValue& doc) {
        // `stage.ground: [width, depth]` makes the floor larger (for the rain, which falls in a volume of 36 m),
        // centred where the 24 x 22 one is.
        float ground_w = 24.0f, ground_d = 22.0f;
        if (const kin::JsonValue* stage = doc.find("stage")) {
            if (const kin::JsonValue* g = stage->find("ground")) {
                if (g->items().size() >= 2) {
                    ground_w = static_cast<float>(g->items()[0].as_number());
                    ground_d = static_cast<float>(g->items()[1].as_number());
                }
            }
        }
        floor = r3d.create_mesh(kin::meshes::box({ground_w, 0.2f, ground_d})).value_or({});
        wall_back = r3d.create_mesh(kin::meshes::box({24.0f, 6.0f, 0.3f})).value_or({});
        wall_side = r3d.create_mesh(kin::meshes::box({0.3f, 6.0f, 22.0f})).value_or({});
        pillar = r3d.create_mesh(kin::meshes::box({0.6f, 4.0f, 0.6f})).value_or({});
        pole = r3d.create_mesh(kin::meshes::box({0.08f, 3.0f, 0.08f})).value_or({});
        post = r3d.create_mesh(kin::meshes::cylinder(0.06f, 1.0f, 10)).value_or({});
        bulb = r3d.create_mesh(kin::meshes::sphere(0.18f, 16, 8)).value_or({});
        quad = r3d.create_mesh(square(1.6f)).value_or({});
        slab = r3d.create_mesh(kin::meshes::box({2.0f, 0.12f, 0.6f})).value_or({});
        block = r3d.create_mesh(kin::meshes::box({0.3f, 0.45f, 0.5f})).value_or({});
        // The leaf texture: green discs in a grid on a transparent ground, a trunk in the lower middle (nearest, so the
        // holes are hard).
        constexpr int n = 64;
        std::vector<kin::u8> texels(static_cast<std::size_t>(n) * n * 4, 0);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const int cx = x % 16 - 8, cy = y % 16 - 8;
                const bool leafy = y < 44 && cx * cx + cy * cy <= 36 && ((x / 16 + y / 16) % 4 != 3);
                const bool trunk = y >= 40 && x >= 29 && x <= 34;
                kin::u8* t = &texels[(static_cast<std::size_t>(y) * n + x) * 4];
                if (leafy) {
                    t[0] = static_cast<kin::u8>(30 + 10 * (x % 3));
                    t[1] = static_cast<kin::u8>(130 + 8 * (y % 4));
                    t[2] = 40;
                    t[3] = 255;
                } else if (trunk) {
                    t[0] = 90;
                    t[1] = 60;
                    t[2] = 35;
                    t[3] = 255;
                }
            }
        }
        leaf = renderer.create_texture_from_rgba(texels.data(), {n, n});
        renderer.set_scale_mode(leaf, kin::ScaleMode::Nearest);
        const auto list = [&](const char* key, std::vector<Vec3f>& out) {
            if (const kin::JsonValue* stage = doc.find("stage")) {
                if (const kin::JsonValue* items = stage->find(key)) {
                    for (const kin::JsonValue& item : items->items()) {
                        const auto& v = item.items();
                        out.push_back({v.size() > 0 ? static_cast<float>(v[0].as_number()) : 0.0f,
                                       v.size() > 2 ? static_cast<float>(v[2].as_number()) : 0.0f,
                                       v.size() > 1 ? static_cast<float>(v[1].as_number()) : 0.0f});
                    }
                }
            }
        };
        // [x, z] in the file; stored as (x, height or 0, z).
        list("pillars", pillars);
        list("poles", poles);
        list("lamps", lamps); // [x, z, height]: stored (x, z -> .z, height -> .y)
        list("trees", trees);
        list("benches", benches);
        if (const kin::JsonValue* stage = doc.find("stage")) {
            walls = stage->bool_at("walls", true);
            room = stage->bool_at("room", false);
            floor_on = stage->bool_at("floor", true);
        }
    }

    void draw(kin::Scene3D& scene) const {
        const kin::Material3D grey{.color = kin::Color::rgb(150, 150, 150), .roughness = 0.9f};
        const kin::Material3D plaster{.color = kin::Color::rgb(205, 198, 185), .roughness = 0.9f};
        const kin::Material3D stone{.color = kin::Color::rgb(190, 180, 170), .roughness = 0.8f};
        const kin::Material3D white{.color = kin::Color::rgb(235, 235, 235), .roughness = 0.7f};
        if (floor_on) {
            (void)scene.draw(floor, Mat4f::translation({0.0f, -0.1f, -3.0f}), grey);
        }
        if (walls) {
            (void)scene.draw(wall_back, Mat4f::translation({0.0f, 3.0f, -13.85f}), plaster);
            (void)scene.draw(wall_side, Mat4f::translation({-12.15f, 3.0f, -3.0f}), plaster);
        }
        if (room) {
            (void)scene.draw(wall_side, Mat4f::translation({12.15f, 3.0f, -3.0f}), plaster);
            (void)scene.draw(wall_back, Mat4f::translation({0.0f, 3.0f, 8.15f}), plaster);
            (void)scene.draw(floor, Mat4f::translation({0.0f, 6.1f, -3.0f}), plaster);
        }
        for (const Vec3f& p : pillars) {
            (void)scene.draw(pillar, Mat4f::translation({p.x, 2.0f, p.z}), stone);
        }
        for (const Vec3f& p : poles) {
            (void)scene.draw(pole, Mat4f::translation({p.x, 1.5f, p.z}), white);
        }
        for (const Vec3f& p : lamps) {
            // The post stops a quarter metre under the bulb: a light right at a post's top would sit in a shadow of the
            // post's own cap.
            const float h = p.y > 0.0f ? p.y : 4.0f;
            const float post_h = h - 0.25f;
            (void)scene.draw(post, Mat4f::trs({p.x, post_h * 0.5f, p.z}, kin::Quatf{}, {0.7f, post_h, 0.7f}),
                             {.color = kin::Color::rgb(60, 62, 66), .metallic = 0.6f, .roughness = 0.5f});
            (void)scene.draw(bulb, Mat4f::translation({p.x, h + 0.1f, p.z}),
                             {.color = kin::Color::rgb(255, 236, 190),
                              .emissive = 3.0f,
                              .unlit = true,
                              .casts_shadow = false,
                              .receives_shadow = false});
        }
        const kin::Material3D leaf_material{.color = kin::Color::rgb(255, 255, 255),
                                            .double_sided = true,
                                            .blend = kin::Blend3D::Mask,
                                            .texture = leaf,
                                            .alpha_cutoff = 0.5f};
        for (const Vec3f& p : trees) {
            (void)scene.draw(quad, Mat4f::translation({p.x, 0.0f, p.z}), leaf_material);
            (void)scene.draw(quad, Mat4f::translation({p.x, 0.0f, p.z}) * Mat4f::rotation_y(90.0f), leaf_material);
        }
        for (const Vec3f& p : benches) {
            (void)scene.draw(slab, Mat4f::translation({p.x, 0.51f, p.z}),
                             {.color = kin::Color::rgb(150, 100, 60), .roughness = 0.7f});
            (void)scene.draw(block, Mat4f::translation({p.x - 0.7f, 0.225f, p.z}), stone);
            (void)scene.draw(block, Mat4f::translation({p.x + 0.7f, 0.225f, p.z}), stone);
        }
    }
};

kin::Bounds3D bounds_of(const std::vector<Placed>& placed) {
    Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Placed& p : placed) {
        const kin::Bounds3D& b = p.model.bounds;
        for (int i = 0; i < 8; ++i) {
            const Vec3f c = p.transform.apply(
                {(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z});
            lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
            hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
        }
    }
    return placed.empty() ? kin::Bounds3D{} : kin::Bounds3D{lo, hi};
}

} // namespace

// The command line's settings that replace a scene's own: --ao, --contact-shadows and --taa turn the feature on for
// every scene, and each number, when above 0, takes the place of the scene's.
struct Overrides {
    bool ao = false, ao_half = false;
    double ao_radius = 0.0, ao_intensity = 0.0;
    bool contact = false;
    double contact_distance = 0.0, contact_thickness = 0.0;
    int contact_steps = 0;
    bool taa = false;
    double taa_blend = 0.0, taa_sharpness = 0.0;
    bool lod = false;
    double lod_error = 0.0, lod_bias = 0.0;
    int lod_force = -2; // -2: the scene's own
    int lod_levels = 0; // 0: the scene's own (none)
    bool lod_clusters = false;
    int lod_gpu = -1; // -1: the scene's (on); 0 the CPU cuts; 1 the GPU
    bool readback = false;
    long lod_room = 0;
    bool lod_occlusion = false;
    int flicker = 0; // --flicker N: after each camera's picture, N more frames read back, the largest and mean
                     // frame-to-frame difference printed (temporal shimmer)
    bool gpu_pass_markers = false; // labels for external GPU profilers
    std::string
        hitch; // --hitch <feature>: the first renders after switching it on at runtime, timed (then the process ends)
    bool warm_up = false;    // with --hitch: Renderer3D::warm_up before the feature goes on
    bool vram_cycle = false; // after a camera: the features off for 130 renders; prints the video memory with them on
                             // and after (see RenderDebugStats3D::feature_bytes)
    bool crowd_move =
        false; // the crowd's enemies drift each frame (a game's crowd always moves): the scene is posed per frame
    bool sky = false,
         no_sky =
             false; // --sky: the procedural sky on for every scene; --no-sky: the scene's own off (to time its cost)
    bool no_fog = false;          // --no-fog
    double fog = 0.0;             // --fog D: distance and height fog of density D, coloured by the sky, on every scene
    bool sky_light = false;       // --sky-light: Lights3D::sky_lights_scene
    bool sky_light_every = false; // --sky-light-every: with it, the panorama is made again every render (to time that)
    bool fog_fixed = false;       // --fog-fixed: with --fog, a fixed colour (the cheaper path) instead of the sky's
    double time_hours = -1.0;     // --time H: the time of day (solar hours) of every scene, with its date and latitude
    int spots = 0, spot_shadows = 0; // --spots N, --spot-shadows M: N spot lights in front of the camera, the first M
                                     // with a shadow map (cost tables)
    int point_lights = 0, point_shadows = 0; // --point-lights N, --point-shadows M
    bool moon_shadow =
        false; // --moon-shadow: the fill light (a moon if the scene has none) casts, with --moon-cascades N (2)
    int moon_cascades = 2;
    int local_res = 0;                            // --local-res N: the local shadow maps' side for every light
    double rain = -1.0, snow = -1.0, wind = -1.0; // --rain I, --snow I, --wind S: the weather's intensities and wind
                                                  // speed for every scene (-1: the scene's)
    bool no_weather = false, no_particles = false;
    int soft = -1;             // --soft 0|1: every particle batch's soft flag
    long particles = 0;        // --particles N: a system with N live particles in the scene's middle
    bool shadow_cache = false; // Lights3D::cache_shadows: off here, because a static scene is rendered many times and
                               // the shadow pass would be timed once
};

// Each feature's settings: the scene file's (if it has any; then on unless it says not), then the command line's.
void merge_ambient_occlusion(kin::AmbientOcclusion3D& ao, const kin::JsonValue* doc, const Overrides& over) {
    if (doc) {
        ao.enabled = doc->bool_at("enabled", true);
        ao.radius = static_cast<float>(doc->number_at("radius", ao.radius));
        ao.intensity = static_cast<float>(doc->number_at("intensity", ao.intensity));
        ao.half_resolution = doc->bool_at("half_resolution", false);
    }
    if (over.ao) {
        ao.enabled = true;
        ao.half_resolution = ao.half_resolution || over.ao_half;
        if (over.ao_radius > 0.0) {
            ao.radius = static_cast<float>(over.ao_radius);
        }
        if (over.ao_intensity > 0.0) {
            ao.intensity = static_cast<float>(over.ao_intensity);
        }
    }
}

void merge_contact_shadows(kin::ContactShadows3D& cs, const kin::JsonValue* doc, const Overrides& over) {
    if (doc) {
        cs.enabled = doc->bool_at("enabled", true);
        cs.distance = static_cast<float>(doc->number_at("distance", cs.distance));
        cs.thickness = static_cast<float>(doc->number_at("thickness", cs.thickness));
        cs.intensity = static_cast<float>(doc->number_at("intensity", cs.intensity));
        cs.steps = static_cast<kin::u32>(doc->number_at("steps", cs.steps));
    }
    if (over.contact) {
        cs.enabled = true;
        if (over.contact_distance > 0.0) {
            cs.distance = static_cast<float>(over.contact_distance);
        }
        if (over.contact_thickness > 0.0) {
            cs.thickness = static_cast<float>(over.contact_thickness);
        }
        if (over.contact_steps > 0) {
            cs.steps = static_cast<kin::u32>(over.contact_steps);
        }
    }
}

void merge_taa(kin::TemporalAA3D& taa, const kin::JsonValue* doc, const Overrides& over) {
    if (doc) {
        taa.enabled = doc->bool_at("enabled", true);
        taa.blend = static_cast<float>(doc->number_at("blend", taa.blend));
        taa.sharpness = static_cast<float>(doc->number_at("sharpness", taa.sharpness));
        taa.jitter_scale = static_cast<float>(doc->number_at("jitter_scale", taa.jitter_scale));
        taa.jitter_samples = static_cast<kin::u32>(doc->number_at("jitter_samples", taa.jitter_samples));
    }
    if (over.taa) {
        taa.enabled = true;
        if (over.taa_blend > 0.0) {
            taa.blend = static_cast<float>(over.taa_blend);
        }
        if (over.taa_sharpness > 0.0) {
            taa.sharpness = static_cast<float>(over.taa_sharpness);
        }
    }
}

void merge_lod(kin::Lod3D& lod, kin::RenderDebugOptions3D& debug, const kin::JsonValue* doc, const Overrides& over) {
    if (doc) {
        lod.enabled = doc->bool_at("enabled", true);
        lod.error_pixels = static_cast<float>(doc->number_at("error_pixels", lod.error_pixels));
        lod.bias = static_cast<float>(doc->number_at("bias", lod.bias));
        debug.force_level = static_cast<kin::i32>(doc->number_at("force_level", debug.force_level));
        if (const kin::JsonValue* occlusion = doc->find("occlusion")) {
            lod.occlusion.enabled = occlusion->is_bool() ? occlusion->as_bool() : occlusion->bool_at("enabled", true);
            lod.occlusion.depth_margin =
                static_cast<float>(occlusion->number_at("depth_margin", lod.occlusion.depth_margin));
            lod.occlusion.pixel_margin =
                static_cast<kin::u32>(occlusion->number_at("pixel_margin", lod.occlusion.pixel_margin));
        }
    }
    if (over.lod_occlusion) {
        lod.enabled = true;
        lod.occlusion.enabled = true;
    }
    if (over.lod) {
        lod.enabled = true;
        if (over.lod_error > 0.0) {
            lod.error_pixels = static_cast<float>(over.lod_error);
        }
        if (over.lod_bias > 0.0) {
            lod.bias = static_cast<float>(over.lod_bias);
        }
    }
    if (over.lod_force > -2) {
        debug.force_level = over.lod_force;
    }
    if (over.lod_gpu >= 0) {
        debug.gpu_culling = over.lod_gpu > 0;
    }
    if (over.lod_room > 0) {
        lod.max_cluster_triangles = static_cast<kin::u32>(over.lod_room);
    }
}

// The process's resident memory, in MB (Linux; 0 elsewhere).
// The GPU's memory in use (MiB), from nvidia-smi: the whole device, so only differences mean anything (this process is
// nearly alone on it); -1 without. The sky, fog and time of day (P3a): the scene file's, then the command line's.
void merge_sky(kin::Sky3D& sky, const kin::JsonValue* doc, const Overrides& over) {
    if (doc) {
        sky.enabled = doc->bool_at("enabled", true);
        sky.mode = doc->string_at("mode", "procedural") == "image" ? kin::SkyMode3D::Image : kin::SkyMode3D::Procedural;
        sky.turbidity = static_cast<float>(doc->number_at("turbidity", sky.turbidity));
        sky.sun_intensity = static_cast<float>(doc->number_at("sun_intensity", sky.sun_intensity));
        sky.intensity = static_cast<float>(doc->number_at("intensity", sky.intensity));
        sky.ground_color = light_color(doc->find("ground_color"), sky.ground_color);
        sky.sun_size = static_cast<float>(doc->number_at("sun_size", sky.sun_size));
        sky.sun_disc = static_cast<float>(doc->number_at("sun_disc", sky.sun_disc));
        sky.stars = doc->bool_at("stars", false);
        sky.star_brightness = static_cast<float>(doc->number_at("star_brightness", sky.star_brightness));
        sky.night_color = light_color(doc->find("night_color"), sky.night_color);
        sky.light_threshold = static_cast<float>(doc->number_at("light_threshold", sky.light_threshold));
        sky.light_interval = static_cast<kin::u32>(doc->number_at("light_interval", sky.light_interval));
        if (const kin::JsonValue* moon = doc->find("moon")) {
            sky.moon = moon->bool_at("enabled", true);
            sky.moon_direction = vec3(moon->find("direction"), sky.moon_direction);
            sky.moon_size = static_cast<float>(moon->number_at("size", sky.moon_size));
            sky.moon_light = static_cast<float>(moon->number_at("light", sky.moon_light));
            sky.moon_color = light_color(moon->find("color"), sky.moon_color);
        }
    }
    if (over.sky_light_every) {
        sky.light_threshold = -1.0f;
        sky.light_interval = 0;
    }
    if (over.sky) {
        sky.enabled = true;
    }
    if (over.no_sky) {
        sky.enabled = false;
    }
}

void merge_fog(kin::Fog3D& fog, const kin::JsonValue* doc, const Overrides& over) {
    if (doc) {
        fog.enabled = doc->bool_at("enabled", true);
        fog.density = static_cast<float>(doc->number_at("density", fog.density));
        fog.height_density = static_cast<float>(doc->number_at("height_density", fog.height_density));
        fog.height_falloff = static_cast<float>(doc->number_at("height_falloff", fog.height_falloff));
        fog.base_height = static_cast<float>(doc->number_at("base_height", fog.base_height));
        fog.start = static_cast<float>(doc->number_at("start", fog.start));
        fog.max_opacity = static_cast<float>(doc->number_at("max_opacity", fog.max_opacity));
        fog.colour = light_color(doc->find("color"), fog.colour);
        fog.colour_from_sky = doc->bool_at("color_from_sky", false);
        fog.sun_scatter = static_cast<float>(doc->number_at("sun_scatter", fog.sun_scatter));
        fog.sun_anisotropy = static_cast<float>(doc->number_at("sun_anisotropy", fog.sun_anisotropy));
    }
    if (over.fog > 0.0) {
        fog.enabled = true;
        fog.density = static_cast<float>(over.fog);
        fog.height_density = static_cast<float>(over.fog);
        fog.colour_from_sky = !over.fog_fixed;
        fog.sun_scatter = over.fog_fixed ? 0.0f : 0.3f;
    }
    if (over.no_fog) {
        fog.enabled = false;
    }
}

// Reference fixtures use the same local-light fields as native and ECS data.
void configure_shadow(auto& light, const kin::JsonValue& doc, int resolution) {
    if (doc.find("shadow"))
        throw std::runtime_error("local-light shadow object removed; use shadow_enabled and shadow_* fields");
    light.shadow_enabled = doc.bool_at("shadow_enabled", false);
    light.shadow_resolution = static_cast<kin::u32>(doc.number_at("shadow_resolution", 0));
    light.shadow_bias = static_cast<float>(doc.number_at("shadow_bias", light.shadow_bias));
    light.shadow_normal_offset = static_cast<float>(doc.number_at("shadow_normal_offset", light.shadow_normal_offset));
    light.shadow_near_plane = static_cast<float>(doc.number_at("shadow_near_plane", light.shadow_near_plane));
    light.shadow_distance = static_cast<float>(doc.number_at("shadow_distance", light.shadow_distance));
    light.shadow_softness = static_cast<float>(doc.number_at("shadow_softness", light.shadow_softness));
    if (resolution > 0 && light.shadow_enabled)
        light.shadow_resolution = static_cast<kin::u32>(resolution);
}

// `hours` of 0 or more replaces the scene's time of day (a camera's own, tod_sweep, or --time).
bool merge_time_of_day(kin::TimeOfDay3D& time, const kin::JsonValue* doc, double hours_override) {
    bool set = false;
    if (doc) {
        time.hours = static_cast<float>(doc->number_at("hours", time.hours));
        time.day_of_year = static_cast<float>(doc->number_at("day_of_year", time.day_of_year));
        time.latitude = static_cast<float>(doc->number_at("latitude", time.latitude));
        time.axial_tilt = static_cast<float>(doc->number_at("axial_tilt", time.axial_tilt));
        time.moon_phase = static_cast<float>(doc->number_at("moon_phase", time.moon_phase));
        time.shadows = doc->bool_at(
            "shadows", time.shadows); // P3b: the sun's and the moon's shadow strength follow the time of day
        set = true;
    }
    if (hours_override >= 0.0) {
        time.hours = static_cast<float>(hours_override);
        set = true;
    }
    return set;
}

kin::ParticleRange3D range_of(const kin::JsonValue* v, kin::ParticleRange3D fallback) {
    if (!v) {
        return fallback;
    }
    if (v->is_number()) {
        return {static_cast<float>(v->as_number()), static_cast<float>(v->as_number())};
    }
    if (v->items().size() >= 2) {
        return {static_cast<float>(v->items()[0].as_number()), static_cast<float>(v->items()[1].as_number())};
    }
    return fallback;
}

kin::ParticleBlend3D blend_of(const std::string& s, kin::ParticleBlend3D fallback) {
    return s == "additive"        ? kin::ParticleBlend3D::Additive
           : s == "premultiplied" ? kin::ParticleBlend3D::Premultiplied
           : s == "alpha"         ? kin::ParticleBlend3D::Alpha
                                  : fallback;
}

kin::ParticleFacing3D facing_of(const std::string& s, kin::ParticleFacing3D fallback) {
    return s == "stretched" ? kin::ParticleFacing3D::Stretched
           : s == "axis"    ? kin::ParticleFacing3D::Axis
           : s == "flat"    ? kin::ParticleFacing3D::Flat
           : s == "camera"  ? kin::ParticleFacing3D::Camera
                            : fallback;
}

void merge_look(kin::ParticleLook3D& look, const kin::JsonValue* d, const Overrides& over) {
    if (d) {
        look.blend = blend_of(d->string_at("blend"), look.blend);
        look.facing = facing_of(d->string_at("facing"), look.facing);
        look.axis = vec3(d->find("axis"), look.axis);
        look.stretch = static_cast<float>(d->number_at("stretch", look.stretch));
        look.stretch_min = static_cast<float>(d->number_at("stretch_min", look.stretch_min));
        look.softness = static_cast<float>(d->number_at("softness", look.softness));
        look.ring = static_cast<float>(d->number_at("ring", look.ring));
        look.soft = d->bool_at("soft", look.soft);
        look.soft_distance = static_cast<float>(d->number_at("soft_distance", look.soft_distance));
        look.lit = static_cast<float>(d->number_at("lit", look.lit));
        look.receives_shadow = d->bool_at("receives_shadow", look.receives_shadow);
        look.casts_shadow = d->bool_at("casts_shadow", look.casts_shadow);
        look.shadow_cutoff = static_cast<float>(d->number_at("shadow_cutoff", look.shadow_cutoff));
        look.fogged = d->bool_at("fog", look.fogged);
        look.sort = d->bool_at("sort", look.sort);
    }
    if (over.soft >= 0) {
        look.soft = over.soft > 0;
    }
}

// A weather layer's numbers (see the header): a bare number is the intensity.
void merge_layer(kin::WeatherLayer3D& layer, const kin::JsonValue* d, const Overrides& over,
                 double override_intensity) {
    if (d) {
        if (d->is_number()) {
            layer.intensity = static_cast<float>(d->as_number());
        } else {
            layer.intensity = static_cast<float>(d->number_at("intensity", layer.intensity));
            layer.max_particles = static_cast<kin::u32>(d->number_at("max", layer.max_particles));
            layer.box = vec3(d->find("box"), layer.box);
            layer.above = static_cast<float>(d->number_at("above", layer.above));
            layer.fall_speed = static_cast<float>(d->number_at("fall_speed", layer.fall_speed));
            layer.fall_variation = static_cast<float>(d->number_at("fall_variation", layer.fall_variation));
            layer.wind_influence = static_cast<float>(d->number_at("wind_influence", layer.wind_influence));
            layer.flutter = static_cast<float>(d->number_at("flutter", layer.flutter));
            const kin::ParticleRange3D size = range_of(d->find("size"), {layer.size_min, layer.size_max});
            layer.size_min = size.min;
            layer.size_max = size.max;
            layer.alpha = static_cast<float>(d->number_at("alpha", layer.alpha));
            layer.colour = light_color(d->find("color"), layer.colour);
            layer.splash = static_cast<float>(d->number_at("splash", layer.splash));
            layer.splash_size = static_cast<float>(d->number_at("splash_size", layer.splash_size));
            layer.edge_fade = static_cast<float>(d->number_at("edge_fade", layer.edge_fade));
            merge_look(layer.look, d, over);
        }
    }
    if (override_intensity >= 0.0) {
        layer.intensity = static_cast<float>(override_intensity);
    }
    if (over.soft >= 0) {
        layer.look.soft = over.soft > 0;
    }
}

// A scene's particles (P3c): the systems stepped at load (the pictures show one moment), and the weather's settings.
struct SceneParticles {
    struct Emitter {
        std::string name;
        kin::ParticleSystem3D system;
        bool wind = true;
    };
    std::vector<Emitter> emitters;
    kin::Weather3D weather;
    bool apply_weather = false;
    double weather_time = 0.0;

    void make(const kin::JsonValue& doc, const Overrides& over, const kin::Bounds3D& bounds) {
        if (const kin::JsonValue* w = doc.find("weather")) {
            merge_layer(weather.rain, w->find("rain"), over, over.rain);
            merge_layer(weather.snow, w->find("snow"), over, over.snow);
            merge_layer(weather.dust, w->find("dust"), over, -1.0);
            merge_layer(weather.mist, w->find("mist"), over, -1.0);
            if (const kin::JsonValue* wind = w->find("wind")) {
                weather.wind.direction = vec3(wind->find("direction"), weather.wind.direction);
                weather.wind.speed = static_cast<float>(wind->number_at("speed", weather.wind.speed));
                weather.wind.gust_strength = static_cast<float>(wind->number_at("gust", weather.wind.gust_strength));
                weather.wind.gust_period = static_cast<float>(wind->number_at("gust_period", weather.wind.gust_period));
                weather.wind.gust_length = static_cast<float>(wind->number_at("gust_length", weather.wind.gust_length));
                weather.wind.turbulence = static_cast<float>(wind->number_at("turbulence", weather.wind.turbulence));
                weather.wind.turbulence_length =
                    static_cast<float>(wind->number_at("turbulence_length", weather.wind.turbulence_length));
            }
            if (const kin::JsonValue* g = w->find("ground")) {
                weather.has_ground = true;
                weather.ground_height = static_cast<float>(g->as_number());
            }
            weather_time = w->number_at("time", 0.0);
            weather.seed = static_cast<kin::u32>(w->number_at("seed", 1.0));
            apply_weather = w->bool_at("apply", false);
        } else {
            merge_layer(weather.rain, nullptr, over, over.rain);
            merge_layer(weather.snow, nullptr, over, over.snow);
        }
        if (over.wind >= 0.0) {
            weather.wind.speed = static_cast<float>(over.wind);
            if (weather.wind.gust_strength == 0.0f) {
                weather.wind.gust_strength = 0.4f;
            }
        }
        if (over.no_weather) {
            weather.rain.intensity = weather.snow.intensity = weather.dust.intensity = weather.mist.intensity = 0.0f;
        }
        if (over.no_particles) {
            return;
        }
        const auto add = [&](const kin::JsonValue& d) {
            Emitter e;
            e.name = d.string_at("name", "particles");
            e.system.set_seed(static_cast<kin::u64>(d.number_at("seed", 1.0)));
            kin::ParticleDesc3D& p = e.system.desc;
            const std::string shape = d.string_at("shape", "point");
            p.shape = shape == "box"      ? kin::ParticleShape3D::Box
                      : shape == "sphere" ? kin::ParticleShape3D::Sphere
                      : shape == "cone"   ? kin::ParticleShape3D::Cone
                      : shape == "disc"   ? kin::ParticleShape3D::Disc
                                          : kin::ParticleShape3D::Point;
            p.extent = vec3(d.find("extent"), p.extent);
            p.surface_only = d.bool_at("surface", false);
            p.cone_angle = static_cast<float>(d.number_at("cone_angle", p.cone_angle));
            p.spread = static_cast<float>(d.number_at("spread", p.spread));
            p.rate = static_cast<float>(d.number_at("rate", p.rate));
            p.max_particles = static_cast<kin::u32>(d.number_at("max", p.max_particles));
            p.lifetime = range_of(d.find("lifetime"), p.lifetime);
            p.speed = range_of(d.find("speed"), p.speed);
            p.size = range_of(d.find("size"), p.size);
            p.rotation = range_of(d.find("rotation_range"), p.rotation);
            p.spin = range_of(d.find("spin"), p.spin);
            p.velocity_min = vec3(d.find("velocity_min"), p.velocity_min);
            p.velocity_max = vec3(d.find("velocity_max"), p.velocity_max);
            p.gravity = vec3(d.find("gravity"), p.gravity);
            p.drag = static_cast<float>(d.number_at("drag", p.drag));
            p.wind_influence = static_cast<float>(d.number_at("wind_influence", p.wind_influence));
            p.size_start = static_cast<float>(d.number_at("size_start", p.size_start));
            p.size_mid = static_cast<float>(d.number_at("size_mid", p.size_mid));
            p.size_end = static_cast<float>(d.number_at("size_end", p.size_end));
            p.colour_start = light_color(d.find("color_start"), p.colour_start);
            p.colour_end = light_color(d.find("color_end"), p.colour_end);
            p.alpha_start = static_cast<float>(d.number_at("alpha_start", p.alpha_start));
            p.alpha_end = static_cast<float>(d.number_at("alpha_end", p.alpha_end));
            p.fade_in = static_cast<float>(d.number_at("fade_in", p.fade_in));
            p.colour_variation = static_cast<float>(d.number_at("color_variation", p.colour_variation));
            if (const kin::JsonValue* g = d.find("ground")) {
                p.ground = true;
                p.ground_height = static_cast<float>(g->as_number());
                p.bounce = static_cast<float>(d.number_at("bounce", 0.0));
            }
            merge_look(p.look, d.find("look"), over);
            e.wind = d.bool_at("wind", true);
            const Vec3f at = vec3(d.find("position"));
            const Vec3f turn = vec3(d.find("rotation"));
            e.system.transform = Mat4f::translation(at) * Mat4f::rotation_y(turn.y) * Mat4f::rotation_x(turn.x) *
                                 Mat4f::rotation_z(turn.z);
            const kin::Wind3D* wind = e.wind ? &weather.wind : nullptr;
            if (d.number_at("burst", 0.0) > 0.0) {
                e.system.burst(static_cast<kin::u32>(d.number_at("burst", 0.0)));
            }
            const double seconds = d.number_at("simulate", 0.0);
            for (double t = 0.0; t + 1e-9 < seconds; t += 1.0 / 60.0) {
                e.system.update(1.0 / 60.0, wind);
            }
            emitters.push_back(std::move(e));
        };
        if (const kin::JsonValue* list = doc.find("particles")) {
            for (const kin::JsonValue& d : list->items()) {
                add(d);
            }
        }
        if (over.particles > 0) {
            // N live particles drifting in the middle of the scene: a soft smoke, alpha-blended and sorted (the cost
            // tables).
            Emitter e;
            e.name = "budget";
            e.system.set_seed(77);
            kin::ParticleDesc3D& p = e.system.desc;
            p.shape = kin::ParticleShape3D::Box;
            p.extent = {std::max(bounds.radius() * 0.25f, 2.0f), 1.5f, std::max(bounds.radius() * 0.25f, 2.0f)};
            p.max_particles = static_cast<kin::u32>(over.particles);
            p.lifetime = {1e6f, 1e6f};
            p.speed = {0.0f, 0.1f};
            p.spread = 180.0f;
            p.size = {0.15f, 0.35f};
            p.alpha_start = p.alpha_end = 0.25f;
            p.colour_start = p.colour_end = {0.8f, 0.85f, 0.95f};
            e.system.transform = Mat4f::translation({bounds.center().x, bounds.min.y + 1.5f, bounds.center().z});
            merge_look(p.look, nullptr, over);
            e.system.burst(static_cast<kin::u32>(over.particles));
            emitters.push_back(std::move(e));
        }
    }

    void draw(kin::Scene3D& scene, double frame_time) const {
        scene.weather = weather;
        scene.weather.time = weather_time + frame_time;
        if (apply_weather && weather.active()) {
            const kin::Fog3D base = scene.fog;
            scene.weather.apply(scene.lights, scene.fog, base);
        }
        for (const Emitter& e : emitters) {
            (void)scene.draw_particles(e.system);
        }
    }
};

double gpu_used_mb() {
    kin::Process process;
    if (!process.start({.args = {"nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"},
                        .pipe_input = false,
                        .capture_output = true,
                        .errors_to_output = true}) ||
        !process.wait(std::chrono::seconds{5}) || process.exit_code().value_or(-1) != 0) {
        return -1.0;
    }
    const auto line = process.read_line();
    return line ? std::atof(line->c_str()) : -1.0;
}

double resident_mb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmRSS:")) {
            return std::atof(line.c_str() + 6) / 1024.0;
        }
    }
    return 0.0;
}

// The most resident memory the process has had, in MB (Linux; 0 elsewhere).
double peak_mb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmHWM:")) {
            return std::atof(line.c_str() + 6) / 1024.0;
        }
    }
    return 0.0;
}

// The linear light of an 8-bit sRGB value, and back.
float decode_srgb(kin::u8 v) {
    return kin::srgb_to_linear(static_cast<float>(v) / 255.0f);
}
kin::u8 encode_srgb(float c) {
    return static_cast<kin::u8>(std::lround(kin::linear_to_srgb(std::clamp(c, 0.0f, 1.0f)) * 255.0f));
}

// A picture of `size` / `supersample` pixels a side made from `large`, each pixel the average, in linear light,
// of the supersample x supersample pixels it covers.
kin::Image supersample_average(const kin::Image& large, kin::Vec2i size, int supersample) {
    kin::Image small;
    small.size = size;
    small.rgba.assign(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 4, 0);
    for (int y = 0; y < size.y; ++y) {
        for (int x = 0; x < size.x; ++x) {
            float sum[3] = {0, 0, 0};
            for (int sy = 0; sy < supersample; ++sy) {
                for (int sx = 0; sx < supersample; ++sx) {
                    const std::size_t i =
                        (static_cast<std::size_t>(y * supersample + sy) * static_cast<std::size_t>(large.size.x) +
                         static_cast<std::size_t>(x * supersample + sx)) *
                        4;
                    for (int ch = 0; ch < 3; ++ch) {
                        sum[ch] += decode_srgb(large.rgba[i + static_cast<std::size_t>(ch)]);
                    }
                }
            }
            const std::size_t o =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x)) * 4;
            for (int ch = 0; ch < 3; ++ch) {
                small.rgba[o + static_cast<std::size_t>(ch)] =
                    encode_srgb(sum[ch] / static_cast<float>(supersample * supersample));
            }
            small.rgba[o + 3] = 255;
        }
    }
    return small;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // a line at a time: what was done shows if it stops
    std::vector<fs::path> scenes;
    fs::path out = "reference_out";
    const char* cache_env = std::getenv("KIN_REFERENCE_CACHE");
    const char* home = std::getenv("HOME");
    fs::path cache = cache_env ? fs::path{cache_env} : fs::path{home ? home : "."} / ".cache" / "kin" / "reference";
    int frames = 60, warmup = 10, repeat = 8, width = 1280, height = 720;
    bool gpu_timing = true;
    int cascades_override = 0; // 0: the scene's own
    double distance_override = 0.0;
    Overrides over;
    int taa_frames = 32, supersample = 1, samples_override = 0; // 0: the scene's
    fs::path bake_dir;                                          // --bake-cache DIR
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--out") {
            out = next();
        } else if (a == "--cache") {
            cache = next();
        } else if (a == "--frames") {
            frames = std::max(1, std::atoi(next().c_str()));
        } else if (a == "--warmup") {
            warmup = std::max(0, std::atoi(next().c_str()));
        } else if (a == "--repeat") {
            repeat = std::max(1, std::atoi(next().c_str()));
        } else if (a == "--cascades") {
            cascades_override = std::max(1, std::atoi(next().c_str()));
        } else if (a == "--shadow-distance") {
            distance_override = std::atof(next().c_str());
        } else if (a == "--ao") {
            over.ao = true;
        } else if (a == "--ao-half") {
            over.ao = over.ao_half = true;
        } else if (a == "--ao-radius") {
            over.ao = true;
            over.ao_radius = std::atof(next().c_str());
        } else if (a == "--ao-intensity") {
            over.ao = true;
            over.ao_intensity = std::atof(next().c_str());
        } else if (a == "--contact-shadows") {
            over.contact = true;
        } else if (a == "--contact-distance") {
            over.contact = true;
            over.contact_distance = std::atof(next().c_str());
        } else if (a == "--contact-thickness") {
            over.contact = true;
            over.contact_thickness = std::atof(next().c_str());
        } else if (a == "--contact-steps") {
            over.contact = true;
            over.contact_steps = std::atoi(next().c_str());
        } else if (a == "--flicker") {
            over.flicker = std::max(2, std::atoi(next().c_str()));
        } else if (a == "--gpu-pass-markers") {
            over.gpu_pass_markers = true;
        } else if (a == "--hitch") {
            over.hitch = next();
        } else if (a == "--warm-up") {
            over.warm_up = true;
        } else if (a == "--vram-cycle") {
            over.vram_cycle = true;
        } else if (a == "--crowd-move") {
            over.crowd_move = true;
        } else if (a == "--shadow-cache") {
            over.shadow_cache = true;
        } else if (a == "--spots") {
            over.spots = std::clamp(std::atoi(next().c_str()), 0, 16);
        } else if (a == "--spot-shadows") {
            over.spot_shadows = std::clamp(std::atoi(next().c_str()), 0, 16);
        } else if (a == "--point-lights") {
            over.point_lights = std::clamp(std::atoi(next().c_str()), 0, 16);
        } else if (a == "--point-shadows") {
            over.point_shadows = std::clamp(std::atoi(next().c_str()), 0, 16);
        } else if (a == "--moon-shadow") {
            over.moon_shadow = true;
        } else if (a == "--moon-cascades") {
            over.moon_shadow = true;
            over.moon_cascades = std::clamp(std::atoi(next().c_str()), 1, 4);
        } else if (a == "--local-res") {
            over.local_res = std::clamp(std::atoi(next().c_str()), 64, 4096);
        } else if (a == "--rain") {
            over.rain = std::atof(next().c_str());
        } else if (a == "--snow") {
            over.snow = std::atof(next().c_str());
        } else if (a == "--wind") {
            over.wind = std::atof(next().c_str());
        } else if (a == "--no-weather") {
            over.no_weather = true;
        } else if (a == "--no-particles") {
            over.no_particles = true;
        } else if (a == "--soft") {
            over.soft = std::atoi(next().c_str()) > 0 ? 1 : 0;
        } else if (a == "--particles") {
            over.particles = std::max(0, std::atoi(next().c_str()));
        } else if (a == "--sky") {
            over.sky = true;
        } else if (a == "--no-sky") {
            over.no_sky = true;
        } else if (a == "--no-fog") {
            over.no_fog = true;
        } else if (a == "--fog") {
            over.fog = std::atof(next().c_str());
        } else if (a == "--sky-light") {
            over.sky_light = true;
        } else if (a == "--sky-light-every") {
            over.sky_light = over.sky_light_every = true;
        } else if (a == "--fog-fixed") {
            over.fog_fixed = true;
        } else if (a == "--time") {
            over.time_hours = std::atof(next().c_str());
        } else if (a == "--taa") {
            over.taa = true;
        } else if (a == "--taa-blend") {
            over.taa = true;
            over.taa_blend = std::atof(next().c_str());
        } else if (a == "--taa-sharpness") {
            over.taa = true;
            over.taa_sharpness = std::atof(next().c_str());
        } else if (a == "--taa-frames") {
            taa_frames = std::max(1, std::atoi(next().c_str()));
        } else if (a == "--lod") {
            over.lod = true;
        } else if (a == "--lod-error") {
            over.lod = true;
            over.lod_error = std::atof(next().c_str());
        } else if (a == "--lod-bias") {
            over.lod = true;
            over.lod_bias = std::atof(next().c_str());
        } else if (a == "--lod-force") {
            over.lod = true;
            over.lod_force = std::atoi(next().c_str());
        } else if (a == "--lod-levels") {
            over.lod_levels = std::clamp(std::atoi(next().c_str()), 1, static_cast<int>(kin::max_mesh_levels));
        } else if (a == "--lod-clusters") {
            over.lod_clusters = true;
        } else if (a == "--lod-gpu") {
            over.lod_gpu = 1;
        } else if (a == "--lod-cpu") {
            over.lod_gpu = 0;
        } else if (a == "--lod-occlusion") {
            over.lod_occlusion = true;
        } else if (a == "--readback") {
            over.readback = true;
        } else if (a == "--lod-room") {
            over.lod_room = std::atol(next().c_str());
        } else if (a == "--samples") {
            samples_override = std::clamp(std::atoi(next().c_str()), 1, 8);
        } else if (a == "--supersample") {
            supersample = std::clamp(std::atoi(next().c_str()), 1, 6);
        } else if (a == "--no-gpu-timing") {
            gpu_timing = false;
        } else if (a == "--bake-cache") {
            bake_dir = next();
        } else if (a == "--width") {
            width = std::max(16, std::atoi(next().c_str()));
        } else if (a == "--height") {
            height = std::max(16, std::atoi(next().c_str()));
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        } else {
            scenes.emplace_back(a);
        }
    }
    if (scenes.empty()) {
        std::fprintf(
            stderr,
            "usage: kin_reference <scene.json>... --out DIR [--cache DIR] [--frames N] [--warmup N] [--repeat N]\n");
        return 2;
    }
    fs::create_directories(out);

    kin::App app{};
    kin::Window& window =
        app.create_window({.title = "kin-reference", .width = width, .height = height, .hidden = true});
    std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
    if (!backend || backend->name() != "SDL_GPU") {
        std::fprintf(stderr, "SDL_GPU is not available\n");
        return 1;
    }
    kin::Renderer2D renderer{std::move(backend)};
    renderer.set_gpu_timing_enabled(gpu_timing);
    kin::JobSystem jobs;
    renderer.set_job_system(&jobs);
    kin::Renderer3D r3d{renderer};
    std::unique_ptr<kin::BakeCache> bake = bake_dir.empty() ? nullptr : std::make_unique<kin::BakeCache>(bake_dir);
    r3d.set_bake_cache(bake.get());
    const kin::Rectf full{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
    std::vector<kin::u8> pixel;
    kin::Vec2i pixel_size{};
    const auto wait_for_gpu = [&] { renderer.read_rgba({0.0f, 0.0f, 1.0f, 1.0f}, pixel, pixel_size); };

    std::ostringstream report;
    report << "{\n \"renderer\": " << quoted(std::string{renderer.backend_name()})
           << ",\n \"vulkan_icd\": " << quoted(std::getenv("VK_ICD_FILENAMES") ? std::getenv("VK_ICD_FILENAMES") : "")
           << ",\n \"size\": [" << width << ", " << height << "],\n \"frames\": " << frames
           << ", \"repeat\": " << repeat << ",\n \"scenes\": [";
    bool first_scene = true;
    int failures = 0;
    for (const fs::path& file : scenes) {
        std::ifstream in(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        kin::JsonParseResult parsed = kin::parse_json(text);
        if (!parsed.ok()) {
            std::fprintf(stderr, "%s: %s\n", file.string().c_str(), parsed.error.c_str());
            ++failures;
            continue;
        }
        const kin::JsonValue& doc = *parsed.value;
        const std::string name = doc.string_at("name", file.stem().string());
        std::printf("%s\n", name.c_str());

        // Its models, loaded and timed.
        const auto load_start = std::chrono::steady_clock::now();
        const double resident_before = resident_mb();
        const auto levels_of = [&](const kin::JsonValue& entry) {
            kin::MeshOptions3D options;
            options.levels = over.lod_levels > 0
                                 ? static_cast<kin::u32>(over.lod_levels)
                                 : static_cast<kin::u32>(std::clamp(entry.number_at("lod_levels", 1.0), 1.0,
                                                                    static_cast<double>(kin::max_mesh_levels)));
            return options;
        };
        const auto model_options_of = [&](const kin::JsonValue& entry) {
            kin::MeshOptions3D options = levels_of(entry);
            options.clusters = over.lod_clusters || entry.bool_at("clusters");
            return options;
        };
        std::vector<Placed> placed;
        bool missing = false;
        if (const kin::JsonValue* models = doc.find("models")) {
            for (const kin::JsonValue& m : models->items()) {
                const fs::path path = cache / m.string_at("file");
                if (!fs::exists(path)) {
                    std::printf("  %s: not in the cache (tools/reference/fetch.py)\n", path.string().c_str());
                    missing = true;
                    break;
                }
                std::string error;
                kin::Model3D model = r3d.load_model(path, model_options_of(m)).value_or({});
                error = kin::diagnostics3d_text(r3d.diagnostics());
                if (!model) {
                    std::fprintf(stderr, "  %s: %s\n", path.string().c_str(), error.c_str());
                    missing = true;
                    break;
                }
                const float scale = static_cast<float>(m.number_at("scale", 1.0));
                placed.push_back(
                    {std::move(model), Mat4f::translation(vec3(m.find("position"))) *
                                           Mat4f::rotation_y(static_cast<float>(m.number_at("rotation_y", 0.0))) *
                                           Mat4f::scaling({scale, scale, scale})});
                placed.back().position = vec3(m.find("position"));
                placed.back().rotation_y = static_cast<float>(m.number_at("rotation_y", 0.0));
                placed.back().uniform_scale = scale;
                // `grid: {count, columns, spacing: [x, z], time_step}`: the model placed `count` times in rows, each
                // copy's animation `time_step` seconds on from the one before (the meshes are shared).
                const kin::JsonValue* grid = m.find("grid");
                const std::size_t base_index = placed.size() - 1;
                if (const kin::JsonValue* a = m.find("animation")) {
                    Placed& back = placed.back();
                    if (back.model.skeletons.empty() || back.model.clips.empty()) {
                        std::fprintf(stderr, "  %s: has an animation entry but no skeleton or clips\n",
                                     path.string().c_str());
                    } else {
                        back.animated = true;
                        const std::string wanted = a->string_at("clip");
                        if (!wanted.empty()) {
                            const kin::AnimClip3D* found = back.model.find_clip(wanted);
                            if (!found) {
                                std::string names;
                                for (const kin::AnimClip3D& c : back.model.clips)
                                    names += " " + c.name;
                                std::fprintf(stderr, "  %s: no clip called %s (it has:%s)\n", path.string().c_str(),
                                             wanted.c_str(), names.c_str());
                                return 1;
                            }
                            back.clip = static_cast<std::size_t>(found - back.model.clips.data());
                        } else {
                            back.clip = std::min<std::size_t>(static_cast<std::size_t>(a->number_at("index", 0.0)),
                                                              back.model.clips.size() - 1);
                        }
                        back.fixed_time = a->find("time") != nullptr;
                        back.time = a->number_at("time", 0.0);
                        back.loop = a->find("loop") ? a->bool_at("loop") : true;
                        back.speed = a->number_at("speed", 1.0);
                        back.remove_root_motion = a->string_at("root_motion") == "remove";
                        back.ecs = a->bool_at("ecs");
                        if (const kin::JsonValue* ik = a->find("ik")) {
                            back.animator.ik_constraints = parse_ik(*ik);
                        }
                        if (const kin::JsonValue* blend = a->find("blend")) {
                            if (const kin::JsonValue* points = blend->find("points")) {
                                for (const kin::JsonValue& point : points->items()) {
                                    const kin::AnimClip3D* found = back.model.find_clip(point.string_at("clip"));
                                    if (!found) {
                                        std::fprintf(stderr, "  %s: blend point: no clip called %s\n",
                                                     path.string().c_str(), point.string_at("clip").c_str());
                                        return 1;
                                    }
                                    back.blend.push_back({static_cast<std::size_t>(found - back.model.clips.data()),
                                                          static_cast<float>(point.number_at("position", 0.0))});
                                }
                            }
                            back.parameter = blend->number_at("parameter", 0.0);
                            back.ramp = blend->find("parameter_to") != nullptr;
                            back.parameter_to = blend->number_at("parameter_to", back.parameter);
                            back.parameter_seconds = blend->number_at("parameter_seconds", 2.0);
                        }
                        const kin::AnimClip3D& clip = back.model.clips[back.clip];
                        std::printf("  animation: %s, %.2f s, %zu clips, %u joints\n", clip.name.c_str(),
                                    static_cast<double>(clip.duration), back.model.clips.size(),
                                    back.model.skeletons[0].joint_count());
                    }
                }
                if (grid) {
                    const int count = std::max(1, static_cast<int>(grid->number_at("count", 1.0)));
                    const int columns = std::max(1, static_cast<int>(grid->number_at("columns", 10.0)));
                    const float sx = static_cast<float>(
                        grid->find("spacing") ? grid->find("spacing")->items()[0].as_number(2.0) : 2.0);
                    const float sz = static_cast<float>(
                        grid->find("spacing") ? grid->find("spacing")->items()[1].as_number(2.0) : 2.0);
                    const double time_step = grid->number_at("time_step", 0.0);
                    const Placed first = placed[base_index];
                    placed.pop_back();
                    for (int i = 0; i < count; ++i) {
                        Placed copy = first;
                        const float x = (static_cast<float>(i % columns) - static_cast<float>(columns - 1) * 0.5f) * sx;
                        const float z =
                            (static_cast<float>(i / columns) - static_cast<float>((count - 1) / columns) * 0.5f) * sz;
                        copy.transform = Mat4f::translation({x, 0.0f, z}) * first.transform;
                        copy.position = first.position + Vec3f{x, 0.0f, z};
                        copy.time += time_step * i;
                        placed.push_back(std::move(copy));
                    }
                    std::printf("  grid: %d copies\n", count);
                }
            }
        }
        if (missing) {
            if (!doc.bool_at("optional")) {
                ++failures;
            }
            continue;
        }
        // `animation: {ecs: true}` models: an entity each, in a world of its own. The picture (frame 0) is 250 steps
        // (4.17 s) in.
        constexpr int ecs_lead = 250;
        kin::EcsWorld ecs_world;
        kin::WorldRenderState3D ecs_state{ecs_world};
        bool any_ecs = false;
        for (Placed& p : placed) {
            if (!p.ecs || !p.animated) {
                continue;
            }
            any_ecs = true;
            p.shared = std::make_shared<const kin::Model3D>(p.model);
            kin::Animator3D& a = p.animator;
            if (!p.blend.empty()) {
                auto machine = std::make_shared<kin::AnimationStateMachine>();
                machine->initial = "move";
                machine->states = {{"move", "move"}};
                a.machine = machine;
                kin::Animator3D::BlendSpace space;
                space.parameter = "speed";
                for (const Placed::BlendPoint& point : p.blend) {
                    space.entries.push_back({p.model.clips[point.clip].name, point.position});
                }
                a.blend_spaces["move"] = space;
                a.params.set_float("speed", static_cast<float>(p.parameter));
            } else {
                a.clip = p.model.clips[p.clip].name;
            }
            a.speed = static_cast<float>(p.speed);
            a.loop = p.loop;
            a.remove_root_motion = p.remove_root_motion;
            p.entity = ecs_world.raw()
                           .entity()
                           .set(kin::Transform3D{.position = p.position,
                                                 .rotation = kin::Quatf::axis_angle({0.0f, 1.0f, 0.0f}, p.rotation_y),
                                                 .scale = {p.uniform_scale, p.uniform_scale, p.uniform_scale}})
                           .set(kin::SkinnedMeshRenderer3D{.model = p.shared})
                           .set(static_cast<const kin::Animator3D&>(a)); // const: flecs moves from a non-const lvalue
        }
        Crowd crowd;
        const bool is_crowd = doc.string_at("procedural") == "crowd";
        if (is_crowd) {
            crowd.make(r3d, static_cast<int>(doc.number_at("count", 20000)), levels_of(doc));
        }
        Motion motion;
        const bool is_motion = doc.string_at("procedural") == "motion";
        if (is_motion) {
            motion.make(r3d, levels_of(doc));
        }
        Landscape landscape;
        const bool is_landscape = doc.string_at("procedural") == "landscape";
        if (is_landscape) {
            landscape.make(r3d, doc);
        }
        Stage stage;
        const bool is_stage = doc.string_at("procedural") == "stage";
        if (is_stage) {
            stage.make(r3d, renderer, doc);
        }
        SceneParticles scene_particles;
        Hidden hidden;
        const bool is_hidden = doc.string_at("procedural") == "hidden";
        if (is_hidden) {
            hidden.make(r3d, model_options_of(doc), doc);
        }
        PhysicsPile pile;
        const bool is_pile = doc.string_at("procedural") == "physics_pile";
        if (is_pile) {
            pile.make(r3d, doc);
        }
        PhysicsCharacter walker;
        int walker_frame = 0; // the frame the current camera shows (its `frame`, else the scene's lead)
        const bool is_walker = doc.string_at("procedural") == "physics_character";
        if (is_walker) {
            walker.make(r3d, doc, cache);
            if (!walker.loaded) {
                ++failures;
                continue;
            }
        }
        PhysicsClimb climber;
        int climber_frame = 0;
        const bool is_climber = doc.string_at("procedural") == "physics_climb";
        if (is_climber) {
            climber.make(r3d, doc, cache);
            if (!climber.loaded) {
                ++failures;
                continue;
            }
        }
        PhysicsRagdoll ragdolls;
        PhysicsRope ropes;
        int physics_frame = 0; // the frame the current camera shows, for the ragdoll and rope scenes
        const bool is_ragdoll = doc.string_at("procedural") == "physics_ragdoll";
        const bool is_ropes = doc.string_at("procedural") == "physics_rope";
        if (is_ragdoll) {
            ragdolls.make(r3d, doc, cache);
            if (!ragdolls.loaded) {
                ++failures;
                continue;
            }
        }
        if (is_ropes) {
            ropes.make(r3d, doc, cache);
            if (!ropes.loaded) {
                ++failures;
                continue;
            }
        }
        const bool animated = doc.bool_at("animated") || (over.crowd_move && doc.string_at("procedural") == "crowd");
        std::array<std::vector<kin::MeshInstance3D>, 3> crowd_moved;
        const double load_ms = ms_since(load_start);
        if (bake) {
            const kin::BakeCacheStats b = bake->stats();
            std::printf("  loaded in %.0f ms with the bake cache: %llu hits, %llu misses, %llu writes so far (%.1f MB "
                        "read, %.1f MB written)\n",
                        load_ms, static_cast<unsigned long long>(b.hits), static_cast<unsigned long long>(b.misses),
                        static_cast<unsigned long long>(b.writes), static_cast<double>(b.bytes_read) / 1048576.0,
                        static_cast<double>(b.bytes_written) / 1048576.0);
        }
        {
            std::size_t meshes = 0, with_levels = 0, level_triangles = 0, base_triangles = 0;
            for (const Placed& p : placed) {
                for (const kin::ModelPart3D& part : p.model.parts) {
                    ++meshes;
                    with_levels += part.mesh.level_count() > 1 ? 1 : 0;
                    base_triangles += part.mesh.level_triangles(0);
                    for (kin::u32 l = 0; l < part.mesh.level_count(); ++l) {
                        level_triangles += part.mesh.level_triangles(l);
                    }
                }
            }
            std::size_t with_clusters = 0, clusters = 0, leaf_triangles = 0;
            std::uint64_t cluster_triangles = 0;
            kin::u32 deepest = 0;
            for (const Placed& p : placed) {
                for (const kin::ModelPart3D& part : p.model.parts) {
                    with_clusters += part.mesh.has_clusters() ? 1 : 0;
                    clusters += part.mesh.cluster_count();
                    leaf_triangles += part.mesh.has_clusters() ? part.mesh.leaf_triangles() : 0;
                    cluster_triangles += part.mesh.cluster_triangles();
                    deepest = std::max(deepest, part.mesh.cluster_levels());
                }
            }
            if (with_clusters > 0) {
                std::printf("  loaded in %.0f ms with clusters: %zu meshes, %zu with clusters (%zu clusters, up to %u "
                            "levels deep, %zu leaf triangles, %llu in all), +%.0f MB resident, peak %.0f MB\n",
                            load_ms, meshes, with_clusters, clusters, deepest, leaf_triangles,
                            static_cast<unsigned long long>(cluster_triangles), resident_mb() - resident_before,
                            peak_mb());
                if (r3d.capabilities().gpu_driven_clusters) {
                    // The video memory the hierarchy takes for the cut in compute: the cluster table (80 bytes a
                    // cluster) and every cluster's indices.
                    std::printf("  GPU hierarchy: %.1f MB (%.1f MB of indices for all levels, %.1f MB table), against "
                                "%.1f MB of leaf indices before G3c\n",
                                (static_cast<double>(clusters) * 80.0 + static_cast<double>(cluster_triangles) * 12.0) /
                                    1048576.0,
                                static_cast<double>(cluster_triangles) * 12.0 / 1048576.0,
                                static_cast<double>(clusters) * 80.0 / 1048576.0,
                                static_cast<double>(leaf_triangles) * 12.0 / 1048576.0);
                }
            }
            if (with_levels > 0 || over.lod_levels > 1) {
                std::printf("  loaded in %.0f ms with levels of detail: %zu meshes, %zu with levels, %zu triangles "
                            "(%zu with all their levels), +%.0f MB resident\n",
                            load_ms, meshes, with_levels, base_triangles, level_triangles,
                            resident_mb() - resident_before);
            }
        }
        const kin::Bounds3D bounds =
            is_crowd       ? kin::Bounds3D{{0.0f, -1.0f, 0.0f}, {48.0f, 3.0f, 32.0f}}
            : is_motion    ? kin::Bounds3D{{-8.0f, -0.2f, -6.0f}, {8.0f, 4.0f, 6.0f}}
            : is_landscape ? kin::Bounds3D{{-3000.0f, -60.0f, -6000.0f}, {3000.0f, 1000.0f, 120.0f}}
            : is_stage     ? kin::Bounds3D{{-12.0f, -0.2f, -14.0f}, {12.0f, 7.0f, 8.0f}}
            : is_hidden
                ? kin::Bounds3D{{-hidden.extent.x, -0.2f, -8.0f}, {hidden.extent.x, hidden.extent.y, hidden.extent.z}}
            : is_pile    ? kin::Bounds3D{{-3.3f, -0.5f, -3.3f}, {3.3f, 4.0f, 3.3f}}
            : is_walker  ? kin::Bounds3D{{-2.0f, -0.5f, -30.0f}, {70.0f, 5.0f, 30.0f}}
            : is_climber ? kin::Bounds3D{{-4.0f, -0.5f, -2.0f}, {64.0f, 7.0f, 6.0f}}
            : is_ragdoll ? kin::Bounds3D{{-14.0f, -0.5f, -8.0f}, {14.0f, 5.0f, 8.0f}}
            : is_ropes   ? kin::Bounds3D{{-6.0f, -0.5f, -4.0f}, {18.0f, 10.0f, 4.0f}}
                         : bounds_of(placed);

        scene_particles.make(doc, over, bounds);

        // The view.
        const kin::JsonValue* v = doc.find("view");
        const kin::JsonValue none{};
        const kin::JsonValue& view_doc = v ? *v : none;
        const auto make_view = [&](kin::Vec2i size, kin::u32 samples) {
            return r3d
                .create_view(size, {.samples = samples,
                                    .background = colour(view_doc.find("background"), kin::Color::rgb(0, 0, 0)),
                                    .hdr = view_doc.bool_at("hdr"),
                                    .exposure = static_cast<float>(view_doc.number_at("exposure", 1.0)),
                                    .bloom = static_cast<float>(view_doc.number_at("bloom", 0.0))})
                .value_or({});
        };
        kin::View3D view =
            make_view({width, height}, static_cast<kin::u32>(samples_override > 0 ? samples_override
                                                                                  : view_doc.number_at("samples", 4)));

        // The lights, the same every frame; an environment (an .hdr in the cache) loaded once.
        kin::Environment3D environment;
        if (const kin::JsonValue* l = doc.find("lights")) {
            if (const kin::JsonValue* env = l->find("environment")) {
                std::string error;
                environment = r3d.load_environment(cache / env->string_at("file")).value_or({});
                error = kin::diagnostics3d_text(r3d.diagnostics());
                if (!environment) {
                    std::fprintf(stderr, "  environment: %s\n", error.c_str());
                    ++failures;
                    continue;
                }
            }
        }

        kin::Scene3D scene;
        // Lights carried with the camera (a flashlight): which light, its offset in the camera's frame (right, up,
        // forward), and a turn from the camera's axis; applied once the frame's camera is set.
        struct Attached {
            bool spot;
            std::size_t index;
            Vec3f offset;
            float yaw, pitch;
        };
        std::vector<Attached> attached;
        double camera_hours = -1.0; // the current camera's own time of day (< 0: the scene's)
        const auto fill_scene = [&](int time) {
            scene.clear();
            const kin::JsonValue* l = doc.find("lights");
            const kin::JsonValue& lights = l ? *l : none;
            if (const kin::JsonValue* sun = lights.find("sun")) {
                scene.lights.sun = {.direction = vec3(sun->find("direction"), {0, 1, 0}),
                                    .color = light_color(sun->find("color"))};
            } else {
                scene.lights.sun.color = {};
            }
            if (const kin::JsonValue* fill = lights.find("fill")) {
                scene.lights.fill = {.direction = vec3(fill->find("direction"), {0, 1, 0}),
                                     .color = light_color(fill->find("color"))};
            }
            scene.lights.ambient = light_color(lights.find("ambient"));
            scene.lights.environment = environment;
            if (const kin::JsonValue* env = lights.find("environment")) {
                scene.lights.environment_intensity = static_cast<float>(env->number_at("intensity", 1.0));
                scene.lights.environment_rotation = static_cast<float>(env->number_at("rotation", 0.0));
            }
            if (const kin::JsonValue* ground = lights.find("ground_ambient")) {
                scene.lights.ground_ambient = {true, light_color(ground)};
            }
            if (const kin::JsonValue* area = lights.find("shadow_area")) {
                if (area->is_string() && area->as_string() == "auto") {
                    scene.lights.shadow_area = bounds;
                } else if (area->items().size() == 2) {
                    scene.lights.shadow_area = {vec3(&area->items()[0]), vec3(&area->items()[1])};
                }
                scene.lights.shadow_resolution = static_cast<kin::u32>(lights.number_at("shadow_resolution", 2048));
                scene.lights.shadow_cascades = static_cast<kin::u32>(lights.number_at("shadow_cascades", 1));
                scene.lights.shadow_distance =
                    static_cast<float>(lights.number_at("shadow_distance", scene.lights.shadow_distance));
                if (cascades_override > 0) {
                    scene.lights.shadow_cascades = static_cast<kin::u32>(cascades_override);
                }
                if (distance_override > 0.0) {
                    scene.lights.shadow_distance = static_cast<float>(distance_override);
                }
            }
            scene.lights.cache_shadows = over.shadow_cache;
            merge_ambient_occlusion(scene.ambient_occlusion, doc.find("ambient_occlusion"), over);
            merge_contact_shadows(scene.contact_shadows, doc.find("contact_shadows"), over);
            scene.taa = {};
            merge_taa(scene.taa, doc.find("taa"), over);
            scene.lod = {};
            merge_lod(scene.lod, scene.debug, doc.find("lod"), over);
            scene.debug.gpu_pass_markers = over.gpu_pass_markers;
            attached.clear();
            if (const kin::JsonValue* points = lights.find("points")) {
                for (const kin::JsonValue& p : points->items()) {
                    scene.add_point_light({.position = vec3(p.find("position")),
                                           .color = light_color(p.find("color")),
                                           .radius = static_cast<float>(p.number_at("radius", 1.0)),
                                           .size = static_cast<float>(p.number_at("size", 0.01)),
                                           .id = static_cast<kin::u32>(scene.lights.points.size() + 1)});
                    configure_shadow(scene.lights.points.back(), p, over.local_res);
                    if (const kin::JsonValue* a = p.find("attach")) {
                        attached.push_back(
                            {false, scene.lights.points.size() - 1, vec3(a->find("offset")), 0.0f, 0.0f});
                    }
                }
            }
            // Spot lights (P3b): `position` and `direction` (or `target`), `attach: {offset: [right, up, forward], yaw,
            // pitch}` carries one with the camera (a flashlight: the offsets are in the camera's frame, `yaw` and
            // `pitch` turn it from the camera's axis).
            if (const kin::JsonValue* spots = lights.find("spots")) {
                for (const kin::JsonValue& sp : spots->items()) {
                    kin::SpotLightSource3D l;
                    l.position = vec3(sp.find("position"));
                    l.direction = sp.find("target") ? vec3(sp.find("target")) - l.position
                                                    : vec3(sp.find("direction"), {0.0f, -1.0f, 0.0f});
                    l.color = light_color(sp.find("color"), {1.0f, 1.0f, 1.0f});
                    l.radius = static_cast<float>(sp.number_at("radius", 10.0));
                    l.size = static_cast<float>(sp.number_at("size", 0.01));
                    l.inner_angle = static_cast<float>(sp.number_at("inner", 20.0));
                    l.outer_angle = static_cast<float>(sp.number_at("outer", 30.0));
                    configure_shadow(l, sp, over.local_res);
                    l.id = static_cast<kin::u32>(scene.lights.spots.size() + 1);
                    scene.add_spot_light(l);
                    if (const kin::JsonValue* a = sp.find("attach")) {
                        attached.push_back({true, scene.lights.spots.size() - 1, vec3(a->find("offset")),
                                            static_cast<float>(a->number_at("yaw", 0.0)),
                                            static_cast<float>(a->number_at("pitch", 0.0))});
                    }
                }
            }
            scene.lights.max_shadowed_spots =
                static_cast<kin::u32>(std::max<double>(lights.number_at("max_shadowed_spots", 4), over.spot_shadows));
            scene.lights.max_shadowed_points =
                static_cast<kin::u32>(std::max<double>(lights.number_at("max_shadowed_points", 2), over.point_shadows));
            scene.lights.local_shadow_resolution = static_cast<kin::u32>(
                over.local_res > 0 ? over.local_res : lights.number_at("local_shadow_resolution", 1024));
            scene.lights.sun_shadow_strength = static_cast<float>(lights.number_at("sun_shadow_strength", 1.0));
            if (const kin::JsonValue* fs = lights.find("fill_shadow")) {
                scene.lights.fill_shadow.enabled = fs->bool_at("enabled", true);
                scene.lights.fill_shadow.strength = static_cast<float>(fs->number_at("strength", 1.0));
                scene.lights.fill_shadow.cascades = static_cast<kin::u32>(fs->number_at("cascades", 2));
                scene.lights.fill_shadow.resolution = static_cast<kin::u32>(fs->number_at("resolution", 0));
                scene.lights.fill_shadow.distance = static_cast<float>(fs->number_at("distance", 0));
            } else {
                scene.lights.fill_shadow = {};
            }
            if (over.moon_shadow) {
                scene.lights.fill_shadow.enabled = true;
                scene.lights.fill_shadow.cascades = static_cast<kin::u32>(std::max(1, over.moon_cascades));
                if (scene.lights.fill.color.r + scene.lights.fill.color.g + scene.lights.fill.color.b <= 0.0f) {
                    scene.lights.fill = {.direction = {0.3f, 0.8f, 0.5f}, .color = {0.25f, 0.3f, 0.45f}};
                }
            }
            // The command line's lights (for the cost tables): in front of the camera, set once it is known (see
            // `attach`).
            for (int i = 0; i < over.spots; ++i) {
                kin::SpotLightSource3D l;
                l.color = {2.0f, 1.9f, 1.7f};
                l.radius = std::clamp(static_cast<float>(bounds.radius()) * 0.6f, 8.0f, 60.0f);
                l.inner_angle = 18.0f;
                l.outer_angle = 32.0f;
                l.shadow_enabled = i < over.spot_shadows;
                l.id = static_cast<kin::u32>(1000 + i);
                scene.add_spot_light(l);
                const float across = static_cast<float>(i) - 0.5f * static_cast<float>(over.spots - 1);
                attached.push_back(
                    {true, scene.lights.spots.size() - 1, {across * 2.5f, 0.6f, 0.5f}, across * 7.0f, -4.0f});
            }
            for (int i = 0; i < over.point_lights; ++i) {
                kin::PointLightSource3D l;
                l.color = {3.0f, 2.8f, 2.4f};
                l.radius = std::clamp(static_cast<float>(bounds.radius()) * 0.5f, 8.0f, 40.0f);
                l.shadow_enabled = i < over.point_shadows;
                l.id = static_cast<kin::u32>(2000 + i);
                scene.add_point_light(l);
                attached.push_back({false,
                                    scene.lights.points.size() - 1,
                                    {(static_cast<float>(i) - 0.5f * static_cast<float>(over.point_lights - 1)) * 6.0f,
                                     1.5f, -(l.radius * 0.4f + 2.0f)},
                                    0.0f,
                                    0.0f});
            }
            if (any_ecs) {
                // The entities start again and take the steps the frame's index stands for (60 a second, 240 before the
                // picture, which is the frame 0, `ecs_lead` steps in): every frame is the same whatever order the run
                // asks for them in.
                const int steps = std::max(0, time + ecs_lead);
                for (Placed& p : placed) {
                    if (p.entity) {
                        const kin::Animator3D& start = p.animator; // const: flecs's set() moves from a non-const lvalue
                        p.entity.set(start);
                    }
                }
                for (int i = 0; i < steps; ++i) {
                    for (Placed& p : placed) {
                        if (p.entity && p.ramp) {
                            const double t =
                                std::min(1.0, static_cast<double>(i) / 60.0 / std::max(p.parameter_seconds, 1e-6));
                            p.entity.get_mut<kin::Animator3D>()->params.set_float(
                                "speed", static_cast<float>(p.parameter + (p.parameter_to - p.parameter) * t));
                        }
                    }
                    ecs_state.animate(1.0f / 60.0f);
                }
                ecs_state.propagate_transforms();
                ecs_state.collect(scene);
            }
            for (const Placed& p : placed) {
                if (p.ecs && p.animated) {
                    continue; // an entity: collected above
                }
                if (p.animated) {
                    const kin::Skeleton3D& skeleton = p.model.skeletons[0];
                    const double seconds = p.fixed_time ? p.time : static_cast<double>(time) / 60.0 * p.speed + p.time;
                    kin::Pose3D pose;
                    if (!p.blend.empty()) {
                        // A 1D blend: a player at the parameter's value for the frame, run for the seconds (its phase
                        // wraps).
                        std::vector<kin::BlendPoint3D> points;
                        for (const Placed::BlendPoint& point : p.blend) {
                            points.push_back({&p.model.clips[point.clip], point.position});
                        }
                        const double ramp = p.ramp ? std::clamp((static_cast<double>(time) + ecs_lead) / 60.0 /
                                                                    std::max(p.parameter_seconds, 1e-6),
                                                                0.0, 1.0)
                                                   : 0.0;
                        kin::AnimationPlayer3D player{&skeleton};
                        player.play_blend(
                            points, static_cast<float>(p.parameter + (p.parameter_to - p.parameter) * ramp), p.loop);
                        player.update(static_cast<float>(seconds));
                        pose = player.pose();
                    } else {
                        kin::sample_clip(p.model.clips[p.clip], skeleton, static_cast<float>(seconds), p.loop, pose);
                    }
                    if (p.remove_root_motion) {
                        kin::remove_root_motion(pose, skeleton, {.x = true, .y = false, .z = true, .rotation = false});
                    }
                    std::vector<Mat4f> matrices;
                    kin::compute_joint_matrices(skeleton, pose, matrices);
                    (void)scene.draw_skinned(p.model, matrices, p.transform);
                } else {
                    (void)scene.draw(p.model, p.transform);
                }
            }
            if (is_crowd) {
                if (over.crowd_move) {
                    crowd.draw_moving(scene, time, crowd_moved);
                } else {
                    (void)crowd.draw(scene);
                }
            }
            if (is_motion) {
                (void)motion.draw(scene, time);
            }
            if (is_pile) {
                (void)pile.draw(r3d, scene, time);
            }
            if (is_walker) {
                (void)walker.draw(r3d, scene, walker_frame, time);
            }
            if (is_climber) {
                (void)climber.draw(r3d, scene, climber_frame, time);
            }
            if (is_ragdoll) {
                (void)ragdolls.draw(r3d, scene, physics_frame, time);
            }
            if (is_ropes) {
                (void)ropes.draw(r3d, scene, physics_frame, time);
            }
            if (is_hidden) {
                (void)hidden.draw(scene);
            }
            if (is_landscape) {
                (void)landscape.draw(scene);
            }
            if (is_stage) {
                (void)stage.draw(scene);
            }
            // The sky, fog and time of day last, so that a time of day's sun and ambient replace the scene's own
            // lights.
            scene.sky = {};
            merge_sky(scene.sky, doc.find("sky"), over);
            scene.fog = {};
            merge_fog(scene.fog, doc.find("fog"), over);
            scene.lights.sky_lights_scene = lights.bool_at("sky_lights_scene") || over.sky_light;
            kin::TimeOfDay3D day;
            if (merge_time_of_day(day, doc.find("time_of_day"), camera_hours >= 0.0 ? camera_hours : over.time_hours)) {
                kin::apply(day, scene.lights, scene.sky, scene.fog);
            }
            // Particles and weather (P3c): the weather's clock is the frame's (0 at the picture, 60 frames a second).
            (void)scene_particles.draw(scene, static_cast<double>(time) / 60.0);
        };

        fs::create_directories(out / name);
        if (!first_scene) {
            report << ",";
        }
        first_scene = false;
        report << "\n  {\"name\": " << quoted(name) << ", \"load_ms\": " << load_ms << ", \"peak_mb\": " << peak_mb()
               << ", \"bounds\": [[" << bounds.min.x << ", " << bounds.min.y << ", " << bounds.min.z << "], ["
               << bounds.max.x << ", " << bounds.max.y << ", " << bounds.max.z << "]], \"cameras\": [";
        bool first_camera = true;
        const kin::JsonValue* cameras = doc.find("cameras");
        for (const kin::JsonValue& c : cameras ? cameras->items() : kin::JsonValue::Array{}) {
            const std::string camera_name = c.string_at("name", "camera");
            camera_hours = c.number_at("hours", -1.0);
            const float fov = static_cast<float>(c.number_at("fov", 50.0));
            Vec3f eye = vec3(c.find("eye")), target = vec3(c.find("target"));
            if (const kin::JsonValue* fit = c.find("fit")) {
                // Framed round the scene's bounds, from yaw (round +Y, from +Z) and pitch.
                target = bounds.center();
                const float radius = std::max(0.01f, bounds.radius());
                const float distance =
                    radius / std::sin(fov * 3.14159265f / 360.0f) * static_cast<float>(fit->number_at("margin", 1.05));
                const float yaw = static_cast<float>(fit->number_at("yaw", 0.0)) * 3.14159265f / 180.0f;
                const float pitch = static_cast<float>(fit->number_at("pitch", 30.0)) * 3.14159265f / 180.0f;
                eye =
                    target +
                    Vec3f{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)} * distance;
            }
            // `at_sun` / `at_moon`: look toward the sun (or moon) of the camera's time of day, plus `target_offset`.
            if (c.bool_at("at_sun") || c.bool_at("at_moon")) {
                kin::TimeOfDay3D day;
                merge_time_of_day(day, doc.find("time_of_day"), camera_hours >= 0.0 ? camera_hours : over.time_hours);
                const kin::SkyState3D state = kin::evaluate(day);
                target = eye + (c.bool_at("at_sun") ? state.sun_direction : state.moon_direction) * 100.0f +
                         vec3(c.find("target_offset"));
            }
            // The scene at `time` (frames; 0 is the picture, earlier is negative), the camera
            // having panned `pan` units a frame to reach `eye` at 0.
            const Vec3f pan = vec3(c.find("pan"));
            if (is_walker) {
                walker_frame = static_cast<int>(c.number_at("frame", walker.lead));
            }
            if (is_climber) {
                climber_frame = static_cast<int>(c.number_at("frame", climber.lead));
            }
            if (is_ragdoll || is_ropes) {
                physics_frame = static_cast<int>(c.number_at("frame", is_ragdoll ? ragdolls.lead : ropes.lead));
            }
            const auto attach = [&] {
                const kin::Camera3D& cam = scene.camera;
                for (const Attached& a : attached) {
                    const Vec3f position =
                        cam.position + cam.right() * a.offset.x + cam.up() * a.offset.y + cam.forward() * a.offset.z;
                    if (a.spot) {
                        kin::SpotLightSource3D& l = scene.lights.spots[a.index];
                        l.position = position;
                        const kin::Quatf turn = kin::Quatf::axis_angle({0.0f, 1.0f, 0.0f}, a.yaw) *
                                                kin::Quatf::axis_angle({1.0f, 0.0f, 0.0f}, a.pitch);
                        l.direction = cam.rotation.rotate(turn.rotate({0.0f, 0.0f, -1.0f}));
                    } else {
                        scene.lights.points[a.index].position = position;
                    }
                }
            };
            const auto pose = [&](int time) {
                fill_scene(time);
                const Vec3f moved = pan * static_cast<float>(time);
                if (is_walker || is_climber) {
                    // `follow: {eye, target}`: offsets from the character's feet (the camera goes where it goes).
                    if (const kin::JsonValue* follow = c.find("follow")) {
                        const Vec3f feet = is_walker ? walker.feet() : climber.feet();
                        scene.camera = kin::Camera3D::look_at(feet + vec3(follow->find("eye")),
                                                              feet + vec3(follow->find("target")), fov);
                        scene.camera.z_near = static_cast<float>(c.number_at("near", 0.05));
                        attach();
                        return;
                    }
                }
                scene.camera = kin::Camera3D::look_at(eye + moved, target + moved, fov);
                scene.camera.z_near = static_cast<float>(c.number_at("near", 0.05));
                attach();
            };
            if (!over.hitch.empty()) {
                // The first renders after a feature goes on at runtime: pipelines and targets made on first use. Steady
                // state first (the scene as it is, 40 renders), then the feature on, each of the next renders timed
                // alone: wall time of render + draw + present on the CPU, render()'s own, and the time until the GPU
                // has finished it. Then 40 more, the steady cost with it on.
                const std::string& feature = over.hitch;
                const auto apply = [&](bool on) {
                    if (feature == "ao") {
                        scene.ambient_occlusion.enabled = on;
                    } else if (feature == "contact") {
                        scene.contact_shadows.enabled = on;
                    } else if (feature == "taa") {
                        scene.taa.enabled = on;
                    } else if (feature == "taa+ao") {
                        scene.taa.enabled = on;
                        scene.ambient_occlusion.enabled = on;
                    } else if (feature == "cascades") {
                        scene.lights.shadow_cascades = on ? 4u : 1u;
                    } else if (feature == "lod") {
                        scene.lod.enabled = on;
                    } else if (feature == "lod-cpu") {
                        scene.lod.enabled = on;
                        scene.debug.gpu_culling = false;
                    } else if (feature == "occlusion") {
                        scene.lod.enabled = on;
                        scene.lod.occlusion.enabled = on;
                    } else if (feature == "all") {
                        scene.ambient_occlusion.enabled = on;
                        scene.contact_shadows.enabled = on;
                        scene.taa.enabled = on;
                        scene.lights.shadow_cascades = on ? 4u : 1u;
                    }
                };
                kin::u32 made = 0;
                const auto timed = [&](int index, bool on) {
                    pose(index);
                    apply(on);
                    renderer.clear(kin::Color::rgb(0, 0, 0));
                    const auto start = std::chrono::steady_clock::now();
                    if (auto result = r3d.render(view, scene); !result)
                        throw std::runtime_error(kin::diagnostics3d_text(result.diagnostics));
                    renderer.draw_texture(view.texture(), full);
                    renderer.present();
                    const double cpu_wall = ms_since(start);
                    wait_for_gpu();
                    made += r3d.debug_stats().pipelines_made;
                    return std::array<double, 3>{cpu_wall, r3d.debug_stats().cpu_ms, ms_since(start)};
                };
                if (over.warm_up) {
                    pose(0);
                    apply(true);
                    if (auto result = r3d.warm_up(view, scene); !result)
                        throw std::runtime_error(kin::diagnostics3d_text(result.diagnostics));
                    pose(0);
                    apply(false);
                }
                for (int f = 0; f < 40; ++f) {
                    timed(f, false);
                }
                made = 0;
                std::printf("  hitch %s %s: first renders with it on (ms: CPU wall of render+present, render(), until "
                            "the GPU is done):",
                            name.c_str(), feature.c_str());
                for (int f = 0; f < 3; ++f) {
                    const auto t = timed(40 + f, true);
                    std::printf("  [%.2f %.2f %.2f]", t[0], t[1], t[2]);
                }
                std::printf("   (%u pipelines made by them)", made);
                std::vector<double> wall, own, done;
                for (int f = 0; f < 40; ++f) {
                    const auto t = timed(43 + f, true);
                    wall.push_back(t[0]);
                    own.push_back(t[1]);
                    done.push_back(t[2]);
                }
                std::printf("   steady with it on: median [%.2f %.2f %.2f]\n", summarize(wall).median,
                            summarize(own).median, summarize(done).median);
                std::exit(0);
            }
            // A temporal history starts afresh at each camera, and runs long enough to settle.
            view.reset_history();
            pose(0); // for what the scene asks of temporal anti-aliasing
            const int run = warmup + frames + (scene.taa.enabled ? std::max(0, taa_frames - warmup - frames) : 0);
            const int each = animated ? 1 : repeat;

            // The first picture: pipelines and targets made, the GPU waited for.
            const auto first_start = std::chrono::steady_clock::now();
            pose(-run - 1);
            renderer.clear(kin::Color::rgb(0, 0, 0));
            if (auto rendered = r3d.render(view, scene); !rendered)
                throw std::runtime_error(kin::diagnostics3d_text(rendered.diagnostics));
            renderer.draw_texture(view.texture(), full);
            wait_for_gpu();
            const double first_ms = ms_since(first_start);
            renderer.present();

            std::vector<double> cpu, gpu, cut_ms, cut_upload_ms;
            for (int f = 0; f < run; ++f) {

                pose(f - run);
                renderer.clear(kin::Color::rgb(0, 0, 0));
                for (int k = 0; k < each; ++k) {
                    if (auto rendered = r3d.render(view, scene); !rendered)
                        throw std::runtime_error(kin::diagnostics3d_text(rendered.diagnostics));
                }
                renderer.draw_texture(view.texture(), full);
                renderer.present();
                if (f >= run - frames) {
                    cpu.push_back(r3d.debug_stats().cpu_ms);
                    cut_ms.push_back(r3d.debug_stats().cluster_ms);
                    cut_upload_ms.push_back(r3d.debug_stats().cluster_upload_ms);
                    gpu.push_back(renderer.backend_stats().last_gpu_frame_ms / each);
                }
            }
            wait_for_gpu();

            // The picture.
            pose(0);
            renderer.clear(kin::Color::rgb(0, 0, 0));
            scene.debug.stats_readback = over.readback; // waits for the GPU: only for this render, not the timed ones
            if (auto rendered = r3d.render(view, scene); !rendered)
                throw std::runtime_error(kin::diagnostics3d_text(rendered.diagnostics));
            scene.debug.stats_readback = false;
            const kin::RenderDebugStats3D stats = r3d.debug_stats();
            renderer.draw_texture(view.texture(), full);
            kin::Image image;
            if (!renderer.read_rgba(full, image.rgba, image.size)) {
                std::fprintf(stderr, "  %s: could not read the picture back\n", camera_name.c_str());
                ++failures;
            }
            renderer.present();
            for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
                image.rgba[i] = 255;
            }
            const fs::path png = out / name / (camera_name + ".png");
            kin::save_image(image, png);
            if (over.flicker > 0) {
                // Consecutive frames from here on (the animation, if any, goes on): how much does a pixel change from
                // one to the next?
                std::vector<kin::u8> previous = image.rgba;
                double sum_mean = 0, worst = 0, over8 = 0;
                for (int k = 1; k <= over.flicker; ++k) {
                    pose(k);
                    renderer.clear(kin::Color::rgb(0, 0, 0));
                    if (auto rendered = r3d.render(view, scene); !rendered)
                        throw std::runtime_error(kin::diagnostics3d_text(rendered.diagnostics));
                    renderer.draw_texture(view.texture(), full);
                    kin::Image next_image;
                    renderer.read_rgba(full, next_image.rgba, next_image.size);
                    renderer.present();
                    if (k > 1) {
                        double sum = 0;
                        std::size_t count = 0, big = 0;
                        for (std::size_t i = 0; i < next_image.rgba.size(); i += 4) {
                            for (std::size_t c = 0; c < 3; ++c) {
                                const int d = std::abs(int(next_image.rgba[i + c]) - int(previous[i + c]));
                                sum += d;
                                worst = std::max(worst, double(d));
                                big += d > 8;
                                ++count;
                            }
                        }
                        sum_mean += sum / double(count);
                        over8 += 100.0 * double(big) / double(count);
                    }
                    previous = next_image.rgba;
                }
                std::printf("FLICKER %s/%s mean %.4f max %.0f over8 %.4f%%\n", name.c_str(), camera_name.c_str(),
                            sum_mean / (over.flicker - 1), worst, over8 / (over.flicker - 1));
            }
            if (over.vram_cycle) {
                const double on_mb = gpu_used_mb();
                const double on_bytes = static_cast<double>(stats.feature_bytes);
                pose(0);
                scene.ambient_occlusion.enabled = false;
                scene.contact_shadows.enabled = false;
                scene.taa.enabled = false;
                scene.lod.occlusion.enabled = false;
                kin::u64 released = 0;
                for (int f = 0; f < 130; ++f) {
                    renderer.clear(kin::Color::rgb(0, 0, 0));
                    if (auto rendered = r3d.render(view, scene); !rendered)
                        throw std::runtime_error(kin::diagnostics3d_text(rendered.diagnostics));
                    renderer.draw_texture(view.texture(), full);
                    renderer.present();
                    released += r3d.debug_stats().targets_released;
                }
                wait_for_gpu();
                std::printf("  vram %s/%s: features on: %.0f MiB used (feature targets %.1f MiB); 130 renders with "
                            "them off: %.0f MiB used (feature targets %.1f MiB, %llu groups released)\n",
                            name.c_str(), camera_name.c_str(), on_mb, on_bytes / 1048576.0, gpu_used_mb(),
                            static_cast<double>(r3d.debug_stats().feature_bytes) / 1048576.0,
                            static_cast<unsigned long long>(released));
                pose(0);
                renderer.clear(kin::Color::rgb(0, 0, 0));
                if (auto rendered = r3d.render(view, scene); !rendered)
                    throw std::runtime_error(kin::diagnostics3d_text(rendered.diagnostics));
                renderer.present();
            }
            // The same moment drawn large and averaged down, with temporal anti-aliasing off.
            if (supersample > 1) {
                const kin::Vec2i big_size{width * supersample, height * supersample};
                kin::View3D big = make_view(big_size, 4);
                kin::RenderTarget target = renderer.create_render_target(big_size);
                pose(0);
                scene.taa.enabled = false;
                kin::Image large;
                if (big && target.valid()) {
                    renderer.push_render_target(target);
                    renderer.clear(kin::Color::rgb(0, 0, 0));
                    if (auto result = r3d.render(big, scene); !result)
                        throw std::runtime_error(kin::diagnostics3d_text(result.diagnostics));
                    const kin::Rectf big_rect{0.0f, 0.0f, static_cast<float>(big_size.x),
                                              static_cast<float>(big_size.y)};
                    renderer.draw_texture(big.texture(), big_rect);
                    renderer.read_rgba(big_rect, large.rgba, large.size);
                    renderer.pop_render_target();
                    renderer.present();
                }
                if (large.size.x == big_size.x && large.size.y == big_size.y) {
                    const kin::Image small = supersample_average(large, {width, height}, supersample);
                    kin::save_image(small, out / name / (camera_name + "_ss.png"));
                } else {
                    std::fprintf(stderr, "  %s: could not draw the supersampled picture\n", camera_name.c_str());
                }
            }
            const Summary cpu_s = summarize(cpu), gpu_s = summarize(gpu);
            std::printf("  %-14s render() %7.3f ms  GPU %7.3f ms  (p10-p90 %.3f-%.3f)  %u draws, %llu triangles, %u "
                        "shadow casters (%u cascades)\n",
                        camera_name.c_str(), cpu_s.median, gpu_s.median, gpu_s.p10, gpu_s.p90, stats.draws,
                        static_cast<unsigned long long>(stats.triangles), stats.shadow_casters, stats.shadow_cascades);
            if (stats.spots > 0 || stats.local_shadow_maps > 0 || stats.moon_cascades > 0) {
                std::printf("                 lights: %u spots, %u shadow maps (%u drawn, %u cached) with %u draws of "
                            "%u casters, %u moon cascades, %.1f MB of maps%s\n",
                            stats.spots, stats.local_shadow_maps, stats.local_shadow_drawn, stats.local_shadow_cached,
                            stats.local_shadow_draws, stats.local_shadow_casters, stats.moon_cascades,
                            static_cast<double>(stats.local_shadow_bytes) / 1048576.0,
                            stats.lights_variant ? ", light shaders" : "");
            }
            if (stats.particles_submitted > 0) {
                std::printf("                 particles: %u submitted (%u weather), %u drawn, %u batches (%u soft), %u "
                            "dropped, CPU %.3f ms\n",
                            stats.particles_submitted, stats.weather_particles, stats.particles_drawn,
                            stats.particle_batches, stats.particle_soft, stats.particles_dropped,
                            stats.particle_cpu_ms);
            }
            if (stats.skinned_copies > 0) {
                std::printf("                 skinned: %u copies seen in %u draws, %u joint matrices uploaded, %u "
                            "copies culled\n",
                            stats.skinned_copies, stats.skinned_draws, stats.skin_matrices, stats.culled);
            }
            if (stats.cluster_copies > 0 && stats.cluster_gpu) {
                const Summary cut_s = summarize(cut_ms);
                std::printf("                 clusters (GPU cut): %u copies, %llu clusters tested, %u compute "
                            "dispatches, %.3f MB handed over, CPU set-up %.3f ms (render() %.3f)",
                            stats.cluster_copies, static_cast<unsigned long long>(stats.clusters_tested),
                            stats.cluster_dispatches, static_cast<double>(stats.cluster_upload_bytes) / 1048576.0,
                            cut_s.median, cpu_s.median);
                if (over.readback) {
                    std::printf("; read back: %u clusters drawn, %llu triangles in the camera's cut, %llu gathered for "
                                "all passes, %u clusters dropped",
                                stats.clusters_drawn, static_cast<unsigned long long>(stats.triangles),
                                static_cast<unsigned long long>(stats.cluster_triangles_all), stats.cluster_overflow);
                    if (stats.occlusion) {
                        std::printf("; occlusion: %u clusters in the first pass, %u occluded (%llu triangles), %u "
                                    "drawn for nothing (%llu triangles)",
                                    stats.clusters_first_pass, stats.clusters_occluded,
                                    static_cast<unsigned long long>(stats.triangles_occluded), stats.clusters_wasted,
                                    static_cast<unsigned long long>(stats.triangles_wasted));
                    }
                }
                if (stats.occlusion) {
                    std::printf("; occlusion on (%u pyramid levels, %u dispatches to build it)", stats.occlusion_levels,
                                stats.occlusion_dispatches);
                }
                std::printf("\n");
            } else if (stats.cluster_copies > 0) {
                const Summary cut_s = summarize(cut_ms);
                std::printf("                 clusters: %u copies cut, %llu tested, %u drawn, %.2f MB of indices "
                            "uploaded, cut %.3f ms (of which compacting and uploading %.3f; render() %.3f)\n",
                            stats.cluster_copies, static_cast<unsigned long long>(stats.clusters_tested),
                            stats.clusters_drawn, static_cast<double>(stats.cluster_upload_bytes) / 1048576.0,
                            cut_s.median, summarize(cut_upload_ms).median, cpu_s.median);
            } else if (scene.lod.enabled) {
                std::printf("                 lod copies by level:");
                for (kin::u32 l = 0; l < kin::max_mesh_levels; ++l) {
                    if (stats.lod_copies_by_level[l] > 0 || l == 0) {
                        std::printf(" %u", stats.lod_copies_by_level[l]);
                    }
                }
                std::printf("\n");
            }
            if (!first_camera) {
                report << ",";
            }
            first_camera = false;
            report << "\n   {\"name\": " << quoted(camera_name)
                   << ", \"png\": " << quoted(fs::relative(png, out).generic_string()) << ", \"eye\": [" << eye.x
                   << ", " << eye.y << ", " << eye.z << "], \"target\": [" << target.x << ", " << target.y << ", "
                   << target.z << "], \"fov\": " << fov << ", \"first_ms\": " << first_ms
                   << ", \"cpu_ms\": " << json_summary(cpu_s) << ", \"gpu_ms\": " << json_summary(gpu_s)
                   << ", \"draws\": " << stats.draws << ", \"instances\": " << stats.instances
                   << ", \"culled\": " << stats.culled << ", \"triangles\": " << stats.triangles
                   << ", \"shadow_draws\": " << stats.shadow_draws << ", \"shadow_casters\": " << stats.shadow_casters
                   << ", \"shadow_cascades\": " << stats.shadow_cascades
                   << ", \"cascade_casters\": " << json_list(stats.cascade_casters)
                   << ", \"ao_draws\": " << stats.ao_draws << ", \"ao_instances\": " << stats.ao_instances
                   << ", \"ao_dispatches\": " << stats.ao_dispatches
                   << ", \"contact_dispatches\": " << stats.contact_dispatches
                   << ", \"taa_dispatches\": " << stats.taa_dispatches << ", \"lights\": " << stats.lights;
            if (stats.spots > 0 || stats.local_shadow_maps > 0 || stats.moon_cascades > 0) {
                report << ", \"spots\": " << stats.spots << ", \"local_shadow_maps\": " << stats.local_shadow_maps
                       << ", \"local_shadow_drawn\": " << stats.local_shadow_drawn
                       << ", \"local_shadow_cached\": " << stats.local_shadow_cached
                       << ", \"local_shadow_draws\": " << stats.local_shadow_draws
                       << ", \"local_shadow_casters\": " << stats.local_shadow_casters
                       << ", \"moon_cascades\": " << stats.moon_cascades
                       << ", \"local_shadow_mb\": " << static_cast<double>(stats.local_shadow_bytes) / 1048576.0;
            }
            if (stats.particles_submitted > 0) {
                report << ", \"particles_submitted\": " << stats.particles_submitted
                       << ", \"particles_drawn\": " << stats.particles_drawn
                       << ", \"particle_batches\": " << stats.particle_batches
                       << ", \"particle_soft\": " << stats.particle_soft
                       << ", \"weather_particles\": " << stats.weather_particles
                       << ", \"particle_cpu_ms\": " << stats.particle_cpu_ms;
            }
            if (stats.cluster_copies > 0) {
                report << ", \"cluster_gpu\": " << (stats.cluster_gpu ? "true" : "false")
                       << ", \"cluster_dispatches\": " << stats.cluster_dispatches
                       << ", \"cluster_overflow\": " << stats.cluster_overflow
                       << ", \"cluster_copies\": " << stats.cluster_copies
                       << ", \"clusters_tested\": " << stats.clusters_tested
                       << ", \"clusters_drawn\": " << stats.clusters_drawn
                       << ", \"cluster_upload_bytes\": " << stats.cluster_upload_bytes
                       << ", \"cluster_ms\": " << json_summary(summarize(cut_ms))
                       << ", \"cluster_upload_ms\": " << json_summary(summarize(cut_upload_ms));
                if (stats.occlusion) {
                    report << ", \"occlusion\": true, \"clusters_first_pass\": " << stats.clusters_first_pass
                           << ", \"clusters_occluded\": " << stats.clusters_occluded
                           << ", \"triangles_occluded\": " << stats.triangles_occluded
                           << ", \"clusters_wasted\": " << stats.clusters_wasted
                           << ", \"triangles_wasted\": " << stats.triangles_wasted;
                }
            }
            if (scene.lod.enabled) {
                report << ", \"lod_copies_by_level\": [";
                for (kin::u32 l = 0; l < kin::max_mesh_levels; ++l) {
                    report << (l ? ", " : "") << stats.lod_copies_by_level[l];
                }
                report << "]";
            }
            report << "}";
        }
        report << "]}";
    }
    report << "\n ]\n}\n";
    std::ofstream(out / "results.json") << report.str();
    renderer.set_job_system(nullptr);
    return failures == 0 ? 0 : 1;
}
