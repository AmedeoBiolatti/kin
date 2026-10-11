#pragma once

// Procedural skinned characters for Sponza Walk (docs/3d_plan.md, P1a and P1b), made in code so that the viewer needs no
// download: make_character, a tube bound to a chain of three joints with three clips (P1a); and make_person, a person on 18
// joints (a leg and an arm each side, arms out, 1.95 m, facing +Z) with the same clip names, which the inverse kinematics section
// poses (P1b). P1c extends them (a real model).
#include <kin/render3d/ik.hpp>
#include <kin/render3d/model.hpp>
#include <kin/render3d/renderer3d.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace sponza_walk {
using namespace kin;

inline std::shared_ptr<const Model> make_character(Renderer3D& r3d, float length = 1.8f, float radius = 0.16f) {
    constexpr int rings = 33, segments = 16;
    const float third = length / 3.0f;
    MeshData mesh;
    for (int r = 0; r < rings; ++r) {
        const float y = length * float(r) / float(rings - 1);
        for (int s = 0; s <= segments; ++s) {
            const float a = 2.0f * 3.14159265f * float(s) / float(segments);
            const Vec3f n{std::cos(a), 0.0f, -std::sin(a)};
            mesh.vertices.push_back({{n.x * radius, y, n.z * radius}, n, {float(s) / float(segments), float(r) / float(rings - 1)}});
            float w[3];
            float sum = 0.0f;
            for (int j = 0; j < 3; ++j) {
                w[j] = std::max(0.0f, 1.0f - std::fabs(y - third * float(j)) / third);
                sum += w[j];
            }
            if (y > 2.0f * third) {
                w[0] = w[1] = 0.0f;
                w[2] = sum = 1.0f;
            }
            MeshSkinVertex k;
            k.joints = {0, 1, 2, 0};
            k.weights = {w[0] / sum, w[1] / sum, w[2] / sum, 0.0f};
            mesh.skin.push_back(k);
        }
    }
    const u32 row = segments + 1;
    for (u32 r = 0; r + 1 < u32(rings); ++r) {
        for (u32 s = 0; s < u32(segments); ++s) {
            const u32 a = r * row + s, b = a + 1, c = a + row + 1, d = a + row;
            mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
        }
    }
    auto model = std::make_shared<Model>();
    Skeleton sk;
    sk.names = {"hips", "spine", "head"};
    sk.parents = {-1, 0, 1};
    sk.rest.resize(3);
    sk.rest[1].translation = {0.0f, third, 0.0f};
    sk.rest[2].translation = {0.0f, third, 0.0f};
    sk.inverse_bind = {Mat4f{}, Mat4f::translation({0.0f, -third, 0.0f}), Mat4f::translation({0.0f, -2.0f * third, 0.0f})};
    sk.root_transform = Mat4f{};
    const auto swing = [&](const char* name, float duration, float a1, float a2) {
        AnimClip3D c;
        c.name = name;
        c.duration = duration;
        c.channels.resize(3);
        for (u32 j = 1; j <= 2; ++j) {
            const float deg = j == 1 ? a1 : a2;
            Track3D<Quatf>& t = c.channels[j].rotation;
            t.times = {0.0f, duration * 0.25f, duration * 0.5f, duration * 0.75f, duration};
            const Quatf z = Quatf::axis_angle({0, 0, 1}, 0.0f);
            t.values = {z, Quatf::axis_angle({0, 0, 1}, deg), z, Quatf::axis_angle({0, 0, 1}, -deg), z};
        }
        return c;
    };
    model->skeletons.push_back(sk);
    model->clips = {swing("Sway", 2.0f, 20.0f, 15.0f), swing("Bend", 1.2f, 45.0f, -35.0f), swing("Wave", 0.6f, 25.0f, 60.0f)};
    ModelPart part;
    part.name = "tube";
    part.mesh = r3d.create_mesh(mesh);
    part.material = {.color = Color::rgb(215, 130, 90), .roughness = 0.6f};
    part.skin = 0;
    model->parts.push_back(part);
    return model;
}

