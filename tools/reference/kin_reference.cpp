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
//                 [--lod-gpu | --lod-cpu] [--readback] [--lod-room TRIANGLES] [--lod-occlusion]
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
// writes <camera>_ss.png too (--samples N sets the view's MSAA for a whole run): the same picture drawn at N x the size with 4x MSAA, with TAA
// off, and averaged down in linear light (the still picture to judge static parts by).
// Levels of detail (G3a): a scene file may have `lod: {enabled, error_pixels, bias, force_level}` and a
// model's entry `lod_levels: N` (a scene's own `lod_levels` is for its procedural meshes): meshes are then
// made with N levels. --lod-levels N builds N levels for every mesh of a run, and --lod turns the selection
// on for every scene (with --lod-error, --lod-bias and --lod-force), each a label of its own. The levels'
// build time and memory are printed with the load.
// Clusters (G3b): a model's entry `clusters: true`, or --lod-clusters for every model of a run (not the
// procedural meshes), builds a hierarchy of clusters of about 128 triangles for each of its meshes (which then
// take no levels); with the selection on, each copy is cut through its hierarchy by the error bound
// (--lod-force N is then the depth of the cut, 0 the leaves). The hierarchy's build time, the process's peak
// resident memory so far, and each camera's cut (copies, clusters tested and drawn, bytes uploaded, CPU time) are printed.
// The cut on the GPU (G3c): the default where the device can (Lod3D::gpu_culling); --lod-cpu cuts on the CPU, --lod-gpu on the GPU (a
// device that cannot falls back, and the run says which cut it made). --readback reads the GPU cut's counts back for the picture's render
// (clusters drawn, triangles, clusters dropped) -- never in the timed frames, which would wait for the GPU. --lod-room N sets
// Lod3D::max_cluster_triangles.
// Occlusion culling (G3d): a scene's `lod: {occlusion: true}` (or `{enabled, depth_margin, pixel_margin}`), or --lod-occlusion
// for every scene (it turns the selection on too): clusters behind others are not drawn. With --readback each camera also prints
// the clusters of the first pass, those occluded and those drawn for nothing. `procedural: "hidden"` builds a wall, a floor, a
// few objects in front of it and 64 copies of a 100,000-triangle torus (`clusters: true`) behind it, the scene it is judged on.

#include <kin/assets/image.hpp>
#include <kin/core/json_value.hpp>
#include <kin/core/jobs.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/taa.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    return {static_cast<float>(a[0].as_number()), static_cast<float>(a[1].as_number()), static_cast<float>(a[2].as_number())};
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
    const auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))]; };
    return {at(0.5), at(0.1), at(0.9), v.front()};
}

std::string json_summary(const Summary& s) {
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, "{\"median\": %.4f, \"p10\": %.4f, \"p90\": %.4f, \"min\": %.4f}", s.median, s.p10, s.p90, s.min);
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


