#include "climber.hpp"

#include "../sponza_walk/character.hpp"

#include <algorithm>
#include <cmath>

namespace summit {

ClimberStyle style_for_player(i32 index) {
    static const ClimberStyle styles[4] = {
        {Color::rgb(206, 62, 48), Color::rgb(46, 54, 74), Color::rgb(238, 196, 52), Color::rgb(70, 92, 126),
         Color::rgb(222, 164, 126), Color::rgb(70, 48, 32)},
        {Color::rgb(52, 112, 198), Color::rgb(120, 104, 78), Color::rgb(236, 236, 230), Color::rgb(206, 120, 40),
         Color::rgb(150, 100, 72), Color::rgb(30, 24, 20)},
        {Color::rgb(70, 160, 92), Color::rgb(70, 74, 80), Color::rgb(232, 120, 40), Color::rgb(60, 60, 70),
         Color::rgb(236, 190, 156), Color::rgb(168, 120, 60)},
        {Color::rgb(150, 82, 170), Color::rgb(36, 44, 60), Color::rgb(60, 190, 200), Color::rgb(220, 200, 80),
         Color::rgb(190, 134, 100), Color::rgb(20, 16, 14)},
    };
    return styles[std::size_t(((index % 4) + 4) % 4)];
}

// A person on person_rig()'s joints in parts of their own (skin, jacket, trousers, boots, hat, pack, gloves, goggles),
// proportions a little more of a climber than the fixture's tubes: broad chest, thick limbs, mitts, a beanie, a pack,
// no tail.
std::vector<ClimberPart> build_climber_meshes(const ClimberStyle& style) {
    const sponza_walk::PersonRig rig = sponza_walk::person_rig();
    const Skeleton3D& sk = rig.skeleton;
    const std::vector<Vec3f>& world = rig.world;
    const auto joint = [&](const char* name) { return sk.find(name); };
    const auto at = [&](const char* name) { return world[std::size_t(joint(name))]; };

    using PartBuild = ClimberPart;
    enum { Skin, Jacket, Trousers, Boots, Hat, Pack, Gloves, Hair, Goggles, Parts };
    PartBuild parts[Parts] = {
        {"skin", style.skin, 0.6f, {}},
        {"jacket", style.jacket, 0.7f, {}},
        {"trousers", style.trousers, 0.8f, {}},
        {"boots", Color::rgb(46, 38, 32), 0.7f, {}},
        {"hat", style.hat, 0.9f, {}},
        {"pack", style.pack, 0.75f, {}},
        {"gloves", Color::rgb(60, 60, 66), 0.8f, {}},
        {"hair", style.hair, 0.8f, {}},
        {"goggles", Color::rgb(24, 26, 34), 0.25f, {}},
    };
    // An elliptical tube from p0 to p1 (radii along the frame's u and v), weights ramping from ja to jb over the last
    // `1 - ramp`.
    const auto tube = [&](int part, Vec3f p0, Vec3f p1, Vec2f r0, Vec2f r1, int ja, int jb, int rings = 7,
                          f32 ramp = 0.55f, Vec3f across = {1, 0, 0}) {
        MeshData3D& mesh = parts[part].mesh;
        constexpr int segments = 12;
        const Vec3f d = normalize(p1 - p0);
        Vec3f u = across - d * dot(across, d);
        if (length(u) < 1e-4f)
            u = cross(Vec3f{0, 0, 1}, d);
        u = normalize(u);
        const Vec3f v = cross(d, u);
        const u32 first = u32(mesh.vertices.size());
        for (int r = 0; r < rings; ++r) {
            const f32 t = f32(r) / f32(rings - 1);
            f32 w = std::clamp((t - ramp) / (1.0f - ramp), 0.0f, 1.0f);
            w = w * w * (3.0f - 2.0f * w);
            for (int s = 0; s <= segments; ++s) {
                const f32 a = 6.2831853f * f32(s) / f32(segments);
                const Vec3f n = normalize(u * (std::cos(a) / std::max(1e-3f, r0.x + (r1.x - r0.x) * t)) +
                                          v * (std::sin(a) / std::max(1e-3f, r0.y + (r1.y - r0.y) * t)));
                const Vec3f pos = lerp(p0, p1, t) + u * (std::cos(a) * (r0.x + (r1.x - r0.x) * t)) +
                                  v * (std::sin(a) * (r0.y + (r1.y - r0.y) * t));
                mesh.vertices.push_back({pos, n, {f32(s) / f32(segments), t}});
                MeshSkinVertex3D k;
                k.joints = {u8(ja), u8(jb), 0, 0};
                k.weights = {1.0f - w, w, 0.0f, 0.0f};
                mesh.skin.push_back(k);
            }
        }
        const u32 row = segments + 1;
        for (u32 r = 0; r + 1 < u32(rings); ++r) {
            for (u32 s = 0; s < u32(segments); ++s) {
                const u32 a = first + r * row + s, b = a + 1, c = a + row + 1, e = a + row;
                // Anticlockwise seen from outside, as the engine's meshes and the blobs below (this ring order was
                // inside out).
                mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, e});
            }
        }
        // Flat caps, a fan from a centre over copies of the end ring (their own normal, -d at p0 and +d at p1, and the
        // ring's weights): the winding is fixed, so back faces are culled and an open end shows straight through the
        // limb.
        for (int end = 0; end < 2; ++end) {
            const u32 ring = first + (end == 0 ? 0u : u32(rings - 1)) * row;
            const Vec3f nrm = end == 0 ? -d : d;
            const u32 centre = u32(mesh.vertices.size());
            mesh.vertices.push_back({end == 0 ? p0 : p1, nrm, {0.5f, 0.5f}});
            mesh.skin.push_back(mesh.skin[ring]);
            for (u32 s = 0; s <= u32(segments); ++s) {
                MeshVertex3D v = mesh.vertices[ring + s];
                v.normal = nrm;
                mesh.vertices.push_back(v);
                mesh.skin.push_back(mesh.skin[ring + s]);
            }
            for (u32 s = 0; s < u32(segments); ++s) {
                const u32 a = centre + 1 + s, b = a + 1;
                // Anticlockwise seen from outside: the ring runs anticlockwise seen from +d.
                mesh.indices.insert(mesh.indices.end(), end == 0 ? std::initializer_list<u32>{centre, b, a}
                                                                 : std::initializer_list<u32>{centre, a, b});
            }
        }
    };
    // An ellipsoid bound to one joint; `top_only` keeps just the upper cap (a beanie), down to `theta_max` radians from
    // the pole.
    const auto blob = [&](int part, Vec3f centre, Vec3f radii, int j, f32 theta_max = 3.14159265f) {
        MeshData3D& mesh = parts[part].mesh;
        constexpr int rings = 8, segments = 14;
        const u32 first = u32(mesh.vertices.size());
        for (int r = 0; r <= rings; ++r) {
            const f32 theta = theta_max * f32(r) / f32(rings);
            for (int s = 0; s <= segments; ++s) {
                const f32 phi = 6.2831853f * f32(s) / f32(segments);
                const Vec3f n{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
                const Vec3f nn = normalize({n.x / radii.x, n.y / radii.y, n.z / radii.z});
                mesh.vertices.push_back({centre + Vec3f{n.x * radii.x, n.y * radii.y, n.z * radii.z},
                                         nn,
                                         {f32(s) / f32(segments), f32(r) / f32(rings)}});
                MeshSkinVertex3D k;
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
        if (theta_max < 3.14f) {
            // A dome's open rim would show the inside from below: close it with a flat disc facing down.
            const u32 ring = first + u32(rings) * row;
            const u32 hub = u32(mesh.vertices.size());
            const Vec3f nrm{0.0f, -1.0f, 0.0f};
            mesh.vertices.push_back({centre + Vec3f{0.0f, std::cos(theta_max) * radii.y, 0.0f}, nrm, {0.5f, 0.5f}});
            mesh.skin.push_back(mesh.skin[ring]);
            for (u32 s = 0; s <= u32(segments); ++s) {
                MeshVertex3D v = mesh.vertices[ring + s];
                v.normal = nrm;
                mesh.vertices.push_back(v);
                mesh.skin.push_back(mesh.skin[ring + s]);
            }
            for (u32 s = 0; s < u32(segments); ++s)
                mesh.indices.insert(mesh.indices.end(), {hub, hub + 1 + s, hub + 2 + s});
        }
    };

    const int pelvis = joint("pelvis"), spine = joint("spine"), chest = joint("chest"), neck = joint("neck"),
              head = joint("head");
    // Torso: hips, waist, ribs, shoulders (elliptical: broad and shallow).
    tube(Jacket, at("pelvis") + Vec3f{0, 0.04f, 0}, at("spine"), {0.17f, 0.115f}, {0.16f, 0.11f}, pelvis, spine);
    tube(Jacket, at("spine"), at("chest"), {0.16f, 0.11f}, {0.20f, 0.125f}, spine, chest);
    tube(Jacket, at("chest"), at("neck") + Vec3f{0, -0.03f, 0}, {0.20f, 0.125f}, {0.15f, 0.10f}, chest, neck);
    tube(Skin, at("neck") + Vec3f{0, -0.04f, 0}, at("head"), {0.052f, 0.052f}, {0.048f, 0.048f}, neck, head);
    // Collar: a roll of jacket round the neck.
    blob(Jacket, at("neck") + Vec3f{0, -0.01f, 0}, {0.085f, 0.04f, 0.085f}, neck);
    // Head, hair tuft, goggles, hat (a dome with a rolled brim and a pompom).
    const Vec3f hc = at("head") + Vec3f{0, 0.075f, 0.005f};
    blob(Skin, hc, {0.098f, 0.115f, 0.105f}, head);
    blob(Skin, hc + Vec3f{0, -0.03f, 0.1f}, {0.022f, 0.026f, 0.03f}, head); // nose
    blob(Hair, hc + Vec3f{0, -0.01f, -0.045f}, {0.1f, 0.1f, 0.085f}, head); // hair, back of the head
    blob(Goggles, hc + Vec3f{0, 0.025f, 0.075f}, {0.092f, 0.036f, 0.045f}, head);
    tube(Goggles, hc + Vec3f{0, 0.025f, 0}, hc + Vec3f{0, 0.025f, 0.001f}, {0.103f, 0.105f}, {0.103f, 0.105f}, head,
         head, 2);
    blob(Hat, hc + Vec3f{0, 0.035f, -0.005f}, {0.112f, 0.105f, 0.118f}, head, 1.95f);
    blob(Hat, hc + Vec3f{0, 0.14f, -0.01f}, {0.034f, 0.034f, 0.034f}, head);
    blob(Hat, hc + Vec3f{0, 0.07f, 0.0f}, {0.116f, 0.04f, 0.122f}, head,
         3.14159265f); // the rolled brim, a band round the head
    // The pack: a body on the back and a rolled mat on top.
    blob(Pack, at("chest") + Vec3f{0, -0.10f, -0.17f}, {0.16f, 0.22f, 0.10f}, chest);
    blob(Pack, at("chest") + Vec3f{0, 0.16f, -0.17f}, {0.15f, 0.05f, 0.075f}, chest);
    for (const char* side : {"L", "R"}) {
        const std::string s = side;
        const f32 sign = s == "L" ? 1.0f : -1.0f;
        const int hip = joint(("hip_" + s).c_str()), knee = joint(("knee_" + s).c_str()),
                  ankle = joint(("ankle_" + s).c_str()), toe = joint(("toe_" + s).c_str());
        const int sh = joint(("shoulder_" + s).c_str()), el = joint(("elbow_" + s).c_str()),
                  hand = joint(("hand_" + s).c_str());
        // Legs: trousers to the ankle, boots over the foot.
        tube(Trousers, at(("hip_" + s).c_str()) + Vec3f{0, 0.05f, 0}, at(("knee_" + s).c_str()), {0.088f, 0.092f},
             {0.066f, 0.066f}, hip, knee, 7, 0.7f, {1, 0, 0});
        tube(Trousers, at(("knee_" + s).c_str()), at(("ankle_" + s).c_str()) + Vec3f{0, 0.12f, 0}, {0.066f, 0.066f},
             {0.058f, 0.058f}, knee, ankle, 7, 0.7f);
        tube(Boots, at(("ankle_" + s).c_str()) + Vec3f{0, 0.14f, 0}, at(("ankle_" + s).c_str()) + Vec3f{0, -0.03f, 0},
             {0.062f, 0.062f}, {0.062f, 0.066f}, ankle, ankle, 3);
        tube(Boots, at(("ankle_" + s).c_str()) + Vec3f{0, -0.02f, -0.05f}, at(("toe_" + s).c_str()), {0.058f, 0.058f},
             {0.052f, 0.04f}, ankle, ankle, 2, 0.6f, {1, 0, 0});
        tube(Boots, at(("toe_" + s).c_str()), at(("ankle_" + s).c_str()) + Vec3f{0, -0.025f, 0.17f}, {0.052f, 0.04f},
             {0.04f, 0.028f}, toe, toe, 2, 0.6f, {1, 0, 0});
        // Arms: a shoulder ball, a sleeve to the wrist, a mitt.
        blob(Jacket, at(("shoulder_" + s).c_str()), {0.075f, 0.075f, 0.075f}, sh);
        tube(Jacket, at(("shoulder_" + s).c_str()), at(("elbow_" + s).c_str()), {0.066f, 0.066f}, {0.054f, 0.054f}, sh,
             el, 7, 0.6f, {0, 1, 0});
        tube(Jacket, at(("elbow_" + s).c_str()), at(("hand_" + s).c_str()) + Vec3f{-sign * 0.05f, 0, 0},
             {0.054f, 0.054f}, {0.046f, 0.046f}, el, hand, 7, 0.6f, {0, 1, 0});
        blob(Jacket, at(("elbow_" + s).c_str()), {0.058f, 0.058f, 0.058f}, el);
        blob(Gloves, at(("hand_" + s).c_str()) + Vec3f{sign * 0.0f, 0, 0}, {0.07f, 0.058f, 0.06f}, hand);
        blob(Skin, at(("hand_" + s).c_str()) + Vec3f{-sign * 0.05f, 0, 0}, {0.04f, 0.038f, 0.04f}, hand);
    }

    std::vector<ClimberPart> out;
    for (PartBuild& p : parts) {
        if (!p.mesh.vertices.empty())
            out.push_back(std::move(p));
    }
    return out;
}

} // namespace summit
