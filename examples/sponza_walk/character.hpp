#pragma once

// A procedural skinned character for Sponza Walk (docs/3d_plan.md, P1a): a tube bound to a chain of three joints with
// three clips, made in code so that the viewer needs no download. It is what P1c extends (a real model, IK targets).
#include <kin/render3d/model.hpp>
#include <kin/render3d/renderer3d.hpp>

#include <cmath>
#include <memory>

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

} // namespace sponza_walk