// `animation: {ecs: true, ik: [...]}` (docs/rendering_3d.md, Inverse kinematics): the entity's IK constraints, in order. Each:
//   {name, kind: "two_bone" | "look_at" | "foot_placer", joints: [...], weight, fade,
//    target: [x, y, z] (the world; none: no target), pole_direction: [x, y, z] (model) | pole_position: [x, y, z] (world),
//    max_stretch, min_bend, target_rotation: {axis, degrees} (world),
//    look_at: weights: [...], max_yaw: [...], max_pitch_up: [...], max_pitch_down: [...] (per joint), smooth, release, aim: [..], up: [..],
//    foot_placer: foot_heights: [...], up, max_tilt, max_pelvis_drop, max_pelvis_raise, smooth,
//      ground: {floor: y, boxes: [{min: [..], max: [..]}], slope: {point, normal}} (what the query hits first, in the world)}
std::vector<kin::IKConstraint> parse_ik(const kin::JsonValue& list) {
    std::vector<kin::IKConstraint> out;
    for (const kin::JsonValue& j : list.items()) {
        kin::IKConstraint k;
        k.name = j.string_at("name");
        const std::string kind = j.string_at("kind");
        k.kind = kind == "look_at" ? kin::IKKind::LookAt : kind == "foot_placer" ? kin::IKKind::FootPlacer : kin::IKKind::TwoBone;
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
        if (const kin::JsonValue* r = j.find("target_rotation")) {
            k.set_target_rotation(kin::Quatf::axis_angle(vec3(r->find("axis"), {0, 1, 0}), static_cast<float>(r->number_at("degrees", 0.0))),
                                  static_cast<float>(r->number_at("weight", 1.0)));
        }
        const auto numbers = [&](const char* key, std::vector<float>& into) {
            if (const kin::JsonValue* a = j.find(key)) {
                for (const kin::JsonValue& n : a->items()) {
                    into.push_back(static_cast<float>(n.as_number()));
                }
            }
        };
        if (k.kind == kin::IKKind::LookAt) {
            std::vector<float> w, yaw, up, down;
            numbers("weights", w);
            numbers("max_yaw", yaw);
            numbers("max_pitch_up", up);
            numbers("max_pitch_down", down);
            for (std::size_t i = 0; i < k.joints.size(); ++i) {
                kin::LookAtJoint l;
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
        if (k.kind == kin::IKKind::FootPlacer) {
            numbers("foot_heights", k.foot_heights);
            k.foot.max_tilt_degrees = static_cast<float>(j.number_at("max_tilt", 30.0));
            k.foot.max_pelvis_drop = static_cast<float>(j.number_at("max_pelvis_drop", 0.5));
            k.foot.max_pelvis_raise = static_cast<float>(j.number_at("max_pelvis_raise", 0.0));
            k.foot.smooth_seconds = static_cast<float>(j.number_at("smooth", 0.1));
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
                const bool has_floor = g->find("floor") != nullptr;
                const float floor_y = static_cast<float>(g->number_at("floor", 0.0));
                const bool has_slope = g->find("slope") != nullptr;
                const Vec3f slope_point = has_slope ? vec3(g->find("slope")->find("point")) : Vec3f{};
                const Vec3f slope_normal = has_slope ? normalize(vec3(g->find("slope")->find("normal"), {0, 1, 0})) : Vec3f{0, 1, 0};
                // The first hit along the segment: the nearest of the boxes' faces, the slope plane and the floor.
                k.ground = [boxes, has_floor, floor_y, has_slope, slope_point, slope_normal](Vec3f from, Vec3f to) -> std::optional<kin::GroundHit> {
                    const Vec3f d = to - from;
                    float best = 2.0f;
                    kin::GroundHit hit;
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
    kin::Model model;
    Mat4f transform = Mat4f::identity();
    // A skinned model's pose (the model's entry `animation: {clip, time, loop, speed}`; docs/rendering_3d.md, Skinned meshes): the
    // clip (a name or an index, the first by default) sampled at `time` seconds when that is given, else at the frame's time
    // (frame N at N / 60 s, 0 at the picture) times `speed`; `loop` (on by default) wraps, else it holds the last key.
    // `root_motion: "remove"` holds the root joint at rest, so the character animates in place.
    bool animated = false;
    std::size_t clip = 0;
    bool fixed_time = false;
    double time = 0.0;
    bool loop = true;
    double speed = 1.0;
    bool remove_root_motion = false;
    // A 1D blend in place of the one clip (`animation: {blend: {parameter, parameter_to, parameter_seconds, points: [{clip, position}]}}`):
    // the clips of the points (indices into the model's) blended by the parameter, which runs from `parameter` to `parameter_to` over
    // `parameter_seconds` of the frames (a constant without `parameter_to`).
    struct BlendPoint {
        std::size_t clip = 0;
        float position = 0.0f;
    };
    std::vector<BlendPoint> blend;
    double parameter = 0.0, parameter_to = 0.0, parameter_seconds = 2.0;
    bool ramp = false;
    // `animation: {ecs: true}`: the model is an entity with SkinnedMeshRenderer and Animator3D, stepped by WorldRenderState3D::animate
    // at 60 steps a second (250 of them before the picture, the frame's index after), and drawn by its collect(): what a game does.
    bool ecs = false;
    Vec3f position{};
    float rotation_y = 0.0f, uniform_scale = 1.0f;
    std::shared_ptr<const kin::Model> shared;
    flecs::entity entity;
    kin::Animator3D animator; // the entity's at the start
};

// Signal Breach's deck (48 x 32 tiles) and `count` enemies over it, three
// meshes instanced, as kin_draw_bench's shadows mode has them.
struct Crowd {
    kin::Mesh tile;
    std::array<kin::Mesh, 3> enemy;
    std::vector<kin::MeshInstance> tiles;
    std::array<std::vector<kin::MeshInstance>, 3> enemies;

    void make(kin::Renderer3D& r3d, int count, const kin::MeshOptions& options) {
        tile = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}), options);
        enemy = {r3d.create_mesh(kin::meshes::star_prism(5, 0.32f, 0.14f, 0.16f), options), r3d.create_mesh(kin::meshes::cylinder(0.4f, 0.16f, 6), options),
                 r3d.create_mesh(kin::meshes::torus(0.26f, 0.07f, 20, 8), options)};
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
                {Mat4f::translation(at) * Mat4f::rotation_y(static_cast<float>(i) * 37.0f), kin::Color::rgb(255, 255, 255)});
        }
    }

    void draw(kin::Scene3D& scene) const {
        scene.draw_instanced(tile, tiles);
        for (std::size_t k = 0; k < 3; ++k) {
            scene.draw_instanced(enemy[k], enemies[k], {.color = kin::Color::rgb(200, 120, 80)});
        }
    }
    // The enemies drifting (`--crowd-move`): each a little from where it was, a pure function of `time` (frames), as a game's moves them.
    void draw_moving(kin::Scene3D& scene, int time, std::array<std::vector<kin::MeshInstance>, 3>& moved) const {
        scene.draw_instanced(tile, tiles);
        for (std::size_t k = 0; k < 3; ++k) {
            moved[k] = enemies[k];
            for (std::size_t i = 0; i < moved[k].size(); ++i) {
                const float phase = static_cast<float>(time) * 0.05f + static_cast<float>(i) * 0.37f;
                moved[k][i].transform.m[12] += std::sin(phase) * 0.15f;
                moved[k][i].transform.m[14] += std::cos(phase) * 0.15f;
            }
            scene.draw_instanced(enemy[k], moved[k], {.color = kin::Color::rgb(200, 120, 80)});
        }
    }
};

// A scene made for temporal anti-aliasing (`procedural: "motion"`): high-contrast thin things
// (a fence of posts and rails, a wire, poles at three depths), and four things moving in
// a way that is a pure function of time: a bar orbiting, a cross turning, a block going to
// and fro, a bright pole sliding in front of the fence. Time is in frames; 0 is the picture.
struct Motion {
    kin::Mesh floor, wall, post, rail, wire, pole, block, bar, cross_bar, ball;

    void make(kin::Renderer3D& r3d, const kin::MeshOptions& options) {
        floor = r3d.create_mesh(kin::meshes::box({16.0f, 0.2f, 12.0f}), options);
        wall = r3d.create_mesh(kin::meshes::box({16.0f, 4.0f, 0.2f}), options);
        post = r3d.create_mesh(kin::meshes::box({0.035f, 1.2f, 0.035f}), options);
        rail = r3d.create_mesh(kin::meshes::box({4.0f, 0.03f, 0.03f}), options);
        wire = r3d.create_mesh(kin::meshes::box({6.0f, 0.012f, 0.012f}), options);
        pole = r3d.create_mesh(kin::meshes::box({0.02f, 2.4f, 0.02f}), options);
        block = r3d.create_mesh(kin::meshes::box({0.5f, 0.5f, 0.5f}), options);
        bar = r3d.create_mesh(kin::meshes::box({0.9f, 0.05f, 0.05f}), options);
        cross_bar = r3d.create_mesh(kin::meshes::box({1.0f, 0.03f, 0.03f}), options);
        ball = r3d.create_mesh(kin::meshes::sphere(0.18f, 24, 12), options);
    }