// What the person's constraints see of the ground, in the person's own (model) space: the floor at y = 0 and, optionally, a step
// (a box on it). The first thing the segment meets.
inline std::optional<GroundHit> floor_and_step(Vec3f from, Vec3f to, bool with_step, Vec3f step_lo, Vec3f step_hi) {
    const Vec3f d = to - from;
    float best = 2.0f;
    GroundHit hit;
    if (d.y < -1e-9f) {
        const float t = -from.y / d.y;
        if (t >= 0.0f && t <= 1.0f) {
            best = t;
            hit = {from + d * t, {0.0f, 1.0f, 0.0f}};
        }
    }
    if (with_step) {
        float t0 = 0.0f, t1 = 1.0f;
        Vec3f n{};
        bool inside = true;
        const float lo[3] = {step_lo.x, step_lo.y, step_lo.z}, hi[3] = {step_hi.x, step_hi.y, step_hi.z};
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
}

// A person: tubes (weights ramping to the next joint over the last 40% of a bone) and balls, on the joints named
// pelvis, spine, chest, neck, head, hip/knee/ankle and shoulder/elbow/hand _L and _R. Clips Sway (the spine), Bend (a bow) and
// Wave (the right arm), named as make_character's so that the viewer's animator drives either.
inline std::shared_ptr<const Model> make_person(Renderer3D& r3d) {
    struct Def {
        const char* name;
        int parent;
        Vec3f at;
    };
    const Def defs[] = {
        {"root", -1, {0, 0, 0}},          {"pelvis", 0, {0, 1.0f, 0}},       {"spine", 1, {0, 0.2f, 0}},        {"chest", 2, {0, 0.25f, 0}},
        {"neck", 3, {0, 0.25f, 0}},       {"head", 4, {0, 0.1f, 0}},         {"hip_L", 1, {0.1f, 0, 0}},        {"knee_L", 6, {0, -0.5f, 0}},
        {"ankle_L", 7, {0, -0.45f, 0}},   {"hip_R", 1, {-0.1f, 0, 0}},       {"knee_R", 9, {0, -0.5f, 0}},      {"ankle_R", 10, {0, -0.45f, 0}},
        {"shoulder_L", 3, {0.2f, 0.2f, 0}}, {"elbow_L", 12, {0.5f, 0, 0}},  {"hand_L", 13, {0.45f, 0, 0}},     {"shoulder_R", 3, {-0.2f, 0.2f, 0}},
        {"elbow_R", 15, {-0.5f, 0, 0}},   {"hand_R", 16, {-0.45f, 0, 0}},
    };
    Skeleton sk;
    std::vector<Vec3f> world;
    for (const Def& d : defs) {
        sk.names.emplace_back(d.name);
        sk.parents.push_back(d.parent);
        JointPose r;
        r.translation = d.at;
        sk.rest.push_back(r);
        world.push_back(d.at + (d.parent >= 0 ? world[std::size_t(d.parent)] : Vec3f{}));
    }
    for (const Vec3f& w : world) sk.inverse_bind.push_back(Mat4f::translation({-w.x, -w.y, -w.z}));
    const auto joint = [&](const char* name) { return sk.find(name); };

    MeshData mesh;
    const auto tube = [&](Vec3f p0, Vec3f p1, float r0, float r1, int ja, int jb, int rings = 8, float ramp = 0.6f) {
        constexpr int segments = 10;
        const Vec3f d = normalize(p1 - p0);
        const Vec3f helper = std::fabs(d.z) < 0.9f ? Vec3f{0, 0, 1} : Vec3f{1, 0, 0};
        const Vec3f u = normalize(cross(helper, d));
        const Vec3f v = cross(d, u);
        const u32 first = u32(mesh.vertices.size());
        for (int r = 0; r < rings; ++r) {
            const float t = float(r) / float(rings - 1);
            float w = std::clamp((t - ramp) / (1.0f - ramp), 0.0f, 1.0f);
            w = w * w * (3.0f - 2.0f * w);
            for (int s = 0; s <= segments; ++s) {
                const float a = 6.2831853f * float(s) / float(segments);
                const Vec3f n = u * std::cos(a) + v * std::sin(a);
                mesh.vertices.push_back({lerp(p0, p1, t) + n * (r0 + (r1 - r0) * t), n, {float(s) / float(segments), t}});
                MeshSkinVertex k;
                k.joints = {u8(ja), u8(jb), 0, 0};
                k.weights = {1.0f - w, w, 0.0f, 0.0f};
                mesh.skin.push_back(k);
            }
        }
        const u32 row = segments + 1;
        for (u32 r = 0; r + 1 < u32(rings); ++r) {
            for (u32 s = 0; s < u32(segments); ++s) {
                const u32 a = first + r * row + s, b = a + 1, c = a + row + 1, e = a + row;
                mesh.indices.insert(mesh.indices.end(), {a, c, b, a, e, c});
            }
        }
    };
    const auto ball = [&](Vec3f centre, float radius, int j) {
        constexpr int rings = 8, segments = 12;
        const u32 first = u32(mesh.vertices.size());
        for (int r = 0; r <= rings; ++r) {
            const float theta = 3.14159265f * float(r) / float(rings);
            for (int s = 0; s <= segments; ++s) {
                const float phi = 6.2831853f * float(s) / float(segments);
                const Vec3f n{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
                mesh.vertices.push_back({centre + n * radius, n, {float(s) / float(segments), float(r) / float(rings)}});
                MeshSkinVertex k;
                k.joints = {u8(j), 0, 0, 0};
                k.weights = {1.0f, 0.0f, 0.0f, 0.0f};
                mesh.skin.push_back(k);
            }
        }
        const u32 row = segments + 1;
        for (u32 r = 0; r < u32(rings); ++r) {
            for (u32 s = 0; s < u32(segments); ++s) {
                const u32 a = first + r * row + s, b = a + 1, c = a + row + 1, e = a + row;
                mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, e});
            }
        }
    };
    const auto bone = [&](const char* a, const char* b, float r0, float r1) {
        tube(world[std::size_t(joint(a))], world[std::size_t(joint(b))], r0, r1, joint(a), joint(b));
    };
    bone("pelvis", "spine", 0.12f, 0.115f);
    bone("spine", "chest", 0.115f, 0.13f);
    bone("chest", "neck", 0.13f, 0.05f);
    bone("neck", "head", 0.05f, 0.045f);
    ball(world[std::size_t(joint("head"))] + Vec3f{0, 0.07f, 0}, 0.105f, joint("head"));
    ball(world[std::size_t(joint("head"))] + Vec3f{0, 0.06f, 0.115f}, 0.032f, joint("head")); // a nose: which way the head faces
    for (const char* side : {"L", "R"}) {
        const std::string s = side;
        bone("pelvis", ("hip_" + s).c_str(), 0.08f, 0.075f);
        bone(("hip_" + s).c_str(), ("knee_" + s).c_str(), 0.075f, 0.055f);
        bone(("knee_" + s).c_str(), ("ankle_" + s).c_str(), 0.055f, 0.04f);
        const Vec3f ankle = world[std::size_t(joint(("ankle_" + s).c_str()))];
        tube(ankle + Vec3f{0, -0.03f, -0.05f}, ankle + Vec3f{0, -0.03f, 0.14f}, 0.04f, 0.035f, joint(("ankle_" + s).c_str()), joint(("ankle_" + s).c_str()), 2);
        bone("chest", ("shoulder_" + s).c_str(), 0.05f, 0.05f);
        bone(("shoulder_" + s).c_str(), ("elbow_" + s).c_str(), 0.05f, 0.04f);
        bone(("elbow_" + s).c_str(), ("hand_" + s).c_str(), 0.04f, 0.032f);
        ball(world[std::size_t(joint(("hand_" + s).c_str()))], 0.055f, joint(("hand_" + s).c_str()));
    }

    const u32 count = sk.joint_count();
    const auto clip = [&](const char* name, float duration, auto&& fill) {
        AnimClip3D c;
        c.name = name;
        c.duration = duration;
        c.channels.resize(count);
        fill(c);
        return c;
    };
    const auto swing = [&](AnimClip3D& c, const char* joint_name, Vec3f axis, float a, float duration) {
        Track3D<Quatf>& t = c.channels[std::size_t(sk.find(joint_name))].rotation;
        const Quatf z{};
        t.times = {0.0f, duration * 0.25f, duration * 0.5f, duration * 0.75f, duration};
        t.values = {z, Quatf::axis_angle(axis, a), z, Quatf::axis_angle(axis, -a), z};
    };
    auto model = std::make_shared<Model>();
    model->skeletons.push_back(sk);
    model->clips.push_back(clip("Sway", 2.5f, [&](AnimClip3D& c) {
        swing(c, "spine", {0, 0, 1}, 6.0f, 2.5f);
        swing(c, "chest", {1, 0, 0}, 3.0f, 2.5f);
        swing(c, "head", {0, 1, 0}, 8.0f, 2.5f);
    }));
    model->clips.push_back(clip("Bend", 2.0f, [&](AnimClip3D& c) {
        Track3D<Quatf>& t = c.channels[std::size_t(sk.find("spine"))].rotation;
        t.times = {0.0f, 1.0f, 2.0f};
        t.values = {Quatf{}, Quatf::axis_angle({1, 0, 0}, 40.0f), Quatf{}};
    }));
    model->clips.push_back(clip("Wave", 1.0f, [&](AnimClip3D& c) {
        Track3D<Quatf>& up = c.channels[std::size_t(sk.find("shoulder_R"))].rotation;
        up.times = {0.0f, 0.5f, 1.0f};
        up.values = {Quatf::axis_angle({0, 0, 1}, -70.0f), Quatf::axis_angle({0, 0, 1}, -85.0f), Quatf::axis_angle({0, 0, 1}, -70.0f)};
        Track3D<Quatf>& fore = c.channels[std::size_t(sk.find("elbow_R"))].rotation;
        fore.times = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
        fore.values = {Quatf::axis_angle({0, 1, 0}, 0.0f), Quatf::axis_angle({0, 1, 0}, 35.0f), Quatf::axis_angle({0, 1, 0}, 0.0f),
                       Quatf::axis_angle({0, 1, 0}, -35.0f), Quatf::axis_angle({0, 1, 0}, 0.0f)};
    }));
    ModelPart part;
    part.name = "person";
    part.mesh = r3d.create_mesh(mesh);
    part.material = {.color = Color::rgb(215, 150, 110), .roughness = 0.65f};
    part.skin = 0;
    model->parts.push_back(part);
    return model;
}

} // namespace sponza_walk