    void draw(kin::Scene3D& scene, int t) const {
        const float time = static_cast<float>(t);
        const kin::Material3D dark{.color = kin::Color::rgb(28, 30, 36), .roughness = 0.9f};
        const kin::Material3D bright{.color = kin::Color::rgb(245, 245, 240), .roughness = 0.6f};
        const kin::Material3D warm{.color = kin::Color::rgb(255, 150, 60), .roughness = 0.5f};
        const kin::Material3D unlit_white{.color = kin::Color::rgb(255, 255, 255), .unlit = true};
        scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = kin::Color::rgb(70, 74, 82), .roughness = 0.9f});
        scene.draw(wall, Mat4f::translation({0.0f, 2.0f, -3.0f}), dark);
        // A fence: 60 posts every 6.5 cm, two rails (white against the dark wall).
        for (int i = 0; i < 60; ++i) {
            scene.draw(post, Mat4f::translation({-1.95f + 0.065f * static_cast<float>(i), 0.6f, -1.0f}), bright);
        }
        scene.draw(rail, Mat4f::translation({0.0f, 0.35f, -1.0f}), bright);
        scene.draw(rail, Mat4f::translation({0.0f, 1.0f, -1.0f}), bright);
        // A wire across, and poles at three depths.
        scene.draw(wire, Mat4f::translation({0.0f, 1.9f, -2.0f}) * Mat4f::rotation_z(4.0f), unlit_white);
        for (int i = 0; i < 3; ++i) {
            scene.draw(pole, Mat4f::translation({2.2f + 0.5f * static_cast<float>(i), 1.2f, -2.5f + 1.2f * static_cast<float>(i)}), bright);
        }
        // Moving: a bar orbiting on a circle at 3 degrees a frame, a cross turning at 4, a block going to and fro, a pole sliding.
        const float orbit = time * 3.0f * 3.14159265f / 180.0f;
        scene.draw(bar, Mat4f::translation({-2.6f + 0.6f * std::cos(orbit), 1.0f + 0.6f * std::sin(orbit), -0.4f}) * Mat4f::rotation_z(time * 3.0f + 90.0f), warm);
        const Mat4f turn = Mat4f::translation({-3.6f, 0.9f, -0.4f}) * Mat4f::rotation_y(time * 4.0f);
        scene.draw(cross_bar, turn, warm);
        scene.draw(cross_bar, turn * Mat4f::rotation_y(90.0f), warm);
        const float to_and_fro = 1.5f * std::sin(time * 0.05f);
        scene.draw(block, Mat4f::translation({3.5f + to_and_fro, 0.25f, -0.2f}) * Mat4f::rotation_y(time * 2.0f), warm);
        scene.draw(ball, Mat4f::translation({-0.8f + 1.2f * std::sin(time * 0.04f), 0.5f, 0.6f}), warm);
        scene.draw(pole, Mat4f::translation({0.6f * std::sin(time * 0.06f), 1.2f, 0.2f}), warm);
    }
};

// Occlusion culling's scene (`procedural: "hidden"`): a floor, a wall 20 m wide and 8 high across the way, 8 tori in front
// of it, and behind it 64 more (8 x 8, turned every which way), each of 100,352 triangles, made with clusters when the
// scene says `clusters: true` (G3d). Seen from in front of the wall, the 64 are hidden; from the side, some show past its
// end; from behind the grid, nothing is hidden by the wall.
struct Hidden {
    kin::Mesh floor, wall, torus;
    std::vector<kin::MeshInstance> behind, front;
    Vec3f extent{12.0f, 8.0f, 21.0f}; // the scene's bounds: +-x, up to y, to z

    // `doc` may set `grid: [nx, nz]` (8 x 8), `spacing: [dx, dz]` (3, 2.4) and `segments` (224: 100,352 triangles a torus).
    void make(kin::Renderer3D& r3d, kin::MeshOptions options, const kin::JsonValue& doc) {
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
        floor = r3d.create_mesh(kin::meshes::box({extent.x * 2.0f + 40.0f, 0.2f, extent.z + 60.0f}), kin::MeshOptions{.levels = options.levels});
        wall = r3d.create_mesh(kin::meshes::box({wall_width, 8.0f, 0.4f}), kin::MeshOptions{.levels = options.levels});
        torus = r3d.create_mesh(kin::meshes::torus(0.8f, 0.3f, segments, segments), options);
        for (int j = 0; j < nz; ++j) {
            for (int i = 0; i < nx; ++i) {
                const Vec3f at{-half + 0.5f * dx + dx * static_cast<float>(i), 1.1f, 3.5f + dz * static_cast<float>(j)};
                behind.push_back({Mat4f::translation(at) * Mat4f::rotation_y(static_cast<float>(i * 8 + j) * 29.0f) * Mat4f::rotation_x(static_cast<float>(i + 3 * j) * 17.0f),
                                  kin::Color::rgb(static_cast<kin::u8>(120 + 16 * (i % 8)), static_cast<kin::u8>(200 - 12 * j), 150)});
            }
        }
        for (int i = 0; i < 8; ++i) {
            front.push_back({Mat4f::translation({-9.1f + 2.6f * static_cast<float>(i), 1.1f, -4.0f - 1.2f * static_cast<float>(i % 3)}) *
                                 Mat4f::rotation_y(static_cast<float>(i) * 41.0f),
                             kin::Color::rgb(230, 170, static_cast<kin::u8>(60 + 15 * i))});
        }
    }

    void draw(kin::Scene3D& scene) const {
        scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = kin::Color::rgb(96, 100, 108), .roughness = 0.9f});
        scene.draw(wall, Mat4f::translation({0.0f, 4.0f, 0.0f}), {.color = kin::Color::rgb(190, 186, 176), .roughness = 0.8f});
        scene.draw_instanced(torus, behind, {.roughness = 0.45f});
        scene.draw_instanced(torus, front, {.roughness = 0.45f});
    }
};

kin::Bounds3 bounds_of(const std::vector<Placed>& placed) {
    Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Placed& p : placed) {
        const kin::Bounds3& b = p.model.bounds;
        for (int i = 0; i < 8; ++i) {
            const Vec3f c = p.transform.apply({(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z});
            lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
            hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
        }
    }
    return placed.empty() ? kin::Bounds3{} : kin::Bounds3{lo, hi};
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
    int flicker = 0; // --flicker N: after each camera's picture, N more frames read back, the largest and mean frame-to-frame difference printed (temporal shimmer)
    std::string ab; // --ab NAME: the timed frames in blocks of eight with the environment variable NAME at 0 and at 1 in turn (the renderer reads its switches on each render), A and B timed apart in one run
    std::string hitch; // --hitch <feature>: the first renders after switching it on at runtime, timed (then the process ends)
    bool warm_up = false; // with --hitch: Renderer3D::warm_up before the feature goes on
    bool vram_cycle = false; // after a camera: the features off for 130 renders; prints the video memory with them on and after (see Render3DStats::feature_bytes)
    bool crowd_move = false; // the crowd's enemies drift each frame (a game's crowd always moves): the scene is posed per frame
    bool shadow_cache = false; // Lights3D::cache_shadows: off here, because a static scene is rendered many times and the shadow pass would be timed once
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

void merge_lod(kin::Lod3D& lod, const kin::JsonValue* doc, const Overrides& over) {
    if (doc) {
        lod.enabled = doc->bool_at("enabled", true);
        lod.error_pixels = static_cast<float>(doc->number_at("error_pixels", lod.error_pixels));
        lod.bias = static_cast<float>(doc->number_at("bias", lod.bias));
        lod.force_level = static_cast<kin::i32>(doc->number_at("force_level", lod.force_level));
        if (const kin::JsonValue* occlusion = doc->find("occlusion")) {
            lod.occlusion.enabled = occlusion->is_bool() ? occlusion->as_bool() : occlusion->bool_at("enabled", true);
            lod.occlusion.depth_margin = static_cast<float>(occlusion->number_at("depth_margin", lod.occlusion.depth_margin));
            lod.occlusion.pixel_margin = static_cast<kin::u32>(occlusion->number_at("pixel_margin", lod.occlusion.pixel_margin));
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
        lod.force_level = over.lod_force;
    }
    if (over.lod_gpu >= 0) {
        lod.gpu_culling = over.lod_gpu > 0;
    }
    if (over.lod_room > 0) {
        lod.max_cluster_triangles = static_cast<kin::u32>(over.lod_room);
    }
}

// The process's resident memory, in MB (Linux; 0 elsewhere).
// The GPU's memory in use (MiB), from nvidia-smi: the whole device, so only differences mean anything (this process is nearly alone on it); -1 without.
double gpu_used_mb() {
    FILE* pipe = popen("nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null", "r");
    if (!pipe) {
        return -1.0;
    }
    char line[64] = {};
    const bool read = std::fgets(line, sizeof line, pipe) != nullptr;
    pclose(pipe);
    return read ? std::atof(line) : -1.0;
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
                    const std::size_t i = (static_cast<std::size_t>(y * supersample + sy) * static_cast<std::size_t>(large.size.x) +
                                           static_cast<std::size_t>(x * supersample + sx)) * 4;
                    for (int ch = 0; ch < 3; ++ch) {
                        sum[ch] += decode_srgb(large.rgba[i + static_cast<std::size_t>(ch)]);
                    }
                }
            }
            const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x)) * 4;
            for (int ch = 0; ch < 3; ++ch) {
                small.rgba[o + static_cast<std::size_t>(ch)] = encode_srgb(sum[ch] / static_cast<float>(supersample * supersample));
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
        } else if (a == "--ab") {
            over.ab = next();
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
        std::fprintf(stderr, "usage: kin_reference <scene.json>... --out DIR [--cache DIR] [--frames N] [--warmup N] [--repeat N]\n");
        return 2;
    }
    fs::create_directories(out);

    kin::App app{};
    kin::Window& window = app.create_window({.title = "kin-reference", .width = width, .height = height, .hidden = true});
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
    const kin::Rectf full{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
    std::vector<kin::u8> pixel;
    kin::Vec2i pixel_size{};
    const auto wait_for_gpu = [&] { renderer.read_rgba({0.0f, 0.0f, 1.0f, 1.0f}, pixel, pixel_size); };

    std::ostringstream report;
    report << "{\n \"renderer\": " << quoted(std::string{renderer.backend_name()}) << ",\n \"vulkan_icd\": "
           << quoted(std::getenv("VK_ICD_FILENAMES") ? std::getenv("VK_ICD_FILENAMES") : "") << ",\n \"size\": [" << width << ", "
           << height << "],\n \"frames\": " << frames << ", \"repeat\": " << repeat << ",\n \"scenes\": [";
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
            kin::MeshOptions options;
            options.levels = over.lod_levels > 0 ? static_cast<kin::u32>(over.lod_levels)
                                                 : static_cast<kin::u32>(std::clamp(entry.number_at("lod_levels", 1.0), 1.0, static_cast<double>(kin::max_mesh_levels)));
            return options;
        };
        const auto model_options_of = [&](const kin::JsonValue& entry) {
            kin::MeshOptions options = levels_of(entry);
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
                kin::Model model = r3d.load_model(path, model_options_of(m), &error);
                if (!model) {
                    std::fprintf(stderr, "  %s: %s\n", path.string().c_str(), error.c_str());
                    missing = true;
                    break;
                }
                const float scale = static_cast<float>(m.number_at("scale", 1.0));
                placed.push_back({std::move(model), Mat4f::translation(vec3(m.find("position"))) *
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
                        std::fprintf(stderr, "  %s: has an animation entry but no skeleton or clips\n", path.string().c_str());
                    } else {
                        back.animated = true;
                        const std::string wanted = a->string_at("clip");
                        if (!wanted.empty()) {
                            const kin::AnimClip3D* found = back.model.find_clip(wanted);
                            if (!found) {
                                std::string names;
                                for (const kin::AnimClip3D& c : back.model.clips) names += " " + c.name;
                                std::fprintf(stderr, "  %s: no clip called %s (it has:%s)\n", path.string().c_str(), wanted.c_str(), names.c_str());
                                return 1;
                            }
                            back.clip = static_cast<std::size_t>(found - back.model.clips.data());
                        } else {
                            back.clip = std::min<std::size_t>(static_cast<std::size_t>(a->number_at("index", 0.0)), back.model.clips.size() - 1);
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
                                        std::fprintf(stderr, "  %s: blend point: no clip called %s\n", path.string().c_str(), point.string_at("clip").c_str());
                                        return 1;
                                    }
                                    back.blend.push_back({static_cast<std::size_t>(found - back.model.clips.data()), static_cast<float>(point.number_at("position", 0.0))});
                                }
                            }
                            back.parameter = blend->number_at("parameter", 0.0);
                            back.ramp = blend->find("parameter_to") != nullptr;
                            back.parameter_to = blend->number_at("parameter_to", back.parameter);
                            back.parameter_seconds = blend->number_at("parameter_seconds", 2.0);
                        }
                        const kin::AnimClip3D& clip = back.model.clips[back.clip];
                        std::printf("  animation: %s, %.2f s, %zu clips, %u joints\n", clip.name.c_str(), static_cast<double>(clip.duration),
                                    back.model.clips.size(), back.model.skeletons[0].joint_count());
                    }
                }
                if (grid) {
                    const int count = std::max(1, static_cast<int>(grid->number_at("count", 1.0)));
                    const int columns = std::max(1, static_cast<int>(grid->number_at("columns", 10.0)));
                    const float sx = static_cast<float>(grid->find("spacing") ? grid->find("spacing")->items()[0].as_number(2.0) : 2.0);
                    const float sz = static_cast<float>(grid->find("spacing") ? grid->find("spacing")->items()[1].as_number(2.0) : 2.0);
                    const double time_step = grid->number_at("time_step", 0.0);
                    const Placed first = placed[base_index];
                    placed.pop_back();
                    for (int i = 0; i < count; ++i) {
                        Placed copy = first;
                        const float x = (static_cast<float>(i % columns) - static_cast<float>(columns - 1) * 0.5f) * sx;
                        const float z = (static_cast<float>(i / columns) - static_cast<float>((count - 1) / columns) * 0.5f) * sz;
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
        // `animation: {ecs: true}` models: an entity each, in a world of its own. The picture (frame 0) is 250 steps (4.17 s) in.
        constexpr int ecs_lead = 250;
        kin::EcsWorld ecs_world;
        kin::WorldRenderState3D ecs_state{ecs_world};
        bool any_ecs = false;
        for (Placed& p : placed) {
            if (!p.ecs || !p.animated) {
                continue;
            }
            any_ecs = true;
            p.shared = std::make_shared<const kin::Model>(p.model);
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
            p.entity = ecs_world.raw().entity()
                           .set(kin::Transform3D{.position = p.position, .rotation = kin::Quatf::axis_angle({0.0f, 1.0f, 0.0f}, p.rotation_y),
                                                 .scale = {p.uniform_scale, p.uniform_scale, p.uniform_scale}})
                           .set(kin::SkinnedMeshRenderer{.model = p.shared})
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
        Hidden hidden;
        const bool is_hidden = doc.string_at("procedural") == "hidden";
        if (is_hidden) {
            hidden.make(r3d, model_options_of(doc), doc);
        }
        const bool animated = doc.bool_at("animated") || (over.crowd_move && doc.string_at("procedural") == "crowd");
        std::array<std::vector<kin::MeshInstance>, 3> crowd_moved;
        const double load_ms = ms_since(load_start);
        {
            std::size_t meshes = 0, with_levels = 0, level_triangles = 0, base_triangles = 0;
            for (const Placed& p : placed) {
                for (const kin::ModelPart& part : p.model.parts) {
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
                for (const kin::ModelPart& part : p.model.parts) {
                    with_clusters += part.mesh.has_clusters() ? 1 : 0;
                    clusters += part.mesh.cluster_count();
                    leaf_triangles += part.mesh.has_clusters() ? part.mesh.leaf_triangles() : 0;
                    cluster_triangles += part.mesh.cluster_triangles();
                    deepest = std::max(deepest, part.mesh.cluster_levels());
                }
            }
            if (with_clusters > 0) {
                std::printf("  loaded in %.0f ms with clusters: %zu meshes, %zu with clusters (%zu clusters, up to %u levels deep, %zu leaf triangles, %llu in all), +%.0f MB resident, peak %.0f MB\n",
                            load_ms, meshes, with_clusters, clusters, deepest, leaf_triangles, static_cast<unsigned long long>(cluster_triangles),
                            resident_mb() - resident_before, peak_mb());
                if (r3d.capabilities().gpu_driven_clusters) {
                    // The video memory the hierarchy takes for the cut in compute: the cluster table (80 bytes a cluster) and every cluster's indices.
                    std::printf("  GPU hierarchy: %.1f MB (%.1f MB of indices for all levels, %.1f MB table), against %.1f MB of leaf indices before G3c\n",
                                (static_cast<double>(clusters) * 80.0 + static_cast<double>(cluster_triangles) * 12.0) / 1048576.0,
                                static_cast<double>(cluster_triangles) * 12.0 / 1048576.0, static_cast<double>(clusters) * 80.0 / 1048576.0,
                                static_cast<double>(leaf_triangles) * 12.0 / 1048576.0);
                }
            }
            if (with_levels > 0 || over.lod_levels > 1) {
                std::printf("  loaded in %.0f ms with levels of detail: %zu meshes, %zu with levels, %zu triangles (%zu with all their levels), +%.0f MB resident\n",
                            load_ms, meshes, with_levels, base_triangles, level_triangles, resident_mb() - resident_before);
            }
        }
        const kin::Bounds3 bounds = is_crowd    ? kin::Bounds3{{0.0f, -1.0f, 0.0f}, {48.0f, 3.0f, 32.0f}}
                                    : is_motion ? kin::Bounds3{{-8.0f, -0.2f, -6.0f}, {8.0f, 4.0f, 6.0f}}
                                    : is_hidden ? kin::Bounds3{{-hidden.extent.x, -0.2f, -8.0f}, {hidden.extent.x, hidden.extent.y, hidden.extent.z}}
                                                : bounds_of(placed);

        // The view.
        const kin::JsonValue* v = doc.find("view");
        const kin::JsonValue none{};
        const kin::JsonValue& view_doc = v ? *v : none;
        const auto make_view = [&](kin::Vec2i size, kin::u32 samples) {
            return r3d.create_view(size, {.samples = samples,
                                          .background = colour(view_doc.find("background"), kin::Color::rgb(0, 0, 0)),
                                          .hdr = view_doc.bool_at("hdr"),
                                          .exposure = static_cast<float>(view_doc.number_at("exposure", 1.0)),
                                          .bloom = static_cast<float>(view_doc.number_at("bloom", 0.0))});
        };
        kin::View3D view = make_view({width, height}, static_cast<kin::u32>(samples_override > 0 ? samples_override : view_doc.number_at("samples", 4)));

        // The lights, the same every frame; an environment (an .hdr in the cache) loaded once.
        kin::Environment3D environment;
        if (const kin::JsonValue* l = doc.find("lights")) {
            if (const kin::JsonValue* env = l->find("environment")) {
                std::string error;
                environment = r3d.load_environment(cache / env->string_at("file"), &error);
                if (!environment) {
                    std::fprintf(stderr, "  environment: %s\n", error.c_str());
                    ++failures;
                    continue;
                }
            }
        }

        kin::Scene3D scene;
        const auto fill_scene = [&](int time) {
            scene.clear();
            const kin::JsonValue* l = doc.find("lights");
            const kin::JsonValue& lights = l ? *l : none;
            if (const kin::JsonValue* sun = lights.find("sun")) {
                scene.lights.sun = {.direction = vec3(sun->find("direction"), {0, 1, 0}), .color = vec3(sun->find("color"))};
            } else {
                scene.lights.sun.color = {};
            }
            if (const kin::JsonValue* fill = lights.find("fill")) {
                scene.lights.fill = {.direction = vec3(fill->find("direction"), {0, 1, 0}), .color = vec3(fill->find("color"))};
            }
            scene.lights.ambient = vec3(lights.find("ambient"));
            scene.lights.environment = environment;
            if (const kin::JsonValue* env = lights.find("environment")) {
                scene.lights.environment_intensity = static_cast<float>(env->number_at("intensity", 1.0));
                scene.lights.environment_rotation = static_cast<float>(env->number_at("rotation", 0.0));
            }
            if (const kin::JsonValue* ground = lights.find("ground_ambient")) {
                scene.lights.ground_ambient = vec3(ground);
            }
            if (const kin::JsonValue* area = lights.find("shadow_area")) {
                if (area->is_string() && area->as_string() == "auto") {
                    scene.lights.shadow_area = bounds;
                } else if (area->items().size() == 2) {
                    scene.lights.shadow_area = {vec3(&area->items()[0]), vec3(&area->items()[1])};
                }
                scene.lights.shadow_resolution = static_cast<kin::u32>(lights.number_at("shadow_resolution", 2048));
                scene.lights.shadow_cascades = static_cast<kin::u32>(lights.number_at("shadow_cascades", 1));
                scene.lights.shadow_distance = static_cast<float>(lights.number_at("shadow_distance", scene.lights.shadow_distance));
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
            merge_lod(scene.lod, doc.find("lod"), over);
            if (const kin::JsonValue* points = lights.find("points")) {
                for (const kin::JsonValue& p : points->items()) {
                    scene.add_light({.position = vec3(p.find("position")), .color = vec3(p.find("color")),
                                     .radius = static_cast<float>(p.number_at("radius", 1.0))});
                }
            }
            if (any_ecs) {
                // The entities start again and take the steps the frame's index stands for (60 a second, 240 before the picture, which
                // is the frame 0, `ecs_lead` steps in): every frame is the same whatever order the run asks for them in.
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
                            const double t = std::min(1.0, static_cast<double>(i) / 60.0 / std::max(p.parameter_seconds, 1e-6));
                            p.entity.get_mut<kin::Animator3D>()->params.set_float("speed", static_cast<float>(p.parameter + (p.parameter_to - p.parameter) * t));
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
                    const kin::Skeleton& skeleton = p.model.skeletons[0];
                    const double seconds = p.fixed_time ? p.time : static_cast<double>(time) / 60.0 * p.speed + p.time;
                    kin::Pose pose;
                    if (!p.blend.empty()) {
                        // A 1D blend: a player at the parameter's value for the frame, run for the seconds (its phase wraps).
                        std::vector<kin::BlendPoint3D> points;
                        for (const Placed::BlendPoint& point : p.blend) {
                            points.push_back({&p.model.clips[point.clip], point.position});
                        }
                        const double ramp = p.ramp ? std::clamp((static_cast<double>(time) + ecs_lead) / 60.0 / std::max(p.parameter_seconds, 1e-6), 0.0, 1.0) : 0.0;
                        kin::AnimationPlayer3D player{&skeleton};
                        player.play_blend(points, static_cast<float>(p.parameter + (p.parameter_to - p.parameter) * ramp), p.loop);
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
                    scene.draw_skinned(p.model, matrices, p.transform);
                } else {
                    scene.draw(p.model, p.transform);
                }
            }
            if (is_crowd) {
                if (over.crowd_move) {
                    crowd.draw_moving(scene, time, crowd_moved);
                } else {
                    crowd.draw(scene);
                }
            }
            if (is_motion) {
                motion.draw(scene, time);
            }
            if (is_hidden) {
                hidden.draw(scene);
            }
        };

        fs::create_directories(out / name);
        if (!first_scene) {
            report << ",";
        }
        first_scene = false;
        report << "\n  {\"name\": " << quoted(name) << ", \"load_ms\": " << load_ms << ", \"peak_mb\": " << peak_mb() << ", \"bounds\": [[" << bounds.min.x << ", "
               << bounds.min.y << ", " << bounds.min.z << "], [" << bounds.max.x << ", " << bounds.max.y << ", " << bounds.max.z
               << "]], \"cameras\": [";
        bool first_camera = true;
        const kin::JsonValue* cameras = doc.find("cameras");
        for (const kin::JsonValue& c : cameras ? cameras->items() : kin::JsonValue::Array{}) {
            const std::string camera_name = c.string_at("name", "camera");
            const float fov = static_cast<float>(c.number_at("fov", 50.0));
            Vec3f eye = vec3(c.find("eye")), target = vec3(c.find("target"));
            if (const kin::JsonValue* fit = c.find("fit")) {
                // Framed round the scene's bounds, from yaw (round +Y, from +Z) and pitch.
                target = bounds.center();
                const float radius = std::max(0.01f, bounds.radius());
                const float distance = radius / std::sin(fov * 3.14159265f / 360.0f) * static_cast<float>(fit->number_at("margin", 1.05));
                const float yaw = static_cast<float>(fit->number_at("yaw", 0.0)) * 3.14159265f / 180.0f;
                const float pitch = static_cast<float>(fit->number_at("pitch", 30.0)) * 3.14159265f / 180.0f;
                eye = target + Vec3f{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)} * distance;
            }
            // The scene at `time` (frames; 0 is the picture, earlier is negative), the camera
            // having panned `pan` units a frame to reach `eye` at 0.
            const Vec3f pan = vec3(c.find("pan"));
            const auto pose = [&](int time) {
                fill_scene(time);
                const Vec3f moved = pan * static_cast<float>(time);
                scene.camera = kin::Camera3D::look_at(eye + moved, target + moved, fov);
                scene.camera.z_near = static_cast<float>(c.number_at("near", 0.05));
            };
            if (!over.hitch.empty()) {
                // The first renders after a feature goes on at runtime: pipelines and targets made on first use. Steady state first (the
                // scene as it is, 40 renders), then the feature on, each of the next renders timed alone: wall time of render + draw + present
                // on the CPU, render()'s own, and the time until the GPU has finished it. Then 40 more, the steady cost with it on.
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
                        scene.lod.gpu_culling = false;
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
                    r3d.render(view, scene);
                    renderer.draw_texture(view.texture(), full);
                    renderer.present();
                    const double cpu_wall = ms_since(start);
                    wait_for_gpu();
                    made += r3d.stats().pipelines_made;
                    return std::array<double, 3>{cpu_wall, r3d.stats().cpu_ms, ms_since(start)};
                };
                if (over.warm_up) {
                    pose(0);
                    apply(true);
                    r3d.warm_up(view, scene);
                    pose(0);
                    apply(false);
                }
                for (int f = 0; f < 40; ++f) {
                    timed(f, false);
                }
                made = 0;
                std::printf("  hitch %s %s: first renders with it on (ms: CPU wall of render+present, render(), until the GPU is done):", name.c_str(), feature.c_str());
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
                std::printf("   steady with it on: median [%.2f %.2f %.2f]\n", summarize(wall).median, summarize(own).median,
                            summarize(done).median);
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
            r3d.render(view, scene);
            renderer.draw_texture(view.texture(), full);
            wait_for_gpu();
            const double first_ms = ms_since(first_start);
            renderer.present();

            std::vector<double> cpu, gpu, cut_ms, cut_upload_ms, ab_gpu[2], ab_cpu[2];
            // --ab NAME or NAME:A:B (the values of the variable in the even and odd blocks; 0 and 1 by default)
            std::string ab_name = over.ab, ab_a = "0", ab_b = "1";
            if (const std::size_t c1 = over.ab.find(':'); c1 != std::string::npos) {
                const std::size_t c2 = over.ab.find(':', c1 + 1);
                ab_name = over.ab.substr(0, c1);
                ab_a = over.ab.substr(c1 + 1, c2 - c1 - 1);
                ab_b = c2 == std::string::npos ? "1" : over.ab.substr(c2 + 1);
            }
            for (int f = 0; f < run; ++f) {
                if (!over.ab.empty()) {
                    setenv(ab_name.c_str(), ((f / 8) & 1) ? ab_b.c_str() : ab_a.c_str(), 1);
                }
                pose(f - run);
                renderer.clear(kin::Color::rgb(0, 0, 0));
                for (int k = 0; k < each; ++k) {
                    r3d.render(view, scene);
                }
                renderer.draw_texture(view.texture(), full);
                renderer.present();
                if (f >= run - frames) {
                    cpu.push_back(r3d.stats().cpu_ms);
                    cut_ms.push_back(r3d.stats().cluster_ms);
                    cut_upload_ms.push_back(r3d.stats().cluster_upload_ms);
                    gpu.push_back(renderer.backend_stats().last_gpu_frame_ms / each);
                    if (!over.ab.empty() && (f % 8) >= 3) { // the GPU time arrives a frame or two late: the block's first three are not counted
                        ab_gpu[(f / 8) & 1].push_back(gpu.back());
                        ab_cpu[(f / 8) & 1].push_back(cpu.back());
                    }
                }
            }
            wait_for_gpu();
            if (!over.ab.empty()) {
                setenv(ab_name.c_str(), ab_a.c_str(), 1);
                const Summary a = summarize(ab_gpu[0]), b = summarize(ab_gpu[1]);
                const Summary ca = summarize(ab_cpu[0]), cb = summarize(ab_cpu[1]);
                std::printf("AB %s/%s %s=A gpu %.4f (p10 %.4f) cpu %.4f  %s=B gpu %.4f (p10 %.4f) cpu %.4f\n", name.c_str(), camera_name.c_str(), ab_name.c_str(),
                            a.median, a.p10, ca.median, ab_name.c_str(), b.median, b.p10, cb.median);
            }

            // The picture.
            pose(0);
            renderer.clear(kin::Color::rgb(0, 0, 0));
            scene.lod.stats_readback = over.readback; // waits for the GPU: only for this render, not the timed ones
            r3d.render(view, scene);
            scene.lod.stats_readback = false;
            const kin::Render3DStats stats = r3d.stats();
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
                // Consecutive frames from here on (the animation, if any, goes on): how much does a pixel change from one to the next?
                std::vector<kin::u8> previous = image.rgba;
                double sum_mean = 0, worst = 0, over8 = 0;
                for (int k = 1; k <= over.flicker; ++k) {
                    pose(k);
                    renderer.clear(kin::Color::rgb(0, 0, 0));
                    r3d.render(view, scene);
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
                std::printf("FLICKER %s/%s mean %.4f max %.0f over8 %.4f%%\n", name.c_str(), camera_name.c_str(), sum_mean / (over.flicker - 1), worst, over8 / (over.flicker - 1));
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
                    r3d.render(view, scene);
                    renderer.draw_texture(view.texture(), full);
                    renderer.present();
                    released += r3d.stats().targets_released;
                }
                wait_for_gpu();
                std::printf("  vram %s/%s: features on: %.0f MiB used (feature targets %.1f MiB); 130 renders with them off: %.0f MiB used (feature targets %.1f MiB, %llu groups released)\n",
                            name.c_str(), camera_name.c_str(), on_mb, on_bytes / 1048576.0, gpu_used_mb(),
                            static_cast<double>(r3d.stats().feature_bytes) / 1048576.0, static_cast<unsigned long long>(released));
                pose(0);
                renderer.clear(kin::Color::rgb(0, 0, 0));
                r3d.render(view, scene);
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
                    r3d.render(big, scene);
                    const kin::Rectf big_rect{0.0f, 0.0f, static_cast<float>(big_size.x), static_cast<float>(big_size.y)};
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
            std::printf("  %-14s render() %7.3f ms  GPU %7.3f ms  (p10-p90 %.3f-%.3f)  %u draws, %llu triangles, %u shadow casters (%u cascades)\n",
                        camera_name.c_str(), cpu_s.median, gpu_s.median, gpu_s.p10, gpu_s.p90, stats.draws,
                        static_cast<unsigned long long>(stats.triangles), stats.shadow_casters, stats.shadow_cascades);
            if (stats.skinned_copies > 0) {
                std::printf("                 skinned: %u copies seen in %u draws, %u joint matrices uploaded, %u copies culled\n", stats.skinned_copies,
                            stats.skinned_draws, stats.skin_matrices, stats.culled);
            }
            if (stats.cluster_copies > 0 && stats.cluster_gpu) {
                const Summary cut_s = summarize(cut_ms);
                std::printf("                 clusters (GPU cut): %u copies, %llu clusters tested, %u compute dispatches, %.3f MB handed over, CPU set-up %.3f ms (render() %.3f)",
                            stats.cluster_copies, static_cast<unsigned long long>(stats.clusters_tested), stats.cluster_dispatches,
                            static_cast<double>(stats.cluster_upload_bytes) / 1048576.0, cut_s.median, cpu_s.median);
                if (over.readback) {
                    std::printf("; read back: %u clusters drawn, %llu triangles in the camera's cut, %llu gathered for all passes, %u clusters dropped",
                                stats.clusters_drawn, static_cast<unsigned long long>(stats.triangles),
                                static_cast<unsigned long long>(stats.cluster_triangles_all), stats.cluster_overflow);
                    if (stats.occlusion) {
                        std::printf("; occlusion: %u clusters in the first pass, %u occluded (%llu triangles), %u drawn for nothing (%llu triangles)",
                                    stats.clusters_first_pass, stats.clusters_occluded, static_cast<unsigned long long>(stats.triangles_occluded),
                                    stats.clusters_wasted, static_cast<unsigned long long>(stats.triangles_wasted));
                    }
                }
                if (stats.occlusion) {
                    std::printf("; occlusion on (%u pyramid levels, %u dispatches to build it)", stats.occlusion_levels, stats.occlusion_dispatches);
                }
                std::printf("\n");
            } else if (stats.cluster_copies > 0) {
                const Summary cut_s = summarize(cut_ms);
                std::printf("                 clusters: %u copies cut, %llu tested, %u drawn, %.2f MB of indices uploaded, cut %.3f ms (of which compacting and uploading %.3f; render() %.3f)\n",
                            stats.cluster_copies, static_cast<unsigned long long>(stats.clusters_tested), stats.clusters_drawn,
                            static_cast<double>(stats.cluster_upload_bytes) / 1048576.0, cut_s.median, summarize(cut_upload_ms).median, cpu_s.median);
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
            report << "\n   {\"name\": " << quoted(camera_name) << ", \"png\": " << quoted(fs::relative(png, out).generic_string())
                   << ", \"eye\": [" << eye.x << ", " << eye.y << ", " << eye.z << "], \"target\": [" << target.x << ", " << target.y
                   << ", " << target.z << "], \"fov\": " << fov
                   << ", \"first_ms\": " << first_ms << ", \"cpu_ms\": " << json_summary(cpu_s) << ", \"gpu_ms\": " << json_summary(gpu_s)
                   << ", \"draws\": " << stats.draws << ", \"instances\": " << stats.instances << ", \"culled\": " << stats.culled
                   << ", \"triangles\": " << stats.triangles << ", \"shadow_draws\": " << stats.shadow_draws
                   << ", \"shadow_casters\": " << stats.shadow_casters << ", \"shadow_cascades\": " << stats.shadow_cascades
                   << ", \"cascade_casters\": " << json_list(stats.cascade_casters) << ", \"ao_draws\": " << stats.ao_draws
                   << ", \"ao_instances\": " << stats.ao_instances
                   << ", \"ao_dispatches\": " << stats.ao_dispatches << ", \"contact_dispatches\": " << stats.contact_dispatches
                   << ", \"taa_dispatches\": " << stats.taa_dispatches << ", \"lights\": " << stats.lights;
            if (stats.cluster_copies > 0) {
                report << ", \"cluster_gpu\": " << (stats.cluster_gpu ? "true" : "false") << ", \"cluster_dispatches\": " << stats.cluster_dispatches
                       << ", \"cluster_overflow\": " << stats.cluster_overflow << ", \"cluster_copies\": " << stats.cluster_copies << ", \"clusters_tested\": " << stats.clusters_tested
                       << ", \"clusters_drawn\": " << stats.clusters_drawn << ", \"cluster_upload_bytes\": " << stats.cluster_upload_bytes
                       << ", \"cluster_ms\": " << json_summary(summarize(cut_ms))
                       << ", \"cluster_upload_ms\": " << json_summary(summarize(cut_upload_ms));
                if (stats.occlusion) {
                    report << ", \"occlusion\": true, \"clusters_first_pass\": " << stats.clusters_first_pass << ", \"clusters_occluded\": " << stats.clusters_occluded
                           << ", \"triangles_occluded\": " << stats.triangles_occluded << ", \"clusters_wasted\": " << stats.clusters_wasted
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
