#include <kin/core/jobs.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/render3d/ik.hpp>
#include <kin/platform/app.hpp>
#include "skin_fixtures.hpp"

#include <kin/assets/image.hpp>
#include <kin/render3d/animation3d.hpp>
#include <kin/render3d/mesh_clusters.hpp>
#include <kin/render3d/mesh_lod.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/shadow_cascades.hpp>
#include <kin/render3d/timeofday3d.hpp>
#include <kin/render3d/taa.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/shader_compiler.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <bit>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using kin::Color;
using kin::Mat4f;
using kin::MeshData;
using kin::Vec3f;

bool gpu_tests_required() {
    const char* value = std::getenv("KIN_REQUIRE_GPU_TESTS");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

void skip_or_require_gpu_test(std::string_view test_name, const std::string& reason) {
    if (gpu_tests_required()) {
        throw std::runtime_error(std::string(test_name) + ": GPU test unavailable: " + reason);
    }
    std::cerr << "[SKIP] " << test_name << ": " << reason
              << " (set KIN_REQUIRE_GPU_TESTS=1 to require GPU coverage)\n";
}

// Renders and checks that it did, the call kept even if asserts are compiled out.
void render_ok(kin::Renderer3D& r3d, kin::View3D& view, const kin::Scene3D& scene) {
    const bool rendered = r3d.render(view, scene);
    assert(rendered);
    static_cast<void>(rendered);
}

// True where the device cannot do `what` (its capability is false: said, and the test is skipped). A device that
// can, whose pass did not run, is a failure of the test's own asserts, never a skip.
bool lacks(bool capable, const char* what) {
    if (!capable) {
        std::cerr << what << ": not available on this device, skipped\n";
    }
    return !capable;
}

std::unique_ptr<kin::Renderer2D> try_create_gpu_renderer(kin::Window& window, std::string& unavailable_reason) {
    try {
        std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
        if (!backend || backend->name() != "SDL_GPU") {
            unavailable_reason = "SDL_GPU backend was not selected";
            return {};
        }
        return std::make_unique<kin::Renderer2D>(std::move(backend));
    } catch (const std::exception& e) {
        unavailable_reason = e.what();
    }
    return {};
}

// Runs `test` with an SDL_GPU renderer over a hidden 160 x 120 window, or
// skips it (fails it with KIN_REQUIRE_GPU_TESTS) where there is none.
void with_gpu(std::string_view test_name, const std::function<void(kin::Renderer2D&)>& test) {
    bool gpu_ready = false;
    try {
        kin::App app{};
        kin::Window& window = app.create_window({.title = "render3d-gpu", .width = 160, .height = 120, .hidden = true});
        std::string reason;
        auto renderer = try_create_gpu_renderer(window, reason);
        if (!renderer) {
            skip_or_require_gpu_test(test_name, reason);
            return;
        }
        gpu_ready = true;
        test(*renderer);
    } catch (const std::exception& e) {
        if (gpu_ready || gpu_tests_required()) {
            throw;
        }
        skip_or_require_gpu_test(test_name, e.what());
    }
}

struct Pixels {
    std::vector<kin::u8> rgba;
    kin::Vec2i size{};

    Color at(int x, int y) const {
        assert(x >= 0 && y >= 0 && x < size.x && y < size.y);
        const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x)) * 4u;
        return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    }
    Color at(kin::Vec2f p) const { return at(static_cast<int>(p.x), static_cast<int>(p.y)); }
};

Pixels read(kin::Renderer2D& renderer) {
    Pixels p;
    const bool ok = renderer.read_rgba({0.0f, 0.0f, 160.0f, 120.0f}, p.rgba, p.size);
    assert(ok && p.size.x == 160 && p.size.y == 120);
    return p;
}

bool near(Color a, Color b, int tolerance = 3) {
    const auto close = [&](kin::u8 x, kin::u8 y) { return std::abs(static_cast<int>(x) - static_cast<int>(y)) <= tolerance; };
    return close(a.r, b.r) && close(a.g, b.g) && close(a.b, b.b);
}

std::ostream& operator<<(std::ostream& out, Color c) {
    return out << '(' << int(c.r) << ", " << int(c.g) << ", " << int(c.b) << ", " << int(c.a) << ')';
}

void expect(Color got, Color want, std::string_view what, int tolerance = 3) {
    if (!near(got, want, tolerance)) {
        std::cerr << what << ": got " << got << ", want " << want << '\n';
        assert(false);
    }
}

// --- Cascaded shadow maps (G2a) ---

// The splits are increasing, end at the distance (never past the far plane),
// and the log blend gives the near cascades less than the linear one.
void test_cascade_splits() {
    const auto s = kin::cascade_splits(0.1f, 100.0f, 4);
    assert(s[0] == 0.1f && s[4] == 100.0f);
    for (int i = 0; i < 4; ++i) {
        assert(s[i + 1] > s[i]);
    }
    assert(s[1] < 25.0f && s[2] < 50.0f && s[3] < 75.0f); // nearer than a linear split
    const auto one = kin::cascade_splits(0.5f, 40.0f, 1);
    assert(one[0] == 0.5f && one[1] == 40.0f);
    const auto log_only = kin::cascade_splits(1.0f, 16.0f, 4, 1.0f);
    assert(std::abs(log_only[1] - 2.0f) < 1e-3f && std::abs(log_only[2] - 4.0f) < 1e-3f && std::abs(log_only[3] - 8.0f) < 1e-3f);
    const auto linear = kin::cascade_splits(0.0f, 100.0f, 4, 0.0f);
    assert(std::abs(linear[2] - 50.0f) < 0.1f);
}

// Where a world point lands in a cascade, in texels of a map `res` square.
kin::Vec3f in_texels(const kin::ShadowCascade& c, Vec3f world, float res) {
    const Vec3f ndc = c.view_projection.apply_projective(world);
    return {(ndc.x * 0.5f + 0.5f) * res, (0.5f - ndc.y * 0.5f) * res, ndc.z};
}

// Each cascade holds its slice of the view (corners inside its box), they
// overlap by the blend, the distance is clamped to the far plane, and the
// grid is snapped: moving the camera shifts the picture by whole texels.
void test_cascade_fit() {
    kin::Camera3D camera = kin::Camera3D::look_at({3.0f, 5.0f, 20.0f}, {0.0f, 0.0f, -10.0f}, 60.0f);
    camera.z_near = 0.1f;
    const kin::Bounds3 area{{-200.0f, -1.0f, -200.0f}, {200.0f, 30.0f, 200.0f}};
    const Vec3f to_sun = kin::normalize(Vec3f{1.0f, 1.2f, 0.3f});
    const float aspect = 16.0f / 9.0f;
    const float res = 2048.0f;
    const kin::ShadowCascades plan = kin::fit_shadow_cascades(camera, aspect, to_sun, area, 4, 120.0f, 2048);
    assert(plan.count == 4);
    for (kin::u32 k = 0; k < 4; ++k) {
        const kin::ShadowCascade& c = plan.cascade[k];
        assert(c.slice_far > c.slice_near && c.texel_world > 0.0f && c.depth_range > 0.0f);
        if (k > 0) {
            // It starts where the one before starts to fade out.
            assert(c.slice_near == plan.cascade[k - 1].blend_from && c.slice_near < plan.cascade[k - 1].slice_far);
            assert(plan.cascade[k - 1].blend_from > plan.cascade[k - 1].slice_near);
        }
        // Every corner of the slice is inside the cascade's box (texels in 0..res, depth in 0..1).
        for (const float d : {c.slice_near, c.slice_far}) {
            const float hh = d * std::tan(30.0f * 3.14159265f / 180.0f), hw = hh * aspect;
            for (int i = 0; i < 4; ++i) {
                const Vec3f world = camera.position + camera.forward() * d + camera.right() * ((i & 1) ? hw : -hw) +
                                    camera.up() * ((i & 2) ? hh : -hh);
                const Vec3f t = in_texels(c, world, res);
                assert(t.x >= -0.5f && t.x <= res + 0.5f && t.y >= -0.5f && t.y <= res + 0.5f && t.z > 0.0f && t.z < 1.0f);
            }
        }
        // Further cascades are coarser (more world per texel).
        if (k > 0) {
            assert(c.texel_world > plan.cascade[k - 1].texel_world);
        }
    }
    assert(plan.cascade[3].slice_far == 120.0f && plan.cascade[3].blend_from == plan.cascade[3].slice_far);

    // Clamped to the far plane; and a single cascade covers it all.
    camera.z_far = 50.0f;
    assert(kin::fit_shadow_cascades(camera, aspect, to_sun, area, 3, 500.0f, 2048).cascade[2].slice_far == 50.0f);
    assert(kin::fit_shadow_cascades(camera, aspect, to_sun, area, 9, 500.0f, 2048).count == 4); // capped
    assert(kin::fit_shadow_cascades(camera, aspect, to_sun, area, 1, 500.0f, 2048).count == 1);
    assert(kin::fit_shadow_cascades(camera, aspect, to_sun, kin::Bounds3{}, 4, 100.0f, 2048).count == 0); // no area, none
    camera.z_far = std::numeric_limits<float>::infinity();

    // Snapping: the camera moved by a fraction of a texel (and turned a
    // little) shifts every point's place in each map by a whole number of
    // texels, and leaves its scale alone, so edges don't crawl.
    const Vec3f probe{7.3f, 0.4f, -12.9f};
    kin::Camera3D moved = camera;
    moved.position = camera.position + Vec3f{0.0137f, 0.0f, -0.0291f};
    moved.rotation = kin::Quatf::yaw_pitch(0.4f, -0.3f);
    const kin::ShadowCascades after = kin::fit_shadow_cascades(moved, aspect, to_sun, area, 4, 120.0f, 2048);
    for (kin::u32 k = 0; k < 4; ++k) {
        const Vec3f a = in_texels(plan.cascade[k], probe, res), b = in_texels(after.cascade[k], probe, res);
        assert(std::abs(plan.cascade[k].texel_world - after.cascade[k].texel_world) < 1e-5f);
        for (const float shift : {b.x - a.x, b.y - a.y}) {
            assert(std::abs(shift - std::round(shift)) < 0.02f);
        }
    }
}

// --- Meshes, scenes and cameras: no GPU ---

Vec3f geometric_normal(const MeshData& m, std::size_t t) {
    const Vec3f a = m.vertices[m.indices[t * 3]].position;
    const Vec3f b = m.vertices[m.indices[t * 3 + 1]].position;
    const Vec3f c = m.vertices[m.indices[t * 3 + 2]].position;
    return kin::cross(b - a, c - a);
}
Vec3f centroid(const MeshData& m, std::size_t t) {
    return (m.vertices[m.indices[t * 3]].position + m.vertices[m.indices[t * 3 + 1]].position +
            m.vertices[m.indices[t * 3 + 2]].position) / 3.0f;
}

// Every triangle counter-clockwise from the side its normals point to, and
// (for convex shapes) facing away from `inside`, a point within.
void check_winding(const MeshData& m, std::string_view name, bool outward, Vec3f inside = {}) {
    std::string error;
    if (!m.validate(&error)) {
        std::cerr << name << ": " << error << '\n';
        assert(false);
    }
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        const Vec3f n = geometric_normal(m, t);
        if (kin::length(n) < 1e-9f) {
            continue; // degenerate (a pole)
        }
        const Vec3f vn = m.vertices[m.indices[t * 3]].normal + m.vertices[m.indices[t * 3 + 1]].normal +
                         m.vertices[m.indices[t * 3 + 2]].normal;
        if (kin::dot(n, vn) <= 0.0f || (outward && kin::dot(n, centroid(m, t) - inside) <= 0.0f)) {
            std::cerr << name << ": triangle " << t << " faces the wrong way\n";
            assert(false);
        }
        for (int k = 0; k < 3; ++k) {
            assert(std::abs(kin::length(m.vertices[m.indices[t * 3 + k]].normal) - 1.0f) < 1e-3f);
        }
    }
}

void test_meshes_face_out() {
    check_winding(kin::meshes::box({1.0f, 2.0f, 3.0f}), "box", true);
    check_winding(kin::meshes::sphere(1.5f), "sphere", true);
    check_winding(kin::meshes::cylinder(1.0f, 2.0f), "cylinder", true);
    check_winding(kin::meshes::cone(1.0f, 2.0f), "cone", true);
    check_winding(kin::meshes::octahedron(1.0f), "octahedron", true);
    check_winding(kin::meshes::star_prism(5, 1.0f, 0.45f, 0.3f), "star", true);
    check_winding(kin::meshes::plane({4.0f, 2.0f}, 3), "plane", false);
    check_winding(kin::meshes::wedge({2.0f, 1.0f, 3.0f}), "wedge", true, {0.0f, -0.25f, -0.75f});
    // The ramp: its top along the -Z edge, the floor along +Z.
    const kin::Bounds3 ramp = kin::meshes::wedge({2.0f, 1.0f, 3.0f}).bounds();
    assert(ramp.min == (Vec3f{-1.0f, -0.5f, -1.5f}) && ramp.max == (Vec3f{1.0f, 0.5f, 1.5f}));
    // The torus faces away from its tube's centre line.
    const MeshData torus = kin::meshes::torus(2.0f, 0.5f);
    check_winding(torus, "torus", false);
    for (std::size_t t = 0; t < torus.triangle_count(); ++t) {
        const Vec3f c = centroid(torus, t);
        const Vec3f tube = kin::normalize(Vec3f{c.x, 0.0f, c.z}) * 2.0f;
        assert(kin::dot(geometric_normal(torus, t), c - tube) > 0.0f);
    }
    // A plane faces up.
    const MeshData plane = kin::meshes::plane({4.0f, 2.0f}, 3);
    for (std::size_t t = 0; t < plane.triangle_count(); ++t) {
        assert(geometric_normal(plane, t).y > 0.0f);
    }
    assert(plane.triangle_count() == 18 && plane.vertices.size() == 16);

    const kin::Bounds3 box = kin::meshes::box({1.0f, 2.0f, 3.0f}).bounds();
    assert(box.min == (Vec3f{-0.5f, -1.0f, -1.5f}) && box.max == (Vec3f{0.5f, 1.0f, 1.5f}));
    assert(kin::meshes::box({1.0f, 1.0f, 1.0f}).triangle_count() == 12);
    assert(std::abs(kin::meshes::sphere(2.0f).bounds().radius() - std::sqrt(12.0f)) < 0.05f);
}

void test_mesh_validation_and_append() {
    MeshData m;
    std::string error;
    assert(!m.validate(&error) && !error.empty());
    m.vertices.resize(4);
    assert(!m.validate(&error)); // four vertices, no indices: not whole triangles
    m.indices = {0, 1, 2, 0, 2, 4};
    assert(!m.validate(&error) && error.find("past") != std::string::npos);
    m.indices = {0, 1, 2, 0, 2, 3};
    assert(m.validate());
    m.vertices[3].position.x = std::nanf("");
    assert(!m.validate(&error));

    // Appending moves the shape and turns its normals; a mirror keeps it facing out.
    MeshData both;
    both.append(kin::meshes::box({1.0f, 1.0f, 1.0f}), Mat4f::translation({3.0f, 0.0f, 0.0f}));
    both.append(kin::meshes::box({1.0f, 1.0f, 1.0f}), Mat4f::scaling({-1.0f, 1.0f, 1.0f}) * Mat4f::translation({3.0f, 0.0f, 0.0f}));
    assert(both.vertices.size() == 48 && both.indices.size() == 72 && both.validate());
    for (std::size_t t = 0; t < both.triangle_count(); ++t) {
        const Vec3f n = geometric_normal(both, t);
        const Vec3f offset = centroid(both, t) - Vec3f{t < 12 ? 3.0f : -3.0f, 0.0f, 0.0f};
        assert(kin::dot(n, offset) > 0.0f);
        assert(kin::dot(n, both.vertices[both.indices[t * 3]].normal) > 0.0f);
    }
    // Unindexed data appended to indexed: the result is indexed.
    MeshData loose;
    loose.vertices.resize(3);
    loose.vertices[1].position = {1.0f, 0.0f, 0.0f};
    loose.vertices[2].position = {0.0f, 1.0f, 0.0f};
    MeshData mixed = loose;
    mixed.append(kin::meshes::box({1.0f, 1.0f, 1.0f}));
    mixed.append(loose);
    assert(mixed.validate() && mixed.triangle_count() == 14 && mixed.indices.size() == 42);
}

void test_scene_merges_draws() {
    // Meshes for the scene: not made on a GPU, but distinct.
    struct Fake final : kin::IMeshBackend {};
    const kin::Mesh a{std::make_shared<Fake>(), 3, 3, {}};
    const kin::Mesh b{std::make_shared<Fake>(), 3, 3, {}};
    kin::Scene3D scene;
    scene.draw(a, Mat4f::translation({1.0f, 0.0f, 0.0f}));
    scene.draw(a, Mat4f::translation({2.0f, 0.0f, 0.0f}), {}, Color::rgb(255, 0, 0));
    scene.draw(a, Mat4f::identity(), {.emissive = 1.0f}); // another material: another draw
    scene.draw(b, Mat4f::identity(), {.emissive = 1.0f});
    const std::vector<Mat4f> transforms(5);
    scene.draw_instanced(b, transforms, {.emissive = 1.0f}); // joins the last
    scene.draw_instanced(a, std::span<const kin::MeshInstance>{});
    assert(scene.draws().size() == 3);
    assert(scene.draws()[0].count == 2 && scene.draws()[0].first == 0);
    assert(scene.draws()[1].count == 1 && scene.draws()[1].first == 2);
    assert(scene.draws()[2].count == 6 && scene.draws()[2].first == 3);
    assert(scene.instances().size() == 9 && scene.instances()[1].tint == Color::rgb(255, 0, 0));
    const auto room = scene.add_instances(a, 4);
    assert(room.size() == 4 && scene.draws().size() == 4 && scene.instances().size() == 13);
    assert(scene.add_instances(a, 0).empty() && scene.draws().size() == 4);
    scene.clear();
    assert(scene.draws().empty() && scene.instances().empty());
}

void test_cameras() {
    // Orbit: from yaw 0 the camera sits on +Z, `pitch` above, looking at the target.
    const kin::Camera3D orbit = kin::Camera3D::orbit({1.0f, 0.0f, 0.0f}, 0.0f, 30.0f, 10.0f);
    assert(std::abs(orbit.position.x - 1.0f) < 1e-4f);
    assert(std::abs(orbit.position.y - 5.0f) < 1e-3f);
    assert(std::abs(orbit.position.z - std::sqrt(75.0f)) < 1e-3f);
    const kin::Rectf rect{0.0f, 0.0f, 400.0f, 200.0f};
    const auto target = orbit.project({1.0f, 0.0f, 0.0f}, rect);
    assert(target && std::abs(target->pos.x - 200.0f) < 0.01f && std::abs(target->pos.y - 100.0f) < 0.01f);
    // Yaw 90: from +X.
    const kin::Camera3D side = kin::Camera3D::orbit({}, 90.0f, 0.0f, 4.0f);
    assert(std::abs(side.position.x - 4.0f) < 1e-3f && std::abs(side.position.z) < 1e-3f);

    const kin::Camera3D look = kin::Camera3D::look_at({0.0f, 10.0f, 10.0f}, {0.0f, 0.0f, 0.0f}, 45.0f);
    assert(kin::length(look.forward() - kin::normalize(Vec3f{0.0f, -1.0f, -1.0f})) < 1e-4f);
    assert(look.up().y > 0.0f && std::abs(look.right().x - 1.0f) < 1e-4f);
    // The ray through the centre meets the ground at the target; through
    // the top it meets further away (smaller z).
    const auto ground = kin::hit_plane_y(look.ray({200.0f, 100.0f}, rect));
    assert(ground && kin::length(*ground) < 1e-3f);
    const auto further = kin::hit_plane_y(look.ray({200.0f, 60.0f}, rect));
    assert(further && further->z < -0.5f);
    // Up into the sky, from a camera looking level: nothing.
    const kin::Camera3D level = kin::Camera3D::look_at({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, -10.0f}, 45.0f);
    assert(!kin::hit_plane_y(level.ray({200.0f, 0.0f}, rect)));
    assert(kin::hit_plane_y(level.ray({200.0f, 199.0f}, rect)));

    // Orthographic: `height` world units top to bottom.
    kin::Camera3D ortho = kin::Camera3D::look_at({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f});
    ortho.projection = kin::Projection3D::Orthographic;
    ortho.height = 4.0f;
    const auto top = ortho.project({0.0f, 2.0f, -50.0f}, rect);
    assert(top && std::abs(top->pos.y) < 0.01f && std::abs(top->pos.x - 200.0f) < 0.01f);
    const auto right_edge = ortho.project({4.0f, 0.0f, 0.0f}, rect); // twice as wide as high
    assert(right_edge && std::abs(right_edge->pos.x - 400.0f) < 0.01f);
}

// Entities: transforms composed through the hierarchy, and meshes collected
// into one instanced draw a mesh and material.
void test_ecs_entities() {
    kin::EcsWorld world;
    kin::WorldRenderState3D state{world};
    struct Fake final : kin::IMeshBackend {};
    const kin::Mesh a{std::make_shared<Fake>(), 3, 3, {}};
    const kin::Mesh b{std::make_shared<Fake>(), 3, 3, {}};
    auto& w = world.raw();
    const flecs::entity parent = w.entity().set(kin::Transform3D{.position = {10.0f, 0.0f, 0.0f},
                                                                 .rotation = kin::Quatf::axis_angle({0, 1, 0}, 90.0f),
                                                                 .scale = {2.0f, 2.0f, 2.0f}});
    const flecs::entity child = w.entity().child_of(parent).set(kin::Transform3D{.position = {1.0f, 0.0f, 0.0f}})
                                    .set(kin::MeshRenderer{.mesh = a});
    const flecs::entity grandchild = w.entity().child_of(child).set(kin::Transform3D{.position = {0.0f, 1.0f, 0.0f}})
                                         .set(kin::MeshRenderer{.mesh = a, .tint = Color::rgb(255, 0, 0)});
    w.entity().set(kin::Transform3D{}).set(kin::MeshRenderer{.mesh = b});
    w.entity().set(kin::Transform3D{}).set(kin::MeshRenderer{.mesh = a, .material = {.emissive = 1.0f}});
    w.entity().set(kin::Transform3D{}).set(kin::MeshRenderer{.mesh = a, .visible = false});
    state.propagate_transforms();
    // The child: 1 along the parent's x, turned 90 degrees (to -z), twice as far.
    const Vec3f child_at = child.get<kin::WorldTransform3D>()->position();
    assert(kin::length(child_at - Vec3f{10.0f, 0.0f, -2.0f}) < 1e-4f);
    const Vec3f grandchild_at = grandchild.get<kin::WorldTransform3D>()->position();
    assert(kin::length(grandchild_at - Vec3f{10.0f, 2.0f, -2.0f}) < 1e-4f); // the parent scales it once

    kin::Scene3D scene;
    state.collect(scene);
    assert(scene.draws().size() == 3 && scene.instances().size() == 4);
    std::size_t with_a = 0;
    for (const auto& draw : scene.draws()) {
        if (draw.mesh == a && draw.material == kin::Material3D{}) {
            with_a = draw.count;
        }
    }
    assert(with_a == 2);
    // Moved, then collected again: the new place, the same draws.
    child.get_mut<kin::Transform3D>()->position = {0.0f, 0.0f, 0.0f};
    state.propagate_transforms();
    assert(kin::length(grandchild.get<kin::WorldTransform3D>()->position() - Vec3f{10.0f, 2.0f, 0.0f}) < 1e-4f);
    scene.clear();
    state.collect(scene);
    assert(scene.draws().size() == 3 && scene.instances().size() == 4);
}

// --- Without SDL_GPU ---

// SDL_Renderer has no depth buffer: no 3D. Meshes come back empty; a view is
// a render target that shows its background.
void test_sdl_renderer_has_no_3d() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "render3d-sdl", .width = 160, .height = 120, .hidden = true});
    kin::Renderer2D renderer{window};
    assert(renderer.backend_name() != "SDL_GPU");
    assert(!renderer.capabilities().rendering_3d);
    assert(renderer.renderer_3d() == nullptr);

    kin::Renderer3D r3d{renderer};
    assert(!r3d.available());
    assert(&r3d.renderer() == &renderer);
    const kin::Renderer3DCapabilities caps = r3d.capabilities();
    assert(caps.depth_bits == 0 && caps.max_samples == 1 && caps.max_samples_hdr == 1);
    assert(!r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f})).valid());

    kin::View3D view = r3d.create_view({40, 30}, {.background = Color::rgb(0, 128, 0)});
    assert(view.valid() && view.size() == (kin::Vec2i{40, 30}) && view.samples() == 1);
    kin::Scene3D scene;
    scene.draw(kin::Mesh{}, Mat4f::identity());
    renderer.clear(kin::colors::black);
    const bool drew = r3d.render(view, scene);
    assert(!drew); // nothing drawn, but the background is there
    renderer.draw_texture(view.texture(), {0.0f, 0.0f, 40.0f, 30.0f});
    std::vector<kin::u8> rgba;
    kin::Vec2i size{};
    if (renderer.read_rgba({0.0f, 0.0f, 160.0f, 120.0f}, rgba, size)) {
        const Pixels p{rgba, size};
        expect(p.at(20, 15), Color::rgb(0, 128, 0), "SDL fallback background");
        expect(p.at(100, 80), kin::colors::black, "outside the view");
    }
    assert(r3d.resize(view, {20, 10}) && view.size() == (kin::Vec2i{20, 10}));
    renderer.present();
}

// --- On SDL_GPU ---

constexpr kin::Rectf screen{0.0f, 0.0f, 160.0f, 120.0f};

// A box mesh of `size`, made on `r3d`.
kin::Mesh make_box(kin::Renderer3D& r3d, Vec3f size) {
    return r3d.create_mesh(kin::meshes::box(size));
}

// The room the occlusion and contact-shadow tests stand things in: a floor (its top at y = 0), a wall behind
// (its front at z = -2) and a pole, all light grey.
struct AoRoom {
    explicit AoRoom(kin::Renderer3D& r3d)
        : floor(make_box(r3d, {20.0f, 0.2f, 20.0f})), wall(make_box(r3d, {20.0f, 4.0f, 0.2f})), pole(make_box(r3d, {0.2f, 1.0f, 0.2f})) {}

    void draw_floor(kin::Scene3D& scene) const { scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), grey); }
    void draw_wall(kin::Scene3D& scene) const { scene.draw(wall, Mat4f::translation({0.0f, 2.0f, -2.1f}), grey); }
    void draw_pole(kin::Scene3D& scene, float x, float z) const { scene.draw(pole, Mat4f::translation({x, 0.5f, z}), grey); }

    kin::Mesh floor, wall, pole;
    kin::Material3D grey{.color = Color::rgb(200, 200, 200)};
};

// Renders `scene` into `view` over a black window, draws the view over all of it and reads the window back.
// It does not present: some tests look at the picture first.
Pixels shoot_view(kin::Renderer2D& renderer, kin::Renderer3D& r3d, kin::View3D& view, const kin::Scene3D& scene) {
    renderer.clear(kin::colors::black);
    render_ok(r3d, view, scene);
    renderer.draw_texture(view.texture(), screen);
    return read(renderer);
}

// A view larger than the 160 x 120 window, rendered once and read back tile by tile at 1:1 (`stride` 1 reads every tile, 3 every third in
// each direction; the tiles not read stay zero, in every picture alike).
Pixels shoot_large(kin::Renderer2D& renderer, kin::Renderer3D& r3d, kin::View3D& view, const kin::Scene3D& scene, int stride = 1) {
    renderer.clear(kin::colors::black);
    render_ok(r3d, view, scene);
    Pixels out;
    out.size = view.size();
    out.rgba.assign(static_cast<std::size_t>(out.size.x) * static_cast<std::size_t>(out.size.y) * 4u, 0);
    for (int ty = 0; ty * 120 < out.size.y; ty += stride) {
        for (int tx = 0; tx * 160 < out.size.x; tx += stride) {
            const int w = std::min(160, out.size.x - tx * 160), h = std::min(120, out.size.y - ty * 120);
            renderer.clear(kin::colors::black);
            renderer.draw_texture(view.texture(), {static_cast<float>(tx * 160), static_cast<float>(ty * 120), static_cast<float>(w), static_cast<float>(h)},
                                  {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
            std::vector<kin::u8> tile;
            kin::Vec2i tile_size{};
            const bool ok = renderer.read_rgba({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)}, tile, tile_size);
            assert(ok && tile_size.x == w && tile_size.y == h);
            for (int y = 0; y < h; ++y) {
                std::memcpy(&out.rgba[(static_cast<std::size_t>(ty * 120 + y) * static_cast<std::size_t>(out.size.x) + static_cast<std::size_t>(tx * 160)) * 4u],
                            &tile[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4u], static_cast<std::size_t>(w) * 4u);
            }
        }
    }
    return out;
}

// SDL_GPU reports 3D views, with a depth buffer and the samples it has.
void test_gpu_reports_3d() {
    with_gpu("test_gpu_reports_3d", [](kin::Renderer2D& renderer) {
        assert(renderer.capabilities().rendering_3d);
        const kin::Renderer3D r3d{renderer};
        assert(r3d.available());
        assert(renderer.renderer_3d() == renderer.renderer_3d()); // made once, kept
        const kin::Renderer3DCapabilities caps = r3d.capabilities();
        assert(caps.depth_bits == 32 || caps.depth_bits == 24 || caps.depth_bits == 16);
        assert(caps.depth_float == (caps.depth_bits == 32));
        for (const kin::u32 samples : {caps.max_samples, caps.max_samples_hdr}) {
            assert(samples == 1 || samples == 2 || samples == 4 || samples == 8);
        }
        std::cerr << "3D: depth " << caps.depth_bits << (caps.depth_float ? " float" : " unorm") << ", samples "
                  << caps.max_samples << " (HDR " << caps.max_samples_hdr << ")\n";
    });
}

// A red box in front of a blue one, whichever is drawn first; the background
// shows through where neither is.
void test_gpu_depth_and_colour() {
    with_gpu("test_gpu_depth_and_colour", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        assert(box.valid() && box.triangle_count() == 12 && box.vertex_count() == 24);
        const kin::Material3D red{.color = Color::rgb(200, 40, 40), .unlit = true};
        const kin::Material3D blue{.color = Color::rgb(40, 40, 200), .unlit = true};
        for (const kin::u32 samples : {1u, 4u}) {
            kin::View3D view = r3d.create_view({160, 120}, {.samples = samples});
            assert(view.valid() && view.size() == (kin::Vec2i{160, 120}) && view.samples() <= samples);
            for (const bool red_first : {true, false}) {
                kin::Scene3D scene;
                scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
                const auto add_red = [&] { scene.draw(box, Mat4f::identity(), red); };
                const auto add_blue = [&] { scene.draw(box, Mat4f::translation({0.0f, 0.0f, -3.0f}) * Mat4f::scaling({3.0f, 3.0f, 3.0f}), blue); };
                if (red_first) {
                    add_red();
                    add_blue();
                } else {
                    add_blue();
                    add_red();
                }
                const Pixels p = shoot_view(renderer, r3d, view, scene);
                assert(r3d.stats().draws == 2 && r3d.stats().instances == 2 && r3d.stats().triangles == 24);
                expect(p.at(80, 60), red.color, "the near box at the centre");
                const auto blue_spot = scene.camera.project({1.0f, 0.9f, -1.5f}, screen);
                assert(blue_spot);
                expect(p.at(blue_spot->pos), blue.color, "the far box round it");
                expect(p.at(2, 2), kin::colors::black, "the background in the corner");
                renderer.present();
            }
        }
    });
}

// Lit from +X with no ambient light: the face toward the sun at full colour,
// the one away from it dark; emissive light shows without any.
void test_gpu_lighting() {
    with_gpu("test_gpu_lighting", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({3.0f, 1.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, 50.0f);
        scene.lights.sun = {.direction = {1.0f, 0.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}};
        scene.lights.ambient = {0.0f, 0.0f, 0.0f};
        scene.draw(box, Mat4f::identity(), {.color = Color::rgb(180, 180, 180)});
        Pixels p = shoot_view(renderer, r3d, view, scene);
        const auto lit = scene.camera.project({0.5f, 0.0f, 0.0f}, screen);
        const auto dark = scene.camera.project({0.0f, 0.0f, 0.5f}, screen);
        assert(lit && dark);
        expect(p.at(lit->pos), Color::rgb(180, 180, 180), "the face toward the sun");
        expect(p.at(dark->pos), Color::rgb(0, 0, 0), "the face at right angles to it");
        renderer.present();

        // The sun behind it, but half its own light: half its colour in linear light.
        scene.clear();
        scene.lights.sun.direction = {-1.0f, 0.0f, 0.0f};
        scene.draw(box, Mat4f::identity(), {.color = Color::rgb(180, 180, 180), .emissive = 0.5f});
        p = shoot_view(renderer, r3d, view, scene);
        const kin::u8 half = static_cast<kin::u8>(std::lround(255.0f * kin::linear_to_srgb(0.5f * kin::srgb_to_linear(180.0f / 255.0f))));
        expect(p.at(lit->pos), Color::rgb(half, half, half), "emissive light alone");
        renderer.present();
    });
}

// Many copies in one draw, each placed and tinted; a second draw's copies
// start further into the instance buffer.
void test_gpu_instances() {
    with_gpu("test_gpu_instances", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {0.8f, 0.8f, 0.8f});
        const kin::Mesh ball = r3d.create_mesh(kin::meshes::sphere(0.5f));
        kin::View3D view = r3d.create_view({160, 120});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        const std::vector<kin::MeshInstance> boxes{
            {Mat4f::translation({-2.0f, 1.0f, 0.0f}), Color::rgb(255, 0, 0)},
            {Mat4f::translation({0.0f, 1.0f, 0.0f}), Color::rgb(0, 255, 0)},
            {Mat4f::translation({2.0f, 1.0f, 0.0f}), Color::rgb(0, 0, 255)},
        };
        scene.draw_instanced(box, boxes, {.unlit = true});
        const auto balls = scene.add_instances(ball, 2, {.color = Color::rgb(255, 255, 0), .unlit = true});
        balls[0] = {Mat4f::translation({-1.0f, -1.2f, 0.0f}), kin::colors::white};
        balls[1] = {Mat4f::translation({1.0f, -1.2f, 0.0f}) * Mat4f::scaling({1.4f, 1.4f, 1.4f}), Color::rgb(255, 128, 255)};
        const Pixels p = shoot_view(renderer, r3d, view, scene);
        assert(r3d.stats().draws == 2 && r3d.stats().instances == 5);
        const auto at = [&](Vec3f world) { return p.at(scene.camera.project(world, screen)->pos); };
        expect(at({-2.0f, 1.0f, 0.4f}), Color::rgb(255, 0, 0), "first box");
        expect(at({0.0f, 1.0f, 0.4f}), Color::rgb(0, 255, 0), "second box");
        expect(at({2.0f, 1.0f, 0.4f}), Color::rgb(0, 0, 255), "third box");
        expect(at({-1.0f, -1.2f, 0.5f}), Color::rgb(255, 255, 0), "first ball");
        // Yellow times pink, multiplied in linear light: red, a little green.
        const kin::u8 g = static_cast<kin::u8>(std::lround(255.0f * kin::linear_to_srgb(kin::srgb_to_linear(128.0f / 255.0f))));
        expect(at({1.0f, -1.2f, 0.7f}), Color::rgb(255, g, 0), "second ball, tinted");
        renderer.present();
    });
}

// Big scenes have their instances filled on worker threads, in pieces: the
// copies far into each draw still land where they should, in their colours.
void test_gpu_instances_on_workers() {
    with_gpu("test_gpu_instances_on_workers", [](kin::Renderer2D& renderer) {
        kin::JobSystem jobs;
        renderer.set_job_system(&jobs);
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {0.8f, 0.8f, 0.8f});
        const kin::Mesh ball = r3d.create_mesh(kin::meshes::sphere(0.5f));
        kin::View3D view = r3d.create_view({160, 120});
        kin::Scene3D scene;
        scene.cull = false; // all of them written (test_gpu_frustum_culling culls)
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        const Mat4f hidden = Mat4f::translation({0.0f, 0.0f, 50.0f}); // behind the camera
        // Small draws first (taken together), then two big ones (split).
        for (int i = 0; i < 300; ++i) {
            scene.draw(i % 2 == 0 ? box : ball, hidden);
        }
        auto boxes = scene.add_instances(box, 7000, {.unlit = true});
        std::fill(boxes.begin(), boxes.end(), kin::MeshInstance{hidden, kin::colors::white});
        boxes[6999] = {Mat4f::translation({-1.5f, 0.0f, 0.0f}), Color::rgb(0, 255, 0)};
        auto balls = scene.add_instances(ball, 5000, {.color = Color::rgb(255, 0, 0), .unlit = true});
        std::fill(balls.begin(), balls.end(), kin::MeshInstance{hidden, kin::colors::white});
        balls[4321] = {Mat4f::translation({1.5f, 0.0f, 0.0f}), kin::colors::white};
        const Pixels p = shoot_view(renderer, r3d, view, scene);
        assert(r3d.stats().instances == 12300 && r3d.stats().draws == 302);
        expect(p.at(scene.camera.project({-1.5f, 0.0f, 0.4f}, screen)->pos), Color::rgb(0, 255, 0), "the last box");
        expect(p.at(scene.camera.project({1.5f, 0.0f, 0.5f}, screen)->pos), Color::rgb(255, 0, 0), "a ball far in");
        expect(p.at(80, 60), kin::colors::black, "nothing else in view");
        renderer.present();
        renderer.set_job_system(nullptr);
    });
}

// Copies outside the view are skipped; the rest still land where they should,
// in their colours, however the work was split.
void test_gpu_frustum_culling() {
    with_gpu("test_gpu_frustum_culling", [](kin::Renderer2D& renderer) {
        kin::JobSystem jobs;
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {0.8f, 0.8f, 0.8f});
        kin::View3D view = r3d.create_view({160, 120});
        for (const bool workers : {false, true}) {
            renderer.set_job_system(workers ? &jobs : nullptr);
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
            // 12,000 copies: three in view, the rest off to the sides, behind, or
            // too far up; a big one whose centre is out of view but whose edge is in.
            auto copies = scene.add_instances(box, 12000, {.unlit = true});
            for (std::size_t i = 0; i < copies.size(); ++i) {
                const float side = (i % 3 == 0) ? -40.0f : (i % 3 == 1) ? 40.0f : 0.0f;
                const float z = (i % 3 == 2) ? 30.0f : 0.0f; // behind the camera
                copies[i] = {Mat4f::translation({side, 0.0f, z}), kin::colors::white};
            }
            copies[5000] = {Mat4f::translation({-2.0f, 0.0f, 0.0f}), Color::rgb(255, 0, 0)};
            copies[9000] = {Mat4f::translation({2.0f, 0.0f, 0.0f}), Color::rgb(0, 0, 255)};
            copies[11999] = {Mat4f::translation({0.0f, 1.5f, 0.0f}), Color::rgb(0, 255, 0)};
            copies[17] = {Mat4f::translation({0.0f, -7.0f, 0.0f}) * Mat4f::scaling({12.0f, 12.0f, 1.0f}), Color::rgb(255, 255, 0)};
            scene.draw(box, Mat4f::translation({0.0f, 0.0f, 80.0f})); // a draw with nothing in view
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            assert(r3d.stats().instances == 4 && r3d.stats().culled == 11997 && r3d.stats().draws == 1);
            const auto at = [&](Vec3f world) { return p.at(scene.camera.project(world, screen)->pos); };
            expect(at({-2.0f, 0.0f, 0.4f}), Color::rgb(255, 0, 0), "a copy in view");
            expect(at({2.0f, 0.0f, 0.4f}), Color::rgb(0, 0, 255), "another");
            expect(at({0.0f, 1.5f, 0.4f}), Color::rgb(0, 255, 0), "the last");
            expect(p.at(80, 118), Color::rgb(255, 255, 0), "the big one's edge");
            renderer.present();
        }
        renderer.set_job_system(nullptr);

        // Not culled when the scene says so: everything is drawn.
        kin::Scene3D all;
        all.cull = false;
        all.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f});
        all.draw(box, Mat4f::translation({0.0f, 0.0f, 30.0f}));
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, all);
        assert(r3d.stats().instances == 1 && r3d.stats().culled == 0);
        renderer.present();
    });
}

// The overlay (a first-person gun) shows in front of the scene, even of what
// is nearer the camera than it, and is seen by its own camera.
void test_gpu_overlay() {
    with_gpu("test_gpu_overlay", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        // A wall right in front of the camera, filling the view.
        scene.draw(box, Mat4f::translation({0.0f, 0.0f, 5.5f}) * Mat4f::scaling({4.0f, 4.0f, 0.1f}),
                   {.color = Color::rgb(90, 90, 90), .unlit = true});
        kin::Scene3D& gun = scene.overlay();
        gun.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 70.0f);
        // Seen by its own camera, 3 m in front of it: further than the wall is from the scene's.
        gun.draw(box, Mat4f::translation({0.6f, -0.4f, -3.0f}), {.color = Color::rgb(250, 120, 20), .unlit = true});
        const Pixels p = shoot_view(renderer, r3d, view, scene);
        assert(r3d.stats().draws == 2 && r3d.stats().instances == 2);
        const auto spot = gun.camera.project({0.6f, -0.4f, -2.5f}, screen);
        assert(spot);
        expect(p.at(spot->pos), Color::rgb(250, 120, 20), "the overlay over the wall");
        expect(p.at(20, 20), Color::rgb(90, 90, 90), "the wall round it");
        renderer.present();

        // Cleared with the scene.
        scene.clear();
        assert(scene.overlay().draws().empty());
    });
}

int brightness(Color c) { return int(c.r) + int(c.g) + int(c.b); }

// A point light lights what is near it and nothing past its radius; at most
// 16 are used, the strongest and nearest.
void test_gpu_point_lights() {
    with_gpu("test_gpu_point_lights", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh wall = make_box(r3d, {20.0f, 20.0f, 0.2f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 8.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
        scene.lights.ambient = {0.0f, 0.0f, 0.0f};
        scene.draw(wall, Mat4f::identity(), {.color = Color::rgb(200, 200, 200)});
        scene.add_light({.position = {-2.0f, 0.0f, 1.0f}, .color = {2.0f, 1.0f, 0.5f}, .radius = 3.0f});
        Pixels p = shoot_view(renderer, r3d, view, scene);
        assert(r3d.stats().lights == 1);
        const auto at = [&](Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        const Color near_light = at({-2.0f, 0.0f, 0.1f});
        assert(near_light.r > 200 && near_light.r > near_light.b + 60); // warm, bright
        expect(at({2.0f, 0.0f, 0.1f}), kin::colors::black, "past its radius");
        renderer.present();

        // Twenty lights all over: sixteen used.
        scene.lights.points.clear();
        for (int i = 0; i < 20; ++i) {
            scene.add_light({.position = {float(i % 5) - 2.0f, float(i / 5) - 1.5f, 1.0f}, .color = {0.3f, 0.3f, 0.3f}, .radius = 2.0f});
        }
        scene.add_light({.position = {0.0f, 0.0f, 40.0f}, .radius = 2.0f}); // behind the camera: none of the view
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        assert(r3d.stats().lights == 16);
        renderer.present();
    });
}

// The sun casts a box's shadow on the floor, where the shadow area says; none without it.
void test_gpu_shadows() {
    with_gpu("test_gpu_shadows", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = make_box(r3d, {20.0f, 0.2f, 20.0f});
        const kin::Mesh box = make_box(r3d, {1.5f, 1.5f, 1.5f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 12.0f, 0.01f}, {0.0f, 0.0f, 0.0f}, 60.0f); // looking down
        scene.lights.sun = {.direction = {1.0f, 1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}}; // from up and +x
        scene.lights.ambient = {0.15f, 0.15f, 0.15f};
        const auto draw = [&](bool area) {
            scene.clear();
            scene.lights.shadow_area = area ? kin::Bounds3{{-10.0f, -0.2f, -10.0f}, {10.0f, 4.0f, 10.0f}} : kin::Bounds3{};
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            scene.draw(box, Mat4f::translation({0.0f, 2.5f, 0.0f}), {.color = Color::rgb(180, 60, 60)});
            return shoot_view(renderer, r3d, view, scene);
        };
        Pixels p = draw(true);
        assert(r3d.stats().shadow_draws == 2);
        const auto at = [&](const Pixels& px, Vec3f w) { return px.at(scene.camera.project(w, screen)->pos); };
        // The box at 2.5 m, the sun at 45 degrees from +x: its shadow 2.5 m to -x.
        const Color shade = at(p, {-2.5f, 0.0f, 0.0f});
        const Color open = at(p, {3.0f, 0.0f, 0.0f});
        assert(brightness(open) > 2 * brightness(shade) && brightness(shade) > 0);
        renderer.present();
        Pixels no_shadow = draw(false);
        assert(r3d.stats().shadow_draws == 0);
        expect(at(no_shadow, {-2.5f, 0.0f, 0.0f}), open, "no shadow without an area", 4);
        renderer.present();
    });
}

// Additive light adds up in any order; alpha blends over what is behind;
// either hides behind something opaque in front.
void test_gpu_blending() {
    with_gpu("test_gpu_blending", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh quad = make_box(r3d, {2.0f, 2.0f, 0.01f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        const kin::Material3D glow{.color = Color::rgb(60, 60, 60), .unlit = true, .blend = kin::Blend3D::Additive};
        const kin::Material3D wall{.color = Color::rgb(200, 0, 0), .unlit = true};
        const kin::Material3D veil{.color = Color::rgba(0, 0, 255, 128), .unlit = true, .blend = kin::Blend3D::Alpha};
        // Additive quads overlapping at the centre, drawn before the wall they are in front of.
        scene.draw(quad, Mat4f::translation({-0.6f, 0.0f, 1.0f}), glow);
        scene.draw(quad, Mat4f::translation({0.6f, 0.0f, 1.2f}), glow);
        scene.draw(quad, Mat4f::translation({0.0f, 0.0f, 0.0f}) * Mat4f::scaling({4.0f, 4.0f, 1.0f}), wall);
        // An alpha veil in front, at the top; another behind the wall (hidden).
        scene.draw(quad, Mat4f::translation({0.0f, 2.0f, 1.0f}), veil);
        scene.draw(quad, Mat4f::translation({0.0f, -2.5f, -1.0f}) * Mat4f::scaling({2.0f, 1.0f, 1.0f}), veil);
        const Pixels p = shoot_view(renderer, r3d, view, scene);
        const auto at = [&](Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        const Color one = at({-1.4f, 0.0f, 1.0f}), both = at({0.0f, 0.0f, 1.2f}), none = at({0.0f, -1.5f, 0.0f});
        // Red saturates over the red wall; green shows the light adding up.
        assert(one.r >= none.r && one.g > 40 && both.g > one.g + 30 && none.g == 0);
        const Color veiled = at({0.0f, 2.0f, 1.0f});
        assert(veiled.b > 80 && veiled.r > 60 && veiled.r < 190); // half blue over the red wall
        expect(at({0.0f, -2.5f, 0.0f}), wall.color, "a veil behind the wall");
        renderer.present();
    });
}

// An HDR view keeps light past white; bloom spreads it round the bright parts.
void test_gpu_hdr_bloom() {
    with_gpu("test_gpu_hdr_bloom", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh cube = make_box(r3d, {0.6f, 0.6f, 0.6f});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.draw(cube, Mat4f::identity(), {.color = Color::rgb(255, 140, 60), .emissive = 6.0f, .unlit = true});
        const auto shot = [&](const kin::View3DOptions& options) {
            kin::View3D view = r3d.create_view({160, 120}, options);
            return shoot_view(renderer, r3d, view, scene);
        };
        const Pixels plain = shot({.samples = 1, .hdr = true});
        const Pixels glow = shot({.samples = 1, .bloom = 1.0f});
        const auto side = scene.camera.project({0.55f, 0.0f, 0.3f}, screen); // a little off the cube
        assert(side);
        assert(brightness(plain.at(side->pos)) < 6);
        assert(brightness(glow.at(side->pos)) > 40);
        // Tonemapped, the cube's middle is bright but not a flat clip of every channel.
        const Color middle = glow.at(80, 60);
        assert(middle.r > 230 && middle.g > 150);
        renderer.present();
        // The background is not lifted by the HDR path: tonemapped (ACES darkens
        // the darkest tones a little), its hue kept.
        kin::View3D dark = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(18, 26, 36), .bloom = 1.0f});
        renderer.clear(kin::colors::black);
        render_ok(r3d, dark, scene);
        renderer.draw_texture(dark.texture(), screen);
        const Color background = read(renderer).at(4, 4);
        assert(background.r <= 18 && background.b <= 36 && background.b > background.g && background.g > background.r);
        renderer.present();
        // Over a transparent background the glow still shows on what is under it.
        kin::View3D clear_view = r3d.create_view({160, 120}, {.samples = 1, .background = kin::colors::transparent, .bloom = 1.0f});
        renderer.clear(Color::rgb(0, 0, 120));
        render_ok(r3d, clear_view, scene);
        renderer.draw_texture(clear_view.texture(), screen);
        const Color over_blue = read(renderer).at(side->pos);
        assert(over_blue.r > 30 && over_blue.b > 60);
        renderer.present();
    });
}

// A material's texture, by the mesh's UVs.
void test_gpu_texture() {
    with_gpu("test_gpu_texture", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        // A 2 x 2 checker: red, green / blue, white.
        const std::vector<kin::u8> texels{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        const kin::Texture checker = renderer.create_texture_from_rgba(texels.data(), {2, 2});
        kin::MeshData square;
        square.vertices = {{{-1, -1, 0}, {0, 0, 1}, {0, 1}}, {{1, -1, 0}, {0, 0, 1}, {1, 1}},
                           {{1, 1, 0}, {0, 0, 1}, {1, 0}}, {{-1, 1, 0}, {0, 0, 1}, {0, 0}}};
        square.indices = {0, 1, 2, 0, 2, 3};
        const kin::Mesh mesh = r3d.create_mesh(square);
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.draw(mesh, Mat4f::identity(), {.unlit = true, .texture = checker});
        const Pixels p = shoot_view(renderer, r3d, view, scene);
        const auto at = [&](Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        expect(at({-0.5f, 0.5f, 0.0f}), Color::rgb(255, 0, 0), "top left texel");
        expect(at({0.5f, 0.5f, 0.0f}), Color::rgb(0, 255, 0), "top right texel");
        expect(at({-0.5f, -0.5f, 0.0f}), Color::rgb(0, 0, 255), "bottom left texel");
        expect(at({0.5f, -0.5f, 0.0f}), Color::rgb(255, 255, 255), "bottom right texel");
        renderer.present();
    });
}

// A glTF model (tests/fixtures/models/make_parts.py): its parts, materials,
// node transforms and texture, drawn; and loaded again when its file changes.
void test_gpu_model() {
    with_gpu("test_gpu_model", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const std::filesystem::path path = std::filesystem::path{KIN_TEST_MODELS} / "parts.glb";
        std::string error;
        const kin::Model model = r3d.load_model(path, &error);
        if (!model.valid()) {
            std::cerr << "load_model: " << error << '\n';
            assert(false);
        }
        assert(model.parts.size() == 3 && error.empty());
        const auto part = [&](std::string_view name) -> const kin::ModelPart& {
            for (const kin::ModelPart& p : model.parts) {
                if (p.name == name) return p;
            }
            assert(false);
            return model.parts.front();
        };
        const kin::ModelPart& base = part("base");
        const kin::ModelPart& panel = part("panel");
        const kin::ModelPart& pane = part("pane");
        assert(base.material.color == Color::rgb(255, 0, 0) && !base.material.texture.valid() && base.mesh.triangle_count() == 12);
        assert(panel.material.texture.valid() && panel.material.unlit && panel.material.texture.size() == (kin::Vec2i{2, 2}));
        assert(pane.material.blend == kin::Blend3D::Alpha && pane.material.double_sided);
        assert(pane.material.color.a == 128 || pane.material.color.a == 127);
        // The panel is the base's child: 1 m up, twice the size.
        assert(std::abs(panel.transform.m[13] - 1.0f) < 1e-5f && std::abs(panel.transform.m[0] - 2.0f) < 1e-5f);
        // Round all of it: the box (1 m), the panel (2 m, to y 2), the pane at x 3.
        assert(std::abs(model.bounds.min.x + 1.0f) < 1e-4f && std::abs(model.bounds.max.x - 3.5f) < 1e-4f);
        assert(std::abs(model.bounds.max.y - 2.0f) < 1e-4f && std::abs(model.bounds.min.y + 0.5f) < 1e-4f);
        // The quad came without normals: flat ones, facing +z, six vertices.
        assert(panel.mesh.vertex_count() == 6);

        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 1.0f, 4.0f}, {0.0f, 1.0f, 0.0f}, 60.0f);
        scene.draw(model, Mat4f::identity());
        const Pixels p = shoot_view(renderer, r3d, view, scene);
        const auto at = [&](Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        expect(at({-0.5f, 1.5f, 0.0f}), Color::rgb(255, 0, 0), "the panel's top left texel");
        expect(at({0.5f, 1.5f, 0.0f}), Color::rgb(0, 255, 0), "its top right texel");
        expect(at({0.5f, 0.75f, 0.0f}), Color::rgb(255, 255, 255), "its bottom right texel (above the box)");
        renderer.present();

        // Hot reload: a copy of the file, rewritten, is loaded again.
        const std::filesystem::path copy = std::filesystem::temp_directory_path() / "kin_render3d_parts.glb";
        std::filesystem::copy_file(path, copy, std::filesystem::copy_options::overwrite_existing);
        kin::ModelFile file{r3d, copy};
        assert(file.model().parts.size() == 3 && !file.poll());
        std::filesystem::last_write_time(copy, std::filesystem::last_write_time(copy) + std::chrono::seconds(2));
        assert(file.poll() && file.model().parts.size() == 3);
        std::filesystem::remove(copy);
        // Nothing to read: empty, and why.
        assert(!r3d.load_model("no/such/model.glb", &error).valid() && !error.empty());
    });
}

// Back faces culled, unless the material is double-sided.
void test_gpu_culling() {
    with_gpu("test_gpu_culling", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = r3d.create_mesh(kin::meshes::plane({4.0f, 4.0f}));
        kin::View3D view = r3d.create_view({160, 120});
        for (const bool double_sided : {false, true}) {
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, -3.0f, 0.01f}, {0.0f, 0.0f, 0.0f}); // from below
            scene.draw(floor, Mat4f::identity(), {.color = Color::rgb(90, 200, 90), .unlit = true, .double_sided = double_sided});
            expect(shoot_view(renderer, r3d, view, scene).at(80, 60), double_sided ? Color::rgb(90, 200, 90) : kin::colors::black,
                   double_sided ? "a double-sided plane from below" : "a plane from below");
            renderer.present();
        }
    });
}

// A view rendered twice in a frame: the 2D drawn between sees the first
// picture, what is drawn after the second.
void test_gpu_interleaves_with_2d() {
    with_gpu("test_gpu_interleaves_with_2d", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {10.0f, 10.0f, 1.0f});
        kin::View3D view = r3d.create_view({40, 40}, {.samples = 4});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 3.0f}, {0.0f, 0.0f, 0.0f});
        renderer.clear(kin::colors::black);
        renderer.fill_rect({0.0f, 0.0f, 160.0f, 120.0f}, Color::rgb(10, 20, 30)); // 2D first
        scene.draw(box, Mat4f::identity(), {.color = Color::rgb(220, 30, 30), .unlit = true});
        render_ok(r3d, view, scene);
        renderer.draw_texture(view.texture(), {0.0f, 0.0f, 40.0f, 40.0f});
        scene.clear();
        scene.draw(box, Mat4f::identity(), {.color = Color::rgb(30, 220, 30), .unlit = true});
        render_ok(r3d, view, scene);
        renderer.draw_texture(view.texture(), {60.0f, 0.0f, 40.0f, 40.0f});
        renderer.fill_rect({120.0f, 0.0f, 20.0f, 20.0f}, Color::rgb(250, 250, 250)); // 2D after
        const Pixels p = read(renderer);
        expect(p.at(20, 20), Color::rgb(220, 30, 30), "the first picture");
        expect(p.at(80, 20), Color::rgb(30, 220, 30), "the second picture");
        expect(p.at(130, 10), Color::rgb(250, 250, 250), "2D after");
        expect(p.at(50, 80), Color::rgb(10, 20, 30), "2D before");
        renderer.present();

        // A transparent background lets the 2D under it show.
        kin::View3D clear_view = r3d.create_view({40, 40}, {.samples = 1});
        renderer.clear(Color::rgb(70, 70, 70));
        scene.clear();
        scene.draw(box, Mat4f::translation({0.0f, 0.0f, -100.0f}), {.unlit = true}); // too small to cover it
        render_ok(r3d, clear_view, scene);
        renderer.draw_texture(clear_view.texture(), {0.0f, 0.0f, 40.0f, 40.0f});
        expect(read(renderer).at(5, 5), Color::rgb(70, 70, 70), "under a transparent background");
        renderer.present();

        // Resized: a new size and texture.
        const kin::Texture before = view.texture();
        assert(r3d.resize(view, {64, 32}) && view.size() == (kin::Vec2i{64, 32}) && !(view.texture() == before));
        assert(view.texture().size() == (kin::Vec2i{64, 32}));
    });
}

// Meshes and views kept past the renderer (as scenes outliving it keep them)
// are let go without touching the device it closed.
void test_gpu_meshes_outlive_renderer() {
    constexpr std::string_view test_name = "test_gpu_meshes_outlive_renderer";
    kin::App app{};
    kin::Window& window = app.create_window({.title = "render3d-gpu", .width = 160, .height = 120, .hidden = true});
    std::string reason;
    auto renderer = try_create_gpu_renderer(window, reason);
    if (!renderer) {
        skip_or_require_gpu_test(test_name, reason);
        return;
    }
    kin::Mesh mesh;
    kin::View3D view;
    {
        kin::Renderer3D r3d{*renderer};
        mesh = r3d.create_mesh(kin::meshes::sphere(1.0f));
        view = r3d.create_view({64, 64});
        kin::Scene3D scene;
        scene.draw(mesh, Mat4f::identity());
        renderer->clear(kin::colors::black);
        render_ok(r3d, view, scene);
        renderer->draw_texture(view.texture(), {0.0f, 0.0f, 64.0f, 64.0f});
        renderer->present();
    }
    renderer.reset();
    assert(mesh.valid() && view.valid());
    mesh = {};
    view = {};
}

// In a linear HDR scene the view follows (float targets), and colours come
// out the same.
void test_gpu_linear_hdr() {
    with_gpu("test_gpu_linear_hdr", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120});
        if (!renderer.set_color_space(kin::ColorSpace::Linear, true)) {
            std::cerr << "[SKIP] test_gpu_linear_hdr: no linear HDR pipeline\n";
            return;
        }
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f});
        scene.draw(box, Mat4f::identity(), {.color = Color::rgb(200, 40, 40), .unlit = true});
        expect(shoot_view(renderer, r3d, view, scene).at(80, 60), Color::rgb(200, 40, 40), "a linear HDR scene", 4);
        renderer.present();
    });
}

// What is under a pixel: the id given with the draw and the point hit, in
// the world; nothing over the background, the overlay or a wireframe. 2D
// drawn after a pick (the frame submitted under it) still shows.
void test_gpu_pick() {
    with_gpu("test_gpu_pick", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.pick = true});
        kin::View3D plain = r3d.create_view({160, 120});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.draw(box, Mat4f::translation({-1.5f, 0.0f, 0.0f}), {.color = Color::rgb(200, 60, 60)}, kin::colors::white, 7);
        scene.draw(box, Mat4f::translation({1.5f, 0.0f, 0.0f}), {.color = Color::rgb(60, 200, 60)}, kin::colors::white, 0);
        scene.draw(box, Mat4f::translation({0.0f, 2.0f, 0.0f}), {.color = Color::rgb(60, 60, 200), .wireframe = true},
                   kin::colors::white, 3);
        scene.overlay().camera = scene.camera;
        scene.overlay().draw(box, Mat4f::translation({0.0f, -2.0f, 0.0f}), {.color = Color::rgb(200, 200, 60)},
                             kin::colors::white, 5);
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        const auto pixel = [&](Vec3f w) {
            const kin::Vec2f p = scene.camera.project(w, screen)->pos;
            return kin::Vec2i{static_cast<int>(p.x), static_cast<int>(p.y)};
        };
        const kin::PickResult left = r3d.pick(view, pixel({-1.5f, 0.0f, 0.5f}));
        assert(left.hit && left.id == 7);
        assert(kin::length(left.position - Vec3f{-1.5f, 0.0f, 0.5f}) < 0.05f); // the front face
        const kin::PickResult right = r3d.pick(view, pixel({1.3f, 0.2f, 0.5f}));
        assert(right.hit && right.id == 0 && std::abs(right.position.z - 0.5f) < 0.02f);
        assert(!r3d.pick(view, {2, 2}).hit);                          // background
        assert(!r3d.pick(view, pixel({0.1f, 2.0f, 0.5f})).hit);       // a wireframe's face
        assert(!r3d.pick(view, pixel({0.0f, -2.0f, 0.5f})).hit);      // the overlay
        assert(!r3d.pick(view, {-1, 0}).hit && !r3d.pick(view, {160, 0}).hit);
        // The frame goes on after the pick.
        renderer.draw_texture(view.texture(), screen);
        renderer.fill_rect({0.0f, 0.0f, 10.0f, 10.0f}, Color::rgb(10, 220, 240));
        const Pixels p = read(renderer);
        expect(p.at(5, 5), Color::rgb(10, 220, 240), "2D after a pick");
        const kin::Vec2i box_pixel = pixel({-1.5f, 0.0f, 0.5f});
        assert(p.at(box_pixel.x, box_pixel.y).r > 100);
        renderer.present();

        // A view made without picking has nothing to say.
        renderer.clear(kin::colors::black);
        render_ok(r3d, plain, scene);
        assert(!r3d.pick(plain, pixel({-1.5f, 0.0f, 0.5f})).hit);
        renderer.present();
    });
}

// A wireframe draws its triangles' edges, unlit, over faces in the same
// place (a little depth bias); its triangles' insides show what is behind.
// A degenerate triangle (a, b, b) is a line.
void test_gpu_wireframe() {
    with_gpu("test_gpu_wireframe", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {2.0f, 2.0f, 2.0f});
        MeshData line_data;
        for (const Vec3f v : {Vec3f{-3.0f, -1.5f, 0.0f}, Vec3f{3.0f, -1.5f, 0.0f}}) {
            line_data.vertices.push_back({.position = v, .normal = {0.0f, 0.0f, 1.0f}});
        }
        line_data.indices = {0, 1, 1};
        const kin::Mesh line = r3d.create_mesh(line_data);
        assert(line.valid());
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
        scene.lights.ambient = {0.0f, 0.0f, 0.0f};
        const Color wire = Color::rgb(250, 40, 40);
        scene.draw(box, Mat4f::identity(), {.color = Color::rgb(90, 90, 90), .unlit = true});
        scene.draw(box, Mat4f::identity(), {.color = wire, .wireframe = true});
        scene.draw(line, Mat4f::identity(), {.color = Color::rgb(40, 250, 40), .wireframe = true});
        const Pixels p = shoot_view(renderer, r3d, view, scene);
        const auto any_near = [&](Vec3f w, Color want) {
            const kin::Vec2f c = scene.camera.project(w, screen)->pos;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (near(p.at(static_cast<int>(c.x) + dx, static_cast<int>(c.y) + dy), want, 4)) {
                        return true;
                    }
                }
            }
            return false;
        };
        // The front face's right edge, unlit, over the face; off the edges and
        // its diagonal, the face.
        assert(any_near({1.0f, 0.3f, 1.0f}, wire));
        expect(p.at(scene.camera.project({0.6f, 0.0f, 1.0f}, screen)->pos), Color::rgb(90, 90, 90), "inside a triangle");
        assert(any_near({0.0f, -1.5f, 0.0f}, Color::rgb(40, 250, 40)) || any_near({2.5f, -1.5f, 0.0f}, Color::rgb(40, 250, 40)));
        expect(p.at(scene.camera.project({2.5f, -1.0f, 0.0f}, screen)->pos), kin::colors::black, "beside the line");
        renderer.present();
    });
}

// A fill light from a second direction (no shadow); the ambient from above
// and from below differ; a material may take no shadow.
void test_gpu_fill_and_ambient() {
    with_gpu("test_gpu_fill_and_ambient", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
        scene.lights.ambient = {0.0f, 0.0f, 0.0f};
        const auto shot = [&](Vec3f eye) {
            scene.clear();
            scene.camera = kin::Camera3D::look_at(eye, {0.0f, 0.0f, 0.0f}, 50.0f);
            scene.draw(box, Mat4f::identity(), {.color = kin::colors::white});
            const Color c = shoot_view(renderer, r3d, view, scene).at(80, 60);
            renderer.present();
            return c;
        };
        // Fill from +z: the front face, in its colour.
        scene.lights.fill = {.direction = {0.0f, 0.0f, 1.0f}, .color = {0.0f, 1.0f, 0.0f}};
        const Color front = shot({0.0f, 0.0f, 5.0f});
        assert(front.g > 240 && front.r < 5 && front.b < 5);
        assert(brightness(shot({0.0f, 0.0f, -5.0f})) < 5); // the back: none
        scene.lights.fill.color = {0.0f, 0.0f, 0.0f};

        // Red from above, blue from below; the side half and half.
        scene.lights.ambient = {0.6f, 0.0f, 0.0f};
        scene.lights.ground_ambient = Vec3f{0.0f, 0.0f, 0.6f};
        const Color top = shot({0.0f, 5.0f, 0.01f});
        const Color bottom = shot({0.0f, -5.0f, 0.01f});
        const Color side = shot({0.0f, 0.0f, 5.0f});
        assert(top.r > 150 && top.b < 5);
        assert(bottom.b > 150 && bottom.r < 5);
        assert(side.r > 80 && side.b > 80 && side.r < top.r && side.b < bottom.b);
        scene.lights.ground_ambient.reset();
        expect(shot({0.0f, -5.0f, 0.01f}), top, "one ambient without a ground's");
    });
}

// A floor that takes no shadow is as bright under the box as beside it.
void test_gpu_receives_no_shadow() {
    with_gpu("test_gpu_receives_no_shadow", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = make_box(r3d, {20.0f, 0.2f, 20.0f});
        const kin::Mesh box = make_box(r3d, {1.5f, 1.5f, 1.5f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 12.0f, 0.01f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.lights.sun = {.direction = {1.0f, 1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}};
        scene.lights.ambient = {0.15f, 0.15f, 0.15f};
        scene.lights.shadow_area = kin::Bounds3{{-10.0f, -0.2f, -10.0f}, {10.0f, 4.0f, 10.0f}};
        scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180), .receives_shadow = false});
        scene.draw(box, Mat4f::translation({0.0f, 2.5f, 0.0f}), {.color = Color::rgb(180, 60, 60)});
        const Pixels p = shoot_view(renderer, r3d, view, scene);
        assert(r3d.stats().shadow_draws == 2);
        const auto at = [&](Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        expect(at({-2.5f, 0.0f, 0.0f}), at({3.0f, 0.0f, 0.0f}), "no shadow taken", 3);
        renderer.present();
    });
}

// A mesh bigger than one upload (it goes up in pieces): its last triangle,
// in the last piece, drawn where it is; with indices too, the last of them
// pointing past the first pieces.
void test_gpu_large_mesh() {
    with_gpu("test_gpu_large_mesh", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        MeshData data;
        constexpr std::size_t hidden = 400'000; // triangles out of sight: 43 MB of vertices
        data.vertices.reserve(hidden * 3 + 3);
        for (std::size_t t = 0; t < hidden; ++t) {
            const float x = 100.0f + static_cast<float>(t % 1000) * 0.01f;
            for (const Vec3f v : {Vec3f{x, 0.0f, 0.0f}, Vec3f{x + 0.005f, 0.0f, 0.0f}, Vec3f{x, 0.005f, 0.0f}}) {
                data.vertices.push_back({.position = v, .normal = {0.0f, 0.0f, 1.0f}, .color = Color::rgb(40, 200, 40)});
            }
        }
        for (const Vec3f v : {Vec3f{-1.0f, -1.0f, 0.0f}, Vec3f{1.0f, -1.0f, 0.0f}, Vec3f{0.0f, 1.0f, 0.0f}}) {
            data.vertices.push_back({.position = v, .normal = {0.0f, 0.0f, 1.0f}, .color = Color::rgb(220, 30, 30)});
        }
        const auto shows_last = [&](const MeshData& mesh_data, std::string_view what) {
            const kin::Mesh mesh = r3d.create_mesh(mesh_data);
            assert(mesh.valid());
            scene.clear();
            scene.draw(mesh, Mat4f::identity(), {.unlit = true});
            expect(shoot_view(renderer, r3d, view, scene).at(scene.camera.project({0.0f, -0.3f, 0.0f}, screen)->pos), Color::rgb(220, 30, 30), what);
            renderer.present();
        };
        shows_last(data, "the last triangle of a large mesh");
        // Indexed: the triangles' order turned round (each wound as it was), the shown one first.
        const auto triangles = static_cast<kin::u32>(data.vertices.size() / 3);
        for (kin::u32 t = triangles; t-- > 0;) {
            data.indices.insert(data.indices.end(), {3 * t, 3 * t + 1, 3 * t + 2});
        }
        shows_last(data, "a large mesh's indices");
    });
}

// The shadow pass culls a single-sided mesh's back faces (a closed mesh's
// map is the same): a plane facing the sun still casts, one facing away
// does not (draw it double-sided to cast from both sides), a double-sided
// one does either way.
void test_gpu_shadow_faces() {
    with_gpu("test_gpu_shadow_faces", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = make_box(r3d, {20.0f, 0.2f, 20.0f});
        const kin::Mesh plane = r3d.create_mesh(kin::meshes::plane({2.0f, 2.0f})); // faces +Y
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 12.0f, 0.01f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.lights.sun = {.direction = {1.0f, 1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}}; // from up and +x
        scene.lights.ambient = {0.15f, 0.15f, 0.15f};
        const auto shade_under = [&](const Mat4f& placed, bool double_sided) {
            scene.clear();
            scene.lights.shadow_area = kin::Bounds3{{-10.0f, -0.2f, -10.0f}, {10.0f, 4.0f, 10.0f}};
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            scene.draw(plane, placed, {.color = Color::rgb(180, 60, 60), .double_sided = double_sided, .receives_shadow = false});
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            const Color under = p.at(scene.camera.project({1.0f, 0.0f, 0.0f}, screen)->pos);
            const Color open = p.at(scene.camera.project({-4.0f, 0.0f, 0.0f}, screen)->pos);
            renderer.present();
            return brightness(under) * 2 < brightness(open);
        };
        // The plane 3 m up at x = 4: its shadow at x = 1, on floor the camera sees past it.
        const Mat4f up = Mat4f::translation({4.0f, 3.0f, 0.0f});
        const Mat4f down = up * Mat4f::rotation_x(180.0f);
        assert(shade_under(up, false));       // facing the sun: casts
        assert(!shade_under(down, false));    // facing away, single-sided: does not
        assert(shade_under(down, true));      // double-sided: casts
    });
}

// A mesh out of the camera's view still casts when its shadow falls in view;
// one whose shadow falls out of view too is left out of the shadow map.
void test_gpu_shadows_into_view() {
    with_gpu("test_gpu_shadows_into_view", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = make_box(r3d, {20.0f, 0.2f, 20.0f});
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 12.0f, 0.01f}, {0.0f, 0.0f, 0.0f}, 30.0f); // the floor within about 4 m of the middle
        scene.lights.sun = {.direction = {1.0f, 1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}}; // from up and +x: shadows fall toward -x
        scene.lights.ambient = {0.15f, 0.15f, 0.15f};
        scene.lights.shadow_area = kin::Bounds3{{-10.0f, -0.2f, -10.0f}, {10.0f, 4.0f, 10.0f}};
        scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
        scene.draw(box, Mat4f::translation({6.5f, 3.0f, 0.0f}), {.color = Color::rgb(180, 60, 60)});  // out of view, its shadow at x = 3.5: in it
        scene.draw(box, Mat4f::translation({-7.0f, 2.0f, 0.0f}), {.color = Color::rgb(180, 60, 60)}); // its shadow at x = -9: out of view
        scene.draw(box, Mat4f::translation({8.0f, 1.0f, 7.0f}), {.color = Color::rgb(180, 60, 60)});  // its shadow at (7, 0, 7): out of view
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        assert(r3d.stats().culled == 3);
        assert(r3d.stats().shadow_casters == 2); // the floor, and the box casting into view
        renderer.draw_texture(view.texture(), screen);
        const Pixels p = read(renderer);
        const auto at = [&](Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        assert(brightness(at({3.5f, 0.0f, 0.0f})) * 2 < brightness(at({-2.0f, 0.0f, 0.0f})));
        renderer.present();
    });
}

// Cascaded shadows (G2a). A block 60 m from the camera, a narrow lens on its
// shadow's far edge: one 2048 map over a 300 m area is coarse there, a cascade
// fitted to that part of the view is not. The edge's width in pixels (the
// pixels between a tenth and nine tenths of the way from shade to light)
// must shrink; the shadow is where it was; one cascade is the single map's
// picture exactly; each cascade draws only its own casters.
void test_gpu_cascades() {
    with_gpu("test_gpu_cascades", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = make_box(r3d, {300.0f, 0.2f, 300.0f});
        const kin::Mesh block = make_box(r3d, {4.0f, 4.0f, 4.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({-6.0f, 40.0f, 0.0f}, {-6.0f, 0.0f, -45.0f}, 12.0f);
        scene.camera.z_near = 1.0f;
        scene.lights.sun = {.direction = {1.0f, 1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}}; // shadows fall toward -x
        scene.lights.ambient = {0.1f, 0.1f, 0.1f};
        scene.lights.shadow_area = kin::Bounds3{{-150.0f, -0.2f, -150.0f}, {150.0f, 10.0f, 150.0f}};
        const auto render = [&](kin::u32 cascades) {
            scene.lights.shadow_cascades = cascades;
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            scene.draw(block, Mat4f::translation({0.0f, 2.0f, -45.0f}), {.color = Color::rgb(180, 60, 60)});
            // Out of every cascade but the farthest: far to the side and behind.
            scene.draw(block, Mat4f::translation({-100.0f, 2.0f, -140.0f}), {.color = Color::rgb(180, 60, 60)});
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        // The edge along z = -45, from open floor (x = -10) into the shadow (x = -4.5); the
        // block's shadow runs from x = -6 (the top's edge) to its foot at x = -2.
        const auto edge = [&](const Pixels& p) {
            const auto luma = [&](float x) { return brightness(p.at(scene.camera.project({x, 0.0f, -45.0f}, screen)->pos)); };
            const int lit = luma(-10.0f), shade = luma(-4.5f);
            assert(lit > 2 * shade && shade > 0); // the shadow is there
            int width = 0;
            const int y = static_cast<int>(scene.camera.project({-6.0f, 0.0f, -45.0f}, screen)->pos.y);
            for (int x = 0; x < 160; ++x) {
                const int b = brightness(p.at(x, y));
                width += (b > shade + (lit - shade) / 10 && b < shade + (lit - shade) * 9 / 10) ? 1 : 0;
            }
            return width;
        };
        const Pixels single = render(1);
        assert(r3d.stats().shadow_cascades == 1 && r3d.stats().cascade_casters[0] == r3d.stats().shadow_casters);
        const int single_edge = edge(single);
        const Pixels none = render(0); // asked for none: one
        assert(r3d.stats().shadow_cascades == 1);
        assert(single.rgba == none.rgba);

        const Pixels cascaded = render(4);
        const kin::Render3DStats stats = r3d.stats();
        assert(stats.shadow_cascades == 4 && stats.shadow_draws >= 4);
        // Each cascade's own casters: the far block is in the last only, so the
        // nearest has fewer than the farthest, and together they are the total.
        assert(stats.cascade_casters[0] + stats.cascade_casters[1] + stats.cascade_casters[2] + stats.cascade_casters[3] == stats.shadow_casters);
        assert(stats.cascade_casters[0] < stats.cascade_casters[3]);
        const int cascaded_edge = edge(cascaded);
        std::cerr << "cascades: shadow edge " << single_edge << " px with one map, " << cascaded_edge << " px with four\n";
        assert(cascaded_edge < single_edge);
        assert(cascaded_edge <= 2);
        renderer.present();
    });
}

// A forest of posts through all four cascades' depth ranges, with tall posts out of the
// camera's view whose shadows fall into it (the copies that only the sun sees, which the
// cascades' bookkeeping must keep apart from the ones the camera sees): every cascade
// gets casters, no fewer than the posts standing inside its slice of the view and no
// more than there are copies; the same on workers as serially; and a shadow in each
// depth band is where it should be.
void test_gpu_cascade_casters() {
    with_gpu("test_gpu_cascade_casters", [](kin::Renderer2D& renderer) {
        kin::JobSystem jobs;
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = make_box(r3d, {400.0f, 0.2f, 400.0f});
        const kin::Mesh post = make_box(r3d, {0.3f, 1.0f, 0.3f});
        const kin::Mesh tall = make_box(r3d, {1.0f, 20.0f, 12.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 10.0f, 20.0f}, {0.0f, 0.0f, -40.0f}, 60.0f);
        scene.camera.z_near = 1.0f;
        scene.lights.sun = {.direction = {1.0f, 0.5f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}}; // shadows fall toward -x
        scene.lights.ambient = {0.1f, 0.1f, 0.1f};
        scene.lights.shadow_area = kin::Bounds3{{-200.0f, -0.2f, -200.0f}, {200.0f, 25.0f, 100.0f}};
        scene.lights.shadow_cascades = 4;
        scene.lights.shadow_distance = 150.0f;

        // 56 rows of 50 posts: 2,800 copies, enough for the workers to take them.
        std::vector<Vec3f> centres;
        std::vector<kin::MeshInstance> posts;
        for (int row = 0; row < 56; ++row) {
            for (int column = 0; column < 50; ++column) {
                const Vec3f at{-147.0f + 6.0f * static_cast<float>(column), 0.5f, 16.0f - 2.5f * static_cast<float>(row)};
                posts.push_back({Mat4f::translation(at), Color::rgb(200, 200, 200)});
                centres.push_back(at);
            }
        }
        // One tall post a band, off the right of the view, its shadow reaching 40 units left:
        // the near, middle and far depths. `x_in` is a spot on the floor inside the view that the shadow covers.
        struct Band {
            float z;
            Vec3f tall_at, spot;
        };
        std::vector<Band> bands;
        for (const float z : {-5.0f, -40.0f, -100.0f}) {
            // The view's half width at that distance along the floor, from the camera's own projection.
            float half = 0.0f;
            for (float x = 0.0f; x < 300.0f; x += 0.5f) {
                const auto on_screen = scene.camera.project({x, 0.0f, z}, screen);
                if (!on_screen || on_screen->pos.x >= 160.0f) {
                    half = x;
                    break;
                }
            }
            assert(half > 5.0f);
            const float px = half + 16.0f;
            bands.push_back({z, {px, 10.0f, z}, {half - 8.0f, 0.0f, z}});
        }
        const auto render = [&](bool with_tall) {
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            scene.draw_instanced(post, posts, {.color = Color::rgb(200, 200, 200)});
            if (with_tall) {
                for (const Band& b : bands) {
                    scene.draw(tall, Mat4f::translation(b.tall_at), {.color = Color::rgb(200, 60, 60)});
                }
            }
            return shoot_view(renderer, r3d, view, scene);
        };
        const auto stats_with_workers = [&](bool workers, Pixels& picture) {
            renderer.set_job_system(workers ? &jobs : nullptr);
            picture = render(true);
            renderer.present();
            return r3d.stats();
        };
        Pixels serial_picture, worker_picture;
        const kin::Render3DStats serial = stats_with_workers(false, serial_picture);
        const kin::Render3DStats workers = stats_with_workers(true, worker_picture);
        renderer.set_job_system(nullptr);
        const std::size_t copies = centres.size() + bands.size();

        assert(serial.shadow_cascades == 4);
        assert(serial.cascade_casters == workers.cascade_casters && serial.shadow_casters == workers.shadow_casters &&
               serial.culled == workers.culled);
        assert(serial_picture.rgba == worker_picture.rgba);
        kin::u32 sum = 0;
        for (const kin::u32 n : serial.cascade_casters) {
            sum += n;
        }
        assert(sum == serial.shadow_casters);

        // The posts standing inside each cascade's slice of the view must be in its list.
        const kin::ShadowCascades plan = kin::fit_shadow_cascades(scene.camera, 160.0f / 120.0f, kin::normalize(scene.lights.sun.direction),
                                                                  scene.lights.shadow_area, 4, scene.lights.shadow_distance, 2048);
        assert(plan.count == 4);
        for (kin::u32 k = 0; k < 4; ++k) {
            const kin::ShadowCascade& c = plan.cascade[k];
            const Mat4f slice = kin::slice_of(scene.camera, c.slice_near, c.slice_far).view_projection(160.0f / 120.0f);
            kin::u32 inside = 0;
            for (const Vec3f& at : centres) {
                const Vec3f ndc = slice.apply_projective(at);
                inside += (std::abs(ndc.x) <= 1.0f && std::abs(ndc.y) <= 1.0f && ndc.z >= 0.0f && ndc.z <= 1.0f) ? 1u : 0u;
            }
            std::cerr << "cascade " << k << ": " << serial.cascade_casters[k] << " casters, at least " << inside << " in its slice\n";
            assert(inside > 0);
            assert(serial.cascade_casters[k] >= inside);
            assert(serial.cascade_casters[k] <= copies);
        }

        // The shadows of the posts out of view are in the picture where they fall, in each band.
        const Pixels with = render(true);
        renderer.present();
        const Pixels without = render(false);
        renderer.present();
        for (const Band& b : bands) {
            const auto spot = scene.camera.project(b.spot, screen);
            assert(spot && spot->pos.x >= 0.0f && spot->pos.x < 160.0f && spot->pos.y >= 0.0f && spot->pos.y < 120.0f);
            // The shadow is a ribbon a few pixels tall this far off: look for it round the spot.
            int darkened = 0;
            for (int dy = -6; dy <= 6; ++dy) {
                for (int dx = -4; dx <= 4; ++dx) {
                    const int x = static_cast<int>(spot->pos.x) + dx, y = static_cast<int>(spot->pos.y) + dy;
                    if (x >= 0 && x < 160 && y >= 0 && y < 120 && brightness(with.at(x, y)) * 10 < brightness(without.at(x, y)) * 7) {
                        ++darkened;
                    }
                }
            }
            std::cerr << "cascade casters: the shadow at z = " << b.z << ": " << darkened << " pixels darker\n";
            assert(darkened >= 3);
        }
    });
}

// Ambient occlusion (G2b): the foot of a wall is darker than with it off and
// open floor is not; only the ambient light is dimmed (the sun is not); off,
// the pre-pass and the compute are not run; the overlay and blended meshes
// are not in it.
void test_gpu_ambient_occlusion() {
    with_gpu("test_gpu_ambient_occlusion", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const AoRoom room{r3d};
        const kin::Mesh pane = make_box(r3d, {1.0f, 1.0f, 0.01f});
        const kin::Mesh block = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::View3D view_msaa = r3d.create_view({160, 120}, {.samples = 4});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 3.0f, 6.0f}, {0.0f, 0.3f, -2.0f}, 50.0f);
        scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
        scene.lights.ambient = {0.6f, 0.6f, 0.6f};
        // The floor's top is at y = 0, the wall's front at z = -2.
        const auto render = [&](const kin::AmbientOcclusion3D& ao, bool pane_in_front = false, bool gun = false, kin::View3D* target = nullptr) {
            scene.clear();
            scene.ambient_occlusion = ao;
            room.draw_floor(scene);
            room.draw_wall(scene);
            if (pane_in_front) {
                scene.draw(pane, Mat4f::translation({0.0f, 0.5f, 0.5f}),
                           {.color = Color::rgba(255, 255, 255, 128), .blend = kin::Blend3D::Alpha});
            }
            if (gun) {
                kin::Scene3D& hands = scene.overlay();
                hands.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 70.0f);
                hands.draw(block, Mat4f::translation({0.5f, -0.3f, -2.0f}), {.color = Color::rgb(200, 120, 60)});
            }
            Pixels p = shoot_view(renderer, r3d, target ? *target : view, scene);
            renderer.present();
            return p;
        };
        const auto at = [&](const Pixels& p, Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        const Vec3f foot{0.0f, 0.0f, -1.8f}, wall_foot{0.0f, 0.15f, -2.0f}, open{0.0f, 0.0f, 2.6f}, high_wall{0.0f, 3.0f, -2.0f};

        const Pixels off = render({.enabled = false, .radius = 3.0f});
        assert(r3d.stats().ao_draws == 0 && r3d.stats().ao_instances == 0 && r3d.stats().ao_dispatches == 0);
        const Pixels plain = render({});
        assert(plain.rgba == off.rgba); // off is off, whatever its other settings

        const Pixels on = render({.enabled = true, .radius = 3.0f});
        if (lacks(r3d.capabilities().ambient_occlusion, "ambient occlusion")) {
            return;
        }
        assert(r3d.stats().ao_dispatches > 0);
        assert(r3d.stats().ao_draws == 2 && r3d.stats().ao_instances == 2 && r3d.stats().ao_dispatches == 3);
        // The crease: both sides of it darker; the open floor and the high wall, not.
        const int foot_off = brightness(at(off, foot)), foot_on = brightness(at(on, foot));
        const int wall_off = brightness(at(off, wall_foot)), wall_on = brightness(at(on, wall_foot));
        std::cerr << "ao: floor at the foot " << foot_off << " -> " << foot_on << ", wall at the foot " << wall_off << " -> " << wall_on
                  << ", open floor " << brightness(at(off, open)) << " -> " << brightness(at(on, open)) << ", high wall "
                  << brightness(at(off, high_wall)) << " -> " << brightness(at(on, high_wall)) << "\n";
        assert(foot_on < foot_off * 95 / 100);
        assert(wall_on < wall_off * 96 / 100);
        expect(at(on, open), at(off, open), "open floor with occlusion", 3);
        expect(at(on, high_wall), at(off, high_wall), "the wall high above the floor with occlusion", 6);
        // Further from the crease it fades: between the foot and the open floor.
        const int mid = brightness(at(on, {0.0f, 0.0f, -0.6f}));
        assert(mid > foot_on && mid <= brightness(at(off, {0.0f, 0.0f, -0.6f})));

        // More intensity is darker; half the size is still occluded; with 4x MSAA too.
        assert(brightness(at(render({.enabled = true, .radius = 3.0f, .intensity = 2.0f}), foot)) < foot_on);
        assert(brightness(at(render({.enabled = true, .radius = 3.0f, .half_resolution = true}), foot)) < foot_off * 96 / 100);
        const Pixels multisampled_off = render({}, false, false, &view_msaa);
        const Pixels multisampled = render({.enabled = true, .radius = 3.0f}, false, false, &view_msaa);
        assert(brightness(at(multisampled, foot)) < brightness(at(multisampled_off, foot)) * 95 / 100);
        expect(at(multisampled, open), at(multisampled_off, open), "open floor with occlusion, multisampled", 3);

        // Only the ambient light is dimmed: with none, a floor lit by the sun
        // is the same with or without; with both, the sun's light stays.
        scene.lights.sun = {.direction = {0.0f, 1.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f}};
        scene.lights.ambient = {0.0f, 0.0f, 0.0f};
        const Pixels sun_off = render({});
        const Pixels sun_on = render({.enabled = true, .radius = 3.0f});
        expect(at(sun_on, foot), at(sun_off, foot), "sunlit floor at the foot of a wall", 3);
        expect(at(sun_on, wall_foot), at(sun_off, wall_foot), "sunlit wall at its foot", 3);
        scene.lights.ambient = {0.2f, 0.2f, 0.2f};
        const Pixels both_off = render({});
        const Pixels both_on = render({.enabled = true, .radius = 3.0f});
        const int sun_part = brightness(at(sun_off, foot));
        assert(brightness(at(both_on, foot)) >= sun_part); // never less than the sun alone gives
        assert(brightness(at(both_on, foot)) < brightness(at(both_off, foot)));
        scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
        scene.lights.ambient = {0.6f, 0.6f, 0.6f};

        // Blended meshes are not in the pre-pass; the overlay is neither occluded nor drawn into it.
        render({.enabled = true, .radius = 3.0f}, true);
        assert(r3d.stats().ao_instances == 2);
        const Pixels gun_off = render({}, false, true);
        const Pixels gun_on = render({.enabled = true, .radius = 3.0f}, false, true);
        assert(r3d.stats().ao_instances == 2);
        const auto hands = scene.overlay().camera.project({0.5f, -0.3f, -1.5f}, screen);
        assert(hands);
        expect(gun_on.at(hands->pos), gun_off.at(hands->pos), "the overlay is not occluded", 2);
    });
}


// Contact shadows (G2c): a thin pole on a floor under a low sun leaves a darker
// patch at its foot that the shadow map is too coarse to give; the floor far
// from it, the pole's top, an unlit face and a floor seen through a plate far
// nearer the camera are not changed; off, no pre-pass and no compute run and
// the picture is the old one; it works without ambient occlusion and shares
// the pre-pass with it; the overlay is not shadowed.
void test_gpu_contact_shadows() {
    with_gpu("test_gpu_contact_shadows", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const AoRoom room{r3d};
        const kin::Mesh plate = make_box(r3d, {0.24f, 0.02f, 0.24f});
        const kin::Mesh block = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::View3D view_msaa = r3d.create_view({160, 120}, {.samples = 4});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({-2.0f, 4.0f, 5.0f}, {0.0f, 0.3f, 0.0f}, 40.0f);
        // Low sun from the right and behind the pole: its shadow falls left and toward the camera.
        scene.lights.sun = {.direction = {1.0f, 0.7f, -0.5f}, .color = {1.0f, 1.0f, 1.0f}};
        scene.lights.ambient = {0.25f, 0.25f, 0.25f};
        // The floor at `beside` is lit by the sun with nothing near; a small
        // plate a metre and a half up hides, on the screen, where the march
        // from there samples (a quarter of the way along), but is far nearer the camera.
        const Vec3f beside{2.0f, 0.0f, 1.0f};
        const Vec3f sun_dir = normalize(scene.lights.sun.direction);
        Vec3f plate_at;
        {
            const Vec3f sample = beside + sun_dir * 0.25f, eye = scene.camera.position;
            const float k = (eye.y - 1.5f) / (eye.y - sample.y); // along the line from the eye to the sample, at y = 1.5
            plate_at = eye + (sample - eye) * k;
        }
        const auto render = [&](const kin::ContactShadows3D& contact, bool with_plate = false, bool gun = false, bool ao = false,
                                kin::View3D* target = nullptr) {
            scene.clear();
            scene.contact_shadows = contact;
            scene.ambient_occlusion = {.enabled = ao, .radius = 1.0f};
            room.draw_floor(scene);
            room.draw_pole(scene, 0.0f, 0.0f);
            if (with_plate) {
                scene.draw(plate, Mat4f::translation(plate_at), {.color = Color::rgb(200, 200, 200)});
            }
            if (gun) {
                kin::Scene3D& hands = scene.overlay();
                hands.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 70.0f);
                hands.draw(block, Mat4f::translation({0.5f, -0.3f, -2.0f}), {.color = Color::rgb(200, 120, 60)});
            }
            Pixels p = shoot_view(renderer, r3d, target ? *target : view, scene);
            renderer.present();
            return p;
        };
        const auto at = [&](const Pixels& p, Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        const Vec3f patch{-0.17f, 0.0f, 0.085f}, far_floor{-2.0f, 0.0f, 1.0f}, top{0.0f, 1.0f, 0.0f}, unlit{0.0f, 0.5f, 0.1f};
        const kin::ContactShadows3D on{.enabled = true, .distance = 0.5f, .thickness = 0.3f, .intensity = 1.0f, .steps = 20};

        const Pixels off = render({.enabled = false, .distance = 2.0f});
        assert(r3d.stats().ao_draws == 0 && r3d.stats().contact_dispatches == 0);
        const Pixels plain = render({});
        assert(plain.rgba == off.rgba); // off is off, whatever its other settings

        const Pixels shadowed = render(on);
        if (lacks(r3d.capabilities().contact_shadows, "contact shadows")) {
            return;
        }
        assert(r3d.stats().contact_dispatches > 0);
        // Without ambient occlusion: the pre-pass of both meshes, the march and one blur (two with KIN_CONTACT_CLASSIC=1).
        assert(r3d.stats().ao_draws == 2 && r3d.stats().ao_instances == 2 && r3d.stats().ao_dispatches == 0 &&
               r3d.stats().contact_dispatches == 2);
        const int patch_off = brightness(at(off, patch)), patch_on = brightness(at(shadowed, patch));
        std::cerr << "contact shadows: patch at the foot " << patch_off << " -> " << patch_on << ", far floor " << brightness(at(off, far_floor))
                  << " -> " << brightness(at(shadowed, far_floor)) << ", top " << brightness(at(off, top)) << " -> "
                  << brightness(at(shadowed, top)) << "\n";
        assert(patch_on < patch_off * 75 / 100);
        expect(at(shadowed, far_floor), at(off, far_floor), "floor far from the pole with contact shadows", 3);
        expect(at(shadowed, top), at(off, top), "the pole's top with contact shadows", 3);
        expect(at(shadowed, unlit), at(off, unlit), "a face turned from the sun with contact shadows", 3);
        // The patch fades with distance from the pole, and with the intensity.
        const int mid = brightness(at(shadowed, {-0.5f, 0.0f, 0.25f}));
        assert(mid >= patch_on && mid <= brightness(at(off, {-0.5f, 0.0f, 0.25f})));
        const int half = brightness(at(render({.enabled = true, .distance = 0.5f, .thickness = 0.15f, .intensity = 0.5f, .steps = 20}), patch));
        assert(half > patch_on && half < patch_off);
        // Nothing within reach, nothing shaded.
        expect(at(render({.enabled = true, .distance = 0.02f, .thickness = 0.15f, .steps = 20}), patch), at(off, patch),
               "a march too short to reach the pole", 3);

        // A plate far in front of the floor, over the screen where the march
        // would sample: further than the thickness, so it blocks nothing.
        const Pixels plate_off = render({}, true), plate_on = render(on, true);
        const auto here = scene.camera.project(beside, screen), there = scene.camera.project(plate_at, screen);
        assert(here && there && std::hypot(here->pos.x - there->pos.x, here->pos.y - there->pos.y) > 5.0f); // the plate is not over the floor point itself
        std::cerr << "contact shadows: plate " << std::hypot(here->pos.x - there->pos.x, here->pos.y - there->pos.y) << " px from the floor point: "
                  << brightness(at(plate_off, beside)) << " -> " << brightness(at(plate_on, beside)) << "\n";
        expect(at(plate_on, beside), at(plate_off, beside), "the floor beside a plate far in front, with contact shadows", 3);

        // Shared pre-pass: both on draw it once; each is as when alone.
        const Pixels ao_alone = render({}, false, false, true);
        const kin::u32 ao_instances = r3d.stats().ao_instances;
        const Pixels both = render(on, false, false, true);
        assert(r3d.stats().ao_instances == ao_instances && r3d.stats().ao_dispatches == 3 && r3d.stats().contact_dispatches == 3);
        assert(brightness(at(both, patch)) < brightness(at(ao_alone, patch)) * 80 / 100);
        // The two blurs are one dispatch a direction (the counts above are still each feature's own), and the picture is the one
        // with two (KIN_BLUR_SEPARATE=1): to the byte.
        assert(r3d.stats().blur_merged == 2);
        // (The contact shadows blurred alone have one pass, so the separate path is compared with KIN_CONTACT_CLASSIC=1, which has two, as the
        // merged blur does.)
        setenv("KIN_BLUR_SEPARATE", "1", 1);
        setenv("KIN_CONTACT_CLASSIC", "1", 1);
        const Pixels both_separate = render(on, false, false, true);
        unsetenv("KIN_CONTACT_CLASSIC");
        assert(r3d.stats().blur_merged == 0 && r3d.stats().ao_dispatches == 3 && r3d.stats().contact_dispatches == 3);
        assert(both.rgba == both_separate.rgba);
        // Without that, the separate path blurs the contact shadows once: two dispatches, and a picture a hair different.
        const Pixels both_one_blur = render(on, false, false, true);
        unsetenv("KIN_BLUR_SEPARATE");
        assert(r3d.stats().blur_merged == 0 && r3d.stats().ao_dispatches == 3 && r3d.stats().contact_dispatches == 2);
        assert(both_one_blur.rgba != both.rgba);
        assert(ao_alone.rgba != both.rgba && (render({}, false, false, true), r3d.stats().blur_merged == 0));

        // Four-times multisampled too.
        const Pixels ms_off = render({}, false, false, false, &view_msaa), ms_on = render(on, false, false, false, &view_msaa);
        assert(brightness(at(ms_on, patch)) < brightness(at(ms_off, patch)) * 80 / 100);
        expect(at(ms_on, far_floor), at(ms_off, far_floor), "far floor, multisampled, with contact shadows", 3);

        // The overlay is not in the pre-pass and not shadowed.
        const Pixels gun_off = render({}, false, true), gun_on = render(on, false, true);
        const auto hands = scene.overlay().camera.project({0.5f, -0.3f, -1.5f}, screen);
        assert(hands);
        expect(gun_on.at(hands->pos), gun_off.at(hands->pos), "the overlay is not shadowed", 2);
        // With no sun there is nothing to march toward: no passes.
        scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
        render(on);
        assert(r3d.stats().contact_dispatches == 0);
    });
}


// The speed-ups of ambient occlusion and contact shadows (opt-in speedups, phase 2): the linear depth chain the occlusion reads in big
// views (KIN_AO_CHAIN=1 forces it, KIN_AO_CLASSIC=1 forbids it; by default it is used from 1.5 million pixels up) and the contact
// shadows' single blur (KIN_CONTACT_CLASSIC=1 blurs twice). Each must give the picture of the classic path within a bound measured on
// these scenes (with margin), on a plane (a ramp of depth: no occlusion anywhere, so a wrong level, a wrong texel or a stale chain
// shows at once), in an orthographic view, at half resolution and under TAA's jitter, and after a resize and a release.
struct Delta {
    double mean = 0;
    int p99 = 0, worst = 0;
};

Delta delta(const Pixels& a, const Pixels& b) {
    assert(a.rgba.size() == b.rgba.size());
    std::vector<int> hist(256, 0);
    double sum = 0;
    std::size_t n = 0;
    int worst = 0;
    for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            const int d = std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c]));
            sum += d;
            ++hist[static_cast<std::size_t>(d)];
            worst = std::max(worst, d);
            ++n;
        }
    }
    std::size_t seen = 0;
    int p99 = 0;
    for (int d = 0; d < 256; ++d) {
        seen += static_cast<std::size_t>(hist[static_cast<std::size_t>(d)]);
        if (seen * 100 >= n * 99) {
            p99 = d;
            break;
        }
    }
    return {sum / static_cast<double>(n), p99, worst};
}

void test_gpu_occlusion_speedups() {
    with_gpu("test_gpu_occlusion_speedups", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const AoRoom room{r3d};
        if (lacks(r3d.capabilities().ambient_occlusion, "ambient occlusion")) {
            return;
        }
        const kin::Mesh block = make_box(r3d, {0.3f, 1.0f, 0.1f});
        const auto chain = [] { setenv("KIN_AO_CHAIN", "1", 1); unsetenv("KIN_AO_CLASSIC"); };
        const auto classic = [] { unsetenv("KIN_AO_CHAIN"); setenv("KIN_AO_CLASSIC", "1", 1); };
        const auto by_default = [] { unsetenv("KIN_AO_CHAIN"); unsetenv("KIN_AO_CLASSIC"); };
        kin::View3D view = r3d.create_view({640, 480}, {.samples = 1});
        kin::Scene3D scene;
        scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
        scene.lights.ambient = {0.6f, 0.6f, 0.6f};
        const kin::Camera3D perspective = kin::Camera3D::look_at({0.0f, 3.0f, 6.0f}, {0.0f, 0.3f, -2.0f}, 50.0f);
        kin::Camera3D ortho_camera = kin::Camera3D::look_at({0.0f, 6.0f, 6.0f}, {0.0f, 0.0f, -2.0f});
        ortho_camera.projection = kin::Projection3D::Orthographic;
        ortho_camera.height = 8.0f;
        const kin::Camera3D ortho = ortho_camera;
        enum class Draw { Room, Plane, Blocks };
        const auto render = [&](const kin::Camera3D& camera, const kin::AmbientOcclusion3D& ao, Draw what = Draw::Room, kin::View3D* target = nullptr,
                                bool taa = false, int stride = 1) {
            scene.clear();
            scene.camera = camera;
            scene.ambient_occlusion = ao;
            scene.taa = {.enabled = taa};
            room.draw_floor(scene);
            if (what == Draw::Room) {
                room.draw_wall(scene);
                room.draw_pole(scene, 1.0f, -0.5f);
            }
            if (what == Draw::Blocks) {
                // Thin things at several distances: where a coarse level of the chain is most likely to be wrong.
                for (int i = 0; i < 6; ++i) {
                    scene.draw(block, Mat4f::translation({-2.5f + static_cast<float>(i), 0.5f, -1.5f + 0.7f * static_cast<float>(i)}),
                               {.color = Color::rgb(200, 120, 60)});
                }
            }
            Pixels p = shoot_large(renderer, r3d, target ? *target : view, scene, stride);
            renderer.present();
            return p;
        };
        const kin::AmbientOcclusion3D ao{.enabled = true, .radius = 3.0f};

        // Which path ran: forced classic, forced chain, and by the size of the view (this one is 0.31 million pixels: classic).
        classic();
        const Pixels reference = render(perspective, ao);
        assert(r3d.stats().ao_chain_built == 0 && r3d.stats().ao_dispatches == 3);
        by_default();
        const Pixels small = render(perspective, ao);
        assert(r3d.stats().ao_chain_built == 0);
        assert(small.rgba == reference.rgba); // below the size where the chain pays, the picture is the old one, to the byte
        chain();
        const Pixels chained = render(perspective, ao);
        assert(r3d.stats().ao_chain_built == 1 && r3d.stats().ao_dispatches == 3); // the chain's own dispatch is not counted
        setenv("KIN_AO_CLASSIC", "1", 1);
        render(perspective, ao);
        assert(r3d.stats().ao_chain_built == 0); // classic wins over a forced chain
        chain();
        const Delta room_delta = delta(chained, reference);
        std::cerr << "ao chain: room mean " << room_delta.mean << " p99 " << room_delta.p99 << " worst " << room_delta.worst << "\n";
        assert(room_delta.mean <= 0.3 && room_delta.p99 <= 6 && room_delta.worst <= 40);
        assert(chained.rgba != reference.rgba); // a different (and not worse) filter: the test must be able to tell

        // A plane, which a chain with a wrong level, a wrong texel or a wrong position bends into creases: no occlusion anywhere.
        // Perspective and orthographic, a depth ramp each way (away from the camera, and across the screen).
        for (const kin::Camera3D* camera : {&perspective, &ortho}) {
            const Pixels flat_off = render(*camera, {}, Draw::Plane);
            classic();
            const Pixels flat_classic = render(*camera, ao, Draw::Plane);
            chain();
            const Pixels flat_chain = render(*camera, ao, Draw::Plane);
            // Away from the horizon (rows near it are a grazing sliver, where either path may darken a little).
            const Delta on_plane = delta(flat_chain, flat_classic);
            int darkest = 0, darkest_classic = 0;
            for (int y = 150; y < 470; y += 7) {
                for (int x = 20; x < 620; x += 7) {
                    darkest = std::max(darkest, brightness(flat_off.at(x, y)) - brightness(flat_chain.at(x, y)));
                    darkest_classic = std::max(darkest_classic, brightness(flat_off.at(x, y)) - brightness(flat_classic.at(x, y)));
                }
            }
            std::cerr << "ao chain: plane (" << (camera == &ortho ? "orthographic" : "perspective") << ") mean " << on_plane.mean << " p99 "
                      << on_plane.p99 << " worst " << on_plane.worst << ", darkest sample " << darkest << " (classic " << darkest_classic
                      << ") summed levels below unoccluded\n";
            assert(on_plane.mean <= 0.03 && on_plane.worst <= 8); // a plane: the two paths agree to a few thousandths of a level (measured 0.002, 0)
            assert(darkest <= darkest_classic + 3); // never creased more than the classic path's own noise
        }

        // Thin things at several distances, perspective and orthographic: within the bound of the classic path.
        for (const kin::Camera3D* camera : {&perspective, &ortho}) {
            classic();
            const Pixels thin_classic = render(*camera, ao, Draw::Blocks);
            chain();
            const Pixels thin_chain = render(*camera, ao, Draw::Blocks);
            const Delta d = delta(thin_chain, thin_classic);
            std::cerr << "ao chain: thin blocks (" << (camera == &ortho ? "orthographic" : "perspective") << ") mean " << d.mean << " p99 "
                      << d.p99 << " worst " << d.worst << "\n";
            assert(d.mean <= 0.3 && d.p99 <= 6 && d.worst <= 40);
        }

        // Half resolution.
        const kin::AmbientOcclusion3D half{.enabled = true, .radius = 3.0f, .half_resolution = true};
        classic();
        const Pixels half_classic = render(perspective, half);
        chain();
        const Pixels half_chain = render(perspective, half);
        assert(r3d.stats().ao_chain_built == 1);
        const Delta half_delta = delta(half_chain, half_classic);
        std::cerr << "ao chain: half resolution mean " << half_delta.mean << " p99 " << half_delta.p99 << " worst " << half_delta.worst << "\n";
        assert(half_delta.mean <= 0.3 && half_delta.p99 <= 6 && half_delta.worst <= 40);

        // Under temporal anti-aliasing of a still camera (the projection is jittered a little each frame: the chain unprojects with it): after 12
        // frames of each path, and frame to frame, the chain's pictures are as steady as the classic's.
        if (r3d.capabilities().temporal_aa) {
            const auto run = [&](bool use_chain) {
                if (use_chain) {
                    chain();
                } else {
                    classic();
                }
                view.reset_history();
                std::vector<Pixels> frames;
                for (int f = 0; f < 12; ++f) {
                    frames.push_back(render(perspective, ao, Draw::Blocks, nullptr, /*taa=*/true));
                }
                return frames;
            };
            const std::vector<Pixels> with_classic = run(false), with_chain = run(true);
            const Delta last = delta(with_chain.back(), with_classic.back());
            const Delta step_classic = delta(with_classic[10], with_classic[11]), step_chain = delta(with_chain[10], with_chain[11]);
            std::cerr << "ao chain under taa: last frame mean " << last.mean << " p99 " << last.p99 << " worst " << last.worst
                      << "; frame to frame classic worst " << step_classic.worst << " mean " << step_classic.mean << ", chain worst "
                      << step_chain.worst << " mean " << step_chain.mean << "\n";
            assert(last.mean <= 0.3 && last.p99 <= 6);
            assert(step_chain.mean <= step_classic.mean * 1.1 + 0.02 && step_chain.worst <= step_classic.worst + 6);
        }

        // A resize: the chain is made again at the new size (a stale one is too small after growing, and read at the wrong size after
        // shrinking): up, then down, each picture the one a fresh view of that size draws.
        chain();
        kin::View3D other = r3d.create_view({512, 384}, {.samples = 1});
        render(perspective, ao, Draw::Room, &other);
        assert(r3d.resize(other, {640, 480}));
        assert(render(perspective, ao, Draw::Room, &other).rgba == chained.rgba);
        assert(r3d.resize(other, {512, 384}));
        const Pixels resized = render(perspective, ao, Draw::Room, &other);
        kin::View3D fresh = r3d.create_view({512, 384}, {.samples = 1});
        assert(resized.rgba == render(perspective, ao, Draw::Room, &fresh).rgba);

        // Given back after a pause and made again: the chain goes with the pre-pass's depth.
        setenv("KIN_RELEASE_FRAMES", "3", 1);
        kin::View3D released = r3d.create_view({640, 480}, {.samples = 1});
        render(perspective, ao, Draw::Room, &released);
        const kin::u64 held = r3d.stats().feature_bytes;
        for (int f = 0; f < 4; ++f) {
            render(perspective, {}, Draw::Room, &released);
        }
        assert(r3d.stats().feature_bytes == 0 && held > 0);
        assert(render(perspective, ao, Draw::Room, &released).rgba == chained.rgba);
        unsetenv("KIN_RELEASE_FRAMES");

        // By default the chain is used from 1.5 million pixels (1600 x 1000 here), and gives the picture it gives when forced.
        by_default();
        kin::View3D large = r3d.create_view({1600, 1000}, {.samples = 1});
        const Pixels large_default = render(perspective, ao, Draw::Room, &large, false, 3);
        assert(r3d.stats().ao_chain_built == 1);
        chain();
        assert(render(perspective, ao, Draw::Room, &large, false, 3).rgba == large_default.rgba);
        classic();
        const Delta large_delta = delta(large_default, render(perspective, ao, Draw::Room, &large, false, 3));
        assert(r3d.stats().ao_chain_built == 0);
        std::cerr << "ao chain: 1600 x 1000 room mean " << large_delta.mean << " p99 " << large_delta.p99 << " worst " << large_delta.worst << "\n";
        assert(large_delta.mean <= 0.3 && large_delta.p99 <= 6 && large_delta.worst <= 40);
        by_default();

        // With contact shadows alone, one blur (the second pass changed the pictures by under 0.3 levels); KIN_CONTACT_CLASSIC=1 blurs twice.
        if (r3d.capabilities().contact_shadows) {
            const kin::Mesh pole = make_box(r3d, {0.2f, 1.0f, 0.2f});
            const auto shoot_contact = [&] {
                scene.clear();
                scene.camera = kin::Camera3D::look_at({-2.0f, 4.0f, 5.0f}, {0.0f, 0.3f, 0.0f}, 40.0f);
                scene.lights.sun = {.direction = {1.0f, 0.7f, -0.5f}, .color = {1.0f, 1.0f, 1.0f}};
                scene.lights.ambient = {0.25f, 0.25f, 0.25f};
                scene.ambient_occlusion = {};
                scene.contact_shadows = {.enabled = true, .distance = 0.5f, .thickness = 0.3f, .steps = 20};
                room.draw_floor(scene);
                scene.draw(pole, Mat4f::translation({0.0f, 0.5f, 0.0f}), room.grey);
                scene.draw(pole, Mat4f::translation({1.2f, 0.5f, -1.0f}), room.grey);
                Pixels p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                return p;
            };
            unsetenv("KIN_CONTACT_CLASSIC");
            const Pixels one = shoot_contact();
            assert(r3d.stats().contact_dispatches == 2); // the march and one blur
            setenv("KIN_CONTACT_CLASSIC", "1", 1);
            const Pixels two = shoot_contact();
            assert(r3d.stats().contact_dispatches == 3);
            unsetenv("KIN_CONTACT_CLASSIC");
            const Delta cd = delta(one, two);
            std::cerr << "contact shadows: one blur against two: mean " << cd.mean << " p99 " << cd.p99 << " worst " << cd.worst << "\n";
            assert(cd.mean <= 0.3 && cd.p99 <= 6 && cd.worst <= 60);
            assert(one.rgba != two.rgba);
            // The shadow is there either way (the pole's foot).
            const auto foot = scene.camera.project({-0.17f, 0.0f, 0.085f}, screen);
            assert(foot && brightness(one.at(foot->pos)) < brightness(two.at(foot->pos)) * 11 / 10);
        }
        by_default();
    });
}

} // namespace

// --- Physically based materials (G1a) ---

// A square in the xy plane facing +z, UV (0, 0) at its top left, `half` a side.
MeshData uv_square(float half) {
    MeshData square;
    square.vertices = {{{-half, -half, 0}, {0, 0, 1}, {0, 1}}, {{half, -half, 0}, {0, 0, 1}, {1, 1}},
                       {{half, half, 0}, {0, 0, 1}, {1, 0}}, {{-half, half, 0}, {0, 0, 1}, {0, 0}}};
    square.indices = {0, 1, 2, 0, 2, 3};
    return square;
}

float linear_level(Color c) { return kin::srgb_to_linear(static_cast<float>(c.r) / 255.0f); }

// A metal has no diffuse light: away from the highlight it is dark where a
// dielectric of the same colour is bright; at the highlight both shine (the
// dielectric's 4% of a sharp lobe still reaches white); and roughness widens it.
void test_gpu_metal_and_roughness() {
    with_gpu("test_gpu_metal_and_roughness", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh sphere = r3d.create_mesh(kin::meshes::sphere(1.0f, 48, 24));
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 30.0f);
        // The sun at the camera: the highlight is at the sphere's centre.
        scene.lights.sun = {.direction = {0.0f, 0.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f}};
        scene.lights.ambient = {0.0f, 0.0f, 0.0f};
        const auto shoot = [&](const kin::Material3D& material) {
            scene.clear();
            scene.draw(sphere, Mat4f::identity(), material);
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const Color grey = Color::rgb(190, 190, 190);
        const Pixels plastic = shoot({.color = grey, .metallic = 0.0f, .roughness = 0.3f});
        const Pixels metal = shoot({.color = grey, .metallic = 1.0f, .roughness = 0.3f});
        // Where the surface leans 45 degrees from the light: outside the lobe.
        const auto lean = scene.camera.project({0.707f, 0.0f, 0.707f}, screen);
        assert(lean);
        const float diffuse = linear_level(plastic.at(lean->pos)), shine = linear_level(metal.at(lean->pos));
        assert(diffuse > 0.2f && shine < 0.2f * diffuse);
        // The centre: both bright; the metal's tinted by its colour, the plastic's white.
        assert(plastic.at(80, 60).r > 230 && metal.at(80, 60).r > 230);

        // The lobe's size: pixels over a quarter of the light, a metal smooth and rough.
        const auto lobe = [&](float roughness) {
            const Pixels p = shoot({.color = Color::rgb(255, 255, 255), .metallic = 1.0f, .roughness = roughness});
            int count = 0;
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    count += linear_level(p.at(x, y)) > 0.25f ? 1 : 0;
                }
            }
            return count;
        };
        const int sharp = lobe(0.2f), wide = lobe(0.6f);
        assert(sharp > 0 && wide > 2 * sharp);

        // A roughness floor: a mirror's lobe is still a patch, not lost between pixels.
        assert(lobe(0.0f) > 0);
    });
}

// A normal map tilts the shading: a pixel's normal leans where the map says,
// +x along u and up (+y here) where green is high; a flat map changes nothing.
void test_gpu_normal_map() {
    with_gpu("test_gpu_normal_map", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh square = r3d.create_mesh(uv_square(0.9f));
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const auto map = [&](kin::u8 r, kin::u8 g, kin::u8 b) {
            const kin::u8 texel[4] = {r, g, b, 255};
            const kin::Texture texture = renderer.create_texture({1, 1}, kin::TextureFormat::Rgba8, texel);
            assert(texture.valid());
            return texture;
        };
        const kin::Texture flat = map(128, 128, 255), toward_x = map(255, 128, 203), away_x = map(0, 128, 203),
                           up = map(128, 255, 203), down = map(128, 0, 203);
        // The light from up and the right, at 45 degrees to the quad's normal both ways.
        const auto shoot = [&](const kin::Texture& normal, Vec3f sun, float scale = 1.0f) {
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, 40.0f);
            scene.lights.sun = {.direction = sun, .color = {1.0f, 1.0f, 1.0f}};
            scene.lights.ambient = {0.0f, 0.0f, 0.0f};
            scene.draw(square, Mat4f::identity(), {.color = Color::rgb(200, 200, 200), .normal = normal, .normal_scale = scale});
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return linear_level(p.at(80, 60));
        };
        const Vec3f from_right{0.5f, 0.0f, 0.5f}, from_above{0.0f, 0.5f, 0.5f};
        const float level = shoot(flat, from_right);
        assert(level > 0.2f);
        assert(shoot({}, from_right) > 0.95f * level && shoot({}, from_right) < 1.05f * level); // no map: the same as flat
        // The normal leaned toward the light is brighter, away from it darker.
        assert(shoot(toward_x, from_right) > 1.15f * level);
        assert(shoot(away_x, from_right) < 0.4f * level);
        // Green is up: along -v, which runs down the image.
        assert(shoot(up, from_above) > 1.15f * shoot(flat, from_above));
        assert(shoot(down, from_above) < 0.4f * shoot(flat, from_above));
        // A scale of nothing is a flat map.
        const float none = shoot(toward_x, from_right, 0.0f);
        assert(none > 0.95f * level && none < 1.05f * level);
    });
}

// Mask: a texture's transparent half is not drawn, casts no shadow and is not
// picked, the opaque half does all three.
void test_gpu_alpha_mask() {
    with_gpu("test_gpu_alpha_mask", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = make_box(r3d, {20.0f, 0.2f, 20.0f});
        // A square lying flat (facing +y), a leaf: left half clear, right half green.
        const kin::Mesh leaf_mesh = r3d.create_mesh(uv_square(1.5f));
        const kin::u8 texels[8] = {20, 200, 20, 0, 20, 200, 20, 255};
        const kin::Texture leaf = renderer.create_texture_from_rgba(texels, {2, 1});
        renderer.set_scale_mode(leaf, kin::ScaleMode::Nearest);
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .pick = true});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 12.0f, 0.01f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.lights.sun = {.direction = {1.0f, 1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}};
        scene.lights.ambient = {0.15f, 0.15f, 0.15f};
        scene.lights.shadow_area = kin::Bounds3{{-10.0f, -0.2f, -10.0f}, {10.0f, 4.0f, 10.0f}};
        const auto shoot = [&](kin::Blend3D blend) {
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            scene.draw(leaf_mesh, Mat4f::translation({0.0f, 2.5f, 0.0f}) * Mat4f::rotation_x(-90.0f),
                       {.color = Color::rgb(255, 255, 255), .double_sided = true, .blend = blend, .texture = leaf}, kin::colors::white, 9);
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const auto where = [&](const Pixels& p, Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        const auto pixel = [&](Vec3f w) {
            const kin::Vec2f p = scene.camera.project(w, screen)->pos;
            return kin::Vec2i{static_cast<int>(p.x), static_cast<int>(p.y)};
        };
        const Pixels cut = shoot(kin::Blend3D::Mask);
        // Seen from above: the floor through the clear half, the leaf on the right.
        const Color floor_colour = where(cut, {-0.75f, 2.5f, 0.0f});
        const Color leaf_colour = where(cut, {0.75f, 2.5f, 0.0f});
        assert(floor_colour.g < 200 && std::abs(int(floor_colour.r) - int(floor_colour.g)) < 12); // grey floor
        assert(leaf_colour.g > leaf_colour.r + 50);                                                // green leaf
        // The leaf's shadow, 2.5 m toward -x, falls from its right half only: it
        // spans x -2.5 to -1 on the floor; the clear half's would be x -4 to -2.5.
        const Color shadow = where(cut, {-1.75f, 0.0f, 0.0f}), lit = where(cut, {-3.25f, 0.0f, 0.0f});
        assert(brightness(lit) > 2 * brightness(shadow) && brightness(shadow) > 0);
        // Picked where drawn.
        assert(r3d.pick(view, pixel({0.75f, 2.5f, 0.0f})).id == 9 && r3d.pick(view, pixel({0.75f, 2.5f, 0.0f})).hit);
        const kin::PickResult through = r3d.pick(view, pixel({-0.75f, 2.5f, 0.0f}));
        assert(!through.hit || through.id != 9);

        // As Opaque the alpha is ignored by the depth tests: the whole square
        // hides the floor and casts all its shadow.
        const Pixels solid = shoot(kin::Blend3D::Opaque);
        assert(brightness(where(solid, {-0.75f, 2.5f, 0.0f})) < 30);
        assert(brightness(where(solid, {-3.25f, 0.0f, 0.0f})) < brightness(lit) / 2);
    });
}

// A point light falls off with the square of the distance: twice as far, a
// quarter of the light (on a rough surface facing it).
void test_gpu_inverse_square() {
    with_gpu("test_gpu_inverse_square", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh wall = make_box(r3d, {40.0f, 40.0f, 0.2f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const auto level = [&](float distance, float radius) {
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 30.0f}, {0.0f, 0.0f, 0.0f}, 20.0f);
            scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
            scene.lights.ambient = {0.0f, 0.0f, 0.0f};
            scene.draw(wall, Mat4f::translation({0.0f, 0.0f, -0.1f}), {.color = Color::rgb(200, 200, 200)});
            scene.add_light({.position = {0.0f, 0.0f, distance}, .color = {3.0f, 3.0f, 3.0f}, .radius = radius});
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return linear_level(p.at(80, 60));
        };
        const float near_level = level(2.0f, 100.0f), far_level = level(4.0f, 100.0f), farther = level(8.0f, 100.0f);
        assert(near_level > 0.3f && near_level < 1.0f);
        assert(near_level / far_level > 3.6f && near_level / far_level < 4.4f);
        assert(far_level / farther > 3.6f && far_level / farther < 4.4f);
        // Windowed to nothing at the radius, and dimmer than the bare law inside it.
        assert(level(4.0f, 4.0f) < 0.01f);
        assert(level(2.0f, 8.0f) < near_level && level(2.0f, 8.0f) > 0.95f * near_level);
    });
}

// A model's materials: every glTF metallic-roughness field read; its maps
// of data are numbers, not colours (a flat normal map shades as none).
void test_gpu_model_materials() {
    with_gpu("test_gpu_model_materials", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        std::string error;
        const kin::Model model = r3d.load_model(std::filesystem::path{KIN_TEST_MODELS} / "pbr.glb", &error);
        assert(model.valid() && model.parts.size() == 3);
        const auto part = [&](std::string_view name) -> const kin::ModelPart& {
            for (const kin::ModelPart& p : model.parts) {
                if (p.name == name) return p;
            }
            assert(false);
            return model.parts.front();
        };
        const kin::Material3D& all = part("everything").material;
        assert(all.metallic == 0.75f && all.roughness == 0.5f && all.normal_scale == 0.5f);
        assert(all.occlusion_strength == 0.25f && all.alpha_cutoff == 0.375f && all.blend == kin::Blend3D::Mask);
        assert(all.texture.valid() && all.metallic_roughness.valid() && all.normal.valid() && all.occlusion.valid() &&
               all.emissive_texture.valid());
        assert(!(all.metallic_roughness == all.texture) && all.double_sided);
        assert(all.wrap_u == kin::Wrap3D::Repeat && all.wrap_v == kin::Wrap3D::Repeat); // glTF's default
        assert(part("plain").material.roughness == 1.0f && !part("plain").material.normal.valid());

        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 7.0f}, {0.0f, 0.0f, 0.0f}, 40.0f);
        scene.lights.sun = {.direction = {0.0f, 0.0f, 1.0f}, .color = {0.8f, 0.8f, 0.8f}};
        scene.lights.ambient = {0.0f, 0.0f, 0.0f};
        scene.draw(part("mapped").mesh, part("mapped").transform, part("mapped").material);
        scene.draw(part("plain").mesh, part("plain").transform, part("plain").material);
        const Pixels p = shoot_view(renderer, r3d, view, scene);
        const auto at = [&](Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        const Color with_map = at({-2.0f, 0.0f, 0.0f}), without = at({2.0f, 0.0f, 0.0f});
        assert(brightness(without) > 300);
        expect(with_map, without, "a flat normal map, as none", 4);
        renderer.present();
    });
}

// Wrap modes: a texture of two texels (red, green) over a quad whose u runs 0
// to 2: clamped, the edge texel goes on; repeated, it tiles; mirrored, every
// other copy is flipped.
void test_gpu_wrap_modes() {
    with_gpu("test_gpu_wrap_modes", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        MeshData wide;
        wide.vertices = {{{-1, -1, 0}, {0, 0, 1}, {0, 1}}, {{1, -1, 0}, {0, 0, 1}, {2, 1}},
                         {{1, 1, 0}, {0, 0, 1}, {2, 0}}, {{-1, 1, 0}, {0, 0, 1}, {0, 0}}};
        wide.indices = {0, 1, 2, 0, 2, 3};
        const kin::Mesh mesh = r3d.create_mesh(wide);
        const kin::u8 texels[8] = {255, 0, 0, 255, 0, 255, 0, 255};
        const kin::Texture texture = renderer.create_texture_from_rgba(texels, {2, 1});
        renderer.set_scale_mode(texture, kin::ScaleMode::Nearest);
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const Color red = Color::rgb(255, 0, 0), green = Color::rgb(0, 255, 0);
        const auto shoot = [&](kin::Wrap3D wrap) {
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, 40.0f);
            scene.draw(mesh, Mat4f::identity(), {.unlit = true, .texture = texture, .wrap_u = wrap, .wrap_v = wrap});
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            const auto at = [&](float x) { return p.at(scene.camera.project({x, 0.0f, 0.0f}, screen)->pos); };
            return std::array<Color, 4>{at(-0.75f), at(-0.25f), at(0.25f), at(0.75f)};
        };
        const auto clamp = shoot(kin::Wrap3D::Clamp), repeat = shoot(kin::Wrap3D::Repeat), mirror = shoot(kin::Wrap3D::Mirror);
        const std::array<Color, 4> want_clamp{red, green, green, green}, want_repeat{red, green, red, green},
            want_mirror{red, green, green, red};
        for (int i = 0; i < 4; ++i) {
            expect(clamp[static_cast<std::size_t>(i)], want_clamp[static_cast<std::size_t>(i)], "clamped");
            expect(repeat[static_cast<std::size_t>(i)], want_repeat[static_cast<std::size_t>(i)], "repeated");
            expect(mirror[static_cast<std::size_t>(i)], want_mirror[static_cast<std::size_t>(i)], "mirrored");
        }
    });
}

// An environment of six colours (+z red, +x green, -x blue, -z yellow, up
// white, down none), in the orientation environment.hpp says.
std::vector<float> six_colour_sky(kin::Vec2i size) {
    std::vector<float> rgb(static_cast<std::size_t>(size.x * size.y) * 3);
    constexpr float pi = 3.14159265f;
    for (int y = 0; y < size.y; ++y) {
        for (int x = 0; x < size.x; ++x) {
            const float phi = ((static_cast<float>(x) + 0.5f) / static_cast<float>(size.x) - 0.5f) * 2.0f * pi;
            const float theta = (0.5f - (static_cast<float>(y) + 0.5f) / static_cast<float>(size.y)) * pi;
            const Vec3f d{std::cos(phi) * std::cos(theta), std::sin(theta), std::sin(phi) * std::cos(theta)};
            Vec3f c{};
            if (d.y > 0.8f) c = {0.8f, 0.8f, 0.8f};
            else if (d.z > 0.8f) c = {0.8f, 0.0f, 0.0f};
            else if (d.x > 0.8f) c = {0.0f, 0.8f, 0.0f};
            else if (d.x < -0.8f) c = {0.0f, 0.0f, 0.8f};
            else if (d.z < -0.8f) c = {0.8f, 0.8f, 0.0f};
            const std::size_t i = static_cast<std::size_t>(y * size.x + x) * 3;
            rgb[i] = c.x;
            rgb[i + 1] = c.y;
            rgb[i + 2] = c.z;
        }
    }
    return rgb;
}

// A polished metal sphere reflects the environment where it should (the
// colours' directions: +z at its centre, -x 45 degrees to the left of it),
// a rougher one blurs it, the diffuse side takes its light from it, and its
// filtered copies are made once.
void test_gpu_environment() {
    with_gpu("test_gpu_environment", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Vec2i size{256, 128};
        const std::vector<float> sky = six_colour_sky(size);
        kin::Environment3D environment = r3d.create_environment(size, sky);
        if (!environment.valid()) {
            std::cerr << "no compute: environment not made\n";
            assert(!r3d.available() || false);
        }
        assert(environment.size() == size);
        const kin::Mesh sphere = r3d.create_mesh(kin::meshes::sphere(1.0f, 64, 32));
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 30.0f);
        scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
        scene.lights.ambient = {1.0f, 0.0f, 1.0f}; // not used with an environment
        scene.lights.environment = environment;
        const auto shoot = [&](const kin::Material3D& material) {
            scene.clear();
            scene.draw(sphere, Mat4f::identity(), material);
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const auto on_sphere = [&](const Pixels& p, Vec3f normal) { return p.at(scene.camera.project(normal, screen)->pos); };
        const Vec3f left = normalize(Vec3f{-1.0f, 0.0f, 1.0f}), right = normalize(Vec3f{1.0f, 0.0f, 1.0f}), top = normalize(Vec3f{0.0f, 1.0f, 1.0f});
        const kin::Material3D mirror{.color = Color::rgb(255, 255, 255), .metallic = 1.0f, .roughness = 0.0f};
        const Pixels sharp = shoot(mirror);
        assert(r3d.stats().environment_prefilters == 1);
        const auto channel = [](Color c) { return std::array<int, 3>{c.r, c.g, c.b}; };
        // 0.8 linear is 231 stored.
        const auto dominant = [&](Color c, int index) {
            const auto v = channel(c);
            return v[static_cast<std::size_t>(index)] > 200 && v[static_cast<std::size_t>((index + 1) % 3)] < 60 &&
                   v[static_cast<std::size_t>((index + 2) % 3)] < 60;
        };
        assert(dominant(on_sphere(sharp, {0.0f, 0.0f, 1.0f}), 0));  // +z: red
        assert(dominant(on_sphere(sharp, right), 1));               // +x: green
        assert(dominant(on_sphere(sharp, left), 2));                // -x: blue
        const Color up = on_sphere(sharp, top);                     // up: white
        assert(up.r > 200 && up.g > 200 && up.b > 200);
        // Filtered once: the next renders reuse it.
        shoot(mirror);
        assert(r3d.stats().environment_prefilters == 0);

        // Rough: the sharp colours run into each other.
        const Color blurred = on_sphere(shoot({.color = Color::rgb(255, 255, 255), .metallic = 1.0f, .roughness = 0.7f}), {0.0f, 0.0f, 1.0f});
        assert(blurred.r < sharp.at(80, 60).r && blurred.g > 40 && blurred.b > 40);
        // A rough dielectric takes its diffuse light from all round: lit from above by the white cap, from below by the coloured sides only.
        const Pixels plastic = shoot({.color = Color::rgb(200, 200, 200), .metallic = 0.0f, .roughness = 1.0f});
        const Color lit = on_sphere(plastic, normalize(Vec3f{0.0f, 1.0f, 0.2f})), unlit = on_sphere(plastic, normalize(Vec3f{0.0f, -1.0f, 0.2f}));
        assert(brightness(lit) > 1.5f * static_cast<float>(brightness(unlit)) && std::abs(int(lit.r) - int(lit.b)) < 25); // the cap is white
        // Intensity scales it; rotation turns it (a quarter turn about +y takes the red to the right).
        scene.lights.environment_rotation = 90.0f;
        const Pixels turned = shoot(mirror);
        assert(dominant(on_sphere(turned, right), 0) || dominant(on_sphere(turned, left), 0));
        scene.lights.environment_rotation = 0.0f;
        scene.lights.environment_intensity = 0.25f;
        const float dim = linear_level(on_sphere(shoot(mirror), {0.0f, 0.0f, 1.0f})), full = linear_level(sharp.at(80, 60));
        assert(dim > 0.2f * full && dim < 0.3f * full); // a quarter, in linear light
    });
}

// The green sky. An environment's picture is 2:1, and the GPU's mip chain (SDL's blits) never wrote its
// last, 1 x 1 level, which the filter's widest lobes read, so the specular levels held whatever that
// memory did (stale numbers, now and then a NaN, which the filter's `min(sample, 1e4)` turns into 1e4:
// the metal spheres of ibl_spheres drew green in some runs). KIN_POISON_TEXTURES fills the levels to be
// generated with NaN first, so a level nothing writes shows every time. A flat environment of 0.25 lit
// by nothing else: a white mirror-ish sphere can't be brighter than 0.25 linear anywhere, and the picture
// is the same with and without the poison.
void test_gpu_environment_levels_written() {
    with_gpu("test_gpu_environment_levels_written", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Vec2i size{512, 256}; // wide enough for the widest lobes to reach the last level
        const std::vector<float> flat(static_cast<std::size_t>(size.x * size.y) * 3, 0.25f);
        const kin::Mesh sphere = r3d.create_mesh(kin::meshes::sphere(1.0f, 64, 32));
        const auto picture = [&](bool poisoned, float roughness) {
            if (poisoned) {
                setenv("KIN_POISON_TEXTURES", "1", 1);
            }
            kin::Environment3D environment = r3d.create_environment(size, flat);
            unsetenv("KIN_POISON_TEXTURES");
            assert(environment.valid());
            kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 30.0f);
            scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
            scene.lights.ambient = {0.0f, 0.0f, 0.0f};
            scene.lights.environment = environment;
            scene.draw(sphere, Mat4f::identity(), {.color = Color::rgb(255, 255, 255), .metallic = 1.0f, .roughness = roughness});
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        // Roughness 1/6 is the filter's level 1, whose widest lobe samples reach the last source level; 0.5 and 1 are the levels after.
        for (const float roughness : {1.0f / 6.0f, 0.5f, 1.0f}) {
            const Pixels clean = picture(false, roughness), poisoned = picture(true, roughness);
            float brightest = 0.0f, dimmest = 1.0f;
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    const Color c = poisoned.at(x, y);
                    const float v = linear_level(Color::rgb(std::max({c.r, c.g, c.b}), 0, 0)); // the brightest channel, linear
                    brightest = std::max(brightest, v);
                    if (x >= 72 && x < 88 && y >= 54 && y < 66) {
                        dimmest = std::min(dimmest, v); // the middle of the sphere
                    }
                }
            }
            if (brightest > 0.3f || dimmest < 0.05f) {
                std::cerr << "roughness " << roughness << ": brightest channel " << brightest << " (at most 0.25 is lit), sphere's middle " << dimmest << "\n";
                assert(false && "the environment's mip levels were not all written");
            }
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    const Color a = clean.at(x, y), b = poisoned.at(x, y);
                    assert(a.r == b.r && a.g == b.g && a.b == b.b && "the picture does not depend on what an unwritten level held");
                }
            }
        }
    });
}

// A material's own fragment shader (tests/shaders, compiled by glslc now,
// against kin_material.inc.glsl): one that calls kin's lighting by hand looks
// as the built-in does; one that ignores it shows what it writes, from the
// parameters and the world position; both change with the parameters, and a
// shader that doesn't compile is refused.
void test_gpu_material_shader() {
    with_gpu("test_gpu_material_shader", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (!kin::shader_compiler_available()) {
            skip_or_require_gpu_test("test_gpu_material_shader", "no glslc");
            return;
        }
        const std::filesystem::path dir{KIN_TEST_MATERIAL_SHADERS};
        std::string error;
        const kin::MaterialShader3D lit = r3d.compile_material_shader(dir / "custom_lit.frag.glsl", &error);
        if (!lit.valid()) {
            std::cerr << error << '\n';
            assert(false);
        }
        const kin::MaterialShader3D flat = r3d.compile_material_shader(dir / "custom_flat.frag.glsl", &error);
        assert(flat.valid() && !(flat == lit));
        const std::filesystem::path broken = std::filesystem::temp_directory_path() / "kin_broken_material.frag.glsl";
        std::ofstream{broken} << "#version 450 core\n#include \"kin_material.inc.glsl\"\nvoid main() { nonsense; }\n";
        assert(!r3d.compile_material_shader(broken, &error).valid() && !error.empty());
        std::filesystem::remove(broken);

        const kin::Mesh sphere = r3d.create_mesh(kin::meshes::sphere(1.0f, 48, 24));
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const auto shoot = [&](const kin::Material3D& material) {
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 30.0f);
            scene.lights.sun = {.direction = {0.5f, 0.6f, 1.0f}, .color = {1.0f, 0.95f, 0.9f}};
            scene.lights.ambient = {0.1f, 0.12f, 0.2f};
            scene.add_light({.position = {-2.0f, 0.0f, 2.0f}, .color = {3.0f, 1.0f, 0.5f}, .radius = 6.0f});
            scene.draw(sphere, Mat4f::identity(), material);
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const kin::Material3D plain{.color = Color::rgb(220, 120, 80), .metallic = 0.3f, .roughness = 0.4f};
        kin::Material3D custom = plain;
        custom.shader = lit;
        custom.shader_params[0] = custom.shader_params[1] = custom.shader_params[2] = 1.0f;
        const Pixels built_in = shoot(plain), by_hand = shoot(custom);
        for (const kin::Vec2f at : {kin::Vec2f{80, 60}, kin::Vec2f{60, 45}, kin::Vec2f{95, 70}, kin::Vec2f{70, 80}}) {
            expect(by_hand.at(at), built_in.at(at), "kin's lighting called from a shader", 2);
        }
        // Its parameters reach it: half the light.
        custom.shader_params[0] = custom.shader_params[1] = custom.shader_params[2] = 0.5f;
        assert(brightness(shoot(custom).at(80, 60)) < brightness(built_in.at(80, 60)) * 0.75);

        // A shader that ignores the lights: its colour, by the world position.
        kin::Material3D unlit{.shader = flat};
        unlit.shader_params[4] = 0.0f;
        unlit.shader_params[5] = 0.8f; // kin_param(1).rgb = (0, 0.8, 0.2)
        unlit.shader_params[6] = 0.2f;
        const Pixels flat_picture = shoot(unlit);
        const Color left_side = flat_picture.at(70, 60), right_side = flat_picture.at(90, 60);
        assert(left_side.g > 100 && left_side.r < 10 && right_side.g > left_side.g); // brighter toward +x
    });
}


// --- Temporal anti-aliasing (G2d) ---

// The jitter: Halton (2, 3) from its second element, in pixels in [-0.5, 0.5),
// repeating with the period, every point of a period different, centred on the
// pixel; a projection moved by it shifts every point by that much whatever its depth.
void test_taa_jitter() {
    const kin::Vec2f first = kin::taa_jitter(0);
    assert(std::abs(first.x - 0.0f) < 1e-6f && std::abs(first.y - (1.0f / 3.0f - 0.5f)) < 1e-6f); // Halton(1): (1/2, 1/3)
    const kin::Vec2f second = kin::taa_jitter(1); // Halton(2): (1/4, 2/3)
    assert(std::abs(second.x - (0.25f - 0.5f)) < 1e-6f && std::abs(second.y - (2.0f / 3.0f - 0.5f)) < 1e-6f);
    kin::Vec2f sum{};
    for (kin::u32 period : {1u, 4u, 8u, 16u}) {
        for (kin::u32 f = 0; f < 3 * period + 5; ++f) {
            const kin::Vec2f j = kin::taa_jitter(f, period);
            assert(j.x >= -0.5f && j.x < 0.5f && j.y >= -0.5f && j.y < 0.5f);
            assert(j.x == kin::taa_jitter(f % period, period).x && j.y == kin::taa_jitter(f % period, period).y); // repeats
        }
    }
    for (kin::u32 a = 0; a < 8; ++a) {
        const kin::Vec2f j = kin::taa_jitter(a);
        sum = {sum.x + j.x, sum.y + j.y};
        for (kin::u32 b = a + 1; b < 8; ++b) {
            const kin::Vec2f k = kin::taa_jitter(b);
            assert(std::abs(j.x - k.x) > 1e-4f || std::abs(j.y - k.y) > 1e-4f); // all different within a period
        }
    }
    assert(std::abs(sum.x / 8.0f) < 0.1f && std::abs(sum.y / 8.0f) < 0.1f); // and centred on the pixel
    // Out of range periods are clamped; 1 is a single point.
    assert(kin::taa_jitter(5, 1).x == kin::taa_jitter(0, 1).x && kin::taa_jitter(0, 0).y == kin::taa_jitter(9, 1).y);
    assert(kin::taa_jitter(17, 99).x == kin::taa_jitter(1, 16).x);
    // The shifted projection: a point near and a point far both land the same distance from where they were, in pixels.
    const kin::Camera3D camera = kin::Camera3D::look_at({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
    const Mat4f vp = camera.view_projection(4.0f / 3.0f);
    const kin::Vec2f pixels{0.3f, -0.4f};
    const Mat4f moved = kin::jitter_projection(vp, pixels, {160, 120});
    for (const Vec3f p : {Vec3f{0.5f, 0.2f, 3.0f}, Vec3f{-1.0f, 0.7f, -20.0f}}) {
        const Vec3f a = vp.apply_projective(p), b = moved.apply_projective(p);
        assert(std::abs((b.x - a.x) * 80.0f - pixels.x) < 1e-3f); // ndc 2 across 160 pixels
        assert(std::abs((a.y - b.y) * 60.0f - pixels.y) < 1e-3f); // rows run down
        assert(std::abs(a.z - b.z) < 1e-6f);
    }
}

int level(Color c) { return c.g; }

// The linear light of an 8-bit sRGB value, and back.
float decode_srgb(kin::u8 v) {
    return kin::srgb_to_linear(static_cast<float>(v) / 255.0f);
}
kin::u8 encode_srgb(float c) {
    return static_cast<kin::u8>(std::lround(kin::linear_to_srgb(std::clamp(c, 0.0f, 1.0f)) * 255.0f));
}

// A fixed orthographic camera, 10 pixels a unit over the 160 x 120 window.
kin::Camera3D flat_camera(float dx = 0.0f, float dy = 0.0f) {
    kin::Camera3D camera;
    camera.position = {dx, dy, 5.0f};
    camera.projection = kin::Projection3D::Orthographic;
    camera.height = 12.0f;
    camera.z_near = 0.1f;
    camera.z_far = 20.0f;
    return camera;
}

// A wide plane (80 x 12 units, at z = 0) with a hard-edged checker texture of 4-pixel cells under flat_camera,
// sampled at a point, so that the texture aliases as a game's fine detail does.
struct PatternSurface {
    kin::Mesh mesh;
    kin::Texture texture;
};

PatternSurface make_pattern_surface(kin::Renderer3D& r3d, kin::Renderer2D& renderer) {
    std::vector<kin::u8> texels(640 * 480 * 4);
    for (int y = 0; y < 480; ++y) {
        for (int x = 0; x < 640; ++x) {
            const kin::u8 v = ((x / 16 + y / 16) % 2 == 0) ? 30 : 220;
            const std::size_t i = static_cast<std::size_t>(y * 640 + x) * 4;
            texels[i] = texels[i + 1] = texels[i + 2] = v;
            texels[i + 3] = 255;
        }
    }
    kin::MeshData plane;
    plane.vertices = {{{-40, -6, 0}, {0, 0, 1}, {0, 1}}, {{40, -6, 0}, {0, 0, 1}, {5, 1}},
                      {{40, 6, 0}, {0, 0, 1}, {5, 0}}, {{-40, 6, 0}, {0, 0, 1}, {0, 0}}};
    plane.indices = {0, 1, 2, 0, 2, 3};
    return {r3d.create_mesh(plane), renderer.create_texture_from_rgba(texels.data(), {640, 480})};
}

// Temporal anti-aliasing: off is the old picture and runs nothing; on, a still
// scene settles to a picture with the intermediate levels along edges that one
// sample a pixel can't make, near what a supersampled render gives; a moving
// object leaves no trail and a new one no ghost; the history is dropped on a
// resize, a reset and by a changed camera kind; a view asked for 4x MSAA draws
// one sample; the gun overlay stays in front and is not smeared; picking does
// not see the jitter; works with ambient occlusion and contact shadows, with an
// HDR view and bloom.
void test_gpu_taa() {
    with_gpu("test_gpu_taa", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh slab = make_box(r3d, {6.0f, 4.0f, 0.1f});
        const kin::Mesh block = make_box(r3d, {2.0f, 2.0f, 0.1f});
        const kin::Mesh other = make_box(r3d, {2.0f, 2.0f, 0.2f});
        const Color black = Color::rgb(0, 0, 0);
        const kin::Material3D white{.color = Color::rgb(255, 255, 255), .unlit = true};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = black});
        kin::Scene3D scene;
        scene.camera = flat_camera();
        const auto shoot = [&](kin::View3D& target, const kin::Scene3D& s) {
            Pixels p = shoot_view(renderer, r3d, target, s);
            renderer.present();
            return p;
        };
        const auto still = [&](const kin::Camera3D& camera) {
            scene.clear();
            scene.camera = camera;
            scene.draw(slab, Mat4f::rotation_z(25.0f), white);
        };

        // Off: the old picture, nothing run; the other settings do nothing.
        still(flat_camera());
        const Pixels plain = shoot(view, scene);
        assert(r3d.stats().taa_dispatches == 0 && !r3d.stats().taa_reset && r3d.stats().samples == 1);
        scene.taa = {.enabled = false, .blend = 0.5f, .sharpness = 1.0f, .jitter_scale = 3.0f};
        assert(shoot(view, scene).rgba == plain.rgba);
        assert(r3d.stats().taa_dispatches == 0 && r3d.stats().taa_jitter.x == 0.0f);

        // On, still: the first frame resets, later ones do not; a dispatch each.
        scene.taa = {.enabled = true};
        const Pixels first = shoot(view, scene);
        if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        assert(r3d.stats().taa_dispatches > 0);
        assert(r3d.stats().taa_dispatches == 1 && r3d.stats().taa_reset && r3d.stats().samples == 1);
        const kin::Vec2f jitter_one = r3d.stats().taa_jitter;
        assert(jitter_one.x == kin::taa_jitter(0).x && jitter_one.y == kin::taa_jitter(0).y);
        // The first frame is the plain picture, a fraction of a pixel off: edge pixels differ, the middle does not.
        expect(first.at(80, 60), Color::rgb(255, 255, 255), "inside the slab, first frame");
        expect(first.at(5, 5), black, "outside the slab, first frame");
        Pixels previous = first, now = first;
        for (int f = 1; f < 24; ++f) {
            previous = now;
            now = shoot(view, scene);
            assert(!r3d.stats().taa_reset && r3d.stats().taa_dispatches == 1);
            const kin::Vec2f j = r3d.stats().taa_jitter, want = kin::taa_jitter(static_cast<kin::u32>(f));
            assert(j.x == want.x && j.y == want.y);
        }
        // Converged: a frame to the next differs little.
        long diff = 0, worst = 0;
        for (std::size_t i = 0; i < now.rgba.size(); ++i) {
            const long d = std::abs(static_cast<long>(now.rgba[i]) - static_cast<long>(previous.rgba[i]));
            diff += d;
            worst = std::max(worst, d);
        }
        const double mean_diff = static_cast<double>(diff) / static_cast<double>(now.rgba.size());
        std::cerr << "taa: still, frame 23 to 24: mean " << mean_diff << ", worst " << worst << " levels\n";
        assert(mean_diff < 0.3 && worst < 45);

        // Edges: more levels than one sample a pixel has, and nearer a 16x supersample than the plain picture.
        std::array<Pixels, 16> samples;
        for (int i = 0; i < 16; ++i) {
            still(flat_camera((static_cast<float>(i % 4) + 0.5f) / 4.0f * 0.1f - 0.05f, (static_cast<float>(i / 4) + 0.5f) / 4.0f * 0.1f - 0.05f));
            scene.taa = {};
            samples[static_cast<std::size_t>(i)] = shoot(view, scene);
        }
        std::vector<int> reference(160 * 120);
        std::vector<std::size_t> edge;
        for (int y = 0; y < 120; ++y) {
            for (int x = 0; x < 160; ++x) {
                float sum = 0.0f;
                for (const Pixels& p : samples) {
                    sum += decode_srgb(p.at(x, y).g);
                }
                reference[static_cast<std::size_t>(y * 160 + x)] = encode_srgb(sum / 16.0f);
                if (reference[static_cast<std::size_t>(y * 160 + x)] > 3 && reference[static_cast<std::size_t>(y * 160 + x)] < 252) {
                    edge.push_back(static_cast<std::size_t>(y * 160 + x));
                }
            }
        }
        const auto error = [&](const Pixels& p) {
            double e = 0.0;
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    const double d = level(p.at(x, y)) - reference[static_cast<std::size_t>(y * 160 + x)];
                    e += d * d;
                }
            }
            return e / (160.0 * 120.0);
        };
        const auto levels = [&](const Pixels& p) {
            std::vector<int> seen;
            for (const std::size_t i : edge) {
                seen.push_back(level(p.at(static_cast<int>(i % 160), static_cast<int>(i / 160))));
            }
            std::sort(seen.begin(), seen.end());
            return static_cast<int>(std::unique(seen.begin(), seen.end()) - seen.begin());
        };
        const double plain_error = error(plain), taa_error = error(now);
        std::cerr << "taa: " << edge.size() << " edge pixels, distinct levels " << levels(plain) << " plain, " << levels(now)
                  << " with; mean square error against 16 samples " << plain_error << " plain, " << taa_error << " with\n";
        assert(levels(plain) <= 3 && levels(now) >= 12);
        assert(taa_error < plain_error * 0.5);

        // Settings: no jitter is no anti-aliasing (the same picture as plain), a frozen one stays put.
        still(flat_camera());
        scene.taa = {.enabled = true, .jitter_scale = 0.0f};
        for (int f = 0; f < 6; ++f) {
            shoot(view, scene);
        }
        assert(levels(shoot(view, scene)) <= 3 && r3d.stats().taa_jitter.x == 0.0f && r3d.stats().taa_jitter.y == 0.0f);
        scene.taa = {.enabled = true, .freeze_jitter = true};
        shoot(view, scene);
        const kin::Vec2f frozen = r3d.stats().taa_jitter;
        shoot(view, scene);
        assert(r3d.stats().taa_jitter.x == frozen.x && r3d.stats().taa_jitter.y == frozen.y);
        scene.taa = {.enabled = true, .jitter_samples = 2};
        const kin::Vec2f a = (shoot(view, scene), r3d.stats().taa_jitter);
        shoot(view, scene);
        shoot(view, scene);
        assert(r3d.stats().taa_jitter.x == a.x && r3d.stats().taa_jitter.y == a.y); // a period of 2

        // Sharpening is one more dispatch: flat areas are left alone and edges are steeper.
        const auto steepness = [](const Pixels& p) {
            long sum = 0;
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x + 1 < 160; ++x) {
                    const long d = level(p.at(x + 1, y)) - level(p.at(x, y));
                    sum += d * d;
                }
            }
            return sum;
        };
        scene.taa = {.enabled = true};
        Pixels soft;
        for (int f = 0; f < 16; ++f) {
            soft = shoot(view, scene);
        }
        scene.taa = {.enabled = true, .sharpness = 1.0f};
        Pixels sharp;
        for (int f = 0; f < 16; ++f) {
            sharp = shoot(view, scene);
        }
        std::cerr << "taa: steepness (sum of squared differences along rows) " << steepness(soft) << " soft, " << steepness(sharp) << " sharpened\n";
        assert(r3d.stats().taa_dispatches == 2);
        assert(steepness(sharp) > steepness(soft) + steepness(soft) / 20);
        expect(sharp.at(80, 60), Color::rgb(255, 255, 255), "inside the slab, sharpened");
        expect(sharp.at(5, 5), black, "outside the slab, sharpened");

        // Resets: a reset_history() and a resize each drop it; the frame after shows no blend (two levels at the edge).
        scene.taa = {.enabled = true};
        for (int f = 0; f < 12; ++f) {
            shoot(view, scene);
        }
        assert(!r3d.stats().taa_reset && levels(shoot(view, scene)) >= 12);
        view.reset_history();
        const Pixels after_reset = shoot(view, scene);
        assert(r3d.stats().taa_reset && levels(after_reset) <= 3);
        assert(!(shoot(view, scene), r3d.stats().taa_reset));
        assert(r3d.resize(view, {160, 120}) && r3d.resize(view, {120, 90}));
        assert(r3d.resize(view, {160, 120}));
        shoot(view, scene);
        assert(r3d.stats().taa_reset);

        // A camera cut: a picture of something else after reset_history() has nothing of the first.
        for (int f = 0; f < 10; ++f) {
            shoot(view, scene);
        }
        scene.clear();
        scene.draw(slab, Mat4f::translation({0.0f, 0.0f, 0.0f}) * Mat4f::rotation_z(25.0f), white);
        const Pixels before_cut = shoot(view, scene);
        assert(level(before_cut.at(80, 60)) > 240);
        scene.clear(); // an empty scene: the slab is gone
        view.reset_history();
        const Pixels cut = shoot(view, scene);
        assert(level(cut.at(80, 60)) < 4 && level(cut.at(70, 55)) < 4);
        // Without a reset the history is clipped by what is there; still no slab after a few frames.
        for (int f = 0; f < 3; ++f) {
            assert(level(shoot(view, scene).at(80, 60)) < 4);
        }

        // A view asked for 4x MSAA draws one sample with it on, and four off.
        kin::View3D multisampled = r3d.create_view({160, 120}, {.samples = 4, .background = black});
        still(flat_camera());
        scene.taa = {};
        shoot(multisampled, scene);
        const kin::u32 msaa_samples = r3d.stats().samples;
        scene.taa = {.enabled = true};
        view.reset_history();
        Pixels one, four;
        for (int f = 0; f < 10; ++f) {
            one = shoot(view, scene);
        }
        for (int f = 0; f < 10; ++f) {
            four = shoot(multisampled, scene);
        }
        std::cerr << "taa: samples " << msaa_samples << " off, " << r3d.stats().samples << " on\n";
        assert(r3d.stats().samples == 1 && (msaa_samples == 1 || msaa_samples == 4 || msaa_samples == 2 || msaa_samples == 8));
        assert(multisampled.samples() == msaa_samples || msaa_samples == 1);
        // The same sequence on the same scene: the same picture (up to a level's rounding).
        long worst_msaa = 0;
        for (std::size_t i = 0; i < one.rgba.size(); ++i) {
            worst_msaa = std::max(worst_msaa, std::labs(static_cast<long>(one.rgba[i]) - static_cast<long>(four.rgba[i])));
        }
        assert(worst_msaa <= 2);
        // And it works again with it off.
        scene.taa = {};
        assert(shoot(multisampled, scene).rgba != four.rgba || msaa_samples == 1);
        assert(r3d.stats().taa_dispatches == 0 && r3d.stats().samples == msaa_samples);

        // Motion: a block moving 3 pixels a frame leaves no trail (matched by place, then by id) and stays white.
        for (const kin::u32 id : {0u, 9u}) {
            view.reset_history();
            scene.taa = {.enabled = true};
            int worst_trail = 0, worst_inside = 255;
            for (int f = 0; f < 40; ++f) {
                scene.clear();
                const float x = -6.0f + 0.3f * static_cast<float>(f); // the centre, in units; 10 pixels a unit
                scene.draw(block, Mat4f::translation({x, 0.0f, 0.0f}), white, kin::colors::white, id);
                const Pixels p = shoot(view, scene);
                if (f >= 6) {
                    const float old_x = x - 0.3f * 6.0f; // where it was six frames ago
                    const int cx = static_cast<int>(80.0f + old_x * 10.0f);
                    // The pixels of the old footprint further than 4 behind the block's trailing edge.
                    for (int dx = -9; dx <= 9; ++dx) {
                        for (int dy = -9; dy <= 9; ++dy) {
                            const int px = cx + dx;
                            if (px >= 0 && px < 160 && px < static_cast<int>(80.0f + x * 10.0f) - 10 - 4) {
                                worst_trail = std::max(worst_trail, level(p.at(px, 60 + dy)));
                            }
                        }
                    }
                    worst_inside = std::min(worst_inside, level(p.at(static_cast<int>(80.0f + x * 10.0f), 60)));
                }
            }
            std::cerr << "taa: moving block (id " << id << "): worst trail " << worst_trail << ", least inside " << worst_inside << "\n";
            assert(worst_trail < 12 && worst_inside > 245);
            assert(r3d.stats().taa_dispatches == 1);
        }

        // A mesh that appears is white at once; so is a copy added to a mesh already drawn (the count changes).
        for (const bool same_mesh : {false, true}) {
            view.reset_history();
            int appeared = 0;
            for (int f = 0; f < 12; ++f) {
                scene.clear();
                scene.draw(slab, Mat4f::translation({0.0f, 3.0f, -1.0f}), {.color = Color::rgb(60, 60, 60), .unlit = true});
                scene.draw(block, Mat4f::translation({-4.0f, -2.0f, 0.0f}), white);
                if (f >= 8) {
                    scene.draw(same_mesh ? block : other, Mat4f::translation({4.0f, -2.0f, 0.0f}), white);
                }
                const Pixels p = shoot(view, scene);
                if (f == 8) {
                    appeared = level(p.at(120, 80));
                }
            }
            std::cerr << "taa: appeared " << (same_mesh ? "as a copy" : "as a mesh") << ": " << appeared << " at once\n";
            assert(appeared > 245);
        }
        // And one that disappears leaves nothing: gone after a frame or two.
        scene.clear();
        scene.draw(block, Mat4f::translation({-4.0f, -2.0f, 0.0f}), white);
        scene.draw(block, Mat4f::translation({4.0f, -2.0f, 0.0f}), white);
        for (int f = 0; f < 6; ++f) {
            shoot(view, scene);
        }
        scene.clear();
        scene.draw(block, Mat4f::translation({-4.0f, -2.0f, 0.0f}), white);
        shoot(view, scene);
        shoot(view, scene);
        assert(level(shoot(view, scene).at(120, 80)) < 12);
    });
}

// Where history could ghost: a block crossing a 1-pixel checkerboard, whose
// neighbourhoods span dark to light so the clip lets a wrong history through,
// and a camera panning over a coarser one. The pixels the block has left must
// look as they do without it, and a panned picture must stay near the supersampled one.
void test_gpu_taa_over_detail() {
    with_gpu("test_gpu_taa_over_detail", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh back = make_box(r3d, {20.0f, 14.0f, 0.1f});
        const kin::Mesh cell = make_box(r3d, {0.1f, 0.1f, 0.1f});
        const kin::Mesh big_cell = make_box(r3d, {0.4f, 0.4f, 0.1f});
        const kin::Mesh block = make_box(r3d, {2.0f, 2.0f, 0.1f});
        const kin::Material3D light{.color = Color::rgb(220, 220, 220), .unlit = true};
        const kin::Material3D dark{.color = Color::rgb(30, 30, 30), .unlit = true};
        const kin::Material3D white{.color = Color::rgb(255, 255, 255), .unlit = true};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        kin::Scene3D scene;
        std::vector<kin::MeshInstance> small_cells, large_cells;
        for (int y = 0; y < 120; ++y) {
            for (int x = 0; x < 160; ++x) {
                if ((x + y) % 2 == 0) {
                    small_cells.push_back({Mat4f::translation({(static_cast<float>(x) - 79.5f) * 0.1f, (59.5f - static_cast<float>(y)) * 0.1f, 0.2f})});
                }
            }
        }
        // Cells of 4 pixels, wide enough for the pan; 0.07 off the pixel grid so that edges fall inside pixels.
        for (int y = -2; y < 33; ++y) {
            for (int x = -80; x < 80; ++x) {
                if ((x + y) % 2 == 0) {
                    large_cells.push_back({Mat4f::translation({static_cast<float>(x) * 0.4f + 0.27f, (14.5f - static_cast<float>(y)) * 0.4f - 0.2f, 0.2f})});
                }
            }
        }
        const auto shoot = [&](kin::View3D& target, const kin::Scene3D& s) {
            Pixels p = shoot_view(renderer, r3d, target, s);
            renderer.present();
            return p;
        };
        const auto checker = [&](float cam_x, bool large, bool with_block, float block_x) {
            scene.clear();
            scene.camera = flat_camera(cam_x, 0.0f);
            scene.draw(back, Mat4f::translation({cam_x, 0.0f, 0.0f}), light);
            scene.draw_instanced(large ? big_cell : cell, large ? std::span<const kin::MeshInstance>{large_cells} : std::span<const kin::MeshInstance>{small_cells}, dark);
            if (with_block) {
                scene.draw(block, Mat4f::translation({block_x, 0.0f, 0.3f}), white);
            }
        };
        const PatternSurface pattern_surface = make_pattern_surface(r3d, renderer);
        const kin::Mesh& surface = pattern_surface.mesh;
        const kin::Texture& pattern = pattern_surface.texture;
        const auto textured = [&](float cam_x) {
            scene.clear();
            scene.camera = flat_camera(cam_x, 0.0f);
            scene.draw(surface, Mat4f::identity(), {.unlit = true, .texture = pattern, .wrap_u = kin::Wrap3D::Repeat});
        };
        // A 16-sample reference (the camera shifted by sub-pixel steps), in linear light, drawn on a view of its own
        // so that the temporal view keeps its history.
        kin::View3D plain_view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        const auto supersample = [&](float cam_x, bool large, bool with_block = false, float block_x = 0.0f, bool use_texture = false) {
            std::vector<float> sum(160 * 120, 0.0f);
            scene.taa = {};
            for (int i = 0; i < 16; ++i) {
                if (use_texture) {
                    textured(cam_x);
                } else {
                    checker(cam_x, large, with_block, block_x);
                }
                scene.camera = flat_camera(cam_x + ((static_cast<float>(i % 4) + 0.5f) / 4.0f - 0.5f) * 0.1f,
                                           ((static_cast<float>(i / 4) + 0.5f) / 4.0f - 0.5f) * 0.1f);
                const Pixels p = shoot(plain_view, scene);
                for (int y = 0; y < 120; ++y) {
                    for (int x = 0; x < 160; ++x) {
                        sum[static_cast<std::size_t>(y * 160 + x)] += decode_srgb(p.at(x, y).g) / 16.0f;
                    }
                }
            }
            std::vector<int> out(sum.size());
            for (std::size_t i = 0; i < sum.size(); ++i) {
                out[i] = encode_srgb(sum[i]);
            }
            return out;
        };
        const auto mean_square = [&](const Pixels& p, const std::vector<int>& truth) {
            double e = 0.0;
            for (int i = 0; i < 160 * 120; ++i) {
                const double d = level(p.at(i % 160, i / 160)) - truth[static_cast<std::size_t>(i)];
                e += d * d;
            }
            return e / (160.0 * 120.0);
        };
        scene.taa = {.enabled = true};
        checker(0.0f, false, false, 0.0f);
        shoot(view, scene);
        if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        assert(r3d.stats().taa_dispatches > 0);
        const std::vector<int> reference = supersample(0.0f, false);

        // The block crosses, 3 pixels a frame. Where it has been, the picture is as bright as the checker alone
        // drawn the same way (a ghost of the white block would lift the zone's mean; the 1-pixel pattern is softened
        // by the reconstruction filter, so what is compared is the same softened picture, and by its mean).
        kin::View3D alone_view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        Pixels alone;
        for (int f = 0; f < 30; ++f) {
            scene.taa = {.enabled = true};
            checker(0.0f, false, false, 0.0f);
            alone = shoot(alone_view, scene);
        }
        view.reset_history();
        double worst_zone = 0.0;
        for (int f = 0; f < 44; ++f) {
            scene.taa = {.enabled = true};
            const float x = -7.0f + 0.3f * static_cast<float>(f);
            checker(0.0f, false, true, x);
            const Pixels p = shoot(view, scene);
            if (f >= 8) {
                // The zone: the pixels the block covered six frames ago, now clear of it by 3 pixels.
                const int old_centre = static_cast<int>(80.0f + (x - 1.8f) * 10.0f);
                double got = 0.0, want = 0.0;
                int count = 0;
                for (int px = old_centre - 10; px < old_centre + 10; ++px) {
                    if (px >= 0 && px < 160 && px < static_cast<int>(80.0f + x * 10.0f) - 10 - 3) {
                        for (int py = 50; py < 70; ++py) {
                            got += level(p.at(px, py));
                            want += level(alone.at(px, py));
                            ++count;
                        }
                    }
                }
                if (count > 0) {
                    worst_zone = std::max(worst_zone, std::abs(got - want) / count);
                }
            }
        }
        std::cerr << "taa: checkerboard behind a moving block: worst difference of the trail zone's mean " << worst_zone << " levels\n";
        assert(worst_zone < 12.0);

        // The camera pans over 4-pixel cells. Over the last 16 frames (two jitter periods) the picture's error
        // against the supersample of each frame's own pose, next to the same pose drawn without it.
        double errors[4][2] = {};
        int index = 0;
        for (const float step : {0.0f, 0.0213f, 0.1f, 0.237f}) {
            view.reset_history();
            double taa_error = 0.0, plain_error = 0.0;
            for (int f = 0; f < 48; ++f) {
                const float cam_x = -5.03f + step * static_cast<float>(f); // 0.3 of a pixel off the pattern's grid
                scene.taa = {.enabled = true};
                textured(cam_x);
                const Pixels p = shoot(view, scene);
                if (f >= 32) {
                    const std::vector<int> truth = supersample(cam_x, true, false, 0.0f, true);
                    scene.taa = {};
                    textured(cam_x);
                    taa_error += mean_square(p, truth) / 16.0;
                    plain_error += mean_square(shoot(plain_view, scene), truth) / 16.0;
                }
            }
            std::cerr << "taa: panning " << step * 10.0f << " px a frame over 4 px cells: mean square error against 16 samples, over a period: "
                      << taa_error << " with, " << plain_error << " without\n";
            errors[index][0] = taa_error;
            errors[index][1] = plain_error;
            ++index;
        }
        // Held still it settles to a fraction of the plain picture's error; panning slowly or by whole pixels a frame
        // it stays well under it; panning fast the history is trusted less (every sub-pixel shift blurs it, and
        // what it lacks is made up by the one sample a frame) and the gain is smaller, but there is some.
        assert(errors[0][0] < errors[0][1] * 0.25);
        assert(errors[1][0] < errors[1][1] * 0.5 && errors[2][0] < errors[2][1] * 0.4);
        assert(errors[3][0] < errors[3][1] * 0.75);

    });
}

// A crowd of one mesh that loses a copy from the middle of its list (and gains
// one at the front) while it moves: the places in the list no longer say which
// copy is which, so the copies are matched by where they were. The pixels a
// block has left must look as they do without it, as with a list that does not change.
void test_gpu_taa_crowd() {
    with_gpu("test_gpu_taa_crowd", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const PatternSurface world = make_pattern_surface(r3d, renderer);
        const kin::Mesh block = make_box(r3d, {1.6f, 1.6f, 0.2f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        kin::View3D alone_view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        kin::Scene3D scene;
        scene.camera = flat_camera(-5.03f, 0.0f);
        const kin::Material3D white{.color = Color::rgb(255, 255, 255), .unlit = true};
        const auto shoot = [&](kin::View3D& target) {
            Pixels p = shoot_view(renderer, r3d, target, scene);
            renderer.present();
            return p;
        };
        const auto draw_world = [&] {
            scene.clear();
            scene.draw(world.mesh, Mat4f::identity(), {.unlit = true, .texture = world.texture, .wrap_u = kin::Wrap3D::Repeat});
        };
        // The pattern alone, drawn the same way.
        scene.taa = {.enabled = true};
        Pixels alone;
        for (int f = 0; f < 30; ++f) {
            draw_world();
            alone = shoot(alone_view);
        }
        if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        assert(r3d.stats().taa_dispatches > 0);
        for (const bool churn : {false, true}) {
            view.reset_history();
            double worst = 0.0;
            for (int f = 0; f < 60; ++f) {
                draw_world();
                std::vector<kin::MeshInstance> crowd;
                // Five blocks moving right 1.7 pixels a frame, 3.2 units apart; with churn, from frame 24 on the first is gone on even
                // frames and another stands first in the list on odd ones: the number of copies changes every frame.
                for (int i = 0; i < 5; ++i) {
                    if (churn && i == 0 && f >= 24 && f % 2 == 0) {
                        continue;
                    }
                    crowd.push_back({Mat4f::translation({-12.0f + 3.2f * static_cast<float>(i) + 0.17f * static_cast<float>(f), 0.0f, 0.3f})});
                }
                if (churn && f >= 24 && f % 2 == 1) {
                    crowd.insert(crowd.begin(), {Mat4f::translation({-5.0f, 3.5f, 0.3f})});
                }
                scene.draw_instanced(block, crowd, white);
                const Pixels p = shoot(view);
                if (f >= 40) {
                    // Behind the block that began third (index 2): where it was 5 frames ago, 3 pixels clear of it now.
                    const float x = -12.0f + 3.2f * 2.0f + 0.17f * static_cast<float>(f);
                    const int old_centre = static_cast<int>(80.0f + (x + 5.03f - 0.85f) * 10.0f);
                    double got = 0.0, want = 0.0;
                    int count = 0;
                    for (int px = old_centre - 7; px < old_centre + 7; ++px) {
                        if (px >= 0 && px < 160 && px < static_cast<int>(80.0f + (x + 5.03f) * 10.0f) - 8 - 3) {
                            for (int py = 52; py < 68; ++py) {
                                got += level(p.at(px, py));
                                want += level(alone.at(px, py));
                                ++count;
                            }
                        }
                    }
                    if (count > 0) {
                        worst = std::max(worst, std::abs(got - want) / count);
                    }
                }
            }
            std::cerr << "taa: crowd " << (churn ? "losing and gaining copies" : "unchanged") << ": worst difference of the trail zone's mean " << worst << " levels\n";
            assert(worst < 12.0);
        }
        // Matched by where they were, the blocks keep their history through the churn: their edges (white on black)
        // are anti-aliased, not the 0 and 255 a frame with no history has.
        double edge_levels[2] = {};
        for (const bool churn : {false, true}) {
            view.reset_history();
            double soft_edges = 0.0;
            for (int f = 0; f < 60; ++f) {
                scene.clear();
                std::vector<kin::MeshInstance> crowd;
                for (int i = 0; i < 5; ++i) {
                    if (churn && i == 0 && f >= 24 && f % 2 == 0) {
                        continue;
                    }
                    crowd.push_back({Mat4f::translation({-12.0f + 3.2f * static_cast<float>(i) + 0.17f * static_cast<float>(f), 0.0f, 0.3f})});
                }
                if (churn && f >= 24 && f % 2 == 1) {
                    crowd.insert(crowd.begin(), {Mat4f::translation({-5.0f, 3.5f, 0.3f})});
                }
                scene.draw_instanced(block, crowd, white);
                const Pixels p = shoot(view);
                if (f >= 40 && f < 50) {
                    const float x = -12.0f + 3.2f * 2.0f + 0.17f * static_cast<float>(f);
                    const int left = static_cast<int>(80.0f + (x + 5.03f - 0.8f) * 10.0f);
                    for (int px = left - 1; px <= left + 1; ++px) {
                        for (int py = 54; py < 66; ++py) {
                            const int v = level(p.at(px, py));
                            soft_edges += (v > 20 && v < 235) ? 0.1 : 0.0; // a tenth for each of ten frames
                        }
                    }
                }
            }
            std::cerr << "taa: crowd " << (churn ? "losing and gaining copies" : "unchanged") << ": anti-aliased pixels on a block's left edge, of 36: " << soft_edges << "\n";
            edge_levels[churn ? 1 : 0] = soft_edges;
        }
        // With the list changing, the edges are as soft as with it unchanged (without a match they would have no history: none).
        assert(edge_levels[0] > 3.0 && edge_levels[1] > edge_levels[0] * 0.6);
    });
}

// Temporal anti-aliasing and the first-person overlay: the gun is drawn in
// front with its own velocity, so it neither smears over a world that moves
// behind it nor leaves a trail when it moves itself. Judged by the picture
// round the gun's silhouette against a supersample of the same frame, with a
// patterned world behind it (a plain one would let the clip hide a wrong history).
void test_gpu_taa_overlay() {
    with_gpu("test_gpu_taa_overlay", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const PatternSurface world = make_pattern_surface(r3d, renderer);
        const kin::Mesh gun = make_box(r3d, {3.0f, 2.0f, 0.5f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        kin::View3D plain_view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        kin::Scene3D scene;
        const kin::Material3D hands{.color = Color::rgb(200, 40, 40), .unlit = true};
        const auto build = [&](float cam_x, float gun_x, float dx, float dy) {
            scene.clear();
            scene.camera = flat_camera(cam_x + dx, dy);
            scene.draw(world.mesh, Mat4f::identity(), {.unlit = true, .texture = world.texture, .wrap_u = kin::Wrap3D::Repeat});
            kin::Scene3D& overlay = scene.overlay();
            overlay.camera = flat_camera(dx, dy);
            overlay.draw(gun, Mat4f::translation({gun_x, -1.5f, 0.0f}), hands);
        };
        const auto shoot = [&](kin::View3D& target) {
            Pixels p = shoot_view(renderer, r3d, target, scene);
            renderer.present();
            return p;
        };
        // The same frame at 16 sub-pixel offsets, averaged in linear light.
        const auto truth = [&](float cam_x, float gun_x) {
            std::vector<float> sum(160 * 120 * 3, 0.0f);
            scene.taa = {};
            for (int i = 0; i < 16; ++i) {
                build(cam_x, gun_x, ((static_cast<float>(i % 4) + 0.5f) / 4.0f - 0.5f) * 0.1f, ((static_cast<float>(i / 4) + 0.5f) / 4.0f - 0.5f) * 0.1f);
                const Pixels p = shoot(plain_view);
                for (int k = 0; k < 160 * 120; ++k) {
                    const Color c = p.at(k % 160, k / 160);
                    sum[static_cast<std::size_t>(k) * 3] += decode_srgb(c.r) / 16.0f;
                    sum[static_cast<std::size_t>(k) * 3 + 1] += decode_srgb(c.g) / 16.0f;
                    sum[static_cast<std::size_t>(k) * 3 + 2] += decode_srgb(c.b) / 16.0f;
                }
            }
            return sum;
        };
        // The pixels within 3 of the gun's silhouette, in and out of it: where it meets the world and nowhere else.
        struct Ring {
            double inside = 0.0, outside = 0.0;
        };
        const auto ring_error = [&](const Pixels& p, const std::vector<float>& want, float gun_x) {
            const auto left = static_cast<int>(80.0f + (gun_x - 1.5f) * 10.0f), right = left + 29, top = static_cast<int>(60.0f + 0.5f * 10.0f),
                       bottom = top + 19;
            double e_in = 0.0, e_out = 0.0;
            int n_in = 0, n_out = 0;
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    const bool inside = x >= left && x <= right && y >= top && y <= bottom;
                    const int depth = inside ? std::min({x - left, right - x, y - top, bottom - y})
                                             : std::max({left - x, x - right, top - y, y - bottom});
                    if (depth > 3) {
                        continue;
                    }
                    const Color c = p.at(x, y);
                    const std::size_t k = static_cast<std::size_t>(y * 160 + x) * 3;
                    const double dr = c.r - encode_srgb(want[k]), dg = c.g - encode_srgb(want[k + 1]);
                    (inside ? e_in : e_out) += (dr * dr + dg * dg) / 2.0;
                    ++(inside ? n_in : n_out);
                }
            }
            return Ring{n_in > 0 ? e_in / n_in : 0.0, n_out > 0 ? e_out / n_out : 0.0};
        };
        // The camera pans 2.4 pixels a frame, the gun fixed to the screen; then the gun moves 1.4 pixels a frame over a still world (rates that do not repeat
        // the jitter's pattern: a pan of exactly half pixels a frame lines the samples up with it and leaves part of each pixel unvisited).
        for (const bool gun_moves : {false, true}) {
            view.reset_history();
            Ring taa_error, plain_error;
            for (int f = 0; f < 48; ++f) {
                const float cam_x = gun_moves ? 0.037f : -5.03f + 0.237f * static_cast<float>(f); // off the pattern's grid, as a game's world is
                const float gun_x = gun_moves ? -4.0f + 0.137f * static_cast<float>(f) : 0.0f;
                scene.taa = {.enabled = true};
                build(cam_x, gun_x, 0.0f, 0.0f);
                const Pixels p = shoot(view);
                if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
                    return;
                }
                assert(r3d.stats().taa_dispatches > 0);
                if (f >= 32) {
                    const std::vector<float> want = truth(cam_x, gun_x);
                    scene.taa = {};
                    build(cam_x, gun_x, 0.0f, 0.0f);
                    const Ring with = ring_error(p, want, gun_x), without = ring_error(shoot(plain_view), want, gun_x);
                    taa_error.inside += with.inside / 16.0;
                    taa_error.outside += with.outside / 16.0;
                    plain_error.inside += without.inside / 16.0;
                    plain_error.outside += without.outside / 16.0;
                    // The gun's own middle is its colour, whatever happens at its edges.
                    expect(p.at(static_cast<int>(80.0f + gun_x * 10.0f), 60 + 10 + 5), Color::rgb(200, 40, 40), "the gun, inside", 4);
                }
            }
            std::cerr << "taa: gun edge error against 16 samples (" << (gun_moves ? "gun moving" : "camera panning") << "), in the gun "
                      << taa_error.inside << " with, " << plain_error.inside << " without; in the world beside it " << taa_error.outside << " with, "
                      << plain_error.outside << " without\n";
            // The gun's side of its edge is no worse than the plain picture's, give or take the reconstruction filter's softening of the edge pixels (a gun given the world's velocity carries its pattern in: over 85);
            // the world's side no more than a little worse, where the history is dropped round the gun.
            assert(taa_error.inside < plain_error.inside * 1.1 + 45.0);
            assert(taa_error.outside < plain_error.outside * 1.15);
        }
    });
}

// Temporal anti-aliasing with nothing else that needs a pre-pass: the main pass writes the velocity itself (one geometry pass fewer).
// The pictures must be those of the pre-pass (KIN_TAA_PREPASS=1 brings it back), frame by frame, with every kind of draw the velocity
// has a rule for: opaque, an instanced run, a cut-out, a blend that must leave the velocity of what is behind, an additive one, a
// wireframe, the overlay; and a custom shader on an opaque mesh (no velocity output: the frame keeps the pre-pass) or on a blended one.
void test_gpu_taa_main_velocity() {
    with_gpu("test_gpu_taa_main_velocity", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        const PatternSurface world = make_pattern_surface(r3d, renderer);
        const kin::Mesh box = make_box(r3d, {1.5f, 1.5f, 0.5f});
        const kin::Mesh leaf_mesh = r3d.create_mesh(uv_square(1.2f));
        const kin::u8 texels[16] = {20, 200, 20, 0, 20, 200, 20, 255, 200, 20, 20, 255, 20, 20, 200, 0};
        const kin::Texture leaf = renderer.create_texture_from_rgba(texels, {4, 1});
        renderer.set_scale_mode(leaf, kin::ScaleMode::Nearest);
        const kin::Mesh gun = make_box(r3d, {2.0f, 1.0f, 0.5f});
        kin::MaterialShader3D flat;
        std::string error;
        if (kin::shader_compiler_available()) {
            flat = r3d.compile_material_shader(std::filesystem::path{KIN_TEST_MATERIAL_SHADERS} / "custom_flat.frag.glsl", &error);
            assert(flat.valid());
        }
        kin::View3D with_main = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(10, 10, 20)});
        kin::View3D with_pre = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(10, 10, 20)});
        enum class Extra { None, CustomOpaque, CustomBlended, Occlusion };
        const auto build = [&](kin::Scene3D& scene, int f, Extra extra) {
            const float t = static_cast<float>(f);
            scene.clear();
            scene.taa = {.enabled = true};
            scene.camera = kin::Camera3D::look_at({-3.0f + 0.21f * t, 0.6f, 9.0f}, {0.4f * t * 0.1f, 0.0f, 0.0f}, 40.0f);
            scene.lights.sun = {.direction = {0.4f, 0.8f, 1.0f}, .color = {1.0f, 0.95f, 0.9f}};
            scene.lights.ambient = {0.2f, 0.2f, 0.25f};
            scene.draw(world.mesh, Mat4f::identity(), {.unlit = true, .texture = world.texture, .wrap_u = kin::Wrap3D::Repeat});
            // An instanced run (three copies of one mesh and material one after another), each moving its own way.
            for (int i = 0; i < 3; ++i) {
                scene.draw(box, Mat4f::translation({-4.0f + 1.8f * static_cast<float>(i), -1.5f + 0.1f * t * static_cast<float>(i - 1), 1.0f}) *
                                    Mat4f::rotation_y(7.0f * t),
                           {.color = Color::rgb(230, 170, 90), .metallic = 0.2f, .roughness = 0.5f});
            }
            scene.draw(leaf_mesh, Mat4f::translation({1.0f - 0.08f * t, 0.5f, 2.0f}) * Mat4f::rotation_x(90.0f),
                       {.color = Color::rgb(255, 255, 255), .double_sided = true, .blend = kin::Blend3D::Mask, .texture = leaf});
            // In front of the cut-out and the run, moving the other way: what they leave behind is the velocity of what is behind.
            scene.draw(box, Mat4f::translation({3.0f - 0.2f * t, -0.8f, 3.0f}), {.color = Color::rgba(60, 200, 255, 140), .unlit = true, .blend = kin::Blend3D::Alpha});
            scene.draw(box, Mat4f::translation({0.5f + 0.15f * t, 1.8f, 3.5f}), {.color = Color::rgb(60, 60, 60), .unlit = true, .blend = kin::Blend3D::Additive});
            scene.draw(box, Mat4f::translation({-1.0f, 2.2f, 2.0f}) * Mat4f::rotation_y(5.0f * t), {.color = Color::rgb(80, 255, 80), .wireframe = true});
            if (extra == Extra::CustomOpaque) {
                kin::Material3D custom{.shader = flat};
                custom.shader_params[4] = 0.9f;
                scene.draw(box, Mat4f::translation({2.5f + 0.1f * t, 1.0f, 1.5f}), custom);
            } else if (extra == Extra::CustomBlended) {
                kin::Material3D custom{.blend = kin::Blend3D::Alpha, .shader = flat};
                custom.shader_params[4] = 0.9f;
                scene.draw(box, Mat4f::translation({2.5f - 0.1f * t, 1.0f, 3.2f}), custom);
            } else if (extra == Extra::Occlusion) {
                scene.ambient_occlusion = {.enabled = true};
            }
            kin::Scene3D& overlay = scene.overlay();
            overlay.camera = kin::Camera3D::look_at({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, 40.0f);
            overlay.draw(gun, Mat4f::translation({2.2f - 0.05f * t, -1.8f, 0.0f}), {.color = Color::rgb(200, 40, 40)});
        };
        const auto shoot = [&](kin::View3D& view, const kin::Scene3D& scene, bool pre_pass) {
            if (pre_pass) {
                setenv("KIN_TAA_PREPASS", "1", 1);
            } else {
                unsetenv("KIN_TAA_PREPASS");
            }
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            unsetenv("KIN_TAA_PREPASS");
            return p;
        };
        // Every kind of frame, over a run of frames (the history is what shows a wrong velocity).
        for (const Extra extra : {Extra::None, Extra::CustomOpaque, Extra::CustomBlended, Extra::Occlusion}) {
            if (extra != Extra::None && !flat.valid() && extra != Extra::Occlusion) {
                continue;
            }
            with_main.reset_history();
            with_pre.reset_history();
            for (int f = 0; f < 14; ++f) {
                kin::Scene3D scene;
                build(scene, f, extra);
                const Pixels a = shoot(with_main, scene, false);
                const bool wrote = r3d.stats().taa_main_velocity;
                const kin::u32 pre_draws = r3d.stats().ao_draws;
                const Pixels b = shoot(with_pre, scene, true);
                assert(!r3d.stats().taa_main_velocity && r3d.stats().ao_draws > 0);
                // Only a frame with nothing else to need a pre-pass draws the velocity in the main pass.
                const bool expect_main = extra == Extra::None || extra == Extra::CustomBlended;
                assert(wrote == expect_main && (wrote ? pre_draws == 0 : pre_draws > 0));
                if (a.rgba != b.rgba) {
                    std::size_t differing = 0;
                    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
                        differing += a.rgba[i] != b.rgba[i] ? 1 : 0;
                    }
                    std::cerr << "taa main velocity: extra " << static_cast<int>(extra) << " frame " << f << ": " << differing << " bytes differ\n";
                }
                assert(a.rgba == b.rgba);
            }
        }
        // And the picture is a real one: the run's last frame has the box where the cut-out leaves its clear half.
        kin::Scene3D scene;
        build(scene, 13, Extra::None);
        const Pixels last = shoot(with_main, scene, false);
        assert(brightness(last.at(80, 60)) > 0.0);
    });
}

// The shadow cache (Lights3D::cache_shadows): the shadow pass is skipped when the sun, the area, the resolution, the camera, the cascades' fit,
// every caster (mesh, material, copies) and the levels of detail are as they were, and the picture is the one drawn again, to the byte. Any
// change of any of them draws again, and the picture then is that of an uncached render of the same scene. A change in a copy the cache's
// cheap sample does not look at is still seen (the full comparison), a cut-out's texture changed in place is seen, a render target is never cached.
void test_gpu_shadow_cache() {
    with_gpu("test_gpu_shadow_cache", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = make_box(r3d, {20.0f, 0.2f, 20.0f});
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        const kin::Mesh other = make_box(r3d, {1.2f, 1.2f, 1.2f});
        kin::MeshOptions levels;
        levels.levels = 3;
        const kin::Mesh ball = r3d.create_mesh(kin::meshes::sphere(1.0f, 32, 16), levels);
        const kin::Mesh leaf_mesh = r3d.create_mesh(uv_square(1.5f));
        const kin::u8 leaf_texels[8] = {20, 200, 20, 0, 20, 200, 20, 255};
        const kin::Texture leaf = renderer.create_texture_from_rgba(leaf_texels, {2, 1});
        renderer.set_scale_mode(leaf, kin::ScaleMode::Nearest);
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::View3D reference_view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        enum class What { Plain, Sun, Camera, Resolution, Area, Cascades, Moved, MovedUnsampled, Added, Material, Mesh, NoCast, Lod, Leaf };
        struct Setup {
            float sun_x = 1.0f;
            float camera_x = 0.0f;
            kin::u32 resolution = 1024, cascades = 1;
            float area_top = 4.0f;
            float box_x = 0.0f, many_x = 0.0f;
            bool extra = false, double_sided = false, other_mesh = false, no_cast = false, lod = false;
            int lod_force = -1;
            float distance = 100.0f;
            kin::Texture leaf_texture;
        } setup;
        setup.leaf_texture = leaf;
        const auto build = [&](bool cache) {
            scene.clear();
            scene.camera = kin::Camera3D::look_at({setup.camera_x, 12.0f, 0.01f}, {0.0f, 0.0f, 0.0f}, 60.0f);
            scene.lights.sun = {.direction = {setup.sun_x, 1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}};
            scene.lights.ambient = {0.15f, 0.15f, 0.15f};
            scene.lights.shadow_area = kin::Bounds3{{-10.0f, -0.2f, -10.0f}, {10.0f, setup.area_top, 10.0f}};
            scene.lights.shadow_resolution = setup.resolution;
            scene.lights.shadow_cascades = setup.cascades;
            scene.lights.cache_shadows = cache;
            scene.lights.shadow_distance = setup.distance;
            scene.lod = {.enabled = setup.lod, .error_pixels = 0.5f, .force_level = setup.lod_force};
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            scene.draw(setup.other_mesh ? other : box, Mat4f::translation({setup.box_x, 2.5f, 0.0f}),
                       {.color = Color::rgb(180, 60, 60), .double_sided = setup.double_sided, .casts_shadow = !setup.no_cast});
            // Forty copies of one mesh in one draw: the cache's sample looks at the first, the last and every fifth.
            std::vector<kin::MeshInstance> copies;
            for (int i = 0; i < 40; ++i) {
                copies.push_back({.transform = Mat4f::translation({-8.0f + 0.4f * static_cast<float>(i), 1.0f + (i == 7 ? setup.many_x : 0.0f), -6.0f})});
            }
            scene.draw_instanced(box, copies, {.color = Color::rgb(60, 60, 180)});
            scene.draw(ball, Mat4f::translation({4.0f, 2.0f, 3.0f}), {.color = Color::rgb(60, 180, 60)});
            scene.draw(leaf_mesh, Mat4f::translation({-4.0f, 2.5f, 3.0f}) * Mat4f::rotation_x(-90.0f),
                       {.color = Color::rgb(255, 255, 255), .double_sided = true, .blend = kin::Blend3D::Mask, .texture = setup.leaf_texture});
            if (setup.extra) {
                scene.draw(box, Mat4f::translation({2.0f, 1.0f, -2.0f}), {.color = Color::rgb(200, 200, 60)});
            }
        };
        const auto shoot = [&](kin::View3D& target, bool cache) {
            build(cache);
            Pixels p = shoot_view(renderer, r3d, target, scene);
            renderer.present();
            return p;
        };
        // What an uncached render of the same setup gives.
        const auto truth = [&]() { return shoot(reference_view, false); };

        // Still: the first two render (the second copies what it drew from), then it hits; the picture is the same, to the byte.
        const Pixels first = shoot(view, true);
        if (r3d.stats().shadow_cascades == 0) {
            std::cerr << "shadows: not available on this device, skipped\n";
            return;
        }
        assert(!r3d.stats().shadow_cached && r3d.stats().shadow_draws > 0);
        const kin::u32 draws_when_drawn = r3d.stats().shadow_draws;
        assert(shoot(view, true).rgba == first.rgba && !r3d.stats().shadow_cached && r3d.stats().shadow_draws == draws_when_drawn);
        for (int f = 0; f < 4; ++f) {
            const Pixels p = shoot(view, true);
            assert(r3d.stats().shadow_cached && r3d.stats().shadow_draws == 0 && r3d.stats().shadow_casters == 0 && r3d.stats().shadow_cascades == 1);
            assert(p.rgba == first.rgba);
        }
        assert(first.rgba == truth().rgba);
        // Off for the scene, or for the renderer: drawn every time.
        shoot(view, false);
        assert(!r3d.stats().shadow_cached && r3d.stats().shadow_draws == draws_when_drawn);
        shoot(view, true);
        shoot(view, true);
        assert(shoot(view, true).rgba == first.rgba && r3d.stats().shadow_cached);
        setenv("KIN_SHADOW_CACHE", "0", 1);
        shoot(view, true);
        assert(!r3d.stats().shadow_cached);
        unsetenv("KIN_SHADOW_CACHE");

        // Each change draws again, and gives what an uncached render gives; then the cache is armed again after two stills.
        // (A change in a copy the sample does not see makes the cache wait a few renders before it copies again: `patience`.)
        const auto settle = [&](int patience) {
            // From the state the last change was restored to, which has not been rendered yet.
            for (int f = 0; f < patience; ++f) {
                shoot(view, true);
            }
            assert(r3d.stats().shadow_cached);
        };
        const auto changes = [&](const char* name, const std::function<void()>& change, const std::function<void()>& restore, int patience = 3) {
            std::cerr << "shadow cache: step " << name << "\n";
            settle(patience);
            change();
            const Pixels changed = shoot(view, true);
            const bool drawn_again = !r3d.stats().shadow_cached;
            const Pixels want = truth();
            if (!drawn_again || changed.rgba != want.rgba) {
                std::cerr << "shadow cache: " << name << ": " << (drawn_again ? "drawn" : "STALE") << ", "
                          << (changed.rgba == want.rgba ? "same" : "DIFFERENT") << " picture as uncached\n";
            }
            assert(drawn_again && changed.rgba == want.rgba);
            for (int f = 0; f < patience; ++f) {
                const Pixels again = shoot(view, true);
                assert(again.rgba == want.rgba);
            }
            assert(r3d.stats().shadow_cached);
            restore();
            // Putting a copy the sample does not see back is such a change too: the cache waits again before it copies.
            for (int f = 0; f < patience; ++f) {
                shoot(view, true);
            }
        };
        changes("sun", [&] { setup.sun_x = 0.6f; }, [&] { setup.sun_x = 1.0f; });
        changes("camera", [&] { setup.camera_x = 0.5f; }, [&] { setup.camera_x = 0.0f; });
        changes("resolution", [&] { setup.resolution = 512; }, [&] { setup.resolution = 1024; });
        changes("area", [&] { setup.area_top = 2.0f; }, [&] { setup.area_top = 4.0f; });
        changes("cascades", [&] { setup.cascades = 3; }, [&] { setup.cascades = 1; });
        changes("a caster moved", [&] { setup.box_x = 0.7f; }, [&] { setup.box_x = 0.0f; });
        // One of forty copies, not among those the sample looks at: the full comparison finds it.
        changes("a copy the sample does not look at", [&] { setup.many_x = 1.5f; }, [&] { setup.many_x = 0.0f; }, 14);
        changes("a caster added", [&] { setup.extra = true; }, [&] { setup.extra = false; });
        changes("a caster's material", [&] { setup.double_sided = true; }, [&] { setup.double_sided = false; });
        changes("a caster's mesh", [&] { setup.other_mesh = true; }, [&] { setup.other_mesh = false; });
        changes("a caster that stops casting", [&] { setup.no_cast = true; }, [&] { setup.no_cast = false; });
        changes("levels of detail", [&] { setup.lod = true; }, [&] { setup.lod = false; });
        // The same under the rest: the levels chosen another way (the camera and the meshes the same), and the cascades' fit from another reach.
        setup.lod = true;
        changes("a forced level of detail", [&] { setup.lod_force = 2; }, [&] { setup.lod_force = -1; });
        setup.lod = false;
        setup.cascades = 3;
        changes("the cascades' reach", [&] { setup.distance = 12.0f; }, [&] { setup.distance = 100.0f; });
        changes("the camera with cascades", [&] { setup.camera_x = 0.4f; }, [&] { setup.camera_x = 0.0f; });
        setup.cascades = 1;
        // A cut-out's texture changed in place: its shadow changes.
        const kin::u8 clear_texels[8] = {20, 200, 20, 255, 20, 200, 20, 0};
        changes("a cut-out's texture written", [&] { renderer.update_texture(leaf, {0, 0}, {2, 1}, clear_texels); },
                [&] { renderer.update_texture(leaf, {0, 0}, {2, 1}, leaf_texels); });

        // A cut-out whose texture is a render target is drawn into without a trace: never cached.
        {
            kin::RenderTarget target = renderer.create_render_target({16, 16});
            if (target.valid()) {
                renderer.push_render_target(target);
                renderer.clear(Color::rgb(20, 200, 20));
                renderer.pop_render_target();
                setup.leaf_texture = target.texture();
                for (int f = 0; f < 5; ++f) {
                    shoot(view, true);
                    assert(!r3d.stats().shadow_cached);
                }
                setup.leaf_texture = leaf;
            }
        }

        // A resize draws again (the view's size is in the key, and the camera's aspect with it).
        for (int f = 0; f < 3; ++f) {
            shoot(view, true);
        }
        assert(r3d.stats().shadow_cached);
        r3d.resize(view, {200, 150});
        {
            build(true);
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.present();
            assert(!r3d.stats().shadow_cached);
        }
        r3d.resize(view, {160, 120});

        // Two views of two scenes alternating: each render is right (the map is shared, so each draws it again).
        for (int f = 0; f < 6; ++f) {
            setup.sun_x = 1.0f;
            const Pixels a = shoot(view, true);
            const Pixels a_want = truth();
            setup.sun_x = 0.5f;
            const Pixels b = shoot(reference_view, true);
            const Pixels b_want = shoot(view, false);
            assert(a.rgba == a_want.rgba);
            assert(b.rgba == b_want.rgba);
        }
        setup.sun_x = 1.0f;
    });
}

// Ambient occlusion and contact shadows with temporal anti-aliasing: the
// pre-pass is drawn once and shared (jittered like the main pass), each
// effect runs, the foot of the wall is dark and the picture settles.
void test_gpu_taa_with_occlusion() {
    with_gpu("test_gpu_taa_with_occlusion", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const AoRoom room{r3d};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 4});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({-2.0f, 3.0f, 6.0f}, {0.0f, 0.3f, -2.0f}, 50.0f);
        scene.lights.sun = {.direction = {1.0f, 0.7f, 0.3f}, .color = {1.0f, 1.0f, 1.0f}};
        scene.lights.ambient = {0.5f, 0.5f, 0.5f};
        const auto render = [&](bool taa, bool ao, bool contact, int frames) {
            scene.taa.enabled = taa;
            scene.ambient_occlusion = {.enabled = ao, .radius = 3.0f};
            scene.contact_shadows = {.enabled = contact};
            Pixels p, last;
            for (int f = 0; f < frames; ++f) {
                scene.clear();
                room.draw_floor(scene);
                room.draw_wall(scene);
                room.draw_pole(scene, 1.0f, 1.0f);
                last = std::move(p);
                p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
            }
            return std::pair{p, last};
        };
        const auto [aoed, aoed_last] = render(false, true, true, 1);
        if (lacks(r3d.capabilities().ambient_occlusion, "ambient occlusion")) {
            return;
        }
        assert(r3d.stats().ao_dispatches > 0);
        const auto at = [&](const Pixels& p, Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        const Vec3f foot{0.0f, 0.0f, -1.8f}, open{-2.0f, 0.0f, 2.0f};
        const auto [off, off_last] = render(false, false, false, 1);
        const auto [on, on_last] = render(true, true, true, 30);
        if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        assert(r3d.stats().taa_dispatches > 0);
        const kin::Render3DStats& stats = r3d.stats();
        std::cerr << "taa+ao+contact: pre-pass draws " << stats.ao_draws << ", ao " << stats.ao_dispatches << ", contact " << stats.contact_dispatches
                  << ", taa " << stats.taa_dispatches << "\n";
        assert(stats.ao_draws == 3 && stats.ao_instances == 3 && stats.ao_dispatches == 3 && stats.contact_dispatches == 3 && stats.taa_dispatches == 1);
        assert(stats.samples == 1);
        // The crease is still dark with it, against the open floor; and the picture is still.
        std::cerr << "taa+ao+contact: the crease " << brightness(at(off, foot)) << " plain, " << brightness(at(aoed, foot)) << " with occlusion, "
                  << brightness(at(on, foot)) << " with that and temporal anti-aliasing\n";
        assert(brightness(at(on, foot)) < brightness(at(off, foot)) * 97 / 100);
        long diff = 0;
        for (std::size_t i = 0; i < on.rgba.size(); ++i) {
            diff += std::labs(static_cast<long>(on.rgba[i]) - static_cast<long>(on_last.rgba[i]));
        }
        std::cerr << "taa+ao+contact: mean frame to frame change " << static_cast<double>(diff) / static_cast<double>(on.rgba.size()) << "\n";
        assert(static_cast<double>(diff) / static_cast<double>(on.rgba.size()) < 0.5);
        // Close to the plain picture where nothing is at an edge.
        expect(at(on, open), at(off, open), "the open floor, with and without temporal anti-aliasing", 6);
        // And TAA alone needs no pre-pass: the main pass writes the velocity (test_gpu_taa_main_velocity has the details).
        render(true, false, false, 2);
        assert(stats.ao_draws == 0 && stats.taa_main_velocity && stats.ao_dispatches == 0 && stats.contact_dispatches == 0 && stats.taa_dispatches == 1);
        assert(!(render(true, true, false, 2), stats.taa_main_velocity)); // with occlusion it shares the pre-pass
        render(false, false, false, 1);
        assert(stats.ao_draws == 0 && stats.taa_dispatches == 0);
    });
}

// Pick is exact: the same pixel gives the same answer every frame with the
// jitter on, and the one without it.
void test_gpu_taa_pick() {
    with_gpu("test_gpu_taa_pick", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {2.0f, 2.0f, 2.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .pick = true});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        // An edge pixel of the box: where the jitter would change what is under it.
        const auto edge = scene.camera.project({1.0f, 0.0f, 1.0f}, screen);
        assert(edge);
        const kin::Vec2i pixel{static_cast<int>(edge->pos.x), static_cast<int>(edge->pos.y)};
        kin::PickResult reference;
        for (const bool taa : {false, true}) {
            scene.taa.enabled = taa;
            for (int f = 0; f < 10; ++f) {
                scene.clear();
                scene.draw(box, Mat4f::identity(), {.color = Color::rgb(200, 60, 60)}, kin::colors::white, 4);
                renderer.clear(kin::colors::black);
                render_ok(r3d, view, scene);
                const kin::PickResult p = r3d.pick(view, pixel);
                if (taa && lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
                    return;
                }
                assert(!taa || r3d.stats().taa_dispatches > 0);
                if (f == 0 && !taa) {
                    reference = p;
                }
                assert(p.hit == reference.hit && p.id == reference.id);
                assert(kin::length(p.position - reference.position) < 1e-5f);
                renderer.present();
            }
        }
        // The camera's own projection is untouched by it.
        const auto again = scene.camera.project({1.0f, 0.0f, 1.0f}, screen);
        assert(again && again->pos.x == edge->pos.x && again->pos.y == edge->pos.y);
    });
}

// An HDR view with bloom: the glow and the tonemap are made from the
// resolved picture, and a bright cube's glow shows as without it.
void test_gpu_taa_hdr() {
    with_gpu("test_gpu_taa_hdr", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh cube = make_box(r3d, {0.6f, 0.6f, 0.6f});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.draw(cube, Mat4f::identity(), {.color = Color::rgb(255, 140, 60), .emissive = 6.0f, .unlit = true});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 4, .bloom = 1.0f});
        const auto side = scene.camera.project({0.55f, 0.0f, 0.3f}, screen);
        assert(side);
        Pixels p;
        for (int f = 0; f < 12; ++f) {
            p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            scene.taa.enabled = true;
        }
        if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        assert(r3d.stats().taa_dispatches > 0);
        assert(brightness(p.at(side->pos)) > 40); // the glow
        const Color middle = p.at(80, 60);
        assert(middle.r > 230 && middle.g > 150);
    });
}

// Copies with a MeshInstance::id keep their history through a reset: the frame
// after the reset frame blends with it, as the same copies without an id do
// (before, the ids were not recorded on the reset frame, and the next frame took
// every id'd copy for a new one: aliased edges for a frame). Two views driven
// in step see the same jitter, so their pictures must be the same.
void test_gpu_taa_ids_after_reset() {
    with_gpu("test_gpu_taa_ids_after_reset", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh block = make_box(r3d, {3.0f, 3.0f, 0.2f});
        std::array<kin::View3D, 2> views{r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)}),
                                         r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)})};
        kin::Scene3D scene;
        scene.camera = flat_camera();
        scene.taa = {.enabled = true};
        const kin::Material3D white{.color = Color::rgb(255, 255, 255), .unlit = true};
        // Turned blocks, so that their edges cross pixels at every fraction.
        const auto shoot = [&](bool with_ids) {
            scene.clear();
            const std::array<kin::MeshInstance, 2> copies{
                kin::MeshInstance{Mat4f::translation({-3.0f, 0.0f, 0.0f}) * Mat4f::rotation_z(25.0f), kin::colors::white, with_ids ? 7u : 0u},
                kin::MeshInstance{Mat4f::translation({3.0f, 0.5f, 0.0f}) * Mat4f::rotation_z(70.0f), kin::colors::white, with_ids ? 8u : 0u}};
            scene.draw_instanced(block, copies, white);
            Pixels p = shoot_view(renderer, r3d, views[with_ids ? 0 : 1], scene);
            renderer.present();
            return p;
        };
        const auto soft = [](const Pixels& p) {
            int n = 0;
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    const int v = static_cast<int>(p.at(x, y).r);
                    n += (v > 20 && v < 235) ? 1 : 0;
                }
            }
            return n;
        };
        for (int f = 0; f < 6; ++f) {
            shoot(true);
            shoot(false);
        }
        if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        for (auto& v : views) {
            v.reset_history();
        }
        for (int f = 0; f < 3; ++f) {
            const Pixels with = shoot(true);
            assert(r3d.stats().taa_dispatches > 0 && r3d.stats().taa_reset == (f == 0));
            const Pixels without = shoot(false);
            assert(r3d.stats().taa_reset == (f == 0));
            if (f == 1) {
                // The second frame has a history to blend (soft edges); the copies with ids found theirs.
                std::cerr << "taa: ids after a reset: soft edge pixels " << soft(with) << " with ids, " << soft(without) << " without\n";
                assert(soft(without) > 10);
            }
            assert(with.rgba == without.rgba);
        }
    });
}

// Matching copies to last frame's (the velocity of each) is the same on the
// workers as on one thread: a scene of static, moving, id'd and new copies, two
// draws of one mesh, with the id'd ones listed in another order now and then
// and one id given twice. Compared by picture, frame by frame (a copy given
// another's past would smear differently).
void test_gpu_taa_match_on_workers() {
    with_gpu("test_gpu_taa_match_on_workers", [](kin::Renderer2D& renderer) {
        kin::JobSystem jobs;
        kin::Renderer3D r3d{renderer};
        const kin::Mesh block = make_box(r3d, {0.12f, 0.12f, 0.12f});
        const kin::Material3D plain{.unlit = true};
        const kin::Material3D other{.color = Color::rgb(200, 120, 40), .unlit = true};
        constexpr int frames = 14;
        std::array<std::vector<Pixels>, 2> shots;
        bool capable = true;
        for (const bool workers : {false, true}) {
            renderer.set_job_system(workers ? &jobs : nullptr);
            kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
            kin::Scene3D scene;
            scene.cull = false;
            scene.camera = flat_camera();
            scene.taa = {.enabled = true};
            for (int f = 0; f < frames; ++f) {
                scene.clear();
                std::vector<kin::MeshInstance> a, b;
                for (int i = 0; i < 4096; ++i) {
                    const float x = -6.3f + 0.2f * static_cast<float>(i % 64), y = -4.7f + 0.15f * static_cast<float>(i / 64);
                    const kin::Color tint = Color::rgb(static_cast<kin::u8>(80 + i % 160), static_cast<kin::u8>(255 - i % 97), 255);
                    switch (i % 4) {
                    case 0: // still, no id
                        a.push_back({Mat4f::translation({x, y, 0.0f}), tint});
                        break;
                    case 1: // moving, no id
                        a.push_back({Mat4f::translation({x + 0.07f * static_cast<float>(f), y, 0.0f}), tint});
                        break;
                    case 2: // moving, with an id (a few share one)
                        a.push_back({Mat4f::translation({x, y + 0.05f * static_cast<float>(f), 0.0f}), tint, i % 500 == 2 ? 999u : static_cast<kin::u32>(i + 1)});
                        break;
                    default: // the other draw: still
                        b.push_back({Mat4f::translation({x, y, 0.0f}), tint});
                        break;
                    }
                }
                if (f % 3 == 0) {
                    std::rotate(a.begin(), a.begin() + 5, a.end()); // the places change; the ids say who is who
                }
                for (int k = 0; k < 7 && f >= 6 && f < 10; ++k) { // new copies, and a count that changes
                    b.push_back({Mat4f::translation({-5.0f + static_cast<float>(k), 5.5f, 0.0f}), kin::colors::white});
                }
                scene.draw_instanced(block, a, plain);
                scene.draw_instanced(block, b, other);
                shots[workers ? 1 : 0].push_back(shoot_view(renderer, r3d, view, scene));
                renderer.present();
                capable = capable && r3d.capabilities().temporal_aa;
                assert(!capable || r3d.stats().taa_dispatches > 0);
            }
        }
        renderer.set_job_system(nullptr);
        if (lacks(capable, "temporal anti-aliasing")) {
            return;
        }
        for (int f = 0; f < frames; ++f) {
            assert(shots[0][static_cast<std::size_t>(f)].rgba == shots[1][static_cast<std::size_t>(f)].rgba);
        }
    });
}

// A sun with no colour casts no shadow: no shadow pass is run (the sun term is
// multiplied by its colour, so the picture is the one without a shadow area).
void test_gpu_sun_off_no_shadow_pass() {
    with_gpu("test_gpu_sun_off_no_shadow_pass", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = make_box(r3d, {20.0f, 0.2f, 20.0f});
        const kin::Mesh box = make_box(r3d, {1.5f, 1.5f, 1.5f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 12.0f, 0.01f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.lights.ambient = {0.3f, 0.3f, 0.3f};
        const auto draw = [&](bool area, kin::Vec3f sun, kin::u32 cascades) {
            scene.clear();
            scene.lights.sun = {.direction = {1.0f, 1.0f, 0.0f}, .color = sun};
            scene.lights.shadow_cascades = cascades;
            scene.lights.shadow_area = area ? kin::Bounds3{{-10.0f, -0.2f, -10.0f}, {10.0f, 4.0f, 10.0f}} : kin::Bounds3{};
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            scene.draw(box, Mat4f::translation({0.0f, 2.5f, 0.0f}), {.color = Color::rgb(180, 60, 60)});
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        for (const kin::u32 cascades : {1u, 4u}) {
            draw(true, {1.0f, 1.0f, 1.0f}, cascades);
            assert(r3d.stats().shadow_draws > 0 && r3d.stats().shadow_cascades > 0);
            const Pixels dark = draw(true, {0.0f, 0.0f, 0.0f}, cascades);
            assert(r3d.stats().shadow_draws == 0 && r3d.stats().shadow_casters == 0 && r3d.stats().shadow_cascades == 0);
            assert(dark.rgba == draw(false, {0.0f, 0.0f, 0.0f}, cascades).rgba);
        }
    });
}

// --- Levels of detail (G3a) ---

// The distance from `p` to the triangle abc (Ericson, Real-Time Collision Detection).
float distance_to_triangle(Vec3f p, Vec3f a, Vec3f b, Vec3f c) {
    const Vec3f ab = b - a, ac = c - a, ap = p - a;
    const float d1 = kin::dot(ab, ap), d2 = kin::dot(ac, ap);
    Vec3f closest;
    if (d1 <= 0.0f && d2 <= 0.0f) {
        closest = a;
    } else {
        const Vec3f bp = p - b;
        const float d3 = kin::dot(ab, bp), d4 = kin::dot(ac, bp);
        const Vec3f cp = p - c;
        const float d5 = kin::dot(ab, cp), d6 = kin::dot(ac, cp);
        const float vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
        if (d3 >= 0.0f && d4 <= d3) {
            closest = b;
        } else if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
            closest = a + ab * (d1 / (d1 - d3));
        } else if (d6 >= 0.0f && d5 <= d6) {
            closest = c;
        } else if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
            closest = a + ac * (d2 / (d2 - d6));
        } else if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
            closest = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        } else {
            const float denom = 1.0f / (va + vb + vc);
            closest = a + ab * (vb * denom) + ac * (vc * denom);
        }
    }
    return kin::length(p - closest);
}

// The most any point of level `l` (sampled on a grid in each of its triangles) lies from the surface of `fine`.
float level_deviation(const MeshData& fine, const kin::MeshLodChain& chain, std::size_t l, std::size_t own_vertices = 0) {
    if (own_vertices == 0) {
        own_vertices = fine.vertices.size();
    }
    const std::vector<kin::u32>& coarse = chain.levels[l].indices;
    float worst = 0.0f;
    for (std::size_t t = 0; t < coarse.size() / 3; ++t) {
        const auto at = [&](kin::u32 i) { return i < own_vertices ? fine.vertices[i].position : chain.extra_vertices[i - own_vertices].position; };
        const Vec3f a = at(coarse[t * 3]), b = at(coarse[t * 3 + 1]), c = at(coarse[t * 3 + 2]);
        for (int i = 0; i <= 4; ++i) {
            for (int j = 0; j <= 4 - i; ++j) {
                const Vec3f point = a + (b - a) * (static_cast<float>(i) / 4.0f) + (c - a) * (static_cast<float>(j) / 4.0f);
                float nearest = 1e30f;
                for (std::size_t u = 0; u < fine.triangle_count(); ++u) {
                    nearest = std::min(nearest, distance_to_triangle(point, fine.vertices[fine.indices[u * 3]].position,
                                                                     fine.vertices[fine.indices[u * 3 + 1]].position,
                                                                     fine.vertices[fine.indices[u * 3 + 2]].position));
                }
                worst = std::max(worst, nearest);
            }
        }
    }
    return worst;
}

// What a chain must always be: level 0 the mesh, fewer triangles and larger errors down the levels, indices in range
// and whole triangles.
void check_chain(const MeshData& data, const kin::MeshLodChain& chain, std::string_view name) {
    assert(chain.count() >= 1 && chain.levels[0].triangles == data.triangle_count() && chain.levels[0].error == 0.0f &&
           chain.levels[0].indices.empty());
    for (std::size_t l = 1; l < chain.count(); ++l) {
        const kin::MeshLodLevel& level = chain.levels[l];
        assert(level.indices.size() == static_cast<std::size_t>(level.triangles) * 3 && level.triangles > 0);
        assert(level.triangles < chain.levels[l - 1].triangles);
        assert(level.error >= chain.levels[l - 1].error);
        for (const kin::u32 i : level.indices) {
            assert(i < data.vertices.size() + chain.extra_vertices.size());
        }
    }
    std::cerr << "lod: " << name << ": " << chain.count() << " levels:";
    for (const kin::MeshLodLevel& level : chain.levels) {
        std::cerr << ' ' << level.triangles << " (error " << level.error << ")";
    }
    std::cerr << '\n';
}

void test_mesh_lod_builder() {
    kin::MeshOptions options;
    options.levels = 4;

    // A sphere: the levels halve the triangles, the errors rise, level 0 is the mesh and the input is left alone.
    const MeshData sphere = kin::meshes::sphere(1.0f, 48, 24);
    const MeshData sphere_before = sphere;
    const kin::MeshLodChain chain = kin::build_mesh_levels(sphere, options);
    check_chain(sphere, chain, "sphere");
    assert(chain.count() == 4);
    assert(sphere.vertices.size() == sphere_before.vertices.size() && sphere.indices == sphere_before.indices &&
           std::memcmp(sphere.vertices.data(), sphere_before.vertices.data(), sphere.vertices.size() * sizeof(kin::MeshVertex)) == 0);
    for (std::size_t l = 1; l < chain.count(); ++l) {
        const double ratio = static_cast<double>(chain.levels[l].triangles) / chain.levels[l - 1].triangles;
        assert(ratio > 0.35 && ratio < 0.7); // about half
        assert(chain.levels[l].error > chain.levels[l - 1].error); // strictly, on a curved surface
    }
    // A mesh is chained alike twice.
    const kin::MeshLodChain again = kin::build_mesh_levels(sphere, options);
    for (std::size_t l = 1; l < chain.count(); ++l) {
        assert(chain.levels[l].indices == again.levels[l].indices && chain.levels[l].error == again.levels[l].error);
    }
    // The error is a bound on how far the level lies from the mesh, and not a loose one.
    for (std::size_t l = 1; l < chain.count(); ++l) {
        const float deviation = level_deviation(sphere, chain, l);
        std::cerr << "lod: sphere level " << l << ": recorded error " << chain.levels[l].error << ", measured deviation " << deviation << '\n';
        assert(deviation <= chain.levels[l].error * 1.5f + 1e-3f);
        assert(chain.levels[l].error <= std::max(deviation, 0.003f) * 8.0f);
    }

    // A torus.
    const MeshData torus = kin::meshes::torus(1.0f, 0.3f, 48, 16);
    const kin::MeshLodChain torus_chain = kin::build_mesh_levels(torus, options);
    check_chain(torus, torus_chain, "torus");
    assert(torus_chain.count() >= 3);
    for (std::size_t l = 1; l < torus_chain.count(); ++l) {
        const float deviation = level_deviation(torus, torus_chain, l);
        std::cerr << "lod: torus level " << l << ": recorded error " << torus_chain.levels[l].error << ", measured deviation " << deviation << '\n';
        assert(deviation <= torus_chain.levels[l].error * 1.5f + 1e-3f);
        assert(torus_chain.levels[l].error <= std::max(deviation, 0.003f) * 8.0f);
    }

    // An open mesh keeps its border: a wavy patch, every vertex on its edge is still used by every level, and the
    // inside is what is simplified.
    MeshData patch = kin::meshes::plane({8.0f, 8.0f}, 32);
    for (kin::MeshVertex& v : patch.vertices) {
        v.position.y = 0.3f * std::sin(v.position.x * 1.3f) * std::cos(v.position.z * 1.1f);
    }
    const kin::MeshLodChain patch_chain = kin::build_mesh_levels(patch, options);
    check_chain(patch, patch_chain, "wavy patch");
    assert(patch_chain.count() >= 3);
    for (std::size_t l = 1; l < patch_chain.count(); ++l) {
        std::vector<char> used(patch.vertices.size(), 0);
        for (const kin::u32 i : patch_chain.levels[l].indices) {
            used[i] = 1;
        }
        int border = 0;
        for (std::size_t i = 0; i < patch.vertices.size(); ++i) {
            const Vec3f& p = patch.vertices[i].position;
            if (std::abs(std::abs(p.x) - 4.0f) < 1e-4f || std::abs(std::abs(p.z) - 4.0f) < 1e-4f) {
                ++border;
                assert(used[i]);
            }
        }
        assert(border == 4 * 32);
    }
    // Without the lock the same patch loses border vertices (so the test above is about the lock).
    kin::MeshOptions unlocked = options;
    unlocked.lock_border = false;
    const kin::MeshLodChain loose_chain = kin::build_mesh_levels(patch, unlocked);
    bool lost_one = false;
    if (loose_chain.count() > 1) {
        std::vector<char> used(patch.vertices.size(), 0);
        for (const kin::u32 i : loose_chain.levels.back().indices) {
            used[i] = 1;
        }
        for (std::size_t i = 0; i < patch.vertices.size(); ++i) {
            const Vec3f& p = patch.vertices[i].position;
            lost_one = lost_one || ((std::abs(std::abs(p.x) - 4.0f) < 1e-4f || std::abs(std::abs(p.z) - 4.0f) < 1e-4f) && !used[i]);
        }
    }
    assert(lost_one);

    // A mesh with no indices (its vertices are the triangles) is chained too, over its own vertices.
    MeshData soup;
    for (std::size_t i = 0; i < torus.indices.size(); ++i) {
        soup.vertices.push_back(torus.vertices[torus.indices[i]]);
    }
    const kin::MeshLodChain soup_chain = kin::build_mesh_levels(soup, options);
    check_chain(soup, soup_chain, "unindexed torus");
    assert(soup_chain.count() >= 2);

    // A flat-shaded mesh with no indices (a model with no normals) gets levels made of vertices of their own, with flat
    // normals, and the mesh's vertices are left as they are.
    MeshData flat;
    for (std::size_t t = 0; t < torus.triangle_count(); ++t) {
        const Vec3f n = kin::normalize(geometric_normal(torus, t));
        for (int k = 0; k < 3; ++k) {
            kin::MeshVertex v = torus.vertices[torus.indices[t * 3 + static_cast<std::size_t>(k)]];
            v.normal = n;
            flat.vertices.push_back(v);
        }
    }
    const kin::MeshLodChain flat_chain = kin::build_mesh_levels(flat, options);
    check_chain(flat, flat_chain, "flat torus");
    assert(flat_chain.count() >= 3 && !flat_chain.extra_vertices.empty());
    for (std::size_t l = 1; l < flat_chain.count(); ++l) {
        const float deviation = level_deviation(torus, flat_chain, l, flat.vertices.size());
        assert(deviation <= flat_chain.levels[l].error * 1.5f + 1e-3f);
        for (const kin::u32 i : flat_chain.levels[l].indices) {
            const kin::MeshVertex& v = flat_chain.extra_vertices[i - flat.vertices.size()];
            assert(std::abs(kin::length(v.normal) - 1.0f) < 1e-3f);
        }
    }

    // Tiny meshes, and a request for one level, give one.
    assert(kin::build_mesh_levels(kin::meshes::box({1.0f, 1.0f, 1.0f}), options).count() == 1);
    assert(kin::build_mesh_levels(kin::meshes::octahedron(1.0f), options).count() == 1);
    MeshData triangle;
    triangle.vertices.resize(3);
    triangle.vertices[1].position = {1.0f, 0.0f, 0.0f};
    triangle.vertices[2].position = {0.0f, 1.0f, 0.0f};
    assert(kin::build_mesh_levels(triangle, options).count() == 1);
    kin::MeshOptions one;
    assert(kin::build_mesh_levels(sphere, one).count() == 1);
    // A flat quad grid cannot be simplified without its border: it loses nothing from it.
    // Levels stop where a level would be tiny: a mesh of 40 triangles has no level of under 16.
    const MeshData small = kin::meshes::sphere(1.0f, 6, 4);
    const kin::MeshLodChain small_chain = kin::build_mesh_levels(small, options);
    for (const kin::MeshLodLevel& level : small_chain.levels) {
        assert(level.triangles >= options.min_triangles);
    }
    // More levels than max_mesh_levels are not made.
    kin::MeshOptions many = options;
    many.levels = 100;
    many.min_triangles = 1;
    many.min_reduction = 0.01f;
    assert(kin::build_mesh_levels(kin::meshes::sphere(1.0f, 128, 64), many).count() <= kin::max_mesh_levels);
}

// The mesh of one level of a chain, as a mesh of its own (the same vertices, that level's triangles): what a copy
// drawn at that level should look like, shadows and all.
MeshData level_as_mesh(const MeshData& data, const kin::MeshLodChain& chain, std::size_t level) {
    MeshData out = data;
    if (level > 0) {
        out.indices = chain.levels[level].indices;
    }
    return out;
}

// The level Scene3D::lod should give a copy of `mesh` under the perspective camera `camera` over a view `height` pixels
// tall: the coarsest whose error, scaled, spans no more than `pixels` at the copy's nearest point. Written out here from
// the setting's description, not from the renderer's code.
kin::u32 expected_level(const kin::Mesh& mesh, const kin::Camera3D& camera, float height, Vec3f at, float scale, float pixels) {
    const float focal = 0.5f * height / std::tan(0.5f * camera.fov * 3.14159265f / 180.0f);
    const float depth = kin::dot(at - camera.position, camera.forward());
    const float nearest = depth - mesh.bounds().radius() * scale;
    kin::u32 level = 0;
    for (kin::u32 l = 1; l < mesh.level_count(); ++l) {
        if (mesh.level_error(l) * scale * focal <= pixels * nearest) {
            level = l;
        }
    }
    return level;
}

// The one level a scene of one copy drew at (the copy counts per level are in the render's statistics).
kin::u32 level_drawn(const kin::Render3DStats& stats) {
    kin::u32 level = ~0u, total = 0;
    for (kin::u32 l = 0; l < kin::max_mesh_levels; ++l) {
        if (stats.lod_copies_by_level[l] > 0) {
            level = l;
            total += stats.lod_copies_by_level[l];
        }
    }
    assert(total == 1);
    return level;
}

// Mesh levels as the API reports them; the selection of a level: by distance, by the copy's scale, by the bias, by
// force_level, for orthographic cameras; blended meshes and meshes without levels are not touched; the statistics
// count what is drawn.
void test_gpu_lod_selection() {
    with_gpu("test_gpu_lod_selection", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.levels = 4;
        const MeshData data = kin::meshes::sphere(1.0f, 64, 32);
        const kin::Mesh mesh = r3d.create_mesh(data, options);
        const kin::Mesh plain = r3d.create_mesh(data);
        assert(plain.level_count() == 1 && plain.level_triangles(0) == data.triangle_count() && plain.level_error(0) == 0.0f);
        const kin::MeshLodChain chain = kin::build_mesh_levels(data, options);
        assert(mesh.level_count() == chain.count() && mesh.level_count() == 4);
        assert(mesh.triangle_count() == data.triangle_count() && mesh.index_count() == data.indices.size());
        for (kin::u32 l = 0; l < mesh.level_count(); ++l) {
            assert(mesh.level_triangles(l) == chain.levels[l].triangles && mesh.level_error(l) == chain.levels[l].error);
            assert(l == 0 || (mesh.level_triangles(l) < mesh.level_triangles(l - 1) && mesh.level_error(l) > mesh.level_error(l - 1)));
        }
        // The same data with the options of one level is a mesh as before.
        assert(r3d.create_mesh(data, kin::MeshOptions{}).level_count() == 1);

        kin::View3D view = r3d.create_view({160, 120});
        kin::Scene3D scene;
        scene.lights.sun.color = {};
        scene.lights.ambient = {1.0f, 1.0f, 1.0f};
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 60.0f);
        const auto shoot_one = [&](const kin::Mesh& m, Vec3f at, float copy_scale, const kin::Material3D& material = {}) {
            scene.clear();
            scene.draw(m, Mat4f::translation(at) * Mat4f::scaling({copy_scale, copy_scale, copy_scale}), material);
            render_ok(r3d, view, scene);
            renderer.present();
        };

        // Off: every copy is level 0 whatever its distance, and a mesh with levels has all the triangles.
        scene.lod = {};
        shoot_one(mesh, {0.0f, 0.0f, -500.0f}, 1.0f);
        assert(r3d.stats().triangles == data.triangle_count() && r3d.stats().lod_copies_by_level[0] == 1);

        // By distance: the level is the expected one at each, rises with the distance, and takes in the first and the last.
        scene.lod = {.enabled = true};
        bool seen[4] = {};
        kin::u32 last = 0;
        for (const float distance : {2.0f, 4.0f, 8.0f, 14.0f, 20.0f, 30.0f, 50.0f, 80.0f, 140.0f, 300.0f}) {
            for (const float pixels : {0.25f, 1.0f, 4.0f}) {
                scene.lod.error_pixels = pixels;
                shoot_one(mesh, {0.0f, 0.0f, -distance}, 1.0f);
                const kin::u32 got = level_drawn(r3d.stats());
                const kin::u32 want = expected_level(mesh, scene.camera, 120.0f, {0.0f, 0.0f, -distance}, 1.0f, pixels);
                if (got != want) {
                    std::cerr << "lod: distance " << distance << ", " << pixels << " px: level " << got << ", want " << want << '\n';
                }
                assert(got == want);
                assert(r3d.stats().triangles == mesh.level_triangles(got));
                if (pixels == 1.0f) {
                    assert(got >= last);
                    last = got;
                    seen[got] = true;
                }
            }
        }
        assert(seen[0] && seen[3]); // the distances reach from the finest level to the coarsest
        scene.lod.error_pixels = 1.0f;

        // The copy's scale: at the same distance, a copy four times as large takes a finer level (its error is four times
        // as large) at some distance, and never a coarser one.
        bool finer_somewhere = false;
        for (const float distance : {10.0f, 14.0f, 17.0f, 20.0f, 25.0f, 30.0f, 40.0f, 60.0f}) {
            shoot_one(mesh, {0.0f, 0.0f, -distance}, 1.0f);
            const kin::u32 small = level_drawn(r3d.stats());
            shoot_one(mesh, {0.0f, 0.0f, -distance}, 4.0f);
            const kin::u32 large = level_drawn(r3d.stats());
            assert(small == expected_level(mesh, scene.camera, 120.0f, {0.0f, 0.0f, -distance}, 1.0f, 1.0f));
            assert(large == expected_level(mesh, scene.camera, 120.0f, {0.0f, 0.0f, -distance}, 4.0f, 1.0f));
            assert(large <= small);
            finer_somewhere = finer_somewhere || large < small;
        }
        assert(finer_somewhere);
        const float stretched_distance = 30.0f;
        shoot_one(mesh, {0.0f, 0.0f, -stretched_distance}, 4.0f);
        const kin::u32 large = level_drawn(r3d.stats());
        // A copy stretched along one axis only counts by its largest.
        scene.clear();
        scene.draw(mesh, Mat4f::translation({0.0f, 0.0f, -stretched_distance}) * Mat4f::scaling({1.0f, 1.0f, 4.0f}));
        render_ok(r3d, view, scene);
        assert(level_drawn(r3d.stats()) == large);

        // The bias: twice the quality is the error bound halved, so no coarser.
        scene.lod.bias = 4.0f;
        shoot_one(mesh, {0.0f, 0.0f, -30.0f}, 1.0f);
        assert(level_drawn(r3d.stats()) == expected_level(mesh, scene.camera, 120.0f, {0.0f, 0.0f, -30.0f}, 1.0f, 0.25f));
        scene.lod.bias = 1.0f;

        // Forced: that level for every copy, the coarsest for a level the mesh lacks; a mesh without levels is level 0.
        for (const kin::i32 force : {0, 1, 2, 3, 9}) {
            scene.lod.force_level = force;
            shoot_one(mesh, {0.0f, 0.0f, -3.0f}, 1.0f);
            assert(level_drawn(r3d.stats()) == std::min<kin::u32>(static_cast<kin::u32>(force), 3));
            assert(r3d.stats().triangles == mesh.level_triangles(std::min<kin::u32>(static_cast<kin::u32>(force), 3)));
            shoot_one(plain, {0.0f, 0.0f, -3.0f}, 1.0f);
            assert(level_drawn(r3d.stats()) == 0 && r3d.stats().triangles == data.triangle_count());
        }
        scene.lod.force_level = -1;

        // A mesh without levels, and a blended one (drawn furthest first, which levels would break up), stay as they are.
        shoot_one(plain, {0.0f, 0.0f, -300.0f}, 1.0f);
        assert(level_drawn(r3d.stats()) == 0);
        shoot_one(mesh, {0.0f, 0.0f, -300.0f}, 1.0f, {.blend = kin::Blend3D::Alpha});
        assert(level_drawn(r3d.stats()) == 0 && r3d.stats().triangles == data.triangle_count());

        // Orthographic: how far it is does not matter, the scale of the view does.
        kin::Camera3D ortho = scene.camera;
        ortho.projection = kin::Projection3D::Orthographic;
        ortho.z_far = 1000.0f;
        kin::u32 distinct_levels = 0, last_orthographic = ~0u;
        for (const float height : {80.0f, 40.0f, 10.0f, 3.0f, 1.0f, 0.5f}) {
            ortho.height = height;
            scene.camera = ortho;
            const float px_per_unit = 120.0f / height;
            kin::u32 want = 0;
            for (kin::u32 l = 1; l < 4; ++l) {
                if (mesh.level_error(l) * px_per_unit <= 1.0f) {
                    want = l;
                }
            }
            shoot_one(mesh, {0.0f, 0.0f, -10.0f}, 1.0f);
            const kin::u32 near_level = level_drawn(r3d.stats());
            shoot_one(mesh, {0.0f, 0.0f, -900.0f}, 1.0f);
            std::cerr << "lod: orthographic, " << px_per_unit << " px a unit: level " << near_level << ", want " << want << '\n';
            assert(near_level == want && level_drawn(r3d.stats()) == want);
            distinct_levels += near_level != last_orthographic ? 1 : 0;
            last_orthographic = near_level;
        }
        assert(distinct_levels >= 3);
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 60.0f);

        // A scene of many copies: one draw a level in use, the copies counted by level, the triangles those drawn.
        scene.clear();
        std::vector<kin::MeshInstance> copies;
        std::array<kin::u32, 4> by_level{};
        for (int i = 0; i < 400; ++i) {
            const float distance = 2.0f + static_cast<float>(i) * 0.75f;
            copies.push_back({Mat4f::translation({std::sin(static_cast<float>(i)) * distance * 0.2f, 0.0f, -distance})});
            ++by_level[expected_level(mesh, scene.camera, 120.0f, {0.0f, 0.0f, -distance}, 1.0f, 1.0f)];
        }
        scene.draw_instanced(mesh, copies);
        render_ok(r3d, view, scene);
        const kin::Render3DStats stats = r3d.stats();
        kin::u64 triangles = 0;
        kin::u32 levels_in_use = 0;
        for (kin::u32 l = 0; l < 4; ++l) {
            assert(stats.lod_copies_by_level[l] == by_level[l]);
            triangles += static_cast<kin::u64>(by_level[l]) * mesh.level_triangles(l);
            levels_in_use += by_level[l] > 0 ? 1 : 0;
        }
        std::cerr << "lod: 400 copies by level " << by_level[0] << " " << by_level[1] << " " << by_level[2] << " " << by_level[3]
                  << ": " << stats.draws << " draws, " << stats.triangles << " triangles\n";
        assert(levels_in_use >= 3 && stats.draws == levels_in_use && stats.triangles == triangles && stats.instances == 400);
        renderer.present();
    });
}

// Levels off, or on but not asked for, or a mesh without levels: the picture, the draws and the triangles are those of a
// mesh as it always was, to the bit.
void test_gpu_lod_off_is_unchanged() {
    with_gpu("test_gpu_lod_off_is_unchanged", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.levels = 4;
        const MeshData data = kin::meshes::torus(1.0f, 0.35f, 40, 16);
        const kin::Mesh with = r3d.create_mesh(data, options), without = r3d.create_mesh(data);
        const kin::Mesh floor = make_box(r3d, {30.0f, 0.2f, 30.0f});
        kin::View3D view = r3d.create_view({160, 120});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 6.0f, 18.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.lights.shadow_area = {{-15.0f, -0.2f, -15.0f}, {15.0f, 6.0f, 15.0f}};
        scene.lights.shadow_cascades = 3;
        scene.ambient_occlusion.enabled = true;
        const auto picture = [&](const kin::Mesh& m) {
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            std::vector<kin::MeshInstance> copies;
            for (int i = 0; i < 30; ++i) {
                copies.push_back({Mat4f::translation({static_cast<float>(i % 6) * 4.0f - 10.0f, 1.5f, static_cast<float>(i / 6) * -5.0f + 6.0f}) *
                                  Mat4f::rotation_y(static_cast<float>(i) * 20.0f)});
            }
            scene.draw_instanced(m, copies, {.color = Color::rgb(220, 120, 60)});
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const Pixels old_picture = picture(without);
        const kin::Render3DStats old_stats = r3d.stats();
        assert(old_stats.triangles > 0 && old_stats.draws == 2 && old_stats.lod_copies_by_level[0] == 31);
        // A mesh with levels, the setting off (its other numbers set): the same.
        scene.lod = {.enabled = false, .error_pixels = 8.0f, .bias = 0.1f, .force_level = 2};
        const Pixels off_picture = picture(with);
        assert(off_picture.rgba == old_picture.rgba);
        assert(r3d.stats().triangles == old_stats.triangles && r3d.stats().draws == old_stats.draws &&
               r3d.stats().shadow_draws == old_stats.shadow_draws && r3d.stats().shadow_casters == old_stats.shadow_casters &&
               r3d.stats().ao_draws == old_stats.ao_draws && r3d.stats().instances == old_stats.instances);
        // On, but with level 0 forced: the same again.
        scene.lod = {.enabled = true, .force_level = 0};
        assert(picture(with).rgba == old_picture.rgba && r3d.stats().triangles == old_stats.triangles);
        // On, a mesh without levels: the same.
        scene.lod = {.enabled = true, .error_pixels = 8.0f};
        assert(picture(without).rgba == old_picture.rgba && r3d.stats().triangles == old_stats.triangles);
        // And on, with an error bound that takes levels: fewer triangles, still a picture.
        const Pixels lod_picture = picture(with);
        assert(r3d.stats().triangles < old_stats.triangles && lod_picture.rgba != old_picture.rgba);
    });
}

// A copy drawn at a level looks as a mesh made of that level does, to the bit: the colours and the depth pre-pass, the
// shadows (one map and cascades) and the occlusion all use the one level, so the shadow matches the surface that casts it.
void test_gpu_lod_same_level_everywhere() {
    with_gpu("test_gpu_lod_same_level_everywhere", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.levels = 4;
        options.target_ratio = 0.3f; // coarse levels: the stand-ins differ clearly from the finest
        const MeshData data = kin::meshes::sphere(1.0f, 48, 24);
        const kin::MeshLodChain chain = kin::build_mesh_levels(data, options);
        assert(chain.count() >= 3);
        const kin::Mesh lod = r3d.create_mesh(data, options);
        std::vector<kin::Mesh> stand_in;
        for (std::size_t l = 0; l < chain.count(); ++l) {
            stand_in.push_back(r3d.create_mesh(level_as_mesh(data, chain, l)));
        }
        const kin::Mesh floor = make_box(r3d, {60.0f, 0.2f, 60.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 5.0f, 24.0f}, {0.0f, 1.0f, 0.0f}, 60.0f);
        scene.lights.sun.direction = {-0.5f, 1.0f, 0.3f};
        scene.lights.shadow_area = {{-30.0f, -0.2f, -30.0f}, {30.0f, 8.0f, 30.0f}};
        // Spheres at several distances, so that several levels are in use, over the floor.
        const std::vector<Vec3f> spots{{-1.5f, 1.5f, 20.5f}, {-4.0f, 1.5f, 6.0f}, {5.0f, 1.5f, -14.0f}, {-8.0f, 1.5f, -30.0f}};
        for (const kin::u32 cascades : {1u, 3u}) {
            for (const bool occlusion : {false, true}) {
                scene.lights.shadow_cascades = cascades;
                scene.ambient_occlusion.enabled = occlusion;
                scene.contact_shadows.enabled = occlusion;
                scene.lod = {.enabled = true, .error_pixels = 0.5f};
                scene.clear();
                scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
                std::vector<kin::MeshInstance> copies;
                for (const Vec3f& at : spots) {
                    copies.push_back({Mat4f::translation(at)});
                }
                scene.draw_instanced(lod, copies, {.color = Color::rgb(220, 120, 60)});
                const Pixels lod_picture = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                const kin::Render3DStats lod_stats = r3d.stats();
                // The levels each copy should have, and a scene made with their stand-ins.
                scene.lod = {};
                scene.clear();
                scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
                std::array<kin::u32, kin::max_mesh_levels> used{};
                kin::u64 triangles = 0;
                for (const Vec3f& at : spots) {
                    const kin::u32 level = expected_level(lod, scene.camera, 120.0f, at, 1.0f, 0.5f);
                    ++used[level];
                    triangles += lod.level_triangles(level);
                    scene.draw(stand_in[level], Mat4f::translation(at), {.color = Color::rgb(220, 120, 60)});
                }
                const Pixels reference = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                kin::u32 levels_in_use = 0;
                for (kin::u32 l = 0; l < kin::max_mesh_levels; ++l) {
                    const kin::u32 want = used[l] + (l == 0 ? 1 : 0); // the floor, a mesh without levels, is level 0
                    if (lod_stats.lod_copies_by_level[l] != want) {
                        std::cerr << "lod: level " << l << ": drawn " << lod_stats.lod_copies_by_level[l] << ", expected " << want << " (errors "
                                  << lod.level_error(1) << " " << lod.level_error(2) << " " << lod.level_error(lod.level_count() - 1) << ")\n";
                    }
                    assert(lod_stats.lod_copies_by_level[l] == want);
                    levels_in_use += used[l] > 0 ? 1 : 0;
                }
                std::cerr << "lod: " << cascades << " cascades" << (occlusion ? ", occlusion and contact shadows" : "") << ": levels in use " << levels_in_use
                          << ", triangles " << lod_stats.triangles << " (full detail " << 4 * data.triangle_count() << ")\n";
                assert(levels_in_use >= 3 && lod_stats.triangles == triangles + 12);
                // The shadow casters: the same number as the stand-in scene's, so the same copies, and the picture is the same one.
                assert(lod_stats.shadow_casters == r3d.stats().shadow_casters);
                if (lod_picture.rgba != reference.rgba) {
                    int different = 0, worst = 0;
                    for (std::size_t i = 0; i < lod_picture.rgba.size(); ++i) {
                        different += lod_picture.rgba[i] != reference.rgba[i] ? 1 : 0;
                        worst = std::max(worst, std::abs(static_cast<int>(lod_picture.rgba[i]) - static_cast<int>(reference.rgba[i])));
                    }
                    std::cerr << "lod: the picture differs from the stand-ins' in " << different << " bytes, by up to " << worst << " levels\n";
                }
                assert(lod_picture.rgba == reference.rgba);
            }
        }
    });
}

// With a level far from the finest, the picture is a small distance from the full-detail one (the levels' errors are
// under the bound on the screen) while the triangles fall.
void test_gpu_lod_picture_bound() {
    with_gpu("test_gpu_lod_picture_bound", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.levels = 4;
        const MeshData data = kin::meshes::sphere(1.0f, 64, 32);
        const kin::Mesh lod = r3d.create_mesh(data, options);
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 4});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 16.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.lights.sun.direction = {-0.5f, 0.8f, 0.6f};
        const auto picture = [&](bool on) {
            scene.lod = {.enabled = on};
            scene.clear();
            std::vector<kin::MeshInstance> copies;
            for (int i = 0; i < 12; ++i) {
                const float a = static_cast<float>(i) * 0.9f;
                copies.push_back({Mat4f::translation({std::cos(a) * 2.5f * (1.0f + 0.1f * static_cast<float>(i)), std::sin(a * 1.3f) * 1.5f,
                                                      static_cast<float>(i) * -6.0f}), Color::rgb(220, 120, 60)});
            }
            scene.draw_instanced(lod, copies);
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const Pixels full = picture(false);
        const kin::u64 full_triangles = r3d.stats().triangles;
        const Pixels reduced = picture(true);
        const kin::u64 reduced_triangles = r3d.stats().triangles;
        double sum = 0.0;
        int worst = 0, over_16 = 0;
        for (std::size_t i = 0; i < full.rgba.size(); i += 4) {
            int d = 0;
            for (int c = 0; c < 3; ++c) {
                d = std::max(d, std::abs(static_cast<int>(full.rgba[i + static_cast<std::size_t>(c)]) - reduced.rgba[i + static_cast<std::size_t>(c)]));
            }
            sum += d;
            worst = std::max(worst, d);
            over_16 += d > 16 ? 1 : 0;
        }
        const double mean = sum / (160.0 * 120.0);
        std::cerr << "lod: far spheres at 1 px: triangles " << reduced_triangles << " of " << full_triangles << ", picture mean difference " << mean
                  << " levels, worst " << worst << ", pixels over 16: " << over_16 << '\n';
        assert(reduced_triangles * 100 < full_triangles * 60); // markedly fewer
        assert(full.rgba != reduced.rgba);                     // it did change something
        assert(mean < 0.35 && over_16 < 80);
    });
}

// The pick pass draws the level the camera sees: the pixels just inside and just outside a coarse sphere's polygon
// answer as they do for a mesh made of that level.
void test_gpu_lod_pick() {
    with_gpu("test_gpu_lod_pick", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.levels = 4;
        options.target_ratio = 0.25f;
        const MeshData data = kin::meshes::sphere(1.0f, 32, 16);
        const kin::MeshLodChain chain = kin::build_mesh_levels(data, options);
        assert(chain.count() >= 3);
        const kin::Mesh lod = r3d.create_mesh(data, options);
        const kin::Mesh coarse = r3d.create_mesh(level_as_mesh(data, chain, chain.count() - 1));
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .pick = true});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 60.0f);
        scene.lod = {.enabled = true, .force_level = static_cast<kin::i32>(chain.count() - 1)};
        scene.draw(lod, Mat4f::translation({0.0f, 0.0f, -3.0f}), {}, kin::colors::white, 7);
        render_ok(r3d, view, scene);
        scene.lod = {};
        std::vector<bool> hits;
        for (int step = 0; step < 72; ++step) {
            const float a = static_cast<float>(step) * 3.14159265f / 36.0f;
            const kin::Vec2i pixel{80 + static_cast<int>(std::lround(std::cos(a) * 34.0f)), 60 + static_cast<int>(std::lround(std::sin(a) * 34.0f))};
            hits.push_back(r3d.pick(view, pixel).hit);
        }
        // The same pixels against the stand-in, and against the finest level, which they must differ from somewhere on the ring.
        scene.clear();
        scene.draw(coarse, Mat4f::translation({0.0f, 0.0f, -3.0f}), {}, kin::colors::white, 7);
        render_ok(r3d, view, scene);
        int different_from_stand_in = 0;
        std::vector<bool> stand_in_hits;
        for (int step = 0; step < 72; ++step) {
            const float a = static_cast<float>(step) * 3.14159265f / 36.0f;
            const kin::Vec2i pixel{80 + static_cast<int>(std::lround(std::cos(a) * 34.0f)), 60 + static_cast<int>(std::lround(std::sin(a) * 34.0f))};
            stand_in_hits.push_back(r3d.pick(view, pixel).hit);
            different_from_stand_in += stand_in_hits.back() != hits[static_cast<std::size_t>(step)] ? 1 : 0;
        }
        scene.clear();
        scene.draw(r3d.create_mesh(data), Mat4f::translation({0.0f, 0.0f, -3.0f}), {}, kin::colors::white, 7);
        render_ok(r3d, view, scene);
        int different_from_finest = 0;
        for (int step = 0; step < 72; ++step) {
            const float a = static_cast<float>(step) * 3.14159265f / 36.0f;
            const kin::Vec2i pixel{80 + static_cast<int>(std::lround(std::cos(a) * 34.0f)), 60 + static_cast<int>(std::lround(std::sin(a) * 34.0f))};
            different_from_finest += r3d.pick(view, pixel).hit != hits[static_cast<std::size_t>(step)] ? 1 : 0;
        }
        std::cerr << "lod: pick ring of 72 pixels: " << different_from_stand_in << " differ from the stand-in's level, " << different_from_finest
                  << " from the finest\n";
        assert(different_from_stand_in == 0 && different_from_finest > 0);
        renderer.present();
    });
}

// The cull and the fill on workers give the picture the serial ones do, with levels in use, shadows, cascades,
// occlusion and contact shadows all on; and the level statistics agree.
void test_gpu_lod_on_workers() {
    with_gpu("test_gpu_lod_on_workers", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.levels = 4;
        const kin::Mesh ring = r3d.create_mesh(kin::meshes::torus(0.5f, 0.15f, 32, 12), options);
        const kin::Mesh ball = r3d.create_mesh(kin::meshes::sphere(0.5f, 32, 16), options);
        const kin::Mesh floor = make_box(r3d, {400.0f, 0.2f, 400.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 6.0f, 30.0f}, {0.0f, 0.0f, -40.0f}, 60.0f);
        scene.lights.shadow_area = {{-200.0f, -0.2f, -200.0f}, {200.0f, 8.0f, 200.0f}};
        scene.lights.shadow_cascades = 3;
        scene.lights.shadow_distance = 150.0f;
        scene.ambient_occlusion.enabled = true;
        scene.contact_shadows.enabled = true;
        scene.lod = {.enabled = true, .error_pixels = 0.4f};
        const auto build = [&] {
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            kin::u32 seed = 3;
            const auto next = [&] {
                seed = seed * 1664525u + 1013904223u;
                return static_cast<float>(seed >> 8) / 16777216.0f;
            };
            // Two big draws, in a mix of distances (some behind the camera), and some small ones between.
            auto rings = scene.add_instances(ring, 5000, {.color = Color::rgb(220, 120, 60)});
            for (auto& r : rings) {
                r = {Mat4f::translation({(next() - 0.5f) * 120.0f, 0.5f + next() * 2.0f, 25.0f - next() * 85.0f}), Color::rgb(255, 255, 255)};
            }
            for (int i = 0; i < 40; ++i) {
                scene.draw(i % 2 ? ring : ball, Mat4f::translation({(next() - 0.5f) * 40.0f, 1.0f, 15.0f - next() * 40.0f}), {.color = Color::rgb(60, 160, 220)});
            }
            auto balls = scene.add_instances(ball, 3000, {.color = Color::rgb(100, 220, 100)});
            for (auto& b : balls) {
                b = {Mat4f::translation({(next() - 0.5f) * 120.0f, 0.5f, 25.0f - next() * 85.0f}) * Mat4f::scaling({1.0f + next(), 1.0f + next(), 1.0f + next()}),
                     Color::rgb(255, 255, 255)};
            }
        };
        build();
        const Pixels serial = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const kin::Render3DStats serial_stats = r3d.stats();
        kin::JobSystem jobs;
        renderer.set_job_system(&jobs);
        build();
        const Pixels parallel = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const kin::Render3DStats parallel_stats = r3d.stats();
        renderer.set_job_system(nullptr);
        kin::u32 total = 0, levels_in_use = 0;
        for (kin::u32 l = 0; l < kin::max_mesh_levels; ++l) {
            assert(serial_stats.lod_copies_by_level[l] == parallel_stats.lod_copies_by_level[l]);
            total += serial_stats.lod_copies_by_level[l];
            levels_in_use += serial_stats.lod_copies_by_level[l] > 0 ? 1 : 0;
        }
        std::cerr << "lod on workers: " << total << " copies seen by level " << serial_stats.lod_copies_by_level[0] << " "
                  << serial_stats.lod_copies_by_level[1] << " " << serial_stats.lod_copies_by_level[2] << " " << serial_stats.lod_copies_by_level[3]
                  << ", " << serial_stats.draws << " draws, " << serial_stats.shadow_draws << " shadow draws\n";
        assert(total == serial_stats.instances && levels_in_use >= 3);
        assert(serial_stats.draws == parallel_stats.draws && serial_stats.triangles == parallel_stats.triangles &&
               serial_stats.shadow_casters == parallel_stats.shadow_casters && serial_stats.shadow_draws == parallel_stats.shadow_draws &&
               serial_stats.ao_draws == parallel_stats.ao_draws && serial_stats.culled == parallel_stats.culled);
        assert(serial.rgba == parallel.rgba);
    });
}

// Temporal anti-aliasing with a copy that changes level: copies are matched from one frame to the next by mesh and
// place, not by level, so a copy that crosses a threshold (and moves to another draw's run, even another place in the
// instance buffer) keeps its velocity and its history, and leaves no trail.
void test_gpu_lod_taa() {
    with_gpu("test_gpu_lod_taa", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.levels = 4;
        const kin::Mesh ball = r3d.create_mesh(kin::meshes::sphere(1.0f, 32, 16), options);
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 60.0f);
        scene.lod = {.enabled = true, .error_pixels = 0.5f};
        // Textured with a checker (the world's pattern), so that a history laid on the wrong place smears the inside of a copy too.
        const PatternSurface pattern = make_pattern_surface(r3d, renderer);
        const kin::Material3D white{.color = Color::rgb(255, 255, 255), .unlit = true, .texture = pattern.texture, .wrap_u = kin::Wrap3D::Repeat,
                                    .wrap_v = kin::Wrap3D::Repeat};
        kin::View3D truth_view = r3d.create_view({160, 120}, {.samples = 8, .background = Color::rgb(0, 0, 0)});
        double error_sum = 0.0;
        int error_frames = 0;
        const float focal = 0.5f * 120.0f / std::tan(30.0f * 3.14159265f / 180.0f);
        // The first copy recedes from a distance where it is drawn coarsely to one where it is coarser (and crosses a threshold
        // on the way), sliding across; the second, drawn after it, stays near, at the finest level, sliding the other way.
        const float near_scale = 0.5f;
        const auto pose = [&](int f, Vec3f& far_at, Vec3f& near_at) {
            far_at = {-5.0f + 0.2f * static_cast<float>(f), 2.0f, -14.0f - 1.3f * static_cast<float>(f)};
            near_at = {2.0f - 0.1f * static_cast<float>(f), -2.0f, -6.0f};
        };
        bool changed = false;
        kin::u32 far_before = ~0u;
        double worst_trail = 0.0;
        int frames_with_history = 0, differing = 0;
        for (int f = 0; f < 60; ++f) {
            Vec3f far_at, near_at;
            pose(f, far_at, near_at);
            scene.clear();
            scene.taa = {.enabled = true};
            scene.draw(ball, Mat4f::translation(far_at), white);
            scene.draw(ball, Mat4f::translation(near_at) * Mat4f::scaling({near_scale, near_scale, near_scale}), white);
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
                return;
            }
            assert(r3d.stats().taa_dispatches > 0);
            frames_with_history += r3d.stats().taa_reset ? 0 : 1;
            const kin::u32 level_far = expected_level(ball, scene.camera, 120.0f, far_at, 1.0f, 0.5f);
            const kin::u32 level_near = expected_level(ball, scene.camera, 120.0f, near_at, near_scale, 0.5f);
            differing += level_far != level_near ? 1 : 0;
            assert(r3d.stats().lod_copies_by_level[level_far] >= 1 && r3d.stats().lod_copies_by_level[level_near] >= 1);
            if (far_before != ~0u && level_far != far_before) {
                changed = true;
                std::cerr << "lod taa: the far copy goes from level " << far_before << " to " << level_far << " at frame " << f << '\n';
            }
            far_before = level_far;
            if (f >= 40) {
                // Against the same frame drawn without temporal anti-aliasing at 8 samples a pixel.
                const kin::Camera3D camera = scene.camera;
                scene.taa = {};
                const Pixels truth = shoot_view(renderer, r3d, truth_view, scene);
                renderer.present();
                double e = 0.0;
                for (int i = 0; i < 160 * 120; ++i) {
                    const double d = level(p.at(i % 160, i / 160)) - level(truth.at(i % 160, i / 160));
                    e += d * d;
                }
                error_sum += e / (160.0 * 120.0);
                ++error_frames;
                scene.camera = camera;
            }
            // The trail: the pixels the far copy left behind it (in its row), a few pixels clear of its edge.
            if (f >= 8) {
                const auto screen_at = [&](Vec3f at) { return scene.camera.project(at, screen)->pos; };
                const kin::Vec2f centre = screen_at(far_at);
                const float radius_px = focal / -far_at.z;
                double got = 0.0;
                int count = 0;
                for (int back = 6; back < 20; ++back) {
                    const int x = static_cast<int>(centre.x - radius_px) - back; // it moves right
                    if (x >= 0 && x < 160) {
                        for (int dy = -1; dy <= 1; ++dy) {
                            got += level(p.at(x, static_cast<int>(centre.y) + dy));
                            ++count;
                        }
                    }
                }
                if (count > 0) {
                    worst_trail = std::max(worst_trail, got / count);
                }
                // And the near copy's, moving left.
                const kin::Vec2f centre_near = screen_at(near_at);
                const float radius_near = focal * near_scale / -near_at.z;
                got = 0.0;
                count = 0;
                for (int back = 6; back < 20; ++back) {
                    const int x = static_cast<int>(centre_near.x + radius_near) + back;
                    if (x >= 0 && x < 160) {
                        for (int dy = -1; dy <= 1; ++dy) {
                            got += level(p.at(x, static_cast<int>(centre_near.y) + dy));
                            ++count;
                        }
                    }
                }
                if (count > 0) {
                    worst_trail = std::max(worst_trail, got / count);
                }
            }
        }
        std::cerr << "lod taa: worst trail behind a moving copy " << worst_trail << " levels, " << frames_with_history << " frames with history\n";
        std::cerr << "lod taa: mean square error against 8 samples over the last 20 frames " << error_sum / error_frames << '\n';
        assert(error_sum / error_frames < 145.0);
        assert(changed && differing >= 40); // the two copies are at different levels, and the far one changes its
        assert(frames_with_history == 59);
        assert(worst_trail < 10.0);
    });
}


// --- Clusters (G3b) ---

// The cut of clustered meshes on the GPU (with its counts read back, which waits for the GPU) or on the CPU: the tests
// of G3b run on both, and must find the same.
kin::Lod3D cut_on(bool gpu, kin::Lod3D lod) {
    lod.gpu_culling = gpu;
    lod.stats_readback = gpu;
    return lod;
}

// How the renderer reads Scene3D::lod for a perspective camera over a view `height` pixels tall, as a cut of `copy`'s
// hierarchy (a copy placed by `transform`): the setting's description, written out here.
kin::ClusterCutView cluster_cut_view(const kin::Camera3D& camera, float height, const Mat4f& transform, float pixels, int force = -1) {
    kin::ClusterCutView v;
    v.transform = transform;
    v.eye = camera.position;
    v.forward = camera.forward();
    v.per_unit = 0.5f * height / std::tan(0.5f * camera.fov * 3.14159265358979f / 180.0f);
    v.pixels = pixels;
    v.nearest = camera.z_near;
    v.force_depth = force;
    return v;
}

// The mesh made of what a cut chose: the hierarchy's own vertices, the chosen clusters' triangles. A copy drawn from the
// cut must look as this mesh does, whatever the pass.
MeshData cut_as_mesh(const kin::MeshClusters& hierarchy, const std::vector<kin::SelectedCluster>& cut) {
    MeshData out;
    out.vertices = hierarchy.vertices;
    for (const kin::SelectedCluster& s : cut) {
        const std::size_t at = out.indices.size();
        out.indices.resize(at + static_cast<std::size_t>(hierarchy.clusters[s.cluster].triangle_count) * 3);
        hierarchy.append_indices(s.cluster, out.indices.data() + at);
    }
    return out;
}

kin::u64 cut_triangles(const kin::MeshClusters& hierarchy, const std::vector<kin::SelectedCluster>& cut) {
    kin::u64 n = 0;
    for (const kin::SelectedCluster& s : cut) {
        n += hierarchy.clusters[s.cluster].triangle_count;
    }
    return n;
}

// A big smooth surface of `triangles` about, wavy, over a few hundred units: the camera can be near one part and far
// from another.
MeshData big_hill(int cells) {
    MeshData m = kin::meshes::plane({160.0f, 160.0f}, cells);
    for (kin::MeshVertex& v : m.vertices) {
        v.position.y = 3.0f * std::sin(v.position.x * 0.07f) * std::cos(v.position.z * 0.05f);
    }
    // Smooth normals for the new height: from the grid's neighbours.
    for (kin::MeshVertex& v : m.vertices) {
        const float dx = 3.0f * 0.07f * std::cos(v.position.x * 0.07f) * std::cos(v.position.z * 0.05f);
        const float dz = -3.0f * 0.05f * std::sin(v.position.x * 0.07f) * std::sin(v.position.z * 0.05f);
        v.normal = kin::normalize(Vec3f{-dx, 1.0f, -dz});
    }
    return m;
}

struct Difference {
    double mean = 0.0;
    int worst = 0;
    int bytes = 0;
};

Difference difference_of(const Pixels& a, const Pixels& b) {
    assert(a.rgba.size() == b.rgba.size());
    Difference d;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        if (i % 4 == 3) {
            continue;
        }
        const int e = std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]));
        sum += e;
        d.worst = std::max(d.worst, e);
        d.bytes += e > 0 ? 1 : 0;
    }
    d.mean = sum / static_cast<double>(a.rgba.size() / 4 * 3);
    return d;
}

// Meshes with clusters report them; a mesh without behaves as before; with the selection off a clustered mesh is
// drawn whole, as the same picture and counts as the mesh made without clusters (a smooth one to the bit: it keeps its
// normals; a flat one makes them in the shader, which differs from the true flat normal by float rounding only); the
// cut forced to depth 0 is the whole mesh again, drawn through the compacted indices.
void test_gpu_clusters_off_is_unchanged(bool gpu) {
    with_gpu(gpu ? "test_gpu_clusters_off_is_unchanged (GPU cut)" : "test_gpu_clusters_off_is_unchanged (CPU cut)", [&](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.clusters = true;
        const kin::MeshData smooth_data = kin::meshes::torus(1.0f, 0.35f, 48, 24);
        const kin::MeshData flat_data = kin::meshes::plane({6.0f, 6.0f}, 40);
        for (const bool flat : {false, true}) {
            const MeshData& data = flat ? flat_data : smooth_data;
            const kin::Mesh plain = r3d.create_mesh(data), clustered = r3d.create_mesh(data, options);
            assert(plain.cluster_count() == 0 && !plain.has_clusters() && plain.cluster_levels() == 0 && plain.cluster_triangles() == 0 &&
                   plain.leaf_triangles() == plain.triangle_count() && !plain.flat_shaded());
            assert(clustered.has_clusters() && clustered.cluster_count() > 4 && clustered.cluster_levels() >= 3 && clustered.cluster_roots() >= 1);
            assert(clustered.leaf_triangles() == data.triangle_count() && clustered.cluster_triangles() > clustered.leaf_triangles());
            assert(clustered.cluster_triangles() < 3 * clustered.leaf_triangles() && clustered.flat_shaded() == flat);
            assert(clustered.triangle_count() == data.triangle_count() && clustered.level_count() == 1);
            // Levels are ignored for a mesh with clusters.
            kin::MeshOptions both = options;
            both.levels = 4;
            assert(r3d.create_mesh(data, both).level_count() == 1);
            const kin::Mesh floor = make_box(r3d, {20.0f, 0.2f, 20.0f});
            kin::View3D view = r3d.create_view({160, 120});
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 5.0f, 9.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
            scene.lights.shadow_area = {{-10.0f, -0.2f, -10.0f}, {10.0f, 6.0f, 10.0f}};
            scene.lights.shadow_cascades = 3;
            scene.ambient_occlusion.enabled = true;
            const auto picture = [&](const kin::Mesh& m) {
                scene.clear();
                scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
                scene.draw(m, flat ? Mat4f::translation({0.0f, 0.5f, 0.0f}) * Mat4f::rotation_x(-30.0f) : Mat4f::translation({0.0f, 1.2f, 0.0f}),
                           {.color = Color::rgb(220, 120, 60)});
                const Pixels p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                return p;
            };
            const Pixels old_picture = picture(plain);
            const kin::Render3DStats old_stats = r3d.stats();
            scene.lod = cut_on(gpu, kin::Lod3D{.enabled = false, .error_pixels = 8.0f, .force_level = 2});
            const Pixels off_picture = picture(clustered);
            const Difference off = difference_of(off_picture, old_picture);
            std::cerr << "clusters off, " << (flat ? "flat" : "smooth") << ": " << off.bytes << " bytes differ, by up to " << off.worst << ", mean " << off.mean << "\n";
            if (flat) {
                assert(off.mean < 0.05 && off.worst <= 24);
            } else {
                assert(off.bytes == 0);
            }
            const kin::Render3DStats off_stats = r3d.stats();
            assert(off_stats.triangles == old_stats.triangles && off_stats.draws == old_stats.draws && off_stats.shadow_draws == old_stats.shadow_draws &&
                   off_stats.shadow_casters == old_stats.shadow_casters && off_stats.ao_draws == old_stats.ao_draws &&
                   off_stats.instances == old_stats.instances);
            assert(off_stats.cluster_copies == 0 && off_stats.cluster_upload_bytes == 0 && off_stats.clusters_tested == 0);
            // On, forced to the leaves: the same triangles, the same picture (to the flat normal), through the compacted indices.
            scene.lod = cut_on(gpu, kin::Lod3D{.enabled = true, .force_level = 0});
            const Pixels leaf_picture = picture(clustered);
            const Difference leaves = difference_of(leaf_picture, old_picture);
            std::cerr << "clusters forced to the leaves: " << leaves.bytes << " bytes differ, by up to " << leaves.worst << ", mean " << leaves.mean << "\n";
            assert(leaves.mean < 0.05 && leaves.worst <= (flat ? 24 : 0));
            assert(r3d.stats().triangles == old_stats.triangles && r3d.stats().cluster_copies == 1);
            assert(gpu || r3d.stats().cluster_upload_bytes >= data.triangle_count() * 12);
            // A mesh without clusters, the selection on: as before.
            scene.lod = cut_on(gpu, kin::Lod3D{.enabled = true, .error_pixels = 4.0f});
            assert(picture(plain).rgba == old_picture.rgba && r3d.stats().triangles == old_stats.triangles && r3d.stats().cluster_copies == 0);
            // An alpha-blended copy is not cut.
            scene.lod = cut_on(gpu, kin::Lod3D{.enabled = true, .error_pixels = 30.0f});
            scene.clear();
            scene.draw(clustered, Mat4f::translation({0.0f, 1.0f, 0.0f}), {.color = Color::rgba(220, 120, 60, 120), .blend = kin::Blend3D::Alpha});
            render_ok(r3d, view, scene);
            assert(r3d.stats().triangles == data.triangle_count() && r3d.stats().cluster_copies == 0);
            renderer.present();
        }
    });
}

// The cut: with the error bound in pixels, triangles near the camera stay fine and far ones are coarse inside one mesh;
// the triangles drawn are those of the cut the same camera makes of the same hierarchy on the CPU (copy culling off, so that
// nothing else removes any); the picture is a small distance from full detail while the triangles fall; forcing a depth
// draws that depth's cut; a copy's scale counts.
void test_gpu_clusters_cut(bool gpu) {
    with_gpu(gpu ? "test_gpu_clusters_cut (GPU cut)" : "test_gpu_clusters_cut (CPU cut)", [&](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.clusters = true;
        const MeshData data = big_hill(110);
        const kin::Mesh plain = r3d.create_mesh(data), hill = r3d.create_mesh(data, options);
        const kin::MeshClusters hierarchy = kin::build_mesh_clusters(data, options);
        assert(hill.cluster_count() == hierarchy.clusters.size() && hill.cluster_levels() == hierarchy.levels);
        kin::View3D view = r3d.create_view({320, 240}, {.samples = 4});
        kin::Scene3D scene;
        scene.cull = false;
        // Standing low at one end of the hill, looking along it: the end under the camera is near, the other 150 units away.
        scene.camera = kin::Camera3D::look_at({0.0f, 6.0f, 80.0f}, {0.0f, 2.0f, -80.0f}, 60.0f);
        scene.lights.sun.direction = {-0.5f, 0.8f, 0.6f};
        const Mat4f at = Mat4f::translation({0.0f, 0.0f, 0.0f});
        const auto draw = [&](const kin::Mesh& m) {
            scene.clear();
            scene.draw(m, at, {.color = Color::rgb(150, 190, 120)});
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        scene.lod = cut_on(gpu, kin::Lod3D{});
        const Pixels full = draw(plain);
        assert(r3d.stats().triangles == data.triangle_count());
        for (const float pixels : {0.5f, 1.0f, 2.0f, 4.0f}) {
            scene.lod = cut_on(gpu, kin::Lod3D{.enabled = true, .error_pixels = pixels});
            const Pixels cut_picture = draw(hill);
            const kin::Render3DStats stats = r3d.stats();
            const std::vector<kin::SelectedCluster> expected = kin::select_clusters(hierarchy, cluster_cut_view(scene.camera, 240.0f, at, pixels), kin::NoClusterCull{});
            const Difference d = difference_of(cut_picture, full);
            std::cerr << "clusters " << pixels << " px: " << stats.triangles << " triangles of " << data.triangle_count() << " (" << expected.size() << " clusters, "
                      << stats.clusters_tested << " tested), picture " << d.mean << " mean, up to " << d.worst << "\n";
            assert(stats.triangles == cut_triangles(hierarchy, expected) && stats.clusters_drawn == expected.size() && stats.cluster_copies == 1);
            assert((gpu || stats.cluster_upload_bytes >= stats.triangles * 12) && stats.draws == 1 && stats.instances == 1);
            assert(gpu ? stats.cluster_gpu && stats.clusters_tested == hierarchy.clusters.size()
                       : !stats.cluster_gpu && stats.clusters_tested >= expected.size() && stats.clusters_tested < hierarchy.clusters.size());
            assert(stats.triangles < data.triangle_count() / 2);
            assert(d.mean < 1.5 + pixels);
            if (pixels == 1.0f) {
                // Inside one mesh: the clusters near the camera are finer (lower depth) than the far ones.
                std::vector<std::pair<float, kin::u32>> by_distance;
                std::set<kin::u32> depths;
                for (const kin::SelectedCluster& s : expected) {
                    const kin::MeshCluster& c = hierarchy.clusters[s.cluster];
                    by_distance.push_back({kin::length(c.cull_center - scene.camera.position), c.depth});
                    depths.insert(c.depth);
                }
                std::sort(by_distance.begin(), by_distance.end());
                const std::size_t quarter = by_distance.size() / 4;
                double near_depth = 0.0, far_depth = 0.0;
                for (std::size_t i = 0; i < quarter; ++i) {
                    near_depth += by_distance[i].second;
                    far_depth += by_distance[by_distance.size() - 1 - i].second;
                }
                std::cerr << "clusters: " << depths.size() << " depths in use, nearest quarter's mean depth " << near_depth / static_cast<double>(quarter)
                          << ", farthest quarter's " << far_depth / static_cast<double>(quarter) << "\n";
                assert(depths.size() >= 4 && near_depth * 2.0 < far_depth);
            }
        }
        // A forced depth draws that depth's cut.
        for (const int depth : {0, 1, 3, 5}) {
            scene.lod = cut_on(gpu, kin::Lod3D{.enabled = true, .force_level = depth});
            draw(hill);
            const std::vector<kin::SelectedCluster> expected = kin::select_clusters(hierarchy, cluster_cut_view(scene.camera, 240.0f, at, 1.0f, depth), kin::NoClusterCull{});
            std::cerr << "clusters forced to depth " << depth << ": " << r3d.stats().triangles << " triangles\n";
            assert(r3d.stats().triangles == cut_triangles(hierarchy, expected) && r3d.stats().clusters_drawn == expected.size());
            assert(depth > 0 || r3d.stats().triangles == data.triangle_count());
        }
        // The copy's scale counts: twice as large, the same camera, and the error is twice as many pixels.
        scene.lod = cut_on(gpu, kin::Lod3D{.enabled = true, .error_pixels = 1.0f});
        scene.clear();
        scene.draw(hill, Mat4f::translation({0.0f, 0.0f, 0.0f}));
        render_ok(r3d, view, scene);
        const kin::u64 unit_triangles = r3d.stats().triangles;
        scene.clear();
        scene.draw(hill, Mat4f::translation({0.0f, 0.0f, 120.0f}) * Mat4f::scaling({0.5f, 0.5f, 0.5f}));
        scene.draw(hill, Mat4f::translation({0.0f, 0.0f, 0.0f}) * Mat4f::scaling({2.0f, 2.0f, 2.0f}));
        render_ok(r3d, view, scene);
        const kin::Mat4f big = Mat4f::translation({0.0f, 0.0f, 0.0f}) * Mat4f::scaling({2.0f, 2.0f, 2.0f});
        const kin::Mat4f small = Mat4f::translation({0.0f, 0.0f, 120.0f}) * Mat4f::scaling({0.5f, 0.5f, 0.5f});
        const kin::u64 expected_two = cut_triangles(hierarchy, kin::select_clusters(hierarchy, cluster_cut_view(scene.camera, 240.0f, big, 1.0f), kin::NoClusterCull{})) +
                                      cut_triangles(hierarchy, kin::select_clusters(hierarchy, cluster_cut_view(scene.camera, 240.0f, small, 1.0f), kin::NoClusterCull{}));
        assert(r3d.stats().triangles == expected_two && r3d.stats().cluster_copies == 2 && r3d.stats().draws == (gpu ? 1u : 2u));
        assert(unit_triangles > 0);
        renderer.present();
    });
}

// A copy drawn from the cut looks as a mesh made of that cut does, to the bit, whatever the pass: the colour, the
// pre-pass (occlusion, contact shadows), the shadow map (one, or cascades), culling of the copies on or off. The
// copies include a smooth and a flat-shaded mesh, and a scaled one.
void test_gpu_clusters_same_cut_everywhere(bool gpu) {
    with_gpu(gpu ? "test_gpu_clusters_same_cut_everywhere (GPU cut)" : "test_gpu_clusters_same_cut_everywhere (CPU cut)", [&](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.clusters = true;
        const MeshData hill_data = big_hill(80);
        const MeshData ball_data = kin::meshes::sphere(1.0f, 64, 32);
        const MeshData slab_data = kin::meshes::plane({30.0f, 30.0f}, 50);
        const kin::Mesh hill = r3d.create_mesh(hill_data, options), ball = r3d.create_mesh(ball_data, options), slab = r3d.create_mesh(slab_data, options);
        const kin::MeshClusters hill_h = kin::build_mesh_clusters(hill_data, options), ball_h = kin::build_mesh_clusters(ball_data, options),
                                slab_h = kin::build_mesh_clusters(slab_data, options);
        const kin::Mesh floor = make_box(r3d, {400.0f, 0.2f, 400.0f});
        kin::View3D view = r3d.create_view({200, 150}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 7.0f, 40.0f}, {0.0f, 1.0f, -30.0f}, 60.0f);
        scene.lights.sun.direction = {-0.5f, 1.0f, 0.3f};
        scene.lights.shadow_area = {{-80.0f, -0.2f, -80.0f}, {80.0f, 12.0f, 80.0f}};
        scene.lights.shadow_distance = 100.0f;
        struct Copy {
            const kin::Mesh* mesh;
            const kin::MeshClusters* hierarchy;
            Mat4f place;
            Color color;
        };
        const std::vector<Copy> copies{
            {&hill, &hill_h, Mat4f::translation({0.0f, 0.0f, -10.0f}), Color::rgb(150, 190, 120)},
            {&ball, &ball_h, Mat4f::translation({-3.0f, 2.0f, 26.0f}) * Mat4f::scaling({2.0f, 2.0f, 2.0f}), Color::rgb(220, 120, 60)},
            {&ball, &ball_h, Mat4f::translation({5.0f, 1.5f, 8.0f}), Color::rgb(60, 130, 220)},
            {&slab, &slab_h, Mat4f::translation({-12.0f, 3.0f, 12.0f}) * Mat4f::rotation_x(-20.0f), Color::rgb(200, 200, 90)},
        };
        for (const kin::u32 cascades : {1u, 3u}) {
            for (const bool passes : {false, true}) {
                for (const bool cull : {false, true}) {
                    scene.lights.shadow_cascades = cascades;
                    scene.ambient_occlusion.enabled = passes;
                    scene.contact_shadows.enabled = passes;
                    scene.cull = cull;
                    scene.lod = cut_on(gpu, kin::Lod3D{.enabled = true, .error_pixels = 0.7f});
                    scene.clear();
                    scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
                    for (const Copy& c : copies) {
                        scene.draw(*c.mesh, c.place, {.color = c.color});
                    }
                    const Pixels cut_picture = shoot_view(renderer, r3d, view, scene);
                    renderer.present();
                    const kin::Render3DStats stats = r3d.stats();
                    // The same scene from the meshes the cuts are.
                    scene.lod = cut_on(gpu, kin::Lod3D{});
                    scene.clear();
                    scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
                    kin::u64 triangles = 0;
                    for (const Copy& c : copies) {
                        const auto cut = kin::select_clusters(*c.hierarchy, cluster_cut_view(scene.camera, 150.0f, c.place, 0.7f), kin::NoClusterCull{});
                        triangles += cut_triangles(*c.hierarchy, cut);
                        scene.draw(r3d.create_mesh(cut_as_mesh(*c.hierarchy, cut)), c.place, {.color = c.color});
                    }
                    const Pixels stand_in = shoot_view(renderer, r3d, view, scene);
                    renderer.present();
                    const kin::Render3DStats reference = r3d.stats();
                    const Difference d = difference_of(cut_picture, stand_in);
                    std::cerr << "clusters: " << cascades << " cascades" << (passes ? ", occlusion and contact shadows" : "") << (cull ? "" : ", no copy culling")
                              << ": " << d.bytes << " bytes differ from the stand-in (up to " << d.worst << "), " << stats.triangles << " triangles (of "
                              << reference.triangles << "), shadow casters " << stats.shadow_casters << "/" << reference.shadow_casters << ", cut "
                              << stats.cluster_copies << " copies, " << stats.clusters_tested << " tested, " << stats.cluster_upload_bytes / 1024 << " KB\n";
                    assert(d.bytes == 0);
                    assert(stats.cluster_copies == copies.size() && stats.shadow_cascades == (cascades > 1 ? cascades : 1));
                    if (!cull) {
                        assert(stats.triangles == triangles + 12);
                    } else {
                        assert(stats.triangles <= triangles + 12);
                    }
                    // Culling clusters spares the shadow maps: never more casters drawn than the stand-in's.
                    assert(stats.ao_draws <= copies.size() + 1 && (passes ? stats.ao_draws > 0 : stats.ao_draws == 0));
                }
            }
        }
    });
}

// Pick sees the cut the pass draws: a ring of pixels over a copy forced to a coarse depth hits as the stand-in does,
// and differs from the full detail somewhere; ids and positions of what is hit match.
void test_gpu_clusters_pick(bool gpu) {
    with_gpu(gpu ? "test_gpu_clusters_pick (GPU cut)" : "test_gpu_clusters_pick (CPU cut)", [&](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.clusters = true;
        const MeshData data = kin::meshes::sphere(1.0f, 40, 20);
        const kin::Mesh clustered = r3d.create_mesh(data, options);
        const kin::MeshClusters hierarchy = kin::build_mesh_clusters(data, options);
        const int depth = static_cast<int>(hierarchy.levels) - 3;
        assert(depth >= 1);
        const Mat4f place = Mat4f::translation({0.0f, 0.0f, -3.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .pick = true});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 60.0f);
        const auto ring = [&](const std::function<void()>& set) {
            scene.clear();
            set();
            render_ok(r3d, view, scene);
            std::vector<kin::PickResult> results;
            for (int step = 0; step < 72; ++step) {
                const float a = static_cast<float>(step) * 3.14159265f / 36.0f;
                for (const float radius : {30.0f, 34.0f}) {
                    results.push_back(r3d.pick(view, {80 + static_cast<int>(std::lround(std::cos(a) * radius)), 60 + static_cast<int>(std::lround(std::sin(a) * radius))}));
                }
            }
            return results;
        };
        scene.lod = cut_on(gpu, kin::Lod3D{.enabled = true, .force_level = depth});
        const auto cut_hits = ring([&] { scene.draw(clustered, place, {}, kin::colors::white, 7); });
        assert(r3d.stats().cluster_copies == 1);
        scene.lod = cut_on(gpu, kin::Lod3D{});
        const auto cut = kin::select_clusters(hierarchy, cluster_cut_view(scene.camera, 120.0f, place, 1.0f, depth), kin::NoClusterCull{});
        const kin::Mesh stand_in = r3d.create_mesh(cut_as_mesh(hierarchy, cut));
        const auto stand_in_hits = ring([&] { scene.draw(stand_in, place, {}, kin::colors::white, 7); });
        const auto full_hits = ring([&] { scene.draw(r3d.create_mesh(data), place, {}, kin::colors::white, 7); });
        int different_from_stand_in = 0, different_from_full = 0, hits = 0;
        for (std::size_t i = 0; i < cut_hits.size(); ++i) {
            different_from_stand_in += cut_hits[i].hit != stand_in_hits[i].hit || (cut_hits[i].hit && cut_hits[i].id != stand_in_hits[i].id) ? 1 : 0;
            different_from_full += cut_hits[i].hit != full_hits[i].hit ? 1 : 0;
            hits += cut_hits[i].hit ? 1 : 0;
            if (cut_hits[i].hit && stand_in_hits[i].hit) {
                assert(kin::length(cut_hits[i].position - stand_in_hits[i].position) < 1e-4f);
            }
        }
        std::cerr << "clusters: pick ring of " << cut_hits.size() << " pixels (" << hits << " hit): " << different_from_stand_in
                  << " differ from the stand-in's cut, " << different_from_full << " from the full detail\n";
        assert(hits > 40 && different_from_stand_in == 0 && different_from_full > 0);
        renderer.present();
    });
}

// The cut and the compaction on workers give the picture and the numbers the serial ones do, with everything on.
void test_gpu_clusters_on_workers(bool gpu) {
    with_gpu(gpu ? "test_gpu_clusters_on_workers (GPU cut)" : "test_gpu_clusters_on_workers (CPU cut)", [&](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.clusters = true;
        const kin::Mesh hill = r3d.create_mesh(big_hill(70), options);
        const kin::Mesh ball = r3d.create_mesh(kin::meshes::sphere(1.0f, 48, 24), options);
        const kin::Mesh ring = r3d.create_mesh(kin::meshes::torus(1.0f, 0.3f, 48, 16), options);
        const kin::Mesh floor = make_box(r3d, {400.0f, 0.2f, 400.0f});
        kin::View3D view = r3d.create_view({200, 150}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 6.0f, 40.0f}, {0.0f, 1.0f, -40.0f}, 60.0f);
        scene.lights.shadow_area = {{-100.0f, -0.2f, -100.0f}, {100.0f, 12.0f, 100.0f}};
        scene.lights.shadow_cascades = 3;
        scene.lights.shadow_distance = 120.0f;
        scene.ambient_occlusion.enabled = true;
        scene.contact_shadows.enabled = true;
        scene.lod = cut_on(gpu, kin::Lod3D{.enabled = true, .error_pixels = 0.5f});
        const auto build = [&] {
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            scene.draw(hill, Mat4f::translation({0.0f, 0.0f, -20.0f}), {.color = Color::rgb(150, 190, 120)});
            kin::u32 seed = 5;
            const auto next = [&] {
                seed = seed * 1664525u + 1013904223u;
                return static_cast<float>(seed >> 8) / 16777216.0f;
            };
            for (int i = 0; i < 60; ++i) {
                scene.draw(i % 3 ? ball : ring, Mat4f::translation({(next() - 0.5f) * 70.0f, 1.0f + next(), 30.0f - next() * 80.0f}) * Mat4f::rotation_y(next() * 360.0f) *
                           Mat4f::scaling({0.5f + next() * 2.0f, 0.5f + next() * 2.0f, 0.5f + next() * 2.0f}), {.color = Color::rgb(60, 160, 220)});
            }
        };
        build();
        const Pixels serial = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const kin::Render3DStats a = r3d.stats();
        kin::JobSystem jobs;
        renderer.set_job_system(&jobs);
        build();
        const Pixels parallel = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const kin::Render3DStats b = r3d.stats();
        renderer.set_job_system(nullptr);
        std::cerr << "clusters on workers: " << a.cluster_copies << " copies cut, " << a.clusters_tested << " clusters tested, " << a.clusters_drawn << " drawn, "
                  << a.triangles << " triangles, " << a.cluster_upload_bytes / 1024 << " KB; cut " << a.cluster_ms << " ms serial, " << b.cluster_ms << " ms on workers\n";
        assert(a.cluster_copies > 20 && a.clusters_drawn > 100);
        assert(a.cluster_copies == b.cluster_copies && a.clusters_tested == b.clusters_tested && a.clusters_drawn == b.clusters_drawn &&
               a.cluster_upload_bytes == b.cluster_upload_bytes && a.triangles == b.triangles && a.draws == b.draws &&
               a.shadow_draws == b.shadow_draws && a.shadow_casters == b.shadow_casters && a.ao_draws == b.ao_draws && a.culled == b.culled);
        assert(serial.rgba == parallel.rgba);
    });
}

// Temporal anti-aliasing with clusters: the same pictures, frame after frame, as a scene drawn from stand-in meshes of
// the cuts; with a camera that moves (so that cuts change from frame to frame) it keeps working, close to the picture drawn
// at 8 samples a pixel without it.
void test_gpu_clusters_taa(bool gpu) {
    with_gpu(gpu ? "test_gpu_clusters_taa (GPU cut)" : "test_gpu_clusters_taa (CPU cut)", [&](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::MeshOptions options;
        options.clusters = true;
        const MeshData data = big_hill(70);
        const kin::Mesh hill = r3d.create_mesh(data, options);
        const kin::MeshClusters hierarchy = kin::build_mesh_clusters(data, options);
        const Mat4f place = Mat4f::translation({0.0f, 0.0f, 0.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        kin::View3D stand_view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        kin::View3D truth_view = r3d.create_view({160, 120}, {.samples = 8, .background = Color::rgb(0, 0, 0)});
        kin::Scene3D scene;
        scene.lights.sun.direction = {-0.5f, 0.8f, 0.6f};
        double error_sum = 0.0;
        int error_frames = 0, equal_frames = 0, frames_with_history = 0;
        kin::u64 smallest = ~0ull, largest = 0;
        kin::Mesh stand_in;
        for (int f = 0; f < 40; ++f) {
            // The camera walks along the hill, and the first twelve frames stand still (the stand-in must match each).
            const float z = f < 12 ? 70.0f : 70.0f - 1.2f * static_cast<float>(f - 12);
            scene.camera = kin::Camera3D::look_at({0.0f, 7.0f, z}, {0.0f, 2.0f, z - 60.0f}, 60.0f);
            scene.taa = {.enabled = true};
            scene.lod = cut_on(gpu, kin::Lod3D{.enabled = true, .error_pixels = 0.7f});
            scene.clear();
            scene.draw(hill, place, {.color = Color::rgb(150, 190, 120)});
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
                return;
            }
            const kin::Render3DStats stats = r3d.stats();
            assert(stats.taa_dispatches > 0 && stats.cluster_copies == 1);
            frames_with_history += stats.taa_reset ? 0 : 1;
            smallest = std::min(smallest, stats.triangles);
            largest = std::max(largest, stats.triangles);
            // The still frames again from the stand-in of the cut (one mesh kept: copies are matched across frames by mesh,
            // and its view's history follows the same frames).
            if (f < 12) {
                if (f == 0) {
                    const auto cut = kin::select_clusters(hierarchy, cluster_cut_view(scene.camera, 120.0f, place, 0.7f), kin::NoClusterCull{});
                    stand_in = r3d.create_mesh(cut_as_mesh(hierarchy, cut));
                }
                scene.lod = cut_on(gpu, kin::Lod3D{});
                scene.clear();
                scene.draw(stand_in, place, {.color = Color::rgb(150, 190, 120)});
                const Pixels stand = shoot_view(renderer, r3d, stand_view, scene);
                renderer.present();
                equal_frames += p.rgba == stand.rgba ? 1 : 0;
            }
            if (f >= 24) {
                scene.taa = {};
                scene.lod = cut_on(gpu, kin::Lod3D{.enabled = true, .error_pixels = 0.7f});
                const Pixels truth = shoot_view(renderer, r3d, truth_view, scene);
                renderer.present();
                double e = 0.0;
                for (int i = 0; i < 160 * 120; ++i) {
                    const double d = level(p.at(i % 160, i / 160)) - level(truth.at(i % 160, i / 160));
                    e += d * d;
                }
                error_sum += e / (160.0 * 120.0);
                ++error_frames;
            }
        }
        std::cerr << "clusters taa: " << equal_frames << " of 12 still frames equal the stand-in's, triangles " << smallest << " to " << largest
                  << ", mean square error against 8 samples " << error_sum / error_frames << "\n";
        assert(frames_with_history == 39 && equal_frames == 12 && smallest < largest);
        assert(error_sum / error_frames < 145.0);
    });
}


// --- Clusters cut on the GPU (G3c) ---

// The cut as the renderer's own camera sees it, orthographic or not, for a view `height` pixels tall.
kin::ClusterCutView cut_view_of(const kin::Camera3D& camera, float height, const Mat4f& transform, float pixels, int force = -1) {
    kin::ClusterCutView v = cluster_cut_view(camera, height, transform, pixels, force);
    if (camera.projection == kin::Projection3D::Orthographic) {
        v.orthographic = true;
        v.per_unit = height / camera.height;
    }
    return v;
}

struct Lcg {
    kin::u32 state = 12345;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }
};

// Edges used once, by position (the hierarchy is welded, so exact float bits do): the open ones of a set of triangles.
std::size_t open_edges_of(const kin::MeshClusters& hierarchy, const std::vector<kin::u32>& clusters) {
    using Key = std::array<kin::u32, 6>;
    std::map<Key, int> uses;
    std::vector<kin::u32> indices;
    const auto bits = [&](kin::u32 v) {
        const Vec3f p = hierarchy.vertices[v].position;
        return std::array<kin::u32, 3>{std::bit_cast<kin::u32>(p.x), std::bit_cast<kin::u32>(p.y), std::bit_cast<kin::u32>(p.z)};
    };
    for (const kin::u32 c : clusters) {
        indices.resize(static_cast<std::size_t>(hierarchy.clusters[c].triangle_count) * 3);
        hierarchy.append_indices(c, indices.data());
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            for (int e = 0; e < 3; ++e) {
                auto a = bits(indices[t + static_cast<std::size_t>(e)]), b = bits(indices[t + static_cast<std::size_t>((e + 1) % 3)]);
                if (b < a) {
                    std::swap(a, b);
                }
                ++uses[{a[0], a[1], a[2], b[0], b[1], b[2]}];
            }
        }
    }
    std::size_t open = 0;
    for (const auto& [key, n] : uses) {
        open += n == 1 ? 1u : 0u;
    }
    return open;
}

// The GPU's chosen clusters for the camera's passes, by copy (the scene's instance index), from the read-back.
std::map<kin::u32, std::set<kin::u32>> camera_picks(kin::Renderer3D& r3d, kin::View3D& view, const kin::Scene3D& scene, std::size_t* shadow_picks = nullptr,
                                                    bool may_be_none = false) {
    std::vector<kin::ClusterPick> picks;
    const bool ok = r3d.read_cluster_picks(view, picks);
    assert(ok || may_be_none); // false: no copy was cut at all
    static_cast<void>(ok);
    std::map<kin::u32, std::set<kin::u32>> by_copy;
    if (shadow_picks) {
        *shadow_picks = 0;
    }
    for (const kin::ClusterPick& p : picks) {
        assert(p.scene == 0);
        if (p.view == 0) {
            by_copy[scene.draws()[p.draw].first + p.copy].insert(p.cluster);
        } else if (shadow_picks) {
            ++*shadow_picks;
        }
    }
    return by_copy;
}

// The GPU's cut chooses the clusters the CPU's select_clusters does, for many cameras (perspective and orthographic,
// near, far and inside the mesh), bounds, transforms (scaled, turned) and forced depths, over a smooth mesh, a flat one,
// a torus and a soup. The two compute the same float expressions in the same order; they differ only where an expression
// lands within a rounding of its bound, which the count below measures. And whatever it chooses covers the surface
// once with no crack: the open edges are the mesh's own.
void test_gpu_cut_equals_cpu_selection() {
    with_gpu("test_gpu_cut_equals_cpu_selection", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().gpu_driven_clusters, "the cluster cut in compute")) {
            return;
        }
        kin::MeshOptions options;
        options.clusters = true;
        struct Subject {
            const char* name;
            MeshData data;
        };
        std::vector<Subject> subjects;
        subjects.push_back({"smooth hill", big_hill(160)});
        subjects.push_back({"flat sphere", kin::meshes::sphere(1.0f, 160, 80)});
        subjects.push_back({"torus", kin::meshes::torus(1.0f, 0.3f, 140, 60)});
        subjects.push_back({"flat plane", kin::meshes::plane({20.0f, 20.0f}, 150)});
        kin::View3D view = r3d.create_view({320, 240}, {.samples = 1});
        Lcg rng;
        std::size_t checked = 0, clusters_compared = 0, different = 0, worst_case = 0;
        for (const Subject& subject : subjects) {
            const kin::Mesh mesh = r3d.create_mesh(subject.data, options);
            const kin::MeshClusters hierarchy = kin::build_mesh_clusters(subject.data, options);
            const std::size_t own_open = open_edges_of(hierarchy, [&] {
                std::vector<kin::u32> leaves;
                for (kin::u32 c = 0; c < hierarchy.leaf_count; ++c) {
                    leaves.push_back(c);
                }
                return leaves;
            }());
            const float size = kin::length(subject.data.bounds().max - subject.data.bounds().min);
            for (int round = 0; round < 30; ++round) {
                kin::Scene3D scene;
                scene.cull = false;
                const Vec3f target{rng.range(-0.3f, 0.3f) * size, 0.0f, rng.range(-0.3f, 0.3f) * size};
                const float distance = size * (round % 5 == 0 ? rng.range(0.02f, 0.2f) : round % 5 == 1 ? rng.range(3.0f, 12.0f) : rng.range(0.4f, 2.5f));
                const float yaw = rng.range(0.0f, 6.28f), pitch = rng.range(-0.6f, 1.2f);
                const Vec3f eye = target + Vec3f{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)} * distance;
                scene.camera = kin::Camera3D::look_at(eye, target, rng.range(30.0f, 90.0f));
                if (round % 6 == 5) {
                    scene.camera.projection = kin::Projection3D::Orthographic;
                    scene.camera.height = size * rng.range(0.3f, 3.0f);
                }
                const float pixels = rng.range(0.25f, 6.0f);
                const int force = round % 7 == 6 ? static_cast<int>(rng.range(0.0f, static_cast<float>(hierarchy.levels) + 1.0f)) : -1;
                scene.lod = {.enabled = true, .error_pixels = pixels, .force_level = force, .stats_readback = true};
                const int copies = 1 + round % 3;
                for (int c = 0; c < copies; ++c) {
                    const float scale = rng.range(0.3f, 3.0f);
                    scene.draw(mesh, Mat4f::translation({rng.range(-0.5f, 0.5f) * size, rng.range(-0.2f, 0.2f) * size, rng.range(-0.5f, 0.5f) * size}) *
                                   Mat4f::rotation_y(rng.range(0.0f, 360.0f)) * Mat4f::rotation_x(rng.range(-40.0f, 40.0f)) *
                                   Mat4f::scaling({scale, scale * rng.range(0.8f, 1.2f), scale}),
                               {.color = Color::rgb(150, 190, 120)}, kin::colors::white, static_cast<kin::u32>(c));
                }
                render_ok(r3d, view, scene);
                const auto picks = camera_picks(r3d, view, scene);
                const kin::Render3DStats stats = r3d.stats();
                assert(stats.cluster_gpu && stats.cluster_copies == scene.instances().size());
                kin::u64 got_clusters = 0;
                for (std::size_t i = 0; i < scene.instances().size(); ++i) {
                    const auto expected = kin::select_clusters(hierarchy, cut_view_of(scene.camera, 240.0f, scene.instances()[i].transform, pixels, force), kin::NoClusterCull{});
                    std::set<kin::u32> want;
                    for (const kin::SelectedCluster& e : expected) {
                        want.insert(e.cluster);
                    }
                    const auto it = picks.find(static_cast<kin::u32>(i));
                    const std::set<kin::u32> got = it == picks.end() ? std::set<kin::u32>{} : it->second;
                    std::size_t only_gpu = 0, only_cpu = 0;
                    for (const kin::u32 c : got) {
                        only_gpu += want.count(c) ? 0u : 1u;
                    }
                    for (const kin::u32 c : want) {
                        only_cpu += got.count(c) ? 0u : 1u;
                    }
                    clusters_compared += want.size();
                    different += only_gpu + only_cpu;
                    worst_case = std::max(worst_case, only_gpu + only_cpu);
                    got_clusters += got.size();
                    ++checked;
                    if (only_gpu + only_cpu > 0) {
                        std::cerr << "  " << subject.name << " round " << round << " copy " << i << ": " << only_gpu << " only on the GPU, " << only_cpu << " only on the CPU (of " << want.size() << ")\n";
                    }
                    // No crack, whatever was chosen.
                    const std::vector<kin::u32> chosen(got.begin(), got.end());
                    assert(open_edges_of(hierarchy, chosen) == own_open);
                }
                assert(stats.clusters_drawn == got_clusters); // the counters and the read-back of the scan agree
            }
        }
        std::cerr << "gpu cut: " << checked << " copies' cuts compared (" << clusters_compared << " clusters), " << different << " clusters differ, worst copy " << worst_case << "\n";
        assert(checked > 150);
        // Borderline rounding only: a handful in tens of thousands; the open-edge assertion above holds for every one.
        assert(different * 2000 <= clusters_compared);
        renderer.present();
    });
}

// The picture the GPU's cut draws is the CPU cut's, to the bit, whatever the pass: with one, three and four cascades,
// occlusion and contact shadows on or off, culling of the copies on or off; and so are the counts, pick, and (with
// temporal anti-aliasing and a moving camera) every frame; and the serial and parallel set-ups of the scene agree.
void test_gpu_cut_picture_equals_cpu() {
    with_gpu("test_gpu_cut_picture_equals_cpu", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().gpu_driven_clusters, "the cluster cut in compute")) {
            return;
        }
        kin::MeshOptions options;
        options.clusters = true;
        const kin::Mesh hill = r3d.create_mesh(big_hill(80), options), ball = r3d.create_mesh(kin::meshes::sphere(1.0f, 64, 32), options),
                        slab = r3d.create_mesh(kin::meshes::plane({30.0f, 30.0f}, 50), options);
        const kin::Mesh floor = make_box(r3d, {400.0f, 0.2f, 400.0f});
        kin::View3D view = r3d.create_view({200, 150}, {.samples = 1, .pick = true});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 7.0f, 40.0f}, {0.0f, 1.0f, -30.0f}, 60.0f);
        scene.lights.sun.direction = {-0.5f, 1.0f, 0.3f};
        scene.lights.shadow_area = {{-80.0f, -0.2f, -80.0f}, {80.0f, 12.0f, 80.0f}};
        scene.lights.shadow_distance = 100.0f;
        const auto build = [&] {
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            scene.draw(hill, Mat4f::translation({0.0f, 0.0f, -10.0f}), {.color = Color::rgb(150, 190, 120)});
            scene.draw(ball, Mat4f::translation({-3.0f, 2.0f, 26.0f}) * Mat4f::scaling({2.0f, 2.0f, 2.0f}), {.color = Color::rgb(220, 120, 60)});
            scene.draw(ball, Mat4f::translation({5.0f, 1.5f, 8.0f}), {.color = Color::rgb(60, 130, 220)});
            scene.draw(ball, Mat4f::translation({-60.0f, 1.5f, 8.0f}), {.color = Color::rgb(60, 130, 220)}); // outside the view, its shadow too far
            scene.draw(slab, Mat4f::translation({-12.0f, 3.0f, 12.0f}) * Mat4f::rotation_x(-20.0f), {.color = Color::rgb(200, 200, 90)});
        };
        for (const kin::u32 cascades : {1u, 3u, 4u}) {
            for (const bool passes : {false, true}) {
                for (const bool cull : {false, true}) {
                    scene.lights.shadow_cascades = cascades;
                    scene.ambient_occlusion.enabled = passes;
                    scene.contact_shadows.enabled = passes;
                    scene.cull = cull;
                    build();
                    scene.lod = cut_on(false, {.enabled = true, .error_pixels = 0.7f});
                    const Pixels cpu = shoot_view(renderer, r3d, view, scene);
                    renderer.present();
                    const kin::Render3DStats on_cpu = r3d.stats();
                    scene.lod = cut_on(true, {.enabled = true, .error_pixels = 0.7f});
                    const Pixels gpu = shoot_view(renderer, r3d, view, scene);
                    renderer.present();
                    const kin::Render3DStats on_gpu = r3d.stats();
                    const Difference d = difference_of(gpu, cpu);
                    std::cerr << "gpu cut: " << cascades << " cascades" << (passes ? ", occlusion and contact shadows" : "") << (cull ? "" : ", no copy culling") << ": "
                              << d.bytes << " bytes differ from the CPU's cut (up to " << d.worst << "), " << on_gpu.triangles << " / " << on_cpu.triangles << " triangles, "
                              << on_gpu.clusters_drawn << " / " << on_cpu.clusters_drawn << " clusters\n";
                    assert(on_gpu.cluster_gpu && !on_cpu.cluster_gpu && d.bytes == 0);
                    assert(on_gpu.triangles == on_cpu.triangles && on_gpu.clusters_drawn == on_cpu.clusters_drawn && on_gpu.cluster_copies == on_cpu.cluster_copies);
                    assert(on_gpu.cluster_overflow == 0);
                }
            }
        }
        // Pick: the ids and the points on a grid of pixels.
        scene.lights.shadow_cascades = 1;
        scene.ambient_occlusion.enabled = scene.contact_shadows.enabled = false;
        scene.cull = true;
        build();
        std::vector<kin::PickResult> hits[2];
        for (const bool on_gpu : {false, true}) {
            scene.lod = cut_on(on_gpu, {.enabled = true, .error_pixels = 0.7f});
            render_ok(r3d, view, scene);
            for (int y = 5; y < 150; y += 9) {
                for (int x = 5; x < 200; x += 9) {
                    hits[on_gpu].push_back(r3d.pick(view, {x, y}));
                }
            }
        }
        int hit = 0;
        for (std::size_t i = 0; i < hits[0].size(); ++i) {
            assert(hits[0][i].hit == hits[1][i].hit && hits[0][i].id == hits[1][i].id && kin::length(hits[0][i].position - hits[1][i].position) < 1e-5f);
            hit += hits[1][i].hit ? 1 : 0;
        }
        std::cerr << "gpu cut: pick on a grid of " << hits[0].size() << " pixels (" << hit << " hit) equal\n";
        assert(hit > 50);

        // Temporal anti-aliasing with a camera that walks: each frame equal.
        if (!lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            kin::View3D taa_cpu = r3d.create_view({200, 150}, {.samples = 1}), taa_gpu = r3d.create_view({200, 150}, {.samples = 1});
            int equal = 0;
            for (int f = 0; f < 14; ++f) {
                scene.camera = kin::Camera3D::look_at({0.0f, 7.0f, 40.0f - 1.5f * static_cast<float>(f)}, {0.0f, 1.0f, -30.0f}, 60.0f);
                scene.taa = {.enabled = true};
                scene.lights.shadow_cascades = 3;
                scene.ambient_occlusion.enabled = true;
                build();
                scene.lod = cut_on(false, {.enabled = true, .error_pixels = 0.7f});
                const Pixels a = shoot_view(renderer, r3d, taa_cpu, scene);
                renderer.present();
                scene.lod = cut_on(true, {.enabled = true, .error_pixels = 0.7f});
                const Pixels b = shoot_view(renderer, r3d, taa_gpu, scene);
                renderer.present();
                equal += a.rgba == b.rgba ? 1 : 0;
            }
            std::cerr << "gpu cut: " << equal << " of 14 frames with temporal anti-aliasing equal the CPU's\n";
            assert(equal == 14);
            scene.taa = {};
        }

        // Many copies, so that the scene's set-up runs on workers: the same picture and counts as serial.
        {
            scene.camera = kin::Camera3D::look_at({0.0f, 7.0f, 40.0f}, {0.0f, 1.0f, -30.0f}, 60.0f);
            scene.lights.shadow_cascades = 3;
            scene.ambient_occlusion.enabled = false;
            const auto crowd = [&] {
                build();
                Lcg rng;
                for (int i = 0; i < 60; ++i) {
                    scene.draw(i % 3 ? ball : slab, Mat4f::translation({rng.range(-30.0f, 30.0f), 1.0f + rng.next(), 30.0f - rng.next() * 70.0f}) * Mat4f::rotation_y(rng.next() * 360.0f) *
                               Mat4f::scaling(Vec3f{0.5f + rng.next() * 1.5f, 0.5f + rng.next() * 1.5f, 0.5f + rng.next() * 1.5f}), {.color = Color::rgb(60, 160, 220)});
                }
            };
            crowd();
            scene.lod = cut_on(true, {.enabled = true, .error_pixels = 0.7f});
            const Pixels serial = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            const kin::Render3DStats a = r3d.stats();
            kin::JobSystem jobs;
            renderer.set_job_system(&jobs);
            crowd();
            const Pixels parallel = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            const kin::Render3DStats b = r3d.stats();
            renderer.set_job_system(nullptr);
            scene.lod = cut_on(false, {.enabled = true, .error_pixels = 0.7f});
            const Pixels cpu = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            std::cerr << "gpu cut: " << a.cluster_copies << " copies, serial and workers equal: " << (serial.rgba == parallel.rgba) << ", equal to the CPU's: " << (serial.rgba == cpu.rgba) << "\n";
            assert(a.cluster_copies > 40 && serial.rgba == parallel.rgba && serial.rgba == cpu.rgba);
            assert(a.triangles == b.triangles && a.clusters_drawn == b.clusters_drawn && a.draws == b.draws && a.shadow_draws == b.shadow_draws && a.ao_draws == b.ao_draws);
        }
    });
}

// Run to run the picture is the same bit for bit, in one renderer and across two (the gathered indices are in their
// clusters' order, not in the order the GPU's threads finished).
void test_gpu_cut_is_deterministic() {
    static std::vector<kin::u8> first_run;
    for (int run = 0; run < 2; ++run) {
        with_gpu("test_gpu_cut_is_deterministic", [&](kin::Renderer2D& renderer) {
            kin::Renderer3D r3d{renderer};
            if (lacks(r3d.capabilities().gpu_driven_clusters, "the cluster cut in compute")) {
                return;
            }
            kin::MeshOptions options;
            options.clusters = true;
            const kin::Mesh hill = r3d.create_mesh(big_hill(100), options);
            const kin::Mesh ball = r3d.create_mesh(kin::meshes::sphere(1.0f, 64, 32), options);
            kin::View3D view = r3d.create_view({200, 150}, {.samples = 4});
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 7.0f, 60.0f}, {0.0f, 1.0f, -30.0f}, 60.0f);
            scene.lights.sun.direction = {-0.5f, 1.0f, 0.3f};
            scene.lights.shadow_cascades = 3;
            scene.lights.shadow_area = {{-80.0f, -0.2f, -80.0f}, {80.0f, 12.0f, 80.0f}};
            scene.lights.shadow_distance = 100.0f;
            scene.lod = cut_on(false, {.enabled = true, .error_pixels = 1.5f});
            scene.lod.gpu_culling = true;
            scene.draw(hill, Mat4f::translation({0.0f, 0.0f, 0.0f}), {.color = Color::rgb(150, 190, 120)});
            scene.draw(ball, Mat4f::translation({0.0f, 2.0f, 30.0f}) * Mat4f::scaling({3.0f, 3.0f, 3.0f}), {.color = Color::rgb(220, 120, 60)});
            for (int i = 0; i < 6; ++i) {
                const Pixels p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                assert(r3d.stats().cluster_gpu);
                if (first_run.empty()) {
                    first_run = p.rgba;
                }
                assert(p.rgba == first_run);
            }
            std::cerr << "gpu cut: 6 renders in run " << run << " byte-identical to the first\n";
        });
    }
}

// A copy outside the camera's view draws nothing in the camera's passes, and its shadow still falls into the view; a
// copy wholly outside the view and its shadows' reach is cut nowhere.
void test_gpu_cut_culls_by_pass() {
    with_gpu("test_gpu_cut_culls_by_pass", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().gpu_driven_clusters, "the cluster cut in compute")) {
            return;
        }
        kin::MeshOptions options;
        options.clusters = true;
        const kin::Mesh ball = r3d.create_mesh(kin::meshes::sphere(1.0f, 64, 32), options);
        const kin::Mesh floor = make_box(r3d, {400.0f, 0.2f, 400.0f});
        kin::View3D view = r3d.create_view({200, 150}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 6.0f, 20.0f}, {0.0f, 0.0f, 0.0f}, 50.0f);
        // The sun is low, to the left: a ball left of the view throws its shadow toward the view.
        scene.lights.sun.direction = {-1.0f, 0.3f, 0.0f};
        scene.lights.shadow_area = {{-80.0f, -0.2f, -80.0f}, {80.0f, 12.0f, 80.0f}};
        scene.lights.shadow_distance = 100.0f;
        scene.lod = cut_on(true, {.enabled = true, .error_pixels = 0.7f});
        for (const kin::u32 cascades : {1u, 3u}) {
            scene.lights.shadow_cascades = cascades;
            const auto render = [&](float ball_x, float ball_y) {
                scene.clear();
                scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
                scene.draw(ball, Mat4f::translation({ball_x, ball_y, 0.0f}) * Mat4f::scaling({2.0f, 2.0f, 2.0f}), {.color = Color::rgb(220, 120, 60)});
                const Pixels p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                return p;
            };
            // The indirect draws are what the cut chose and nothing else: every run's triangles add up to the gathered ones, the
            // empty runs have a count of 0 (not what an earlier render left), and each run is where its copy's slot is.
            const auto check_draws = [&] {
                std::vector<kin::ClusterDraw> draws;
                const bool ok = r3d.read_cluster_draws(view, draws);
                assert(ok);
                static_cast<void>(ok);
                kin::u64 triangles = 0;
                for (const kin::ClusterDraw& d : draws) {
                    assert(d.index_count % 3 == 0);
                    triangles += d.index_count / 3;
                }
                assert(triangles == r3d.stats().cluster_triangles_all);
            };
            // In view: the camera's cut has clusters.
            render(0.0f, 2.0f);
            check_draws();
            std::size_t shadow = 0;
            auto camera = camera_picks(r3d, view, scene, &shadow);
            assert(!camera.empty() && camera.begin()->second.size() > 3 && shadow > 0);
            // Left of the view: the camera cuts none (and the stats say 0), the shadow maps do cut it.
            const Pixels with_ball = render(-18.0f, 2.0f);
            check_draws();
            camera = camera_picks(r3d, view, scene, &shadow);
            const kin::Render3DStats stats = r3d.stats();
            std::cerr << "gpu cut: " << cascades << " cascades, ball off to the left: " << stats.clusters_drawn << " clusters in the camera's cut, " << shadow << " in the shadow maps\n";
            assert(camera.empty() && stats.clusters_drawn == 0 && stats.triangles == 12 && shadow > 3);
            // Its shadow is on the floor: the picture is not the one without the ball.
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            const Pixels without = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            assert(difference_of(with_ball, without).bytes > 100);
            // Far away on the other side: nowhere.
            render(300.0f, 2.0f);
            camera = camera_picks(r3d, view, scene, &shadow, /*may_be_none=*/true);
            assert(camera.empty() && shadow == 0);
        }
    });
}

// A room too small for the cut: the clusters that do not fit are not drawn (counted), nothing is written past the buffer,
// and the picture is a poorer one of the same scene.
void test_gpu_cut_overflow_clamps() {
    with_gpu("test_gpu_cut_overflow_clamps", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().gpu_driven_clusters, "the cluster cut in compute")) {
            return;
        }
        kin::MeshOptions options;
        options.clusters = true;
        const kin::Mesh hill = r3d.create_mesh(big_hill(100), options);
        kin::View3D view = r3d.create_view({200, 150}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 7.0f, 60.0f}, {0.0f, 1.0f, -30.0f}, 60.0f);
        scene.lights.shadow_cascades = 3;
        scene.lights.shadow_area = {{-80.0f, -0.2f, -80.0f}, {80.0f, 12.0f, 80.0f}};
        scene.lights.shadow_distance = 100.0f;
        scene.draw(hill, Mat4f::translation({0.0f, 0.0f, 0.0f}), {.color = Color::rgb(150, 190, 120)});
        scene.lod = cut_on(true, {.enabled = true, .error_pixels = 1.0f});
        const Pixels whole = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const kin::Render3DStats ample = r3d.stats();
        assert(ample.cluster_overflow == 0 && ample.cluster_triangles_all > 1000);
        for (const kin::u32 room : {static_cast<kin::u32>(ample.cluster_triangles_all / 2), 600u, 100u, 1u}) {
            scene.lod.max_cluster_triangles = room;
            const Pixels short_of_room = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            const kin::Render3DStats stats = r3d.stats();
            std::cerr << "gpu cut: room for " << room << " triangles: " << stats.cluster_overflow << " clusters dropped, " << stats.cluster_triangles_all << " gathered, " << stats.triangles
                      << " drawn by the camera, picture " << difference_of(short_of_room, whole).mean << " mean from the whole\n";
            assert(stats.cluster_overflow > 0 && stats.cluster_triangles_all <= room && stats.triangles <= room);
            assert(room < 1000 ? stats.triangles < ample.triangles : true);
        }
        // Room for one triangle: next to nothing of the mesh is drawn (never what the buffer held before), so the picture is
        // that of the scene without it.
        {
            scene.lod.max_cluster_triangles = 1;
            const Pixels one = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            kin::Scene3D empty;
            empty.camera = scene.camera;
            empty.lights = scene.lights;
            const Pixels none = shoot_view(renderer, r3d, view, empty);
            renderer.present();
            const Difference d = difference_of(one, none);
            std::cerr << "gpu cut: room for one triangle: picture " << d.mean << " mean from the scene without the mesh\n";
            assert(d.mean < 1.0);
        }
        // And back: with room again the whole cut is drawn, the picture the first one's.
        scene.lod.max_cluster_triangles = 4'000'000;
        const Pixels again = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        assert(again.rgba == whole.rgba && r3d.stats().cluster_overflow == 0);
    });
}

// A device without the cut in compute (here: KIN_GPU_CLUSTERS=0) says so and cuts on the CPU, with the picture the
// GPU cut makes.
void test_gpu_cut_falls_back() {
    std::vector<kin::u8> gpu_picture;
    for (const bool disabled : {false, true}) {
        if (disabled) {
            setenv("KIN_GPU_CLUSTERS", "0", 1);
        }
        with_gpu("test_gpu_cut_falls_back", [&](kin::Renderer2D& renderer) {
            kin::Renderer3D r3d{renderer};
            if (!disabled && lacks(r3d.capabilities().gpu_driven_clusters, "the cluster cut in compute")) {
                return;
            }
            assert(r3d.capabilities().gpu_driven_clusters == !disabled);
            kin::MeshOptions options;
            options.clusters = true;
            const kin::Mesh hill = r3d.create_mesh(big_hill(80), options);
            kin::View3D view = r3d.create_view({200, 150}, {.samples = 1});
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 7.0f, 60.0f}, {0.0f, 1.0f, -30.0f}, 60.0f);
            scene.lights.shadow_area = {{-80.0f, -0.2f, -80.0f}, {80.0f, 12.0f, 80.0f}};
            scene.draw(hill, Mat4f::translation({0.0f, 0.0f, 0.0f}), {.color = Color::rgb(150, 190, 120)});
            scene.lod = {.enabled = true, .error_pixels = 1.0f}; // gpu_culling left on
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            const kin::Render3DStats stats = r3d.stats();
            assert(stats.cluster_gpu == !disabled && stats.cluster_copies == 1);
            assert(disabled ? stats.cluster_upload_bytes > 1000 && stats.clusters_drawn > 0 && stats.triangles > 0 : stats.clusters_drawn == 0);
            if (!disabled) {
                gpu_picture = p.rgba;
            } else {
                assert(p.rgba == gpu_picture);
                std::cerr << "gpu cut: capability off, the CPU cut (" << stats.triangles << " triangles) draws the GPU cut's picture\n";
            }
        });
        if (disabled) {
            unsetenv("KIN_GPU_CLUSTERS");
        }
    }
}

// 64 copies of a mesh of 100 thousand triangles, spread out: the GPU cut gives the CPU cut's counts and picture, and
// the CPU's part of render() stays small however many clusters there are.
void test_gpu_cut_many_copies() {
    with_gpu("test_gpu_cut_many_copies", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().gpu_driven_clusters, "the cluster cut in compute")) {
            return;
        }
        kin::MeshOptions options;
        options.clusters = true;
        const MeshData data = kin::meshes::sphere(1.0f, 224, 224);
        const kin::Mesh ball = r3d.create_mesh(data, options);
        const kin::Mesh floor = make_box(r3d, {400.0f, 0.2f, 400.0f});
        kin::View3D view = r3d.create_view({320, 240}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 14.0f, 50.0f}, {0.0f, 0.0f, -20.0f}, 60.0f);
        scene.lights.shadow_cascades = 3;
        scene.lights.shadow_area = {{-100.0f, -0.2f, -100.0f}, {100.0f, 12.0f, 100.0f}};
        scene.lights.shadow_distance = 150.0f;
        scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
        for (int i = 0; i < 64; ++i) {
            const float x = static_cast<float>(i % 8) * 14.0f - 49.0f, z = 40.0f - static_cast<float>(i / 8) * 20.0f;
            scene.draw(ball, Mat4f::translation({x, 2.5f, z}) * Mat4f::scaling({2.5f, 2.5f, 2.5f}), {.color = Color::rgb(60 + static_cast<kin::u8>(i * 3), 130, 200)});
        }
        double cpu_ms[2] = {1e9, 1e9};
        kin::Render3DStats stats[2];
        Pixels pictures[2];
        for (const bool gpu : {false, true}) {
            scene.lod = cut_on(gpu, {.enabled = true, .error_pixels = 1.0f});
            for (int f = 0; f < 5; ++f) {
                pictures[gpu] = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                cpu_ms[gpu] = std::min(cpu_ms[gpu], r3d.stats().cpu_ms);
            }
            stats[gpu] = r3d.stats();
        }
        std::cerr << "gpu cut: 64 copies of " << data.triangle_count() << " triangles: " << stats[1].triangles << " drawn (CPU cut " << stats[0].triangles << "), render() "
                  << cpu_ms[1] << " ms against the CPU cut's " << cpu_ms[0] << " ms (readback on)\n";
        assert(stats[1].cluster_copies > 40 && stats[1].cluster_copies == stats[0].cluster_copies && stats[1].triangles == stats[0].triangles && stats[1].clusters_drawn == stats[0].clusters_drawn);
        assert(pictures[0].rgba == pictures[1].rgba && stats[1].cluster_overflow == 0 && stats[1].triangles > 0);
        // Without the read-back (the way a game runs it) the CPU does almost nothing for the cut.
        scene.lod = {.enabled = true, .error_pixels = 1.0f};
        double lean = 1e9;
        for (int f = 0; f < 8; ++f) {
            render_ok(r3d, view, scene);
            renderer.present();
            lean = std::min(lean, r3d.stats().cpu_ms);
        }
        std::cerr << "gpu cut: render() " << lean << " ms with the cut in compute and no read-back (cut set-up " << r3d.stats().cluster_ms << " ms)\n";
        assert(lean < cpu_ms[0]);
    });
}

// Meshes fill the cut's shared buffers chunk by chunk (16 MB of indices a chunk): twelve meshes of 60 thousand triangles need two,
// and copies of one mesh drawn apart (A, B, A, ...) are cut together; the picture and the counts are the CPU cut's, and the compute
// is one dispatch a chunk, not a mesh.
void test_gpu_cut_chunks() {
    with_gpu("test_gpu_cut_chunks", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().gpu_driven_clusters, "the cluster cut in compute")) {
            return;
        }
        kin::MeshOptions options;
        options.clusters = true;
        std::vector<kin::Mesh> meshes;
        for (int i = 0; i < 12; ++i) {
            meshes.push_back(r3d.create_mesh(kin::meshes::sphere(1.0f, 173 + i, 173), options)); // about 60 thousand triangles
        }
        kin::View3D view = r3d.create_view({240, 180}, {.samples = 1, .pick = true});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 9.0f, 30.0f}, {0.0f, 0.0f, -10.0f}, 60.0f);
        scene.lights.shadow_cascades = 3;
        scene.lights.shadow_area = {{-60.0f, -0.2f, -60.0f}, {60.0f, 12.0f, 60.0f}};
        scene.lights.shadow_distance = 90.0f;
        const kin::Mesh floor = make_box(r3d, {400.0f, 0.2f, 400.0f});
        scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
        for (int i = 0; i < 30; ++i) {
            const float x = static_cast<float>(i % 6) * 8.0f - 20.0f, z = 12.0f - static_cast<float>(i / 6) * 9.0f, scale = 1.5f + static_cast<float>(i % 3);
            scene.draw(meshes[static_cast<std::size_t>((i * 5) % 12)], Mat4f::translation({x, scale, z}) * Mat4f::scaling({scale, scale, scale}),
                       {.color = Color::rgb(static_cast<kin::u8>(60 + i * 6), 150, static_cast<kin::u8>(220 - i * 5))}, kin::colors::white, static_cast<kin::u32>(i));
        }
        scene.lod = cut_on(false, {.enabled = true, .error_pixels = 1.0f});
        const Pixels cpu = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const kin::Render3DStats on_cpu = r3d.stats();
        scene.lod = cut_on(true, {.enabled = true, .error_pixels = 1.0f});
        const Pixels gpu = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const kin::Render3DStats on_gpu = r3d.stats();
        std::cerr << "gpu cut: 12 meshes in chunks, " << on_gpu.cluster_copies << " copies, " << on_gpu.cluster_dispatches << " dispatches, " << on_gpu.triangles << " triangles (CPU cut "
                  << on_cpu.triangles << "), " << difference_of(gpu, cpu).bytes << " bytes differ\n";
        assert(on_gpu.cluster_copies == 30 && on_gpu.cluster_copies == on_cpu.cluster_copies);
        assert(on_gpu.triangles == on_cpu.triangles && on_gpu.clusters_drawn == on_cpu.clusters_drawn && gpu.rgba == cpu.rgba);
        assert(on_gpu.cluster_dispatches == 8); // two chunks: a cull and a gather dispatch each, the three scans and the arguments
        // The same through the pick pass.
        scene.lod = cut_on(false, {.enabled = true, .error_pixels = 1.0f});
        render_ok(r3d, view, scene);
        std::vector<kin::PickResult> a, b;
        for (int y = 5; y < 180; y += 11) {
            for (int x = 5; x < 240; x += 11) {
                a.push_back(r3d.pick(view, {x, y}));
            }
        }
        scene.lod = cut_on(true, {.enabled = true, .error_pixels = 1.0f});
        render_ok(r3d, view, scene);
        for (int y = 5; y < 180; y += 11) {
            for (int x = 5; x < 240; x += 11) {
                b.push_back(r3d.pick(view, {x, y}));
            }
        }
        int hits = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            assert(a[i].hit == b[i].hit && a[i].id == b[i].id);
            hits += b[i].hit ? 1 : 0;
        }
        assert(hits > 30);
    });
}

// --- Occlusion culling (G3d) ---

// A room for hiding things: a floor, a wall 8 m wide and 3 high across the way at z = 0 (in two halves round a slit when
// asked), and balls of clustered meshes (18 thousand triangles each) in front of it and behind. The camera stands at z = 12.
struct OcclusionRoom {
    explicit OcclusionRoom(kin::Renderer3D& r3d) {
        kin::MeshOptions options;
        options.clusters = true;
        floor = make_box(r3d, {60.0f, 0.2f, 60.0f});
        unit = make_box(r3d, {1.0f, 3.0f, 0.4f});
        ball = r3d.create_mesh(kin::meshes::sphere(1.0f, 96, 96), options);
    }

    // `gap`: a slit of that width in the middle of the wall (0: none); `wall_x` moves the wall sideways. Balls at `at`, ids from 1.
    void fill(kin::Scene3D& scene, const std::vector<Vec3f>& at, float gap = 0.0f, float scale = 1.0f, float wall_x = 0.0f) const {
        scene.clear();
        scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(150, 150, 150)});
        const kin::Material3D plaster{.color = Color::rgb(200, 190, 170)};
        if (gap <= 0.0f) {
            scene.draw(unit, Mat4f::translation({wall_x, 1.5f, 0.0f}) * Mat4f::scaling({8.0f, 1.0f, 1.0f}), plaster);
        } else {
            const float side = 4.0f - gap * 0.5f;
            scene.draw(unit, Mat4f::translation({wall_x - (gap * 0.5f + side * 0.5f), 1.5f, 0.0f}) * Mat4f::scaling({side, 1.0f, 1.0f}), plaster);
            scene.draw(unit, Mat4f::translation({wall_x + gap * 0.5f + side * 0.5f, 1.5f, 0.0f}) * Mat4f::scaling({side, 1.0f, 1.0f}), plaster);
        }
        kin::u32 id = 1;
        for (const Vec3f& v : at) {
            scene.draw(ball, Mat4f::translation(v) * Mat4f::scaling({scale, scale, scale}),
                       {.color = Color::rgb(70, static_cast<kin::u8>(120 + 14 * (id % 7)), 210), .roughness = 0.5f}, kin::colors::white, id);
            ++id;
        }
    }

    kin::Mesh floor, unit, ball;
};

// Balls wholly behind the wall from the camera (0, 1.7, 12), and some in front.
const std::vector<Vec3f> hidden_balls{{-1.5f, 1.0f, -5.0f}, {1.5f, 1.0f, -7.0f}, {0.0f, 1.0f, -9.0f}};
const std::vector<Vec3f> open_balls{{-5.5f, 1.0f, 5.0f}, {5.5f, 1.0f, 3.0f}, {0.0f, 1.0f, 8.0f}};

kin::Camera3D room_camera(float x = 0.0f) {
    return kin::Camera3D::look_at({x, 1.7f, 12.0f}, {x, 1.3f, -4.0f}, 60.0f);
}

// Two views of one renderer drawn from the same scenes, one with Lod3D::occlusion and one without: occlusion culling must never
// change what the view shows (the second pass tests every cluster against this frame's own depth), so every frame of the one
// is the other's, to the byte.
struct Twin {
    Twin(kin::Renderer2D& r, kin::Renderer3D& r3, kin::u32 samples) : renderer(r), r3d(r3), off(r3.create_view({160, 120}, {.samples = samples, .background = Color::rgb(20, 24, 30), .pick = true})),
                                                                       on(r3.create_view({160, 120}, {.samples = samples, .background = Color::rgb(20, 24, 30), .pick = true})) {}

    // Renders `scene` into both. Returns the bytes that differ.
    int frame(kin::Scene3D& scene) {
        scene.lod.enabled = true;
        scene.lod.error_pixels = 0.1f; // finer than the default: the balls are far, and few clusters would show
        scene.lod.stats_readback = true;
        scene.lod.occlusion.enabled = false;
        off_picture = shoot_view(renderer, r3d, off, scene);
        renderer.present();
        off_stats = r3d.stats();
        scene.lod.occlusion.enabled = true;
        on_picture = shoot_view(renderer, r3d, on, scene);
        renderer.present();
        on_stats = r3d.stats();
        scene.lod.occlusion.enabled = false;
        ++frames;
        return difference_of(off_picture, on_picture).bytes;
    }

    // The ids under a grid of pixels in the two views' last frames: how many differ.
    int pick_differences() {
        int different = 0;
        for (int y = 3; y < 120; y += 9) {
            for (int x = 3; x < 160; x += 9) {
                const kin::PickResult a = r3d.pick(off, {x, y}), b = r3d.pick(on, {x, y});
                different += a.hit != b.hit || a.id != b.id ? 1 : 0;
            }
        }
        return different;
    }

    kin::Renderer2D& renderer;
    kin::Renderer3D& r3d;
    kin::View3D off, on;
    Pixels off_picture, on_picture;
    kin::Render3DStats off_stats, on_stats;
    int frames = 0;
};

// With KIN_OCCLUSION_DUMP=<directory> a sequence writes its frames there (<name>_<frame>_off|on|diff.ppm, the difference times eight), to look at.
void dump_frames(const Twin& twin, const char* name, int frame) {
    const char* dir = std::getenv("KIN_OCCLUSION_DUMP");
    if (!dir || !dir[0]) {
        return;
    }
    const auto write = [&](const char* suffix, const std::vector<kin::u8>& rgba) {
        std::ofstream out(std::string(dir) + "/" + name + "_" + std::to_string(frame) + "_" + suffix + ".ppm", std::ios::binary);
        out << "P6\n160 120\n255\n";
        for (std::size_t i = 0; i < rgba.size(); i += 4) {
            out.write(reinterpret_cast<const char*>(&rgba[i]), 3);
        }
    };
    write("off", twin.off_picture.rgba);
    write("on", twin.on_picture.rgba);
    std::vector<kin::u8> diff(twin.off_picture.rgba.size(), 255);
    for (std::size_t i = 0; i < diff.size(); ++i) {
        if (i % 4 != 3) {
            diff[i] = static_cast<kin::u8>(std::min(255, 8 * std::abs(static_cast<int>(twin.off_picture.rgba[i]) - static_cast<int>(twin.on_picture.rgba[i]))));
        }
    }
    write("diff", diff);
}

// Whether a scene with the clustered meshes runs the two passes: the capability, Lod3D::occlusion with the selection and the GPU cut.
bool occlusion_capable(kin::Renderer3D& r3d) {
    return !lacks(r3d.capabilities().occlusion_culling && r3d.capabilities().gpu_driven_clusters, "occlusion culling");
}

// What is off is as before (the pyramid and its passes are not made; the cut's dispatches, draws and counts are G3c's), and what is on
// never changes the picture: static views, with and without MSAA, one shadow map or three, occlusion and contact shadows, temporal
// anti-aliasing; clusters occluded behind the wall and none in the open; the pick pass finds the same copies.
void test_gpu_occlusion_picture_and_stats() {
    with_gpu("test_gpu_occlusion_picture_and_stats", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (!occlusion_capable(r3d)) {
            return;
        }
        const OcclusionRoom room{r3d};
        struct Config {
            const char* name;
            kin::u32 samples, cascades;
            bool passes, taa;
        };
        const std::vector<Config> configs{{"1 sample", 1, 1, false, false},     {"4 samples", 4, 1, false, false}, {"3 cascades", 4, 3, false, false},
                                          {"ambient occlusion + contact", 4, 1, true, false}, {"taa", 1, 1, false, true}, {"taa, cascades, passes", 1, 3, true, true}};
        for (const Config& config : configs) {
            if (config.taa && lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
                continue;
            }
            Twin twin{renderer, r3d, config.samples};
            kin::Scene3D scene;
            scene.camera = room_camera();
            scene.lights.sun.direction = {-0.85f, 0.5f, 0.1f};
            scene.lights.shadow_area = {{-30.0f, -0.2f, -30.0f}, {30.0f, 8.0f, 30.0f}};
            scene.lights.shadow_cascades = config.cascades;
            scene.lights.shadow_distance = 60.0f;
            if (config.passes) {
                scene.ambient_occlusion = {.enabled = true};
                scene.contact_shadows = {.enabled = true};
            }
            scene.taa = {.enabled = config.taa};
            std::vector<Vec3f> balls = hidden_balls;
            balls.insert(balls.end(), open_balls.begin(), open_balls.end());
            int worst = 0, picks = 0;
            for (int f = 0; f < (config.taa ? 8 : 3); ++f) {
                room.fill(scene, balls);
                worst = std::max(worst, twin.frame(scene));
                picks += twin.pick_differences();
            }
            const kin::Render3DStats& on = twin.on_stats;
            const kin::Render3DStats& off = twin.off_stats;
            std::cerr << "occlusion, " << config.name << ": " << worst << " bytes differ, " << picks << " picks differ; " << on.clusters_drawn << " clusters drawn of "
                      << off.clusters_drawn << ", " << on.clusters_first_pass << " in the first pass, " << on.clusters_occluded << " occluded (" << on.triangles_occluded << " triangles), "
                      << on.clusters_wasted << " drawn for nothing; " << on.triangles << " triangles against " << off.triangles << "\n";
            assert(worst == 0 && picks == 0);
            assert(!off.occlusion && off.occlusion_dispatches == 0 && off.clusters_occluded == 0 && off.clusters_first_pass == 0);
            assert(on.occlusion && on.occlusion_levels >= 5 && on.occlusion_dispatches >= 3 && on.cluster_dispatches == 2 * off.cluster_dispatches);
            assert(on.clusters_occluded > 40 && on.triangles_occluded > 4000 && on.triangles < off.triangles && on.clusters_drawn < off.clusters_drawn);
            assert(on.clusters_drawn + on.clusters_occluded == off.clusters_drawn); // nothing in the cut is lost: drawn or occluded
        }
        // In the open the balls hide only their own far sides (a closed mesh's back clusters lie behind its front ones), and what is
        // not hidden is drawn: the clusters of the cut are those drawn and those occluded, to the cluster, whatever the history.
        {
            Twin twin{renderer, r3d, 4};
            kin::Scene3D scene;
            scene.camera = room_camera();
            for (int f = 0; f < 3; ++f) {
                room.fill(scene, open_balls);
                assert(twin.frame(scene) == 0);
            }
            std::cerr << "occlusion, in the open: " << twin.on_stats.clusters_drawn << " clusters drawn of " << twin.off_stats.clusters_drawn << ", occluded " << twin.on_stats.clusters_occluded
                      << " (the balls' far sides), " << twin.on_stats.clusters_wasted << " drawn for nothing\n";
            assert(twin.on_stats.clusters_drawn + twin.on_stats.clusters_occluded == twin.off_stats.clusters_drawn);
            assert(twin.on_stats.clusters_occluded * 10 < twin.off_stats.clusters_drawn && twin.on_stats.clusters_wasted == 0);
            assert(twin.on_stats.clusters_first_pass == twin.on_stats.clusters_drawn); // the history held all the visible ones after the first frame
        }
        // A surface that cannot hide itself, seen from above: nothing is occluded at all.
        {
            Twin twin{renderer, r3d, 4};
            kin::MeshOptions options;
            options.clusters = true;
            const kin::Mesh sheet = r3d.create_mesh(big_hill(120), options); // gently wavy: its slopes are under 0.25
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 36.0f, 48.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
            for (int f = 0; f < 3; ++f) {
                scene.clear();
                scene.draw(sheet, Mat4f::identity(), {.color = Color::rgb(120, 180, 120)});
                assert(twin.frame(scene) == 0);
            }
            std::cerr << "occlusion, a flat sheet: " << twin.on_stats.clusters_drawn << " clusters drawn of " << twin.off_stats.clusters_drawn << ", occluded " << twin.on_stats.clusters_occluded << "\n";
            assert(twin.on_stats.clusters_occluded == 0 && twin.on_stats.clusters_drawn == twin.off_stats.clusters_drawn && twin.on_stats.clusters_drawn > 40);
        }
        // The overlay (a clustered mesh in the gun's place) is drawn whole by the first pass and never tested, whatever the world does.
        {
            Twin twin{renderer, r3d, 4};
            kin::Scene3D scene;
            scene.camera = room_camera();
            int worst = 0;
            kin::u32 occluded = 0;
            for (int f = 0; f < 4; ++f) {
                room.fill(scene, hidden_balls);
                kin::Scene3D& gun = scene.overlay();
                gun.clear();
                gun.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 50.0f);
                gun.lod = {.enabled = true, .error_pixels = 0.1f};
                gun.draw(room.ball, Mat4f::translation({0.35f, -0.25f, -1.2f}) * Mat4f::scaling({0.3f, 0.3f, 0.3f}), {.color = Color::rgb(230, 90, 60)});
                worst = std::max(worst, twin.frame(scene));
                occluded = twin.on_stats.clusters_occluded;
            }
            std::cerr << "occlusion, with an overlay: " << worst << " bytes differ, " << occluded << " clusters occluded, " << twin.on_stats.clusters_drawn << " drawn of " << twin.off_stats.clusters_drawn << "\n";
            assert(worst == 0 && occluded > 20 && twin.on_stats.clusters_drawn + occluded == twin.off_stats.clusters_drawn);
        }
        // The first frame, with no history: the first pass draws nothing, the second tests everything against the wall alone.
        {
            Twin twin{renderer, r3d, 4};
            kin::Scene3D scene;
            scene.camera = room_camera();
            room.fill(scene, hidden_balls);
            assert(twin.frame(scene) == 0);
            assert(twin.on_stats.clusters_first_pass == 0 && twin.on_stats.clusters_occluded > 40);
        }
    });
}

// A camera that strafes past the wall's end across 60 frames, so that balls behind it come into view a frame at a time; another that
// turns on the spot toward them; and a ball that moves out from behind the wall. Every frame is the other view's, to the byte: nothing
// pops, because a cluster hidden last frame is tested against this frame's own depth.
void test_gpu_occlusion_moving() {
    with_gpu("test_gpu_occlusion_moving", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (!occlusion_capable(r3d)) {
            return;
        }
        const OcclusionRoom room{r3d};
        kin::Scene3D scene;
        scene.lights.sun.direction = {-0.85f, 0.5f, 0.1f};
        scene.lights.shadow_area = {{-30.0f, -0.2f, -30.0f}, {30.0f, 8.0f, 30.0f}};
        scene.lights.shadow_cascades = 3;
        scene.lights.shadow_distance = 60.0f;
        // 1. The camera strafes from x = 0 to 14 in 60 steps: the balls behind the wall are revealed.
        {
            Twin twin{renderer, r3d, 4};
            int worst = 0, revealed_at = -1;
            kin::u32 occluded_first = 0, occluded_last = 0;
            for (int f = 0; f < 60; ++f) {
                scene.camera = room_camera(0.24f * static_cast<float>(f));
                room.fill(scene, hidden_balls);
                worst = std::max(worst, twin.frame(scene));
                dump_frames(twin, "strafe", f);
                if (f == 0) {
                    occluded_first = twin.on_stats.clusters_occluded;
                }
                if (revealed_at < 0 && twin.on_stats.clusters_occluded < occluded_first / 2) {
                    revealed_at = f;
                }
                occluded_last = twin.on_stats.clusters_occluded;
            }
            std::cerr << "occlusion, strafing: " << worst << " bytes differ at worst in 60 frames; occluded " << occluded_first << " at the start, " << occluded_last << " at the end, revealed from frame "
                      << revealed_at << "\n";
            assert(worst == 0 && occluded_first > 40 && occluded_last < occluded_first / 4 && revealed_at > 5);
        }
        // 2. The camera turns on the spot, from looking away to the wall and the balls behind it, and on past it.
        {
            Twin twin{renderer, r3d, 4};
            int worst = 0;
            kin::u32 most = 0, least = ~0u;
            for (int f = 0; f < 60; ++f) {
                const float yaw = (-20.0f + 2.9f * static_cast<float>(f)) * 3.14159265f / 180.0f;
                scene.camera = kin::Camera3D::look_at({5.0f, 1.7f, 4.0f}, {5.0f + std::cos(yaw) * 10.0f, 1.4f, 4.0f - std::sin(yaw) * 10.0f}, 60.0f);
                room.fill(scene, hidden_balls);
                worst = std::max(worst, twin.frame(scene));
                dump_frames(twin, "turn", f);
                most = std::max(most, twin.on_stats.clusters_occluded);
                least = std::min(least, twin.on_stats.clusters_occluded);
            }
            std::cerr << "occlusion, turning: " << worst << " bytes differ at worst in 60 frames; clusters occluded " << least << " to " << most << "\n";
            assert(worst == 0 && most > 40);
        }
        // 3. A ball goes from behind the wall to the open, and back.
        {
            Twin twin{renderer, r3d, 4};
            int worst = 0;
            kin::u32 hidden_at_start = 0, drawn_in_the_open = 0;
            scene.camera = room_camera();
            for (int f = 0; f < 60; ++f) {
                const float x = f < 30 ? 0.3f * static_cast<float>(f) : 9.0f - 0.3f * static_cast<float>(f - 30);
                room.fill(scene, {{x, 1.0f, -3.0f}, {-3.0f, 1.0f, -6.0f}});
                worst = std::max(worst, twin.frame(scene));
                if (f == 0) {
                    hidden_at_start = twin.on_stats.clusters_occluded;
                }
                if (f == 29) {
                    drawn_in_the_open = twin.on_stats.clusters_drawn;
                }
            }
            std::cerr << "occlusion, a ball out from behind the wall and back: " << worst << " bytes differ at worst; " << hidden_at_start << " clusters occluded at the start, "
                      << drawn_in_the_open << " drawn with the ball in the open\n";
            assert(worst == 0 && hidden_at_start > 40 && drawn_in_the_open > hidden_at_start / 4);
        }
    });
}

// A slit in the wall with a ball behind it, and a ball partly off the screen: not culled, as pictures show. Slits of a pixel and more
// are never taken for solid; one narrower than a pixel can fall between the single-sample depth's pixel centres, which a multisampled
// picture can see through: its difference is measured and said, not asserted.
void test_gpu_occlusion_gaps_and_edges() {
    with_gpu("test_gpu_occlusion_gaps_and_edges", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (!occlusion_capable(r3d)) {
            return;
        }
        const OcclusionRoom room{r3d};
        kin::Scene3D scene;
        scene.camera = room_camera();
        // A pixel is about 0.115 m across at the wall (12 m away, a 60 degree view of 120 rows).
        for (const float gap : {0.9f, 0.45f, 0.25f, 0.12f, 0.06f, 0.02f}) {
            for (const kin::u32 samples : {1u, 4u}) {
                Twin twin{renderer, r3d, samples};
                int worst = 0;
                for (int f = 0; f < 3; ++f) {
                    room.fill(scene, {{0.0f, 1.0f, -4.0f}}, gap);
                    worst = std::max(worst, twin.frame(scene));
                }
                const double pixels = static_cast<double>(gap) / 0.115;
                std::cerr << "occlusion, slit of " << gap << " m (" << pixels << " pixels), " << samples << " samples: " << worst << " bytes differ; " << twin.on_stats.clusters_occluded
                          << " clusters occluded of " << twin.off_stats.clusters_drawn << "\n";
                if (pixels >= 1.0) {
                    assert(worst == 0);
                }
                // Even where nothing is wrong, the ball behind a wide slit is drawn.
                if (pixels >= 3.0) {
                    assert(twin.on_stats.clusters_drawn > 0 && twin.on_stats.clusters_occluded < twin.off_stats.clusters_drawn);
                }
            }
        }
        // Partly behind the wall's edge, partly at the screen's edge, partly under the floor's horizon.
        for (const float x : {4.1f, 4.5f, 5.2f}) {
            Twin twin{renderer, r3d, 4};
            int worst = 0;
            for (int f = 0; f < 3; ++f) {
                room.fill(scene, {{x, 1.0f, -3.0f}, {-x, 1.0f, -3.0f}});
                worst = std::max(worst, twin.frame(scene));
            }
            std::cerr << "occlusion, ball at the wall's edge x = " << x << ": " << worst << " bytes differ, " << twin.on_stats.clusters_drawn << " drawn of " << twin.off_stats.clusters_drawn << "\n";
            assert(worst == 0 && twin.on_stats.clusters_drawn > 0);
        }
        // Balls half sunk into the floor and half through the wall: the nearest point of each lies in front of the surface that covers its centre.
        {
            Twin twin{renderer, r3d, 4};
            int worst = 0;
            scene.camera = room_camera();
            for (int f = 0; f < 3; ++f) {
                room.fill(scene, {{-2.0f, 0.0f, 3.0f}, {1.5f, 1.0f, -0.1f}, {3.0f, 0.2f, 6.0f}});
                worst = std::max(worst, twin.frame(scene));
            }
            std::cerr << "occlusion, balls through the floor and the wall: " << worst << " bytes differ, " << twin.on_stats.clusters_drawn << " drawn of " << twin.off_stats.clusters_drawn << "\n";
            assert(worst == 0 && twin.on_stats.clusters_drawn > 40);
        }
        {
            Twin twin{renderer, r3d, 4};
            int worst = 0;
            scene.camera = kin::Camera3D::look_at({0.0f, 1.7f, 12.0f}, {-3.0f, 1.3f, 0.0f}, 60.0f);
            for (int f = 0; f < 3; ++f) {
                room.fill(scene, {{-9.0f, 1.0f, 2.0f}, {-7.0f, 1.0f, 6.0f}, {0.0f, 1.0f, -5.0f}});
                worst = std::max(worst, twin.frame(scene));
            }
            std::cerr << "occlusion, balls at the screen's edge: " << worst << " bytes differ\n";
            assert(worst == 0);
        }
    });
}

// Resizing the view or View3D::reset_history() drops the history: the next frame's first pass draws nothing (and the picture is still the
// same). The history only ever costs or saves work; a camera cut without a reset is just as right a picture.
void test_gpu_occlusion_history() {
    with_gpu("test_gpu_occlusion_history", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (!occlusion_capable(r3d)) {
            return;
        }
        const OcclusionRoom room{r3d};
        Twin twin{renderer, r3d, 4};
        kin::Scene3D scene;
        scene.camera = room_camera();
        std::vector<Vec3f> balls = hidden_balls;
        balls.insert(balls.end(), open_balls.begin(), open_balls.end());
        for (int f = 0; f < 3; ++f) {
            room.fill(scene, balls);
            assert(twin.frame(scene) == 0);
        }
        const kin::u32 held = twin.on_stats.clusters_first_pass;
        assert(held > 40);
        // A camera cut without a reset: wrong history, right picture.
        scene.camera = kin::Camera3D::look_at({0.0f, 4.0f, -14.0f}, {0.0f, 1.0f, 3.0f}, 60.0f);
        room.fill(scene, balls);
        assert(twin.frame(scene) == 0);
        std::cerr << "occlusion, a camera cut without a reset: the picture is right; " << twin.on_stats.clusters_first_pass << " clusters in the first pass, " << twin.on_stats.clusters_wasted
                  << " of them drawn for nothing\n";
        // A reset.
        scene.camera = room_camera();
        twin.on.reset_history();
        room.fill(scene, balls);
        assert(twin.frame(scene) == 0);
        std::cerr << "occlusion, after reset_history: " << twin.on_stats.clusters_first_pass << " clusters in the first pass (" << held << " before)\n";
        assert(twin.on_stats.clusters_first_pass == 0 && twin.on_stats.clusters_occluded > 40);
        room.fill(scene, balls);
        assert(twin.frame(scene) == 0 && twin.on_stats.clusters_first_pass > 40);
        // A resize.
        assert(r3d.resize(twin.on, {120, 90}) && r3d.resize(twin.off, {120, 90}));
        room.fill(scene, balls);
        assert(twin.frame(scene) == 0);
        assert(twin.on_stats.clusters_first_pass == 0 && twin.on_stats.clusters_occluded > 20);
        room.fill(scene, balls);
        assert(twin.frame(scene) == 0 && twin.on_stats.clusters_first_pass > 20);
        // And a change of the scene's draws (a ball less): a new layout, history from nothing, the same picture.
        balls.pop_back();
        room.fill(scene, balls);
        assert(twin.frame(scene) == 0 && twin.on_stats.clusters_first_pass == 0);
    });
}

// Two runs alike: the same frames and counts, byte for byte, from fresh views.
void test_gpu_occlusion_is_deterministic() {
    with_gpu("test_gpu_occlusion_is_deterministic", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (!occlusion_capable(r3d)) {
            return;
        }
        const OcclusionRoom room{r3d};
        std::vector<std::vector<kin::u8>> frames[2];
        std::vector<kin::u32> counts[2];
        for (int run = 0; run < 2; ++run) {
            Twin twin{renderer, r3d, 4};
            kin::Scene3D scene;
            scene.lights.sun.direction = {-0.85f, 0.5f, 0.1f};
            scene.lights.shadow_area = {{-30.0f, -0.2f, -30.0f}, {30.0f, 8.0f, 30.0f}};
            for (int f = 0; f < 20; ++f) {
                scene.camera = room_camera(0.7f * static_cast<float>(f));
                room.fill(scene, hidden_balls);
                twin.frame(scene);
                frames[run].push_back(twin.on_picture.rgba);
                counts[run].push_back(twin.on_stats.clusters_drawn * 1000003u + twin.on_stats.clusters_occluded * 101u + twin.on_stats.clusters_wasted);
            }
        }
        assert(frames[0] == frames[1] && counts[0] == counts[1]);
        std::cerr << "occlusion: 20 frames twice from fresh views, identical\n";
    });
}

// The shadow of a ball hidden from the camera still falls into view: the shadow maps are not culled with the camera's pyramid.
void test_gpu_occlusion_keeps_shadows() {
    with_gpu("test_gpu_occlusion_keeps_shadows", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (!occlusion_capable(r3d)) {
            return;
        }
        const OcclusionRoom room{r3d};
        for (const kin::u32 cascades : {1u, 3u}) {
            Twin twin{renderer, r3d, 4};
            kin::Scene3D scene;
            scene.camera = room_camera(6.0f);
            scene.lights.sun.direction = {-0.94f, 0.34f, 0.05f}; // low, from the left: the shadows run right, past the wall's end
            scene.lights.shadow_area = {{-30.0f, -0.2f, -30.0f}, {30.0f, 8.0f, 30.0f}};
            scene.lights.shadow_cascades = cascades;
            scene.lights.shadow_distance = 60.0f;
            int worst = 0;
            for (int f = 0; f < 3; ++f) {
                room.fill(scene, {{1.0f, 1.0f, -1.5f}, {2.5f, 1.0f, -1.0f}});
                worst = std::max(worst, twin.frame(scene));
            }
            // Without the balls the floor right of the wall is lit; with them (hidden from this camera) their shadows darken it.
            room.fill(scene, {});
            scene.lod.enabled = true;
            const Pixels without = shoot_view(renderer, r3d, twin.off, scene);
            renderer.present();
            const Difference shadow = difference_of(without, twin.on_picture);
            std::cerr << "occlusion, shadows of hidden balls, " << cascades << " cascades: " << worst << " bytes differ with and without occlusion; the balls' shadows change " << shadow.bytes
                      << " bytes of the picture; " << twin.on_stats.clusters_occluded << " clusters occluded\n";
            assert(worst == 0 && shadow.bytes > 50 && twin.on_stats.clusters_occluded > 20);
        }
    });
}

// Not capable, or not asked in full: ignored, and the picture is the same. KIN_GPU_CLUSTERS=0 takes the GPU cut away (the CPU cuts,
// there is nothing to test against); the selection off, or Lod3D::gpu_culling off, likewise; a scene with no clustered mesh has nothing to cull.
void test_gpu_occlusion_is_ignored() {
    for (const bool disabled : {false, true}) {
        if (disabled) {
            setenv("KIN_GPU_CLUSTERS", "0", 1);
        }
        with_gpu(disabled ? "test_gpu_occlusion_is_ignored (no GPU cut)" : "test_gpu_occlusion_is_ignored", [&](kin::Renderer2D& renderer) {
            kin::Renderer3D r3d{renderer};
            kin::Renderer3D& r = r3d;
            if (!disabled && !occlusion_capable(r)) {
                return;
            }
            assert(r3d.capabilities().occlusion_culling == (!disabled && r3d.capabilities().gpu_driven_clusters));
            const OcclusionRoom room{r3d};
            kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(20, 24, 30)});
            kin::Scene3D scene;
            scene.camera = room_camera();
            room.fill(scene, hidden_balls);
            scene.lod = {.enabled = true, .stats_readback = true};
            const Pixels plain = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            for (int variant = 0; variant < 3; ++variant) {
                scene.lod = {.enabled = variant != 0, .gpu_culling = variant != 1, .stats_readback = true, .occlusion = {.enabled = true}};
                const Pixels p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                const kin::Render3DStats stats = r3d.stats();
                // Only the third (the selection, the GPU cut and Lod3D::occlusion all on) culls, where the device can.
                assert(stats.occlusion == (variant == 2 && !disabled));
                if (variant == 0) {
                    assert(stats.cluster_copies == 0); // no selection: the whole mesh
                } else {
                    assert(p.rgba == plain.rgba);
                }
            }
            // Only plain meshes: nothing to cull, and the one line about it.
            kin::Scene3D bare;
            bare.camera = room_camera();
            bare.lod = {.enabled = true, .occlusion = {.enabled = true}};
            const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
            bare.draw(box, Mat4f::translation({0.0f, 1.0f, 0.0f}));
            render_ok(r3d, view, bare);
            assert(!r3d.stats().occlusion && r3d.stats().cluster_copies == 0);
        });
        if (disabled) {
            unsetenv("KIN_GPU_CLUSTERS");
        }
    }
}

// The room for the cut's gathered indices runs out while occlusion is on: the picture degrades as G3c's does, nothing is written past
// the end, the dropped clusters are counted; with room again it is the full picture.
void test_gpu_occlusion_with_overflow() {
    with_gpu("test_gpu_occlusion_with_overflow", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (!occlusion_capable(r3d)) {
            return;
        }
        const OcclusionRoom room{r3d};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(20, 24, 30)});
        kin::Scene3D scene;
        scene.camera = room_camera();
        std::vector<Vec3f> balls = hidden_balls;
        balls.insert(balls.end(), open_balls.begin(), open_balls.end());
        room.fill(scene, balls);
        scene.lod = {.enabled = true, .stats_readback = true, .occlusion = {.enabled = true}};
        Pixels full;
        for (int f = 0; f < 3; ++f) {
            full = shoot_view(renderer, r3d, view, scene);
            renderer.present();
        }
        const kin::u64 fine = r3d.stats().triangles;
        for (const kin::u32 room_triangles : {2000u, 100u, 1u}) {
            scene.lod.max_cluster_triangles = room_triangles;
            Pixels small;
            for (int f = 0; f < 3; ++f) {
                small = shoot_view(renderer, r3d, view, scene);
                renderer.present();
            }
            const kin::Render3DStats stats = r3d.stats();
            std::cerr << "occlusion, room of " << room_triangles << " triangles: " << stats.triangles << " drawn (" << fine << " with room), " << stats.cluster_overflow << " clusters dropped\n";
            assert(stats.triangles <= static_cast<kin::u64>(room_triangles) * 2 + 24 && stats.cluster_overflow > 0 && stats.triangles < fine);
        }
        scene.lod.max_cluster_triangles = 4'000'000;
        const Pixels again = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        assert(again.rgba == full.rgba && r3d.stats().cluster_overflow == 0);
    });
}

// The targets of the opt-in features (ambient occlusion, contact shadows, the pre-pass, temporal anti-aliasing, occlusion culling) are given back
// when a feature has not been used for a number of renders (120; KIN_RELEASE_FRAMES here makes it 5) and made again when it is on: the video memory
// (`Render3DStats::feature_bytes`) falls to nothing, a feature toggled faster than that never lets go, and what is drawn after a release is what a
// fresh view draws, to the byte (temporal anti-aliasing included: its history starts afresh as after reset_history()).
void test_gpu_feature_release() {
    with_gpu("test_gpu_feature_release", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const AoRoom room{r3d};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        setenv("KIN_RELEASE_FRAMES", "5", 1);
        const auto make_view = [&] { return r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(20, 24, 30)}); };
        kin::View3D view = make_view();
        kin::Scene3D scene;
        const auto render = [&](kin::View3D& target, bool ao, bool contact, bool taa, int frame) {
            scene.clear();
            scene.camera = kin::Camera3D::look_at({0.0f + 0.05f * static_cast<float>(frame), 2.0f, 6.0f}, {0.0f, 0.5f, -2.0f}, 60.0f);
            scene.lights.sun = {.direction = {-1.0f, 0.4f, 0.1f}, .color = {1.0f, 1.0f, 1.0f}};
            scene.lights.ambient = {0.4f, 0.4f, 0.45f};
            scene.lights.shadow_area = {{-10.0f, -0.2f, -10.0f}, {10.0f, 4.0f, 10.0f}};
            scene.ambient_occlusion = {.enabled = ao, .radius = 2.0f};
            scene.contact_shadows = {.enabled = contact};
            scene.taa = {.enabled = taa};
            room.draw_floor(scene);
            room.draw_wall(scene);
            room.draw_pole(scene, -0.5f, -1.0f);
            scene.draw(box, Mat4f::translation({1.0f + 0.1f * static_cast<float>(frame), 0.5f, 0.0f}), {.color = Color::rgb(200, 80, 80)});
            Pixels p = shoot_view(renderer, r3d, target, scene);
            renderer.present();
            return p;
        };
        render(view, false, false, false, 0);
        if (lacks(r3d.capabilities().ambient_occlusion && r3d.capabilities().temporal_aa, "ambient occlusion and temporal anti-aliasing")) {
            unsetenv("KIN_RELEASE_FRAMES");
            return;
        }
        assert(r3d.stats().feature_bytes == 0);
        // Everything on: targets are made.
        for (int f = 0; f < 3; ++f) {
            render(view, true, true, true, f);
        }
        const kin::u64 held = r3d.stats().feature_bytes;
        std::cerr << "release: all on holds " << held << " bytes at 160 x 120\n";
        assert(held > 160 * 120 * 4 * 6);
        // Off: kept for four renders, given back by the fifth.
        for (int f = 0; f < 4; ++f) {
            render(view, false, false, false, f);
            assert(r3d.stats().feature_bytes == held && r3d.stats().targets_released == 0);
        }
        render(view, false, false, false, 4);
        assert(r3d.stats().feature_bytes == 0 && r3d.stats().targets_released >= 4);
        render(view, false, false, false, 5);
        assert(r3d.stats().targets_released == 0 && r3d.stats().feature_bytes == 0);

        // On again: what a fresh view draws, frame by frame (the history starts afresh).
        kin::View3D fresh = make_view();
        for (int f = 0; f < 8; ++f) {
            const Pixels after = render(view, true, true, true, f);
            const Pixels truth = render(fresh, true, true, true, f);
            assert(after.rgba == truth.rgba);
        }
        assert(r3d.stats().feature_bytes == held);
        // Off for fewer renders than the limit and back on: the history was dropped the moment it was not used, so it too is a fresh start.
        for (int f = 0; f < 3; ++f) {
            render(view, false, false, false, f);
        }
        fresh = make_view();
        for (int f = 0; f < 8; ++f) {
            const Pixels after = render(view, true, true, true, f);
            const Pixels truth = render(fresh, true, true, true, f);
            assert(after.rgba == truth.rgba);
        }
        // Temporal anti-aliasing alone, off and on again over a release (the velocity target is the pre-pass's normals).
        for (int f = 0; f < 6; ++f) {
            render(view, false, false, false, f);
        }
        assert(r3d.stats().feature_bytes == 0);
        fresh = make_view();
        for (int f = 0; f < 8; ++f) {
            const Pixels after = render(view, false, false, true, f);
            const Pixels truth = render(fresh, false, false, true, f);
            assert(after.rgba == truth.rgba && r3d.stats().taa_main_velocity);
            assert(r3d.stats().targets_released == 0); // a feature in use is not given back, past the limit too
        }

        // Each feature on its own clock: with only the occlusion running, the contact shadows' targets go (and the temporal anti-aliasing's) while the
        // occlusion's stay; the bytes held are those of a fresh view that has only ever run the occlusion.
        {
            kin::View3D only_ao = make_view();
            render(only_ao, true, false, false, 0);
            const kin::u64 want = r3d.stats().feature_bytes;
            for (int f = 0; f < 3; ++f) {
                render(view, true, true, true, f);
            }
            assert(r3d.stats().feature_bytes > want);
            for (int f = 0; f < 8; ++f) {
                render(view, true, false, false, f);
            }
            assert(r3d.stats().feature_bytes == want);
            // And contact shadows back on: a fresh view's picture.
            kin::View3D twin = make_view();
            render(twin, true, false, false, 0);
            assert(render(view, true, true, false, 9).rgba == render(twin, true, true, false, 9).rgba);
        }
        // Toggled faster than the limit: never given back (and no new target is made once they exist).
        kin::u64 released = 0;
        for (int f = 0; f < 40; ++f) {
            const bool on = f % 2 == 0;
            render(view, on, on, on, f);
            released += r3d.stats().targets_released;
        }
        assert(released == 0 && r3d.stats().feature_bytes > 0);
        // Toggled every four renders (limit five): still never.
        for (int f = 0; f < 40; ++f) {
            const bool on = (f / 4) % 2 == 0;
            render(view, on, on, on, f);
            released += r3d.stats().targets_released;
        }
        assert(released == 0);
        // Toggled every six: given back each time (and made again each time, with the same pictures).
        kin::u64 gave_back = 0;
        for (int f = 0; f < 36; ++f) {
            const bool on = (f / 6) % 2 == 0;
            render(view, on, on, on, f);
            gave_back += r3d.stats().targets_released > 0 ? 1 : 0;
        }
        assert(gave_back >= 2);
        // Never (KIN_RELEASE_FRAMES=0): kept.
        setenv("KIN_RELEASE_FRAMES", "0", 1);
        render(view, true, true, true, 0);
        const kin::u64 kept = r3d.stats().feature_bytes;
        for (int f = 0; f < 20; ++f) {
            render(view, false, false, false, f);
        }
        assert(r3d.stats().feature_bytes == kept && kept > 0);
        unsetenv("KIN_RELEASE_FRAMES");
    });
}

// Occlusion culling's targets and the renderer's second gathered buffer go the same way, and the picture after is the one of a fresh view.
void test_gpu_occlusion_release() {
    with_gpu("test_gpu_occlusion_release", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (!occlusion_capable(r3d)) {
            return;
        }
        const OcclusionRoom room{r3d};
        setenv("KIN_RELEASE_FRAMES", "5", 1);
        const auto make_view = [&] { return r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(20, 24, 30)}); };
        kin::View3D view = make_view();
        kin::Scene3D scene;
        const auto render = [&](kin::View3D& target, bool occlusion) {
            room.fill(scene, hidden_balls);
            scene.camera = room_camera();
            scene.lod = {.enabled = true, .error_pixels = 0.1f};
            scene.lod.occlusion.enabled = occlusion;
            Pixels p = shoot_view(renderer, r3d, target, scene);
            renderer.present();
            return p;
        };
        render(view, false);
        const kin::u64 plain = r3d.stats().feature_bytes;
        for (int f = 0; f < 3; ++f) {
            render(view, true);
        }
        const kin::u64 held = r3d.stats().feature_bytes;
        std::cerr << "release: occlusion holds " << (held - plain) / 1024 << " KiB more than without\n";
        assert(r3d.stats().occlusion && held > plain + 256u * 1024);
        for (int f = 0; f < 4; ++f) {
            render(view, false);
            assert(r3d.stats().feature_bytes == held);
        }
        render(view, false);
        assert(r3d.stats().feature_bytes == plain && r3d.stats().targets_released >= 2);
        kin::View3D fresh = make_view();
        for (int f = 0; f < 4; ++f) {
            assert(render(view, true).rgba == render(fresh, true).rgba);
        }
        assert(r3d.stats().occlusion && r3d.stats().feature_bytes == held);
        // Toggled every other render: kept.
        kin::u64 released = 0;
        for (int f = 0; f < 30; ++f) {
            render(view, f % 2 == 0);
            released += r3d.stats().targets_released;
        }
        assert(released == 0);
        unsetenv("KIN_RELEASE_FRAMES");
    });
}

// Renderer3D::warm_up makes the pipelines (and the targets) a scene's first render would make, without drawing: the first render after it makes none
// (`Render3DStats::pipelines_made`), for every kind of draw and every opt-in pass, while without it the same render makes some (the control: a
// fresh device for each, so that no earlier test has made them).
void test_gpu_warm_up() {
    struct Config {
        const char* name;
        kin::u32 samples;
        bool hdr, pick, cascades, ao, contact, taa, custom, overlay, occlusion;
    };
    const std::vector<Config> configs{
        {"plain", 4, false, true, false, false, false, false, false, false, false},
        {"cascades, hdr", 1, true, false, true, false, false, false, true, true, false},
        {"taa", 1, false, false, false, false, false, true, true, true, false},
        {"taa, no custom shaders (the main pass writes the velocity)", 1, false, true, false, false, false, true, false, true, false},
        {"taa + ao + contact + cascades", 1, true, true, true, true, true, true, true, true, false},
        {"ao + contact", 4, false, false, false, true, true, false, false, true, false},
        {"occlusion", 1, false, false, false, false, false, false, false, false, true},
        {"occlusion + ao + taa", 1, false, false, true, true, false, true, false, false, true},
    };
    for (const Config& config : configs) {
        kin::u32 made[2] = {0, 0};
        for (const bool warm : {false, true}) {
            with_gpu("test_gpu_warm_up", [&](kin::Renderer2D& renderer) {
                kin::Renderer3D r3d{renderer};
                if (config.occlusion && !occlusion_capable(r3d)) {
                    made[warm] = warm ? 0 : 1;
                    return;
                }
                if ((config.taa && !r3d.capabilities().temporal_aa) || ((config.ao || config.contact) && !r3d.capabilities().ambient_occlusion)) {
                    made[warm] = warm ? 0 : 1;
                    return;
                }
                kin::MaterialShader3D flat;
                if (config.custom && kin::shader_compiler_available()) {
                    flat = r3d.compile_material_shader(std::filesystem::path{KIN_TEST_MATERIAL_SHADERS} / "custom_flat.frag.glsl");
                }
                const AoRoom room{r3d};
                const OcclusionRoom clustered{r3d};
                const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
                const kin::Mesh leaf_mesh = r3d.create_mesh(uv_square(1.0f));
                const kin::u8 texels[8] = {20, 200, 20, 0, 20, 200, 20, 255};
                const kin::Texture leaf = renderer.create_texture_from_rgba(texels, {2, 1});
                kin::View3D view = r3d.create_view({160, 120}, {.samples = config.samples, .hdr = config.hdr, .pick = config.pick});
                kin::Scene3D scene;
                if (config.occlusion) {
                    clustered.fill(scene, hidden_balls);
                    scene.camera = room_camera();
                    scene.lod = {.enabled = true, .error_pixels = 0.1f};
                    scene.lod.occlusion.enabled = true;
                } else {
                    scene.camera = kin::Camera3D::look_at({0.0f, 2.0f, 6.0f}, {0.0f, 0.5f, -2.0f}, 60.0f);
                    room.draw_floor(scene);
                    room.draw_wall(scene);
                    scene.draw(box, Mat4f::translation({1.0f, 0.5f, 0.0f}), {.color = Color::rgb(200, 80, 80), .double_sided = true});
                    scene.draw(leaf_mesh, Mat4f::translation({-1.0f, 1.0f, 0.5f}), {.double_sided = true, .blend = kin::Blend3D::Mask, .texture = leaf});
                    scene.draw(box, Mat4f::translation({0.0f, 1.0f, 1.0f}), {.color = Color::rgba(60, 200, 255, 140), .unlit = true, .blend = kin::Blend3D::Alpha});
                    scene.draw(box, Mat4f::translation({2.0f, 1.5f, 1.0f}), {.color = Color::rgb(60, 60, 60), .unlit = true, .blend = kin::Blend3D::Additive});
                    scene.draw(box, Mat4f::translation({-2.0f, 0.5f, 0.0f}), {.color = Color::rgb(80, 255, 80), .wireframe = true});
                    if (flat.valid()) {
                        kin::Material3D opaque_custom{.shader = flat};
                        scene.draw(box, Mat4f::translation({3.0f, 0.5f, 0.0f}), opaque_custom);
                        kin::Material3D blended_custom{.blend = kin::Blend3D::Alpha, .shader = flat};
                        scene.draw(box, Mat4f::translation({-3.0f, 0.5f, 1.0f}), blended_custom);
                    }
                    if (config.overlay) {
                        scene.overlay().camera = kin::Camera3D::look_at({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, 40.0f);
                        scene.overlay().draw(box, Mat4f::translation({1.5f, -1.5f, 0.0f}), {.color = Color::rgb(200, 40, 40)});
                    }
                }
                scene.lights.sun = {.direction = {-1.0f, 0.5f, 0.1f}, .color = {1.0f, 1.0f, 1.0f}};
                scene.lights.shadow_area = {{-30.0f, -0.2f, -30.0f}, {30.0f, 8.0f, 30.0f}};
                scene.lights.shadow_cascades = config.cascades ? 3 : 1;
                scene.ambient_occlusion = {.enabled = config.ao};
                scene.contact_shadows = {.enabled = config.contact};
                scene.taa = {.enabled = config.taa, .sharpness = 0.5f};
                if (warm) {
                    assert(r3d.warm_up(view, scene));
                    // Nothing was drawn by it.
                    assert(r3d.stats().draws == 0);
                }
                renderer.clear(kin::colors::black);
                render_ok(r3d, view, scene);
                renderer.present();
                made[warm] = r3d.stats().pipelines_made;
                // And the second render makes none either way.
                render_ok(r3d, view, scene);
                renderer.present();
                assert(r3d.stats().pipelines_made == 0);
            });
        }
        std::cerr << "warm_up, " << config.name << ": first render makes " << made[0] << " pipelines without, " << made[1] << " after\n";
        assert(made[0] > 0 && made[1] == 0);
    }
}

// --- Skinned meshes (P1a, part 1) ---

// How many pixels differ between two pictures by more than `tolerance` levels in some channel.
int pixels_differing(const Pixels& a, const Pixels& b, int tolerance = 4) {
    assert(a.rgba.size() == b.rgba.size());
    int count = 0;
    for (std::size_t i = 0; i + 3 < a.rgba.size(); i += 4) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            worst = std::max(worst, std::abs(int(a.rgba[i + static_cast<std::size_t>(c)]) - int(b.rgba[i + static_cast<std::size_t>(c)])));
        }
        count += worst > tolerance ? 1 : 0;
    }
    return count;
}

// How many pixels are not black.
int pixels_lit(const Pixels& p) {
    int count = 0;
    for (std::size_t i = 0; i + 3 < p.rgba.size(); i += 4) {
        count += (p.rgba[i] + p.rgba[i + 1] + p.rgba[i + 2]) > 12 ? 1 : 0;
    }
    return count;
}

// Writes a picture for a person to look at, when KIN_TEST_DUMP names a folder.
void dump(const Pixels& p, const std::string& name) {
    const char* folder = std::getenv("KIN_TEST_DUMP");
    if (!folder || !folder[0]) {
        return;
    }
    kin::Image image;
    image.size = p.size;
    image.rgba = p.rgba;
    kin::save_image(image, std::filesystem::path{folder} / (name + ".png"));
}

// The bent tube of skin_fixtures as a skinned mesh, and the same posed on the CPU as a plain one.
struct SkinRig {
    explicit SkinRig(kin::Renderer3D& r3d, int rings = 25, int segments = 12, float length = 3.0f, float radius = 0.35f)
        : tube(skin_fixtures::make_tube(rings, segments, length, radius)), mesh(r3d.create_mesh(tube.mesh)) {
        assert(mesh.valid() && mesh.skinned() && mesh.skin_joint_count() == 3);
    }

    std::vector<Mat4f> matrices(float a, float b, Vec3f base = {}) const {
        std::vector<Mat4f> m;
        kin::compute_joint_matrices(tube.skeleton, skin_fixtures::bent(tube, a, b, base), m);
        return m;
    }
    // The tube posed by `m` on the CPU, as a plain mesh.
    kin::Mesh baked(kin::Renderer3D& r3d, std::span<const Mat4f> m) const { return r3d.create_mesh(kin::skin_mesh(tube.mesh, m)); }

    skin_fixtures::Tube tube;
    kin::Mesh mesh;
};

kin::Camera3D skin_camera(bool orthographic) {
    kin::Camera3D camera = kin::Camera3D::look_at({0.0f, 1.8f, 8.0f}, {0.0f, 1.4f, 0.0f}, 50.0f);
    if (orthographic) {
        camera.projection = kin::Projection3D::Orthographic;
        camera.height = 6.5f;
    }
    return camera;
}

void skin_lights(kin::Scene3D& scene, kin::u32 cascades) {
    scene.lights.sun = {.direction = {0.9f, 1.4f, 0.6f}, .color = {1.0f, 1.0f, 1.0f}};
    scene.lights.ambient = {0.15f, 0.15f, 0.18f};
    scene.lights.shadow_area = kin::Bounds3{{-8.0f, -0.3f, -8.0f}, {8.0f, 6.0f, 8.0f}};
    scene.lights.shadow_cascades = cascades;
    scene.lights.shadow_distance = 30.0f;
}

// A skinned copy is the picture of the same mesh skinned on the CPU and drawn as a plain one: perspective and orthographic, with one
// shadow map and with three cascades, lit (the normals are skinned too) and shadowed (the shadow pass poses the vertex).
void test_gpu_skinned_matches_cpu() {
    with_gpu("test_gpu_skinned_matches_cpu", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {16.0f, 0.2f, 16.0f});
        SkinRig rig{r3d};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const kin::Material3D grey{.color = Color::rgb(200, 200, 200)};
        const kin::Material3D red{.color = Color::rgb(210, 70, 60)};
        int worst = 0;
        for (const bool ortho : {false, true}) {
            for (const kin::u32 cascades : {1u, 3u}) {
                kin::Scene3D scene;
                scene.camera = skin_camera(ortho);
                skin_lights(scene, cascades);
                const std::vector<Mat4f> m = rig.matrices(70.0f, -60.0f, {0.3f, 0.0f, 0.0f});
                const Mat4f place = Mat4f::translation({-0.2f, 0.0f, 0.4f}) * Mat4f::rotation_y(25.0f);
                const kin::Mesh baked = rig.baked(r3d, m);
                const auto shoot = [&](bool skinned) {
                    scene.clear();
                    scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), grey);
                    if (skinned) {
                        scene.draw_skinned(rig.mesh, m, place, red);
                    } else {
                        scene.draw(baked, place, red);
                    }
                    Pixels p = shoot_view(renderer, r3d, view, scene);
                    renderer.present();
                    return p;
                };
                const Pixels gpu = shoot(true);
                const kin::Render3DStats stats = r3d.stats();
                assert(stats.skinned_copies == 1 && stats.skin_matrices == 3 && stats.skinned_draws == 1);
                assert(stats.shadow_cascades == cascades && stats.shadow_casters >= cascades + 0u);
                const Pixels cpu = shoot(false);
                assert(r3d.stats().skinned_copies == 0 && r3d.stats().skin_matrices == 0 && r3d.stats().skinned_draws == 0);
                const int differing = pixels_differing(gpu, cpu);
                worst = std::max(worst, differing);
                dump(gpu, std::string{"skinned_"} + (ortho ? "ortho" : "persp") + "_c" + std::to_string(cascades) + "_gpu");
                dump(cpu, std::string{"skinned_"} + (ortho ? "ortho" : "persp") + "_c" + std::to_string(cascades) + "_cpu");
                // The tube is there, and so is its shadow on the floor (the picture is not the floor alone).
                assert(pixels_lit(gpu) > 3000);
                const Pixels lone = [&] {
                    scene.clear();
                    scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), grey);
                    return shoot_view(renderer, r3d, view, scene);
                }();
                renderer.present();
                assert(pixels_differing(gpu, lone) > 400);
                if (differing > 12) {
                    std::cerr << "skinned vs CPU: " << differing << " pixels differ (ortho " << ortho << ", cascades " << cascades << ")\n";
                    assert(false);
                }
            }
        }
        std::cerr << "skinned vs CPU skinning: at most " << worst << " of 19200 pixels differ by more than 4 levels\n";

        // A skinned mesh drawn with draw(), as any mesh, is the mesh at rest.
        kin::Scene3D scene;
        scene.camera = skin_camera(false);
        skin_lights(scene, 1);
        scene.draw(rig.mesh, Mat4f::identity(), red);
        const Pixels plain = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        assert(r3d.stats().skinned_copies == 0);
        scene.clear();
        scene.draw_skinned(rig.mesh, rig.matrices(0.0f, 0.0f), Mat4f::identity(), red);
        const Pixels rest = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        assert(pixels_differing(plain, rest) <= 12);
    });
}

// The shadow of a posed copy that the camera does not see (it is above the view; only its shadow is) is the shadow of the same mesh
// skinned on the CPU: the casters only the sun sees take the posed bound, and the shadow pass poses the vertices.
void test_gpu_skinned_shadow_out_of_view() {
    with_gpu("test_gpu_skinned_shadow_out_of_view", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {20.0f, 0.2f, 20.0f});
        SkinRig rig{r3d};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        for (const kin::u32 cascades : {1u, 3u}) {
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 12.0f, 0.01f}, {0.0f, 0.0f, 0.0f}, 60.0f); // looking down; the tube is behind it
            scene.lights.sun = {.direction = {1.0f, 2.0f, 0.3f}, .color = {1.0f, 1.0f, 1.0f}};
            scene.lights.ambient = {0.15f, 0.15f, 0.15f};
            scene.lights.shadow_area = kin::Bounds3{{-10.0f, -0.2f, -10.0f}, {10.0f, 22.0f, 10.0f}};
            scene.lights.shadow_cascades = cascades;
            scene.lights.shadow_distance = 40.0f;
            const std::vector<Mat4f> m = rig.matrices(80.0f, -70.0f);
            const kin::Mesh baked = rig.baked(r3d, m);
            const Mat4f high = Mat4f::translation({0.0f, 14.0f, 0.0f});
            const auto shoot = [&](bool skinned) {
                scene.clear();
                scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
                if (skinned) {
                    scene.draw_skinned(rig.mesh, m, high);
                } else {
                    scene.draw(baked, high);
                }
                Pixels p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                return p;
            };
            const Pixels gpu = shoot(true);
            const kin::Render3DStats stats = r3d.stats();
            assert(stats.skinned_copies == 0 && stats.culled == 1);        // the camera does not see it
            assert(stats.shadow_casters >= 1 && stats.shadow_draws >= 1);  // the sun does
            const Pixels cpu = shoot(false);
            assert(r3d.stats().shadow_casters >= 1);
            const int differing = pixels_differing(gpu, cpu);
            std::cerr << "skinned shadow out of view (" << cascades << " cascades): " << differing << " pixels differ\n";
            dump(gpu, "skinned_shadow_c" + std::to_string(cascades));
            assert(differing <= 12);
            // The shadow is there: the floor is not all lit.
            int dark = 0, brightest = 0;
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    brightest = std::max(brightest, brightness(gpu.at(x, y)));
                }
            }
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    dark += brightness(gpu.at(x, y)) < brightest * 7 / 10 ? 1 : 0;
                }
            }
            std::cerr << "shadow pixels " << dark << " of brightest " << brightest << "\n";
            assert(dark > 60);
            // And it is the bent tube's shadow, not the straight one's.
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
            scene.draw_skinned(rig.mesh, rig.matrices(0.0f, 0.0f), high);
            const Pixels straight = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            assert(pixels_differing(gpu, straight) > 60);
        }
    });
}

// A cut-out material's skinned copy casts the cut-out shadow (the shadow_mask variant poses the vertex too).
void test_gpu_skinned_cutout_shadow() {
    with_gpu("test_gpu_skinned_cutout_shadow", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {16.0f, 0.2f, 16.0f});
        SkinRig rig{r3d};
        // Stripes round the tube: alternate columns of the texture clear (u runs round it).
        const kin::u8 texels[16] = {40, 200, 40, 255, 40, 200, 40, 0, 40, 200, 40, 255, 40, 200, 40, 0};
        const kin::Texture stripes = renderer.create_texture_from_rgba(texels, {4, 1});
        renderer.set_scale_mode(stripes, kin::ScaleMode::Nearest);
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = skin_camera(false);
        skin_lights(scene, 1);
        const std::vector<Mat4f> m = rig.matrices(60.0f, -50.0f);
        const kin::Mesh baked = rig.baked(r3d, m);
        const kin::Material3D leafy{.color = Color::rgb(255, 255, 255), .double_sided = true, .blend = kin::Blend3D::Mask, .texture = stripes};
        const auto shoot = [&](bool skinned) {
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
            if (skinned) {
                scene.draw_skinned(rig.mesh, m, Mat4f::identity(), leafy);
            } else {
                scene.draw(baked, Mat4f::identity(), leafy);
            }
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const Pixels gpu = shoot(true);
        assert(r3d.stats().skinned_copies == 1 && r3d.stats().shadow_casters >= 1);
        const Pixels cpu = shoot(false);
        const int differing = pixels_differing(gpu, cpu);
        std::cerr << "skinned cut-out: " << differing << " pixels differ\n";
        dump(gpu, "skinned_cutout");
        assert(differing <= 12);
        // The cut-outs are cut: the picture is not the solid tube's.
        const kin::Material3D solid{.color = Color::rgb(255, 255, 255), .double_sided = true};
        scene.clear();
        scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
        scene.draw_skinned(rig.mesh, m, Mat4f::identity(), solid);
        const Pixels full = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        assert(pixels_differing(full, gpu) > 300);
    });
}

// Copies of one mesh in different poses in one frame (each has its own first matrix in the side buffer), with a plain mesh between
// two skinned ones, and a run of skinned copies drawn as one.
void test_gpu_skinned_poses_and_mix() {
    with_gpu("test_gpu_skinned_poses_and_mix", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {20.0f, 0.2f, 20.0f});
        const kin::Mesh box = make_box(r3d, {0.8f, 0.8f, 0.8f});
        SkinRig rig{r3d};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 2.5f, 11.0f}, {0.0f, 1.2f, 0.0f}, 50.0f);
        skin_lights(scene, 1);
        const std::vector<std::vector<Mat4f>> poses = {rig.matrices(70.0f, 0.0f), rig.matrices(-50.0f, 60.0f), rig.matrices(0.0f, 0.0f),
                                                      rig.matrices(110.0f, -80.0f), rig.matrices(20.0f, 20.0f)};
        const std::vector<float> xs = {-5.0f, -2.5f, 0.0f, 2.5f, 5.0f};
        const kin::Material3D red{.color = Color::rgb(210, 70, 60)};
        std::vector<kin::Mesh> baked;
        for (const auto& pose : poses) {
            baked.push_back(rig.baked(r3d, pose));
        }
        // 0, 1: a run; a box; 2; the box again; 3, 4.
        const auto build = [&](bool skinned) {
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
            for (std::size_t i = 0; i < poses.size(); ++i) {
                if (i == 2 || i == 3) {
                    scene.draw(box, Mat4f::translation({xs[i] - 1.2f, 0.4f, 1.5f}), {.color = Color::rgb(60, 60, 220)});
                }
                const Mat4f at = Mat4f::translation({xs[i], 0.0f, 0.0f});
                if (skinned) {
                    scene.draw_skinned(rig.mesh, poses[i], at, red);
                } else {
                    scene.draw(baked[i], at, red);
                }
            }
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const Pixels gpu = build(true);
        const kin::Render3DStats stats = r3d.stats();
        assert(stats.skinned_copies == 5 && stats.skin_matrices == 15);
        assert(stats.skinned_draws == 3); // the run of two, then two single ones apart by a box
        assert(scene.draws().size() == 6); // floor, a run of two, a box, one, a box, a run of two
        const Pixels cpu = build(false);
        const int differing = pixels_differing(gpu, cpu);
        std::cerr << "five poses of one mesh in a frame: " << differing << " pixels differ from the CPU-skinned copies\n";
        dump(gpu, "skinned_poses");
        assert(differing <= 20);
        assert(pixels_lit(gpu) > 3000);
    });
}

// A posed copy is culled by where its pose puts it, not where the mesh rests: one wholly out of view is not drawn; an arm that swings
// into view from above (outside the rest pose's bound) is; a copy in view is drawn at any pose.
void test_gpu_skinned_culling() {
    with_gpu("test_gpu_skinned_culling", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        SkinRig arm{r3d, 25, 12, 6.0f};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f}, 50.0f);
        scene.lights.ambient = {0.6f, 0.6f, 0.6f};
        const kin::Material3D red{.color = Color::rgb(220, 60, 50), .unlit = true};
        const Mat4f shoulder = Mat4f::translation({0.0f, 8.0f, 0.0f});
        const auto shoot = [&](const std::vector<Mat4f>& m, const Mat4f& at) {
            scene.clear();
            scene.draw_skinned(arm.mesh, m, at, red);
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        // Rest: standing up from y = 8, to 14: above the view (which reaches y = 4.7 at the origin's depth).
        Pixels p = shoot(arm.matrices(0.0f, 0.0f), shoulder);
        assert(r3d.stats().instances == 0 && r3d.stats().culled == 1 && r3d.stats().skinned_copies == 0 && pixels_lit(p) == 0);
        // The whole arm swung down by 180 degrees about Z at the shoulder hangs from y = 8 to 2: its end is in view.
        std::vector<Mat4f> m = arm.matrices(0.0f, 0.0f);
        {
            kin::Pose pose = arm.tube.skeleton.rest_pose();
            pose.joints[0].rotation = kin::Quatf::axis_angle({0.0f, 0.0f, 1.0f}, 180.0f);
            kin::compute_joint_matrices(arm.tube.skeleton, pose, m);
        }
        p = shoot(m, shoulder);
        assert(r3d.stats().instances == 1 && r3d.stats().culled == 0 && r3d.stats().skinned_copies == 1);
        const auto at = [&](Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        expect(at({0.0f, 3.5f, 0.0f}), Color::rgb(220, 60, 50), "the arm that swung into view", 6);
        assert(pixels_lit(p) > 100);
        // Folded halfway (an elbow): the end comes down to y = 8 - 3 - 3 = 2 too; and the pose that puts it far to the side is culled.
        {
            kin::Pose pose = arm.tube.skeleton.rest_pose();
            pose.joints[0].rotation = kin::Quatf::axis_angle({0.0f, 0.0f, 1.0f}, -90.0f);
            kin::compute_joint_matrices(arm.tube.skeleton, pose, m);
        }
        p = shoot(m, Mat4f::translation({-9.0f, 0.0f, 0.0f})); // lying along +x from x = -9 to -3 (the view reaches -6.2 at z = 0): half in
        assert(r3d.stats().instances == 1 && pixels_lit(p) > 100);
        p = shoot(m, Mat4f::translation({-30.0f, 0.0f, 0.0f})); // wholly out
        assert(r3d.stats().instances == 0 && r3d.stats().culled == 1 && pixels_lit(p) == 0);
        p = shoot(arm.matrices(30.0f, 30.0f), Mat4f::translation({0.0f, 0.0f, 40.0f})); // behind the camera
        assert(r3d.stats().instances == 0 && r3d.stats().culled == 1);

        // In view at any pose: never culled, and drawn.
        SkinRig tube{r3d};
        scene.camera = kin::Camera3D::look_at({0.0f, 1.5f, 16.0f}, {0.0f, 1.5f, 0.0f}, 60.0f);
        std::mt19937 rng{99};
        std::uniform_real_distribution<float> angle(-150.0f, 150.0f), shift(-1.0f, 1.0f);
        for (int i = 0; i < 40; ++i) {
            const std::vector<Mat4f> pose = tube.matrices(angle(rng), angle(rng), {shift(rng), shift(rng), shift(rng)});
            scene.clear();
            scene.draw_skinned(tube.mesh, pose, Mat4f::identity(), red);
            p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            assert(r3d.stats().skinned_copies == 1 && r3d.stats().culled == 0);
            if (pixels_lit(p) <= 30) {
                std::cerr << "pose " << i << " draws only " << pixels_lit(p) << " pixels\n";
                assert(false);
            }
        }
        // Several copies, some in and some out, each by its own pose: three in a row along x, the outer two out of view, the middle in; the
        // left one folded over so that it swings in.
        scene.clear();
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f}, 50.0f);
        std::vector<Mat4f> flat = arm.matrices(0.0f, 0.0f);
        kin::Pose lying = arm.tube.skeleton.rest_pose();
        lying.joints[0].rotation = kin::Quatf::axis_angle({0.0f, 0.0f, 1.0f}, -90.0f);
        std::vector<Mat4f> lying_m;
        kin::compute_joint_matrices(arm.tube.skeleton, lying, lying_m);
        scene.draw_skinned(arm.mesh, flat, Mat4f::translation({-12.0f, 0.0f, 0.0f}), red);   // standing, far left: out
        scene.draw_skinned(arm.mesh, lying_m, Mat4f::translation({-9.5f, 0.0f, 0.0f}), red); // lying toward +x: in
        scene.draw_skinned(arm.mesh, flat, Mat4f::translation({0.0f, -3.0f, 0.0f}), red);   // standing in the middle: in
        scene.draw_skinned(arm.mesh, flat, Mat4f::translation({12.0f, 0.0f, 0.0f}), red);   // far right: out
        render_ok(r3d, view, scene);
        assert(r3d.stats().skinned_copies == 2 && r3d.stats().culled == 2 && r3d.stats().instances == 2);
        renderer.present();
    });
}

// A scene with no skinned copy gives the picture it gave and counts none; one with a skinned copy far out of view changes nothing in the
// picture of the rest.
void test_gpu_skinned_off_path() {
    with_gpu("test_gpu_skinned_off_path", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {16.0f, 0.2f, 16.0f});
        const kin::Mesh box = make_box(r3d, {1.0f, 2.0f, 1.0f});
        SkinRig rig{r3d};
        kin::View3D view = r3d.create_view({160, 120});
        kin::Scene3D scene;
        scene.camera = skin_camera(false);
        skin_lights(scene, 1);
        const auto build = [&](bool with_skinned) {
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
            scene.draw(box, Mat4f::translation({1.0f, 1.0f, 0.0f}), {.color = Color::rgb(60, 200, 90)});
            if (with_skinned) {
                scene.draw_skinned(rig.mesh, rig.matrices(40.0f, 40.0f), Mat4f::translation({200.0f, 0.0f, 0.0f}));
            }
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const Pixels plain = build(false);
        const kin::Render3DStats before = r3d.stats();
        assert(before.skinned_copies == 0 && before.skin_matrices == 0 && before.skinned_draws == 0);
        const Pixels with = build(true);
        const kin::Render3DStats after = r3d.stats();
        assert(after.skin_matrices == 3 && after.skinned_copies == 0 && after.culled == before.culled + 1);
        assert(plain.rgba == with.rgba);
        assert(after.draws == before.draws && after.instances == before.instances && after.shadow_draws == before.shadow_draws);
        // A mesh made with a skin and drawn plain is an ordinary mesh: its levels and clusters are refused (logged), and it still draws.
        const kin::Mesh refused = r3d.create_mesh(rig.tube.mesh, {.levels = 3, .clusters = true});
        assert(refused.valid() && refused.skinned() && refused.level_count() == 1 && !refused.has_clusters());
        // draw_skinned with too few matrices draws nothing; with a mesh that has no skin it is draw().
        scene.clear();
        const std::vector<Mat4f> two(2);
        scene.draw_skinned(rig.mesh, two, Mat4f::identity());
        assert(scene.draws().empty() && scene.joint_matrices().empty());
        scene.draw_skinned(box, two, Mat4f::identity());
        assert(scene.draws().size() == 1 && !scene.draws()[0].skinned && scene.joint_matrices().empty());
        scene.draw_skinned(rig.mesh, rig.matrices(0, 0), Mat4f::identity());
        assert(scene.draws().size() == 2 && scene.draws()[1].skinned && scene.joint_matrices().size() == 3 && scene.skin_offsets().size() == 1);
        assert(scene.skin_offsets()[scene.draws()[1].skin_first] == 0);
        scene.draw_skinned(rig.mesh, rig.matrices(10, 10), Mat4f::identity()); // the next copy of the mesh joins the draw, with its own first matrix
        assert(scene.draws().size() == 2 && scene.draws()[1].count == 2 && scene.skin_offsets().size() == 2 && scene.joint_matrices().size() == 6);
        assert(scene.skin_offsets()[scene.draws()[1].skin_first + 1] == 3);
        scene.draw(rig.mesh, Mat4f::identity()); // the same mesh drawn plain after: not merged into the skinned draw
        assert(scene.draws().size() == 3 && !scene.draws()[2].skinned);
        scene.clear();
        assert(scene.joint_matrices().empty() && scene.skin_offsets().empty() && scene.draws().empty());
    });
}

// Skinned copies on worker threads give the picture of the serial run, byte for byte; a thousand-odd copies in different poses, the joint
// buffer growing as they do and shrinking back to a few.
void test_gpu_skinned_on_workers() {
    with_gpu("test_gpu_skinned_on_workers", [](kin::Renderer2D& renderer) {
        kin::JobSystem jobs;
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {80.0f, 0.2f, 80.0f});
        SkinRig rig{r3d, 9, 8};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const auto build = [&](kin::Scene3D& scene, int count) {
            scene.camera = kin::Camera3D::look_at({0.0f, 30.0f, 26.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
            skin_lights(scene, 1);
            scene.lights.shadow_area = kin::Bounds3{{-40.0f, -0.3f, -40.0f}, {40.0f, 8.0f, 40.0f}};
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
            const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count))));
            for (int i = 0; i < count; ++i) {
                const float a = static_cast<float>((i * 37) % 160) - 80.0f, b = static_cast<float>((i * 91) % 160) - 80.0f;
                const std::vector<Mat4f> m = rig.matrices(a, b, {0.0f, 0.0f, 0.0f});
                const float x = (static_cast<float>(i % side) - static_cast<float>(side) * 0.5f) * (60.0f / static_cast<float>(side));
                const float z = (static_cast<float>(i / side) - static_cast<float>(side) * 0.5f) * (60.0f / static_cast<float>(side));
                scene.draw_skinned(rig.mesh, m, Mat4f::translation({x, 0.0f, z}) * Mat4f::scaling({0.5f, 0.5f, 0.5f}), {.color = Color::rgb(200, 90, 70)});
            }
        };
        for (const int count : {2600, 40, 3000, 1}) {
            kin::Scene3D scene;
            build(scene, count);
            renderer.set_job_system(nullptr);
            const Pixels serial = shoot_view(renderer, r3d, view, scene);
            const kin::Render3DStats s1 = r3d.stats();
            renderer.present();
            renderer.set_job_system(&jobs);
            const Pixels parallel = shoot_view(renderer, r3d, view, scene);
            const kin::Render3DStats s2 = r3d.stats();
            renderer.present();
            renderer.set_job_system(nullptr);
            assert(serial.rgba == parallel.rgba);
            assert(s1.skinned_copies == s2.skinned_copies && s1.culled == s2.culled && s1.skin_matrices == s2.skin_matrices);
            assert(s1.skin_matrices == static_cast<kin::u32>(count) * 3 && s1.skinned_copies > 0);
            assert(pixels_lit(serial) > 2000);
            if (count == 2600) {
                dump(serial, "skinned_crowd");
            }
        }
    });
}

// A skinned copy with ambient occlusion, contact shadows, temporal anti-aliasing and pick on is in every pass (the pre-pass, the velocity,
// the pick pass): counted in the pre-pass's instances, named by pick, and the frames are whole. (The pictures' details are in the
// P1a part 2 tests below.)
void test_gpu_skinned_with_features() {
    with_gpu("test_gpu_skinned_with_features", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {16.0f, 0.2f, 16.0f});
        SkinRig rig{r3d};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 4, .pick = true});
        kin::Scene3D scene;
        scene.camera = skin_camera(false);
        skin_lights(scene, 1);
        const std::vector<Mat4f> m = rig.matrices(60.0f, -40.0f);
        const auto build = [&] {
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)}, kin::colors::white, 3);
            scene.draw_skinned(rig.mesh, m, Mat4f::translation({0.0f, 0.5f, 0.0f}), {.color = Color::rgb(210, 70, 60)}, kin::colors::white, 8);
        };
        build();
        const Pixels plain = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const auto pixel = [&](Vec3f w) {
            const kin::Vec2f p = scene.camera.project(w, screen)->pos;
            return kin::Vec2i{static_cast<int>(p.x), static_cast<int>(p.y)};
        };
        // Pick: the tube's own pixel names the tube.
        const kin::PickResult over = r3d.pick(view, kin::Vec2i{80, 60});
        assert(over.hit && over.id == 8);
        if (r3d.capabilities().ambient_occlusion) {
            scene.ambient_occlusion.enabled = true;
            scene.contact_shadows.enabled = true;
            build();
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            const kin::Render3DStats stats = r3d.stats();
            renderer.present();
            assert(stats.skinned_copies == 1 && stats.ao_instances == 2); // the floor and the tube are in the pre-pass
            assert(pixels_differing(p, plain, 30) < 3000 && pixels_lit(p) > 3000);
            // The tube is drawn all the same.
            scene.clear();
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)}, kin::colors::white, 3);
            const Pixels lone = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            assert(pixels_differing(p, lone, 20) > 500);
            scene.ambient_occlusion.enabled = false;
            scene.contact_shadows.enabled = false;
        }
        if (r3d.capabilities().temporal_aa) {
            scene.taa.enabled = true;
            for (int frame = 0; frame < 6; ++frame) {
                build();
                const Pixels p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                assert(r3d.stats().skinned_copies == 1 && r3d.stats().taa_main_velocity && pixels_lit(p) > 3000);
                if (frame == 5) {
                    dump(p, "skinned_taa");
                }
            }
            // With the pre-pass (ambient occlusion too): still drawn.
            if (r3d.capabilities().ambient_occlusion) {
                scene.ambient_occlusion.enabled = true;
                build();
                const Pixels p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                assert(r3d.stats().skinned_copies == 1 && !r3d.stats().taa_main_velocity && pixels_lit(p) > 3000);
            }
        }
    });
}

// A skinned copy in the overlay (a first-person arm) has its own joint matrices in the frame's buffer after the scene's: both are right,
// each in front of or behind as the overlay says.
void test_gpu_skinned_overlay() {
    with_gpu("test_gpu_skinned_overlay", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        SkinRig rig{r3d};
        const kin::Mesh wall = make_box(r3d, {12.0f, 12.0f, 0.2f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 1.5f, 9.0f}, {0.0f, 1.5f, 0.0f}, 50.0f);
        scene.lights.ambient = {0.7f, 0.7f, 0.7f};
        const kin::Material3D red{.color = Color::rgb(220, 60, 50), .unlit = true};
        const kin::Material3D blue{.color = Color::rgb(50, 60, 220), .unlit = true};
        // The world: a tube bent one way, in front of a wall. The overlay: a tube bent the other way, near the camera's left side.
        const std::vector<Mat4f> world = rig.matrices(60.0f, 0.0f), arm = rig.matrices(-70.0f, 20.0f);
        const auto build = [&](bool skinned) {
            scene.clear();
            scene.overlay().camera = scene.camera;
            scene.draw(wall, Mat4f::translation({0.0f, 0.0f, -2.0f}), {.color = Color::rgb(90, 90, 90), .unlit = true});
            const kin::Mesh baked_world = rig.baked(r3d, world), baked_arm = rig.baked(r3d, arm);
            const Mat4f at = Mat4f::translation({1.5f, 0.0f, 0.0f}), arm_at = Mat4f::translation({-1.0f, 0.0f, 6.0f});
            if (skinned) {
                scene.draw_skinned(rig.mesh, world, at, red);
                scene.overlay().draw_skinned(rig.mesh, arm, arm_at, blue);
            } else {
                scene.draw(baked_world, at, red);
                scene.overlay().draw(baked_arm, arm_at, blue);
            }
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const Pixels gpu = build(true);
        assert(r3d.stats().skinned_copies == 2 && r3d.stats().skin_matrices == 6);
        const Pixels cpu = build(false);
        const int differing = pixels_differing(gpu, cpu);
        std::cerr << "skinned copies in the scene and its overlay: " << differing << " pixels differ\n";
        dump(gpu, "skinned_overlay");
        assert(differing <= 12 && pixels_lit(gpu) > 3000);
    });
}

// Many copies in different poses (the crowd of a game): a few thousand triangles each, with and without shadows; the cost goes to the log.
void test_gpu_skinned_many_copies() {
    with_gpu("test_gpu_skinned_many_copies", [](kin::Renderer2D& renderer) {
        kin::JobSystem jobs;
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {80.0f, 0.2f, 80.0f});
        SkinRig rig{r3d, 60, 24}; // 1,416 vertices a ring set: about 2,800 triangles
        std::cerr << "many copies: the mesh has " << rig.mesh.triangle_count() << " triangles\n";
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 36.0f, 48.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        skin_lights(scene, 1);
        scene.lights.shadow_area = kin::Bounds3{{-40.0f, -0.3f, -40.0f}, {40.0f, 8.0f, 40.0f}};
        std::vector<std::vector<Mat4f>> poses;
        for (int i = 0; i < 200; ++i) {
            poses.push_back(rig.matrices(static_cast<float>((i * 53) % 150) - 75.0f, static_cast<float>((i * 29) % 140) - 70.0f));
        }
        const auto build = [&](bool shadows) {
            scene.clear();
            scene.lights.shadow_area = shadows ? kin::Bounds3{{-40.0f, -0.3f, -40.0f}, {40.0f, 8.0f, 40.0f}} : kin::Bounds3{};
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
            for (int i = 0; i < 200; ++i) {
                const float x = (static_cast<float>(i % 20) - 9.5f) * 3.0f, z = (static_cast<float>(i / 20) - 4.5f) * 4.0f;
                scene.draw_skinned(rig.mesh, poses[static_cast<std::size_t>(i)], Mat4f::translation({x, 0.0f, z}) * Mat4f::scaling({0.8f, 0.8f, 0.8f}),
                                   {.color = Color::rgb(200, 90, 70)});
            }
        };
        for (const bool shadows : {false, true}) {
            build(shadows);
            for (int warm = 0; warm < 5; ++warm) {
                shoot_view(renderer, r3d, view, scene);
                renderer.present();
            }
            std::vector<double> cpu;
            for (int i = 0; i < 40; ++i) {
                render_ok(r3d, view, scene);
                cpu.push_back(r3d.stats().cpu_ms);
                renderer.present();
            }
            std::sort(cpu.begin(), cpu.end());
            const kin::Render3DStats stats = r3d.stats();
            std::cerr << "200 skinned copies, " << stats.triangles << " triangles drawn, " << (shadows ? "with" : "without") << " shadows: render() CPU median "
                      << cpu[cpu.size() / 2] << " ms (min " << cpu.front() << "), " << stats.skinned_copies << " copies, " << stats.skinned_draws
                      << " draws, " << stats.skin_matrices << " matrices, " << stats.shadow_draws << " shadow draws\n";
            assert(stats.skinned_copies == 200 && stats.skinned_draws == 1);
        }
    });
}

// With an argument, only the tests whose names contain it run (e.g. `kin_render3d_tests skinned`).
// --- Skinned meshes (P1a, part 2): velocity, the pre-pass, pick, the shadow cache, shared ranges, the ECS animator ---

// A 4 x 4 texture of one-texel checks, bright and dark, drawn with repeat: where a wrong velocity shows (a history read at the wrong
// place lands on the other colour, and the clip against the neighbourhood lets it through).
kin::Texture checker_texture(kin::Renderer2D& renderer) {
    kin::u8 texels[4 * 4 * 4];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const bool bright = (x + y) % 2 == 0;
            kin::u8* t = texels + (y * 4 + x) * 4;
            t[0] = bright ? 235 : 25;
            t[1] = bright ? 235 : 25;
            t[2] = bright ? 225 : 35;
            t[3] = 255;
        }
    }
    kin::Texture texture = renderer.create_texture_from_rgba(texels, {4, 4});
    renderer.set_scale_mode(texture, kin::ScaleMode::Nearest);
    return texture;
}

// Left half transparent (alpha 0), right half opaque: a cut-out tube.
kin::Texture half_cut_texture(kin::Renderer2D& renderer) {
    const kin::u8 texels[8] = {20, 200, 20, 0, 230, 90, 60, 255};
    kin::Texture texture = renderer.create_texture_from_rgba(texels, {2, 1});
    renderer.set_scale_mode(texture, kin::ScaleMode::Nearest);
    return texture;
}

// The lower half transparent along v (the tube's length: its foot is a hole), the upper half opaque.
kin::Texture lower_cut_texture(kin::Renderer2D& renderer) {
    const kin::u8 texels[8] = {20, 200, 20, 0, 230, 90, 60, 255};
    kin::Texture texture = renderer.create_texture_from_rgba(texels, {1, 2});
    renderer.set_scale_mode(texture, kin::ScaleMode::Nearest);
    return texture;
}

// The tube skinned, and the same tube skinned on the CPU and drawn as a plain mesh, in two views with histories of their own, moved
// the same rigid way over frames (a translation, a rotation, split between the pose and the placing, or nothing but the camera): under
// temporal anti-aliasing every frame of the one is the other's, to a few pixels, whichever route writes the velocity (the main pass, or
// the pre-pass when ambient occlusion is on), whether the copies are matched by id or by order, and in the overlay. A velocity that is
// the camera's alone (the part 1 behaviour) reads the history of the wrong place and ghosts the checks.
void test_gpu_skinned_velocity() {
    with_gpu("test_gpu_skinned_velocity", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes") || lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {16.0f, 0.2f, 16.0f});
        SkinRig rig{r3d};
        const kin::Texture checks = checker_texture(renderer);
        const kin::Texture cut = half_cut_texture(renderer);
        const std::vector<Mat4f> rest = rig.matrices(60.0f, -40.0f);
        const kin::Mesh baked = rig.baked(r3d, rest);
        enum class Motion { Pose, Split, Rotate, Camera };
        struct Case {
            const char* name;
            bool ao;
            kin::u32 id;
            Motion motion;
            bool overlay;
            bool cutout;
        };
        const std::vector<Case> cases{
            {"taa alone (main pass velocity), translating in the pose, by order", false, 0, Motion::Pose, false, false},
            {"taa alone, translating half in the pose and half in the placing, by id", false, 7, Motion::Split, false, false},
            {"taa alone, rotating, by id", false, 7, Motion::Rotate, false, false},
            {"taa alone, still pose under a moving camera", false, 0, Motion::Camera, false, false},
            {"taa + ao (pre-pass velocity), translating, by order", true, 0, Motion::Pose, false, false},
            {"taa + ao, rotating, by id", true, 9, Motion::Rotate, false, false},
            {"taa + ao, still pose under a moving camera", true, 0, Motion::Camera, false, false},
            {"taa alone, the overlay, rotating", false, 0, Motion::Rotate, true, false},
            {"taa + ao, the overlay, translating", true, 0, Motion::Pose, true, false},
            {"taa + ao, cut-out, translating", true, 0, Motion::Pose, false, true},
            {"taa alone, cut-out, rotating, by id", false, 5, Motion::Rotate, false, true},
        };
        int worst_of_all = 0;
        for (const Case& c : cases) {
            kin::View3D view_skinned = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(20, 24, 30)});
            kin::View3D view_plain = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(20, 24, 30)});
            const kin::Material3D material = c.cutout ? kin::Material3D{.color = Color::rgb(255, 255, 255), .double_sided = true, .blend = kin::Blend3D::Mask, .texture = cut}
                                                      : kin::Material3D{.color = Color::rgb(235, 235, 235), .texture = checks, .wrap_u = kin::Wrap3D::Repeat, .wrap_v = kin::Wrap3D::Repeat};
            int worst = 0;
            bool route_ok = true;
            for (int f = 0; f < 14; ++f) {
                const float x = -0.8f + 0.12f * static_cast<float>(f);
                Mat4f motion = Mat4f::identity();
                Mat4f place = Mat4f::translation({0.0f, 0.0f, 0.0f});
                Mat4f plain_place = place;
                std::vector<Mat4f> matrices = rest;
                float camera_x = 0.0f;
                switch (c.motion) {
                case Motion::Pose:
                    motion = Mat4f::translation({x, 0.0f, 0.0f});
                    plain_place = place * motion;
                    break;
                case Motion::Split:
                    motion = Mat4f::translation({x * 0.5f, 0.0f, 0.0f});
                    place = Mat4f::translation({x * 0.5f, 0.0f, 0.0f});
                    plain_place = Mat4f::translation({x, 0.0f, 0.0f});
                    break;
                case Motion::Rotate:
                    motion = Mat4f::rotation_y(7.0f * static_cast<float>(f));
                    plain_place = place * motion;
                    break;
                case Motion::Camera:
                    camera_x = 0.12f * static_cast<float>(f);
                    plain_place = place;
                    break;
                }
                for (Mat4f& m : matrices) {
                    m = motion * m;
                }
                const auto build = [&](kin::Scene3D& scene, bool skinned) {
                    scene.clear();
                    scene.camera = skin_camera(false);
                    scene.camera.position.x += camera_x;
                    skin_lights(scene, 1);
                    scene.taa = {.enabled = true};
                    scene.ambient_occlusion = {.enabled = c.ao};
                    scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(190, 190, 190)});
                    kin::Scene3D& target = c.overlay ? scene.overlay() : scene;
                    if (c.overlay) {
                        target.camera = scene.camera;
                    }
                    if (skinned) {
                        target.draw_skinned(rig.mesh, matrices, place, material, kin::colors::white, c.id);
                    } else {
                        target.draw(baked, plain_place, material, kin::colors::white, c.id);
                    }
                };
                kin::Scene3D scene_skinned, scene_plain;
                build(scene_skinned, true);
                build(scene_plain, false);
                const Pixels a = shoot_view(renderer, r3d, view_skinned, scene_skinned);
                const kin::Render3DStats stats = r3d.stats();
                renderer.present();
                const Pixels b = shoot_view(renderer, r3d, view_plain, scene_plain);
                renderer.present();
                route_ok = route_ok && stats.skinned_copies == 1 && stats.taa_main_velocity == !c.ao;
                if (f >= 1) {
                    worst = std::max(worst, pixels_differing(a, b, 8));
                }
                if (f == 13) {
                    dump(a, std::string{"skinned_velocity_"} + std::to_string(&c - cases.data()) + "_skinned");
                    dump(b, std::string{"skinned_velocity_"} + std::to_string(&c - cases.data()) + "_plain");
                    assert(pixels_lit(a) > 3000);
                }
            }
            std::cerr << "skinned velocity, " << c.name << ": worst frame " << worst << " of 19200 pixels differ from the CPU-skinned mesh by more than 8 levels\n";
            assert(route_ok);
            assert(worst <= 40);
            worst_of_all = std::max(worst_of_all, worst);
        }
        std::cerr << "skinned velocity: at most " << worst_of_all << " pixels differ\n";
    });
}

// A copy that appears has no history (its velocity is far off the screen, as a plain copy's is), whether it joins the draw of copies already
// there or comes with another mesh; and copies matched by id keep their own histories when the order of the draws changes between frames.
void test_gpu_skinned_velocity_new_and_reordered() {
    with_gpu("test_gpu_skinned_velocity_new_and_reordered", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes") || lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {16.0f, 0.2f, 16.0f});
        SkinRig rig{r3d};
        const kin::Texture checks = checker_texture(renderer);
        const kin::Material3D material{.color = Color::rgb(235, 235, 235), .texture = checks, .wrap_u = kin::Wrap3D::Repeat, .wrap_v = kin::Wrap3D::Repeat};
        const std::vector<Mat4f> rest = rig.matrices(50.0f, -30.0f);
        const kin::Mesh baked = rig.baked(r3d, rest);
        for (const bool reorder : {false, true}) {
            kin::View3D view_skinned = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(20, 24, 30)});
            kin::View3D view_plain = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(20, 24, 30)});
            int worst = 0, new_frame = 0;
            for (int f = 0; f < 16; ++f) {
                // Two characters, passing each other; a third from frame 8 on.
                struct Who {
                    kin::u32 id;
                    Vec3f at;
                };
                std::vector<Who> who{{1, {-1.5f + 0.1f * static_cast<float>(f), 0.0f, 0.0f}}, {2, {1.5f - 0.1f * static_cast<float>(f), 0.0f, 0.5f}}};
                if (reorder && f >= 6) {
                    std::swap(who[0], who[1]); // the same ids, drawn in the other order
                }
                if (f >= 8) {
                    who.push_back({3, {0.0f, 0.0f, -1.5f}});
                }
                const auto build = [&](kin::Scene3D& scene, bool skinned) {
                    scene.clear();
                    scene.camera = skin_camera(false);
                    skin_lights(scene, 1);
                    scene.taa = {.enabled = true};
                    scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(190, 190, 190)});
                    for (const Who& w : who) {
                        std::vector<Mat4f> m = rest;
                        for (Mat4f& x : m) {
                            x = Mat4f::translation(w.at) * x;
                        }
                        if (skinned) {
                            scene.draw_skinned(rig.mesh, m, Mat4f::identity(), material, kin::colors::white, w.id);
                        } else {
                            scene.draw(baked, Mat4f::translation(w.at), material, kin::colors::white, w.id);
                        }
                    }
                };
                kin::Scene3D scene_skinned, scene_plain;
                build(scene_skinned, true);
                build(scene_plain, false);
                const Pixels a = shoot_view(renderer, r3d, view_skinned, scene_skinned);
                renderer.present();
                const Pixels b = shoot_view(renderer, r3d, view_plain, scene_plain);
                renderer.present();
                if (f >= 1) {
                    const int differing = pixels_differing(a, b, 8);
                    worst = std::max(worst, differing);
                    if (f == 8 || f == 9) {
                        new_frame = std::max(new_frame, differing);
                    }
                }
            }
            std::cerr << "skinned velocity, " << (reorder ? "reordered by id" : "a copy appearing") << ": worst frame " << worst << " pixels differ, " << new_frame
                      << " when the new copy appears\n";
            assert(worst <= 40);
        }
    });
}

// Ambient occlusion and contact shadows with a skinned copy are the same scene's with the copy skinned on the CPU and drawn as a plain mesh,
// opaque and cut-out; and they show (the picture with them differs from the one without by hundreds of pixels at the creases).
void test_gpu_skinned_prepass_features() {
    with_gpu("test_gpu_skinned_prepass_features", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes") || lacks(r3d.capabilities().ambient_occlusion, "ambient occlusion")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {16.0f, 0.2f, 16.0f});
        const kin::Mesh wall = make_box(r3d, {0.3f, 3.0f, 6.0f});
        SkinRig rig{r3d};
        const kin::Texture cut = half_cut_texture(renderer);
        const std::vector<Mat4f> pose = rig.matrices(50.0f, -35.0f, {0.0f, 0.0f, 0.0f});
        const kin::Mesh baked = rig.baked(r3d, pose);
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 4});
        int worst = 0, shown = 1 << 30;
        for (const bool cutout : {false, true}) {
            for (const kin::u32 cascades : {1u, 3u}) {
                const kin::Material3D material = cutout ? kin::Material3D{.color = Color::rgb(255, 255, 255), .double_sided = true, .blend = kin::Blend3D::Mask, .texture = cut}
                                                        : kin::Material3D{.color = Color::rgb(210, 70, 60)};
                kin::Scene3D scene;
                const auto shoot = [&](bool skinned, bool features) {
                    scene.clear();
                    scene.camera = skin_camera(false);
                    skin_lights(scene, cascades);
                    scene.lights.ambient = {0.7f, 0.7f, 0.75f}; // a bright sky, so that the occlusion of it shows
                    scene.ambient_occlusion = {.enabled = features, .radius = 1.0f, .intensity = 2.0f};
                    scene.contact_shadows = {.enabled = features, .distance = 1.0f};
                    scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
                    scene.draw(wall, Mat4f::translation({-1.0f, 1.5f, -0.5f}), {.color = Color::rgb(200, 190, 170)});
                    const Mat4f place = Mat4f::translation({0.1f, 0.0f, 0.2f}) * Mat4f::rotation_y(20.0f);
                    if (skinned) {
                        scene.draw_skinned(rig.mesh, pose, place, material);
                    } else {
                        scene.draw(baked, place, material);
                    }
                    Pixels p = shoot_view(renderer, r3d, view, scene);
                    renderer.present();
                    return p;
                };
                const Pixels skinned = shoot(true, true);
                const kin::Render3DStats stats = r3d.stats();
                const Pixels plain = shoot(false, true);
                const Pixels without = shoot(true, false);
                const int differing = pixels_differing(skinned, plain);
                assert(differing <= 20);
                assert(stats.skinned_copies == 1 && stats.ao_instances == 3 && stats.ao_dispatches > 0 && stats.contact_dispatches > 0);
                const int visible = pixels_differing(skinned, without, 3);
                worst = std::max(worst, differing);
                shown = std::min(shown, visible);
                dump(skinned, std::string{"skinned_ao_"} + (cutout ? "cutout_" : "opaque_") + "c" + std::to_string(cascades));
                std::cerr << "skinned ao + contact, " << (cutout ? "cut-out" : "opaque") << ", " << cascades << " cascades: " << differing
                          << " pixels differ from the CPU-skinned mesh; the features change " << visible << " pixels\n";
                assert(differing <= 20);
                assert(visible > 150);
            }
        }
        std::cerr << "skinned ao + contact: at most " << worst << " pixels differ, the features show in at least " << shown << "\n";
    });
}

// A skinned wall is an occluder: the clusters of the balls behind it are culled as behind a plain wall (the same numbers), the picture is
// the one without occlusion culling to the byte, and the cut-out variant of the occluder pass (a wall of a masked material) does the same.
void test_gpu_skinned_occluder() {
    with_gpu("test_gpu_skinned_occluder", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes") || !occlusion_capable(r3d)) {
            return;
        }
        const OcclusionRoom room{r3d};
        // The wall as a skinned mesh of one joint (weight 1): the box 1 x 3 x 0.4 of the room, scaled 8 times along x.
        kin::MeshData wall_data = kin::meshes::box({1.0f, 3.0f, 0.4f});
        wall_data.skin.assign(wall_data.vertices.size(), kin::MeshSkinVertex{{0, 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}});
        const kin::Mesh wall = r3d.create_mesh(wall_data);
        assert(wall.skinned() && wall.skin_joint_count() == 1);
        const kin::u8 solid[4] = {200, 190, 170, 255};
        const kin::Texture solid_texture = renderer.create_texture_from_rgba(solid, {1, 1});
        const std::vector<Mat4f> one{Mat4f::identity()};
        struct Config {
            const char* name;
            bool skinned, cutout, passes, taa;
        };
        const std::vector<Config> configs{
            {"plain wall", false, false, false, false},
            {"skinned wall", true, false, false, false},
            {"skinned cut-out wall", true, true, false, false},
            {"skinned wall, ambient occlusion and contact shadows", true, false, true, false},
            {"skinned wall, taa", true, false, false, true},
        };
        kin::u32 reference_occluded = 0, reference_drawn = 0;
        for (const Config& config : configs) {
            if (config.taa && lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
                continue;
            }
            Twin twin{renderer, r3d, config.taa ? 1u : 4u};
            kin::Scene3D scene;
            int worst = 0;
            for (int f = 0; f < (config.taa ? 6 : 3); ++f) {
                scene.clear();
                scene.camera = room_camera(0.05f * static_cast<float>(f));
                scene.lights.sun.direction = {-0.85f, 0.5f, 0.1f};
                scene.lights.shadow_area = {{-30.0f, -0.2f, -30.0f}, {30.0f, 8.0f, 30.0f}};
                if (config.passes) {
                    scene.ambient_occlusion = {.enabled = true};
                    scene.contact_shadows = {.enabled = true};
                }
                scene.taa = {.enabled = config.taa};
                scene.draw(room.floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(150, 150, 150)});
                const Mat4f place = Mat4f::translation({0.0f, 1.5f, 0.0f}) * Mat4f::scaling({8.0f, 1.0f, 1.0f});
                kin::Material3D plaster{.color = Color::rgb(200, 190, 170)};
                if (config.cutout) {
                    plaster = {.color = Color::rgb(255, 255, 255), .blend = kin::Blend3D::Mask, .texture = solid_texture};
                }
                if (config.skinned) {
                    scene.draw_skinned(wall, one, place, plaster);
                } else {
                    scene.draw(room.unit, place, plaster);
                }
                kin::u32 id = 1;
                for (const Vec3f& v : hidden_balls) {
                    scene.draw(room.ball, Mat4f::translation(v), {.color = Color::rgb(70, 140, 210), .roughness = 0.5f}, kin::colors::white, id++);
                }
                worst = std::max(worst, twin.frame(scene));
            }
            const kin::Render3DStats& on = twin.on_stats;
            const kin::Render3DStats& off = twin.off_stats;
            std::cerr << "skinned occluder, " << config.name << ": " << worst << " bytes differ with and without culling; " << on.clusters_occluded
                      << " clusters occluded of " << off.clusters_drawn << "\n";
            assert(worst == 0);
            assert(on.clusters_occluded > 40 && on.clusters_drawn + on.clusters_occluded == off.clusters_drawn);
            if (!config.skinned) {
                reference_occluded = on.clusters_occluded;
                reference_drawn = on.clusters_drawn;
            } else if (!config.passes && !config.taa) {
                // The same wall, the same clusters: the skinned one hides what the plain one does.
                assert(on.clusters_occluded == reference_occluded && on.clusters_drawn == reference_drawn);
            }
        }
    });
}

// Pick finds skinned copies where their posed triangles are: the same ids over a grid of pixels as the CPU-skinned meshes drawn plain with the
// same ids; the pixels of a posed vertex name the copy, an empty one nothing, a cut-out's hole the floor behind it.
void test_gpu_skinned_pick() {
    with_gpu("test_gpu_skinned_pick", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {6.0f, 0.2f, 6.0f});
        SkinRig rig{r3d};
        const kin::Texture cut = lower_cut_texture(renderer);
        struct Pose {
            float a, b;
            Vec3f at;
            kin::u32 id;
        };
        const std::vector<Pose> poses{{0.0f, 0.0f, {-2.2f, 0.0f, 0.0f}, 11}, {70.0f, -50.0f, {0.0f, 0.0f, 0.0f}, 12}, {-45.0f, 60.0f, {2.2f, 0.0f, 0.0f}, 13}};
        int opaque_floor = 0;
        for (const bool cutout : {false, true}) {
            const kin::Material3D material = cutout ? kin::Material3D{.color = Color::rgb(255, 255, 255), .double_sided = true, .blend = kin::Blend3D::Mask, .texture = cut}
                                                    : kin::Material3D{.color = Color::rgb(210, 70, 60)};
            kin::View3D view_skinned = r3d.create_view({160, 120}, {.samples = 1, .pick = true});
            kin::View3D view_plain = r3d.create_view({160, 120}, {.samples = 1, .pick = true});
            kin::Scene3D scene_skinned, scene_plain;
            std::vector<kin::MeshData> posed;
            for (kin::Scene3D* scene : {&scene_skinned, &scene_plain}) {
                scene->camera = kin::Camera3D::look_at({0.0f, 2.2f, 9.0f}, {0.0f, 1.2f, 0.0f}, 50.0f);
                skin_lights(*scene, 1);
                scene->draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)}, kin::colors::white, 3);
            }
            for (const Pose& p : poses) {
                const std::vector<Mat4f> m = rig.matrices(p.a, p.b);
                scene_skinned.draw_skinned(rig.mesh, m, Mat4f::translation(p.at), material, kin::colors::white, p.id);
                const kin::Mesh baked = rig.baked(r3d, m);
                scene_plain.draw(baked, Mat4f::translation(p.at), material, kin::colors::white, p.id);
                posed.push_back(kin::skin_mesh(rig.tube.mesh, m));
            }
            shoot_view(renderer, r3d, view_skinned, scene_skinned);
            assert(r3d.stats().skinned_copies == 3);
            renderer.present();
            shoot_view(renderer, r3d, view_plain, scene_plain);
            renderer.present();
            int compared = 0, differ = 0;
            std::array<int, 3> hits{};
            int floors = 0, empty = 0;
            for (int y = 3; y < 120; y += 6) {
                for (int x = 3; x < 160; x += 6) {
                    const kin::PickResult a = r3d.pick(view_skinned, {x, y}), b = r3d.pick(view_plain, {x, y});
                    ++compared;
                    differ += (a.hit != b.hit || (a.hit && a.id != b.id)) ? 1 : 0;
                    if (a.hit) {
                        for (std::size_t k = 0; k < poses.size(); ++k) {
                            hits[k] += a.id == poses[k].id ? 1 : 0;
                        }
                        floors += a.id == 3 ? 1 : 0;
                    } else {
                        ++empty;
                    }
                }
            }
            std::cerr << "skinned pick, " << (cutout ? "cut-out" : "opaque") << ": " << differ << " of " << compared << " pixels differ from the plain meshes; ids "
                      << hits[0] << "/" << hits[1] << "/" << hits[2] << ", floor " << floors << ", nothing " << empty << "\n";
            assert(differ == 0);
            assert(hits[0] > 5 && hits[1] > 5 && hits[2] > 5 && floors > 20 && empty > 100);
            if (!cutout) {
                opaque_floor = floors;
            } else {
                assert(floors > opaque_floor); // the feet are holes: the floor shows through where they are
            }
            // A posed vertex's pixel names its copy (the CPU's posed positions, projected).
            if (!cutout) {
                int checked = 0, right = 0;
                for (std::size_t k = 0; k < poses.size(); ++k) {
                        // The centre of each ring of the tube (its axis), well inside the silhouette.
                    const std::size_t row = 13; // SkinRig: 12 segments, 13 vertices a ring (the last repeats the first)
                    for (std::size_t v = 0; v + row <= posed[k].vertices.size(); v += row) {
                        Vec3f centre{};
                        for (std::size_t i = 0; i + 1 < row; ++i) {
                            centre = centre + posed[k].vertices[v + i].position;
                        }
                        const Vec3f world = centre * (1.0f / static_cast<float>(row - 1)) + poses[k].at;
                        const auto screen_point = scene_skinned.camera.project(world, screen);
                        if (!screen_point || screen_point->pos.x < 1 || screen_point->pos.y < 1 || screen_point->pos.x > 158 || screen_point->pos.y > 118) {
                            continue;
                        }
                        const kin::PickResult r = r3d.pick(view_skinned, {static_cast<int>(screen_point->pos.x), static_cast<int>(screen_point->pos.y)});
                        ++checked;
                        right += r.hit && r.id == poses[k].id ? 1 : 0;
                    }
                }
                std::cerr << "skinned pick: " << right << " of " << checked << " posed vertices' pixels name their copy\n";
                assert(checked > 30 && right * 10 >= checked * 9);
            }
        }
        // Without a pick pass nothing is made (the view has none): pick answers nothing.
        kin::View3D no_pick = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = skin_camera(false);
        scene.draw_skinned(rig.mesh, rig.matrices(0.0f, 0.0f), Mat4f::identity(), {}, kin::colors::white, 5);
        shoot_view(renderer, r3d, no_pick, scene);
        renderer.present();
        assert(!r3d.pick(no_pick, {80, 60}).hit);
    });
}

// The shadow cache with skinned casters: a still pose hits, a changed pose (any copy of forty, in a way the cheap sample need not see) draws
// again, a different skeleton draws again, and what the cache serves is always what an uncached render of the same scene shows.
void test_gpu_skinned_shadow_cache() {
    with_gpu("test_gpu_skinned_shadow_cache", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {20.0f, 0.2f, 20.0f});
        SkinRig rig{r3d};
        SkinRig other{r3d, 25, 12, 2.0f};
        SkinRig fat{r3d, 25, 12, 3.0f, 0.7f}; // the same skeleton and so the same matrices, another mesh
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::View3D reference_view = r3d.create_view({160, 120}, {.samples = 1});
        struct Setup {
            float a = 40.0f, b = -30.0f;
            float one_copy = 0.0f; // copy 7's bend, in a draw of forty
            bool other_skeleton = false;
            bool fat = false;
            bool cascades = false;
        } setup;
        kin::Scene3D scene;
        const auto build = [&](bool cache) {
            scene.clear();
            scene.camera = kin::Camera3D::look_at({0.0f, 14.0f, 0.01f}, {0.0f, 0.0f, 0.0f}, 60.0f);
            scene.lights.sun = {.direction = {1.0f, 1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}};
            scene.lights.ambient = {0.15f, 0.15f, 0.15f};
            scene.lights.shadow_area = kin::Bounds3{{-10.0f, -0.2f, -10.0f}, {10.0f, 6.0f, 10.0f}};
            scene.lights.shadow_resolution = 1024;
            scene.lights.shadow_cascades = setup.cascades ? 3 : 1;
            scene.lights.cache_shadows = cache;
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            const SkinRig& mesh = setup.other_skeleton ? other : setup.fat ? fat : rig;
            scene.draw_skinned(mesh.mesh, mesh.matrices(setup.a, setup.b), Mat4f::translation({-3.0f, 0.0f, 0.0f}), {.color = Color::rgb(200, 60, 60)});
            for (int i = 0; i < 40; ++i) {
                scene.draw_skinned(rig.mesh, rig.matrices(20.0f + (i == 7 ? setup.one_copy : 0.0f), -20.0f),
                                   Mat4f::translation({-8.0f + 0.4f * static_cast<float>(i), 0.0f, -6.0f}) * Mat4f::scaling({0.2f, 0.2f, 0.2f}),
                                   {.color = Color::rgb(60, 60, 200)});
            }
        };
        const auto shoot = [&](kin::View3D& target, bool cache) {
            build(cache);
            Pixels p = shoot_view(renderer, r3d, target, scene);
            renderer.present();
            return p;
        };
        const auto truth = [&] { return shoot(reference_view, false); };

        const Pixels first = shoot(view, true);
        if (r3d.stats().shadow_cascades == 0) {
            std::cerr << "shadows: not available on this device, skipped\n";
            return;
        }
        assert(!r3d.stats().shadow_cached && r3d.stats().shadow_draws > 0 && r3d.stats().skinned_copies == 41);
        shoot(view, true);
        for (int f = 0; f < 3; ++f) {
            const Pixels p = shoot(view, true);
            assert(r3d.stats().shadow_cached && r3d.stats().shadow_draws == 0 && p.rgba == first.rgba);
        }
        assert(first.rgba == truth().rgba);
        std::cerr << "skinned shadow cache: still poses hit\n";

        // The settle: renders until the cache is armed again (a change makes it wait a few renders before it copies).
        const auto settle = [&] {
            for (int i = 0; i < 14; ++i) {
                shoot(view, true);
                if (r3d.stats().shadow_cached) {
                    return true;
                }
            }
            return false;
        };
        // A posed copy that moves invalidates it, and the picture is the uncached one's; still again, it hits again.
        const auto change = [&](const char* what, const std::function<void()>& apply) {
            apply();
            const Pixels moved = shoot(view, true);
            assert(!r3d.stats().shadow_cached && r3d.stats().shadow_draws > 0);
            assert(moved.rgba == truth().rgba);
            assert(settle());
            assert(shoot(view, true).rgba == moved.rgba && r3d.stats().shadow_cached);
            std::cerr << "skinned shadow cache: " << what << ": drawn again, then hit\n";
            return moved;
        };
        const Pixels before = first;
        const Pixels after_bend = change("one posed copy bends", [&] { setup.a = 55.0f; });
        assert(pixels_differing(after_bend, before, 3) > 0);
        change("copy 7 of forty bends (a copy the sample does not look at)", [&] { setup.one_copy = 25.0f; });
        change("another mesh with the same skeleton and the same pose", [&] { setup.fat = true; });
        change("a different skeleton in the same place", [&] {
            setup.fat = false;
            setup.other_skeleton = true;
        });
        change("the pose goes back", [&] {
            setup.fat = false;
            setup.other_skeleton = false;
            setup.a = 40.0f;
            setup.one_copy = 0.0f;
        });
        // The same with cascades (lists of copies a cascade).
        change("cascades on", [&] { setup.cascades = true; });
        change("a posed copy bends, with cascades", [&] { setup.b = 10.0f; });
    });
}

// One range of matrices for all of a model's parts: the same picture as a call for each part with the pose, and a third the matrices.
void test_gpu_skinned_shared_range() {
    with_gpu("test_gpu_skinned_shared_range", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {16.0f, 0.2f, 16.0f});
        SkinRig rig{r3d};
        // A model of three parts on the one skeleton (three materials of the one mesh, as a character of three materials has).
        kin::Model model;
        model.skeletons.push_back(rig.tube.skeleton);
        const Color colours[3] = {Color::rgb(210, 70, 60), Color::rgb(70, 190, 90), Color::rgb(70, 90, 210)};
        for (int i = 0; i < 3; ++i) {
            kin::ModelPart part;
            part.name = "part" + std::to_string(i);
            part.mesh = rig.mesh;
            part.material = {.color = colours[i], .double_sided = i == 1};
            part.skin = 0;
            model.parts.push_back(part);
        }
        const std::vector<Mat4f> pose = rig.matrices(60.0f, -45.0f);
        for (const bool features : {false, true}) {
            if (features && lacks(r3d.capabilities().ambient_occlusion && r3d.capabilities().temporal_aa, "ambient occlusion and temporal anti-aliasing")) {
                continue;
            }
            Pixels shared, separate;
            kin::u32 shared_matrices = 0, separate_matrices = 0;
            for (const bool use_model : {true, false}) {
                kin::View3D view = r3d.create_view({160, 120}, {.samples = 4}); // a view each: the jitter's sequence starts alike
                for (int frame = 0; frame < (features ? 4 : 1); ++frame) {
                    kin::Scene3D scene;
                    scene.camera = skin_camera(false);
                    skin_lights(scene, 2);
                    scene.ambient_occlusion = {.enabled = features};
                    scene.taa = {.enabled = features};
                    scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
                    const Mat4f place = Mat4f::translation({0.0f, 0.0f, 0.2f});
                    if (use_model) {
                        scene.draw_skinned(model, pose, place, kin::colors::white, 4);
                        assert(scene.joint_matrices().size() == 3 && scene.skin_offsets().size() == 3);
                        assert(scene.skin_offsets()[0] == scene.skin_offsets()[1] && scene.skin_offsets()[1] == scene.skin_offsets()[2]);
                    } else {
                        for (const kin::ModelPart& part : model.parts) {
                            scene.draw_skinned(part.mesh, pose, place, part.material, kin::colors::white, 4);
                        }
                        assert(scene.joint_matrices().size() == 9);
                    }
                    Pixels p = shoot_view(renderer, r3d, view, scene);
                    const kin::Render3DStats stats = r3d.stats();
                    renderer.present();
                    assert(stats.skinned_copies == 3);
                    (use_model ? shared : separate) = p;
                    (use_model ? shared_matrices : separate_matrices) = stats.skin_matrices;
                }
            }
            std::cerr << "skinned shared range" << (features ? " (ao + taa)" : "") << ": " << shared_matrices << " matrices against " << separate_matrices << "; "
                      << pixels_differing(shared, separate, 0) << " pixels differ\n";
            assert(shared_matrices == 3 && separate_matrices == 9);
            assert(shared.rgba == separate.rgba);
            assert(pixels_lit(shared) > 3000);
        }
    });
}

// A character as an entity: Transform3D, SkinnedMeshRenderer and Animator3D, stepped by animate() over fixed steps and drawn by collect().
// The pictures of two runs are the same to the byte, and with a job system of one worker and one of many (2,200 characters: past the size at
// which render() uses its workers) they are as serial; characters destroyed take their copies with them.
void test_gpu_ecs_character() {
    with_gpu("test_gpu_ecs_character", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        SkinRig rig{r3d, 9, 8};
        auto model = std::make_shared<kin::Model>();
        model->skeletons.push_back(rig.tube.skeleton);
        {
            kin::AnimClip3D sway;
            sway.name = "sway";
            sway.duration = 1.0f;
            sway.channels.resize(3);
            kin::Track3D<kin::Quatf>& r = sway.channels[1].rotation;
            r.times = {0.0f, 0.5f, 1.0f};
            r.values = {kin::Quatf::axis_angle({0, 0, 1}, -40.0f), kin::Quatf::axis_angle({0, 0, 1}, 60.0f), kin::Quatf::axis_angle({0, 0, 1}, -40.0f)};
            model->clips.push_back(sway);
            kin::AnimClip3D stand;
            stand.name = "stand";
            stand.duration = 1.0f;
            stand.channels.resize(3);
            model->clips.push_back(stand);
        }
        kin::ModelPart part;
        part.name = "tube";
        part.mesh = rig.mesh;
        part.material = {.color = Color::rgb(210, 90, 70)};
        part.skin = 0;
        model->parts.push_back(part);
        const kin::Mesh floor = make_box(r3d, {80.0f, 0.2f, 80.0f});
        kin::JobSystem one{{.workers = 1}};
        kin::JobSystem many;
        const kin::Texture unused;
        static_cast<void>(unused);
        const auto run = [&](int count, kin::JobSystem* jobs, int steps, std::vector<Pixels>* frames, kin::u32* skinned_seen) {
            kin::EcsWorld world;
            kin::WorldRenderState3D state{world};
            auto& w = world.raw();
            const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count))));
            std::vector<flecs::entity> entities;
            for (int i = 0; i < count; ++i) {
                const float x = (static_cast<float>(i % side) - static_cast<float>(side) * 0.5f) * (60.0f / static_cast<float>(side));
                const float z = (static_cast<float>(i / side) - static_cast<float>(side) * 0.5f) * (60.0f / static_cast<float>(side));
                kin::Animator3D a;
                a.clip = "sway";
                a.speed = 0.5f + 0.2f * static_cast<float>(i % 7);
                entities.push_back(w.entity().set(kin::Transform3D{.position = {x, 0.0f, z}, .scale = {0.4f, 0.4f, 0.4f}})
                                       .set(kin::SkinnedMeshRenderer{.model = model}).set(a));
            }
            kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
            renderer.set_job_system(jobs);
            kin::Scene3D scene;
            for (int step = 0; step < steps; ++step) {
                state.animate(1.0f / 60.0f);
                if (step % 5 == 4) {
                    scene.clear();
                    scene.camera = kin::Camera3D::look_at({0.0f, 30.0f, 26.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
                    skin_lights(scene, 1);
                    scene.lights.shadow_area = kin::Bounds3{{-40.0f, -0.3f, -40.0f}, {40.0f, 8.0f, 40.0f}};
                    scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
                    state.propagate_transforms();
                    state.collect(scene);
                    Pixels p = shoot_view(renderer, r3d, view, scene);
                    if (skinned_seen) {
                        *skinned_seen = r3d.stats().skinned_copies;
                    }
                    renderer.present();
                    if (frames) {
                        frames->push_back(std::move(p));
                    }
                }
            }
            // The characters destroyed take their copies with them (and nothing dangles: the next frame is fine).
            for (std::size_t i = 0; i < entities.size(); i += 2) {
                entities[i].destruct();
            }
            scene.clear();
            scene.camera = kin::Camera3D::look_at({0.0f, 30.0f, 26.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
            state.propagate_transforms();
            state.collect(scene);
            std::size_t drawn = 0;
            for (const kin::Scene3D::Draw& d : scene.draws()) {
                drawn += d.skinned ? d.count : 0u;
            }
            assert(drawn == static_cast<std::size_t>(count) / 2u);
            shoot_view(renderer, r3d, view, scene);
            renderer.present();
            renderer.set_job_system(nullptr);
        };
        std::vector<Pixels> a, b, c, d;
        kin::u32 seen = 0;
        run(30, nullptr, 40, &a, &seen);
        std::cerr << "ecs characters: " << seen << " of 30 in view\n";
        assert(seen > 10 && seen <= 30);
        run(30, nullptr, 40, &b, nullptr);
        run(30, &one, 40, &c, nullptr);
        assert(a.size() == 8 && a.size() == b.size() && a.size() == c.size());
        bool moved = false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            assert(a[i].rgba == b[i].rgba && a[i].rgba == c[i].rgba);
            moved = moved || (i > 0 && a[i].rgba != a[i - 1].rgba);
        }
        assert(moved && pixels_lit(a.back()) > 2000);
        dump(a.back(), "ecs_characters");
        // 2,200 characters: render() on its workers is the serial render's picture.
        run(2200, nullptr, 10, &c, nullptr);
        run(2200, &many, 10, &d, nullptr);
        assert(d.size() == 2 && d.back().rgba == c.back().rgba);
        std::cerr << "ecs characters: 40 fixed steps, two runs, one worker and 2,200 on workers: the same pictures\n";
    });
}


// --- Inverse kinematics (P1b): the posed copy at the target, culling, TAA, the shadow cache, workers ---

namespace ik_scene {

// A biped model: a grey body of boxes along the bones and a bright cube on the left hand (unlit, so that its pixels are told).
struct Biped {
    std::shared_ptr<kin::Model> model;
    kin::Mesh body, hand;
    std::vector<Mat4f> rest_world;
};

Biped make_biped(kin::Renderer3D& r3d) {
    Biped b;
    b.model = std::make_shared<kin::Model>();
    b.model->skeletons.push_back(skin_fixtures::make_biped());
    const kin::Skeleton& sk = b.model->skeletons[0];
    b.body = r3d.create_mesh(skin_fixtures::make_biped_body(sk));
    b.hand = r3d.create_mesh(skin_fixtures::make_joint_cube(sk, skin_fixtures::biped::hand_l));
    assert(b.body.valid() && b.body.skinned() && b.hand.skinned());
    kin::ModelPart body;
    body.name = "body";
    body.mesh = b.body;
    body.material = {.color = Color::rgb(170, 170, 180)};
    body.skin = 0;
    kin::ModelPart hand;
    hand.name = "hand";
    hand.mesh = b.hand;
    hand.material = {.color = Color::rgb(255, 40, 30), .unlit = true};
    hand.skin = 0;
    b.model->parts = {body, hand};
    kin::compute_world_matrices(sk, sk.rest_pose(), b.rest_world);
    return b;
}

kin::IKConstraint left_hand() {
    kin::IKConstraint k;
    k.name = "hand";
    k.kind = kin::IKKind::TwoBone;
    k.joints = {"shoulder_L", "elbow_L", "hand_L"};
    k.pole = kin::IKConstraint::Pole::ModelDirection;
    k.pole_vector = {0.0f, 0.0f, -1.0f};
    return k;
}

kin::Camera3D camera() { return kin::Camera3D::look_at({0.0f, 1.3f, 5.0f}, {0.0f, 1.1f, 0.0f}, 40.0f); }

// The centre and count of the pixels that are the marker's red.
struct Blob {
    float x = 0.0f, y = 0.0f;
    int count = 0;
};
Blob red_blob(const Pixels& p) {
    Blob b;
    for (int y = 0; y < p.size.y; ++y) {
        for (int x = 0; x < p.size.x; ++x) {
            const Color c = p.at(x, y);
            if (c.r > 200 && c.g < 90 && c.b < 90) {
                b.x += static_cast<float>(x) + 0.5f;
                b.y += static_cast<float>(y) + 0.5f;
                ++b.count;
            }
        }
    }
    if (b.count > 0) {
        b.x /= static_cast<float>(b.count);
        b.y /= static_cast<float>(b.count);
    }
    return b;
}

} // namespace ik_scene

// The hand of a skinned character, posed by IK to a point of the world, is drawn where that point projects: the marker cube's centre of
// mass within a pixel and a half of it, for several targets, for an entity moved and turned and scaled, and not there with the weight 0.
void test_gpu_ik_hand_at_target() {
    with_gpu("test_gpu_ik_hand_at_target", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const ik_scene::Biped biped = ik_scene::make_biped(r3d);
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(30, 34, 40)});
        struct Case {
            const char* what;
            kin::Transform3D transform;
            Vec3f target_model; // in the model's space
        };
        const std::vector<Case> cases = {
            {"in front", {}, {0.45f, 1.75f, 0.45f}},
            {"out to the side and down", {}, {0.9f, 1.2f, 0.2f}},
            {"across the chest", {}, {-0.15f, 1.5f, 0.5f}},
            {"entity moved and turned", {.position = {0.3f, 0.0f, 0.4f}, .rotation = kin::Quatf::axis_angle({0, 1, 0}, 30.0f)}, {0.5f, 1.7f, 0.4f}},
            {"entity scaled", {.position = {-0.2f, 0.0f, 0.0f}, .scale = {1.3f, 1.3f, 1.3f}}, {0.6f, 1.8f, 0.5f}},
        };
        for (const Case& c : cases) {
            kin::EcsWorld world;
            kin::WorldRenderState3D state{world};
            kin::Animator3D animator;
            animator.add_ik(ik_scene::left_hand());
            const flecs::entity e = world.raw().entity().set(c.transform).set(kin::SkinnedMeshRenderer{.model = biped.model}).set(animator);
            const Mat4f entity_matrix = c.transform.matrix();
            const Vec3f target = entity_matrix.apply(c.target_model);
            const auto shoot = [&](float weight) {
                kin::Animator3D& a = *e.get_mut<kin::Animator3D>();
                a.ik("hand").set_target(target).set_weight(weight);
                state.animate(1.0f / 60.0f);
                kin::Scene3D scene;
                scene.camera = ik_scene::camera();
                skin_lights(scene, 1);
                state.propagate_transforms();
                state.collect(scene);
                Pixels p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                const auto where = scene.camera.project(target, screen);
                assert(where.has_value());
                return std::pair{p, where->pos};
            };
            const auto [p, at] = shoot(1.0f);
            const ik_scene::Blob blob = ik_scene::red_blob(p);
            const float error = std::hypot(blob.x - at.x, blob.y - at.y);
            std::cerr << "ik hand, " << c.what << ": marker at (" << blob.x << ", " << blob.y << ") px, target projects to (" << at.x << ", " << at.y
                      << "), " << error << " px apart, " << blob.count << " marker pixels\n";
            assert(blob.count > 25 && error < 1.5f);
            assert(pixels_lit(p) > 1200);
            dump(p, std::string{"ik_hand_"} + std::to_string(&c - cases.data()));
            // Weight 0: the clips' pose, the hand elsewhere.
            const auto [q, unchanged] = shoot(0.0f);
            const ik_scene::Blob rest = ik_scene::red_blob(q);
            std::cerr << "  weight 0: marker at (" << rest.x << ", " << rest.y << "), " << rest.count << " pixels\n";
            assert(rest.count > 10 && std::hypot(rest.x - at.x, rest.y - at.y) > 5.0f);
        }
    });
}

// A character the camera does not see by its rest pose is not culled when IK puts a limb in view: the bound is made of the joints' posed
// matrices, which include the IK. (Below the view: the head's top at y = -1.0; the hand reaching up to y = -0.4, inside the view's bottom.)
void test_gpu_ik_not_culled() {
    with_gpu("test_gpu_ik_not_culled", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const ik_scene::Biped biped = ik_scene::make_biped(r3d);
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::EcsWorld world;
        kin::WorldRenderState3D state{world};
        kin::Animator3D animator;
        animator.add_ik(ik_scene::left_hand());
        const kin::Transform3D below{.position = {0.0f, -3.0f, 0.0f}};
        const flecs::entity e = world.raw().entity().set(below).set(kin::SkinnedMeshRenderer{.model = biped.model}).set(animator);
        const Vec3f target = below.matrix().apply({0.35f, 2.55f, 0.3f}); // the arm held up: 0.9 above the shoulder
        const auto frame = [&](float weight) {
            e.get_mut<kin::Animator3D>()->ik("hand").set_target(target).set_weight(weight);
            state.animate(1.0f / 60.0f);
            kin::Scene3D scene;
            scene.camera = ik_scene::camera();
            skin_lights(scene, 1);
            state.propagate_transforms();
            state.collect(scene);
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        Pixels p = frame(0.0f);
        // At rest the hand's part is culled (the body's bound, a sphere round everything, may still touch the view's edge: nothing of it shows).
        assert(r3d.stats().culled >= 1 && pixels_lit(p) == 0);
        p = frame(1.0f);
        assert(r3d.stats().culled == 0);
        const ik_scene::Blob blob = ik_scene::red_blob(p);
        const auto at = ik_scene::camera().project(target, screen);
        assert(blob.count > 10 && std::hypot(blob.x - at->pos.x, blob.y - at->pos.y) < 1.5f);
        std::cerr << "ik culling: out of view at rest (culled), the hand reaching in is drawn (" << blob.count << " marker pixels)\n";
        dump(p, "ik_reach_into_view");
    });
}

// The previous joint matrices include the IK: a hand that moves by IK under TAA gives, frame by frame, the picture of the same poses
// played from a clip (baked from the IK's own poses), whose history is the same as any skinned animation's.
void test_gpu_ik_taa_matches_clip() {
    with_gpu("test_gpu_ik_taa_matches_clip", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes") || lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        const ik_scene::Biped biped = ik_scene::make_biped(r3d);
        const kin::Mesh floor = make_box(r3d, {8.0f, 0.2f, 8.0f});
        const int steps = 16;
        const float dt = 1.0f / 30.0f;
        const auto target_at = [](int i) {
            const float a = 0.35f * static_cast<float>(i);
            return Vec3f{0.45f + 0.25f * std::cos(a), 1.65f + 0.3f * std::sin(a), 0.4f};
        };
        // The IK run, recording the pose it made at each step.
        std::vector<kin::Pose> poses;
        std::vector<Pixels> from_ik, from_clip;
        const auto draw = [&](kin::WorldRenderState3D& state, kin::View3D& view) {
            kin::Scene3D scene;
            scene.camera = ik_scene::camera();
            skin_lights(scene, 1);
            scene.taa = {.enabled = true};
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(190, 190, 190)});
            state.propagate_transforms();
            state.collect(scene);
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        {
            kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(30, 34, 40)});
            kin::EcsWorld world;
            kin::WorldRenderState3D state{world};
            kin::Animator3D animator;
            animator.add_ik(ik_scene::left_hand());
            const flecs::entity e = world.raw().entity().set(kin::Transform3D{}).set(kin::SkinnedMeshRenderer{.model = biped.model}).set(animator);
            for (int i = 0; i < steps; ++i) {
                e.get_mut<kin::Animator3D>()->ik("hand").set_target(target_at(i));
                state.animate(dt);
                from_ik.push_back(draw(state, view));
                assert(r3d.stats().skinned_copies == 2); // the body and the marker, parts of one model
                poses.push_back(e.get<kin::Animator3D>()->ik_pose);
            }
        }
        // The same poses as a clip: key k + 1 is the pose of step k (the player's time after step k is (k + 1) dt), and a key 0.
        auto model = std::make_shared<kin::Model>(*biped.model);
        {
            const kin::Skeleton& sk = model->skeletons[0];
            kin::AnimClip3D clip;
            clip.name = "baked";
            clip.duration = static_cast<float>(steps + 1) * dt;
            clip.channels.resize(sk.joint_count());
            for (std::size_t j = 0; j < sk.joint_count(); ++j) {
                kin::JointChannels& ch = clip.channels[j];
                for (int k = 0; k <= steps; ++k) {
                    const kin::JointPose& jp = poses[static_cast<std::size_t>(std::max(k - 1, 0))].joints[j];
                    const float t = static_cast<float>(k) * dt;
                    ch.translation.times.push_back(t);
                    ch.translation.values.push_back(jp.translation);
                    ch.rotation.times.push_back(t);
                    ch.rotation.values.push_back(jp.rotation);
                    ch.scale.times.push_back(t);
                    ch.scale.values.push_back(jp.scale);
                }
            }
            model->clips.push_back(std::move(clip));
        }
        {
            kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(30, 34, 40)});
            kin::EcsWorld world;
            kin::WorldRenderState3D state{world};
            world.raw().entity().set(kin::Transform3D{}).set(kin::SkinnedMeshRenderer{.model = model}).set(kin::Animator3D{.clip = "baked"});
            for (int i = 0; i < steps; ++i) {
                state.animate(dt);
                from_clip.push_back(draw(state, view));
            }
        }
        int worst = 0;
        for (int i = 0; i < steps; ++i) {
            worst = std::max(worst, pixels_differing(from_ik[static_cast<std::size_t>(i)], from_clip[static_cast<std::size_t>(i)], 4));
        }
        const ik_scene::Blob first = ik_scene::red_blob(from_ik.front()), last = ik_scene::red_blob(from_ik.back());
        std::cerr << "ik under taa: the marker moves " << std::hypot(last.x - first.x, last.y - first.y) << " px over " << steps
                  << " frames; the IK run and the baked clip differ in at most " << worst << " of 19200 pixels by more than 4 levels\n";
        assert(std::hypot(last.x - first.x, last.y - first.y) > 4.0f);
        assert(worst <= 10);
        dump(from_ik.back(), "ik_taa_last");
    });
}

// Only IK moves the limb: the shadow cache sees it (the matrices are in its key), the picture is the uncached render's, and it arms again.
void test_gpu_ik_shadow_cache() {
    with_gpu("test_gpu_ik_shadow_cache", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const ik_scene::Biped biped = ik_scene::make_biped(r3d);
        const kin::Mesh floor = make_box(r3d, {10.0f, 0.2f, 10.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::View3D reference_view = r3d.create_view({160, 120}, {.samples = 1});
        kin::EcsWorld world;
        kin::WorldRenderState3D state{world};
        kin::Animator3D animator;
        animator.add_ik(ik_scene::left_hand());
        const flecs::entity e = world.raw().entity().set(kin::Transform3D{}).set(kin::SkinnedMeshRenderer{.model = biped.model}).set(animator);
        Vec3f target{0.5f, 1.7f, 0.4f};
        const auto shoot = [&](kin::View3D& v, bool cache) {
            e.get_mut<kin::Animator3D>()->ik("hand").set_target(target);
            state.animate(1.0f / 60.0f);
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 4.0f, 4.0f}, {0.0f, 0.9f, 0.0f}, 50.0f);
            scene.lights.sun = {.direction = {1.0f, 1.0f, 0.4f}, .color = {1.0f, 1.0f, 1.0f}};
            scene.lights.ambient = {0.15f, 0.15f, 0.15f};
            scene.lights.shadow_area = kin::Bounds3{{-5.0f, -0.2f, -5.0f}, {5.0f, 3.0f, 5.0f}};
            scene.lights.shadow_resolution = 1024;
            scene.lights.cache_shadows = cache;
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180)});
            state.propagate_transforms();
            state.collect(scene);
            Pixels p = shoot_view(renderer, r3d, v, scene);
            renderer.present();
            return p;
        };
        const Pixels first = shoot(view, true);
        if (r3d.stats().shadow_cascades == 0) {
            std::cerr << "shadows: not available on this device, skipped\n";
            return;
        }
        assert(!r3d.stats().shadow_cached);
        for (int i = 0; i < 4; ++i) {
            shoot(view, true);
        }
        assert(r3d.stats().shadow_cached && r3d.stats().shadow_draws == 0);
        // The hand's target moves: nothing else does.
        target = {0.7f, 1.3f, 0.2f};
        const Pixels moved = shoot(view, true);
        assert(!r3d.stats().shadow_cached && r3d.stats().shadow_draws > 0);
        assert(moved.rgba == shoot(reference_view, false).rgba);
        assert(pixels_differing(moved, first, 3) > 0);
        bool armed = false;
        for (int i = 0; i < 14 && !armed; ++i) {
            shoot(view, true);
            armed = r3d.stats().shadow_cached;
        }
        assert(armed);
        assert(shoot(view, true).rgba == moved.rgba);
        std::cerr << "ik shadow cache: a hand moved only by IK invalidates it, the picture is the uncached one's, and it arms again\n";
    });
}

// Many characters with IK, every hand somewhere else: the same pictures twice, and on workers as serial.
void test_gpu_ik_on_workers() {
    with_gpu("test_gpu_ik_on_workers", [](kin::Renderer2D& renderer) {
        kin::JobSystem jobs;
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const ik_scene::Biped biped = ik_scene::make_biped(r3d);
        const kin::Mesh floor = make_box(r3d, {80.0f, 0.2f, 80.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const auto run = [&](kin::JobSystem* workers, int count) {
            kin::EcsWorld world;
            kin::WorldRenderState3D state{world};
            const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count))));
            for (int i = 0; i < count; ++i) {
                kin::Animator3D a;
                a.add_ik(ik_scene::left_hand()).set_target(Vec3f{0.0f, 0.0f, 0.0f});
                const float x = (static_cast<float>(i % side) - static_cast<float>(side) * 0.5f) * (60.0f / static_cast<float>(side));
                const float z = (static_cast<float>(i / side) - static_cast<float>(side) * 0.5f) * (60.0f / static_cast<float>(side));
                const Vec3f shoulder{x + 0.2f, 1.65f, z};
                a.ik("hand").set_target(shoulder + Vec3f{0.3f * std::sin(static_cast<float>(i)), 0.4f * std::cos(static_cast<float>(i) * 0.7f), 0.5f});
                world.raw().entity().set(kin::Transform3D{.position = {x, 0.0f, z}}).set(kin::SkinnedMeshRenderer{.model = biped.model}).set(a);
            }
            renderer.set_job_system(workers);
            Pixels p;
            for (int step = 0; step < 3; ++step) {
                state.animate(1.0f / 60.0f);
                kin::Scene3D scene;
                scene.camera = kin::Camera3D::look_at({0.0f, 30.0f, 26.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
                skin_lights(scene, 1);
                scene.lights.shadow_area = kin::Bounds3{{-40.0f, -0.3f, -40.0f}, {40.0f, 8.0f, 40.0f}};
                scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
                state.propagate_transforms();
                state.collect(scene);
                p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
            }
            renderer.set_job_system(nullptr);
            return p;
        };
        for (const int count : {40, 2200}) {
            const Pixels a = run(nullptr, count);
            const Pixels b = run(nullptr, count);
            const Pixels c = run(&jobs, count);
            assert(a.rgba == b.rgba && a.rgba == c.rgba);
            assert(pixels_lit(a) > 1500);
            if (count == 2200) {
                dump(a, "ik_crowd");
            }
        }
        std::cerr << "ik characters: two runs, and 40 and 2,200 on workers: the same pictures\n";
    });
}

// --- The limits of P1b, fixed: FABRIK as a constraint, joint limits, foot locking and toes, as the renderer draws them ---

namespace ik_scene {

// A biped with toes (skin_fixtures::make_biped_toes) and one bright cube bound to `joint` at `offset` in the joint's own space (unlit: its
// pixels are told), so that where the renderer draws a joint can be read from the picture.
struct Marked {
    std::shared_ptr<kin::Model> model;
    kin::Mesh body, marker;
};

Marked make_marked(kin::Renderer3D& r3d, int joint, Vec3f offset = {}) {
    Marked b;
    b.model = std::make_shared<kin::Model>();
    b.model->skeletons.push_back(skin_fixtures::make_biped_toes());
    const kin::Skeleton& sk = b.model->skeletons[0];
    b.body = r3d.create_mesh(skin_fixtures::make_biped_body(sk));
    std::vector<Mat4f> world;
    kin::compute_world_matrices(sk, sk.rest_pose(), world);
    const Mat4f& w = world[static_cast<std::size_t>(joint)];
    const Vec3f at = Vec3f{w.m[12], w.m[13], w.m[14]} + offset;
    kin::MeshData cube;
    const float r = 0.06f;
    skin_fixtures::append_box(cube, at - Vec3f{0, 0, r}, at + Vec3f{0, 0, r}, r, joint, joint);
    b.marker = r3d.create_mesh(cube);
    assert(b.body.valid() && b.marker.valid());
    kin::ModelPart body;
    body.name = "body";
    body.mesh = b.body;
    body.material = {.color = Color::rgb(170, 170, 180)};
    body.skin = 0;
    kin::ModelPart marker;
    marker.name = "marker";
    marker.mesh = b.marker;
    marker.material = {.color = Color::rgb(255, 40, 30), .unlit = true};
    marker.skin = 0;
    b.model->parts = {body, marker};
    return b;
}

// One frame of an entity: animate, propagate, collect, draw.
Pixels frame(kin::Renderer2D& renderer, kin::Renderer3D& r3d, kin::View3D& view, kin::WorldRenderState3D& state, float dt = 1.0f / 60.0f) {
    state.animate(dt);
    kin::Scene3D scene;
    scene.camera = camera();
    skin_lights(scene, 1);
    state.propagate_transforms();
    state.collect(scene);
    Pixels p = shoot_view(renderer, r3d, view, scene);
    renderer.present();
    return p;
}

// Where `local` of joint `j` is in the world, by the animator's own IK matrices (the CPU's answer), and where that is on the screen.
Vec3f joint_world(const kin::Animator3D& a, int j, const Mat4f& entity, Vec3f local = {}) {
    return entity.apply(a.ik_world[static_cast<std::size_t>(j)].apply(local));
}
kin::Vec2f on_screen(Vec3f p) { return ik_scene::camera().project(p, screen)->pos; }

kin::IKConstraint feet_constraint(kin::GroundQuery ground, bool toes) {
    kin::IKConstraint k;
    k.name = "feet";
    k.kind = kin::IKKind::FootPlacer;
    k.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
    k.foot_heights = {0.05f, 0.05f};
    k.pole = kin::IKConstraint::Pole::ModelDirection;
    k.pole_vector = {0.0f, 0.0f, 1.0f};
    k.foot.smooth_seconds = 0.0f;
    k.two_bone.max_stretch = 1.3f;
    if (toes) {
        k.toe_joints = {"toe_L", "toe_R"};
        k.toe_heights = {0.0f, 0.0f};
    }
    k.ground = std::move(ground);
    return k;
}

} // namespace ik_scene

// FABRIK as a constraint kind, joint limits, foot locking and toes: the marker the renderer draws on a joint is where the CPU's IK pose puts that
// joint (within a pixel and a half), and the behaviour each of them is for shows in the picture.
void test_gpu_ik_limits_fabrik_locking_toes() {
    with_gpu("test_gpu_ik_limits_fabrik_locking_toes", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        namespace B = skin_fixtures::biped;
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(30, 34, 40)});

        // FABRIK: the head on the end of the spine chain goes to the target, and stays there as the pole turns the chain's bulge.
        {
            const ik_scene::Marked m = ik_scene::make_marked(r3d, B::head);
            kin::EcsWorld world;
            kin::WorldRenderState3D state{world};
            const kin::Transform3D placed{.position = {0.1f, 0.0f, 0.2f}, .rotation = kin::Quatf::axis_angle({0, 1, 0}, 20.0f)};
            kin::Animator3D animator;
            kin::IKConstraint k;
            k.name = "spine";
            k.kind = kin::IKKind::Fabrik;
            k.joints = {"pelvis", "spine", "chest", "neck", "head"};
            animator.add_ik(k);
            const flecs::entity e = world.raw().entity().set(placed).set(kin::SkinnedMeshRenderer{.model = m.model}).set(animator);
            const Vec3f target = placed.matrix().apply({0.35f, 1.5f, 0.35f});
            kin::Vec2f first{};
            Pixels pictures[2];
            for (int side = 0; side < 2; ++side) {
                e.get_mut<kin::Animator3D>()->ik("spine").set_target(target).set_pole_direction({side == 0 ? 1.0f : -1.0f, 0.0f, 0.0f});
                Pixels p;
                for (int i = 0; i < 3; ++i) {
                    p = ik_scene::frame(renderer, r3d, view, state);
                }
                const ik_scene::Blob blob = ik_scene::red_blob(p);
                const kin::Vec2f at = ik_scene::on_screen(target);
                const float error = std::hypot(blob.x - at.x, blob.y - at.y);
                std::cerr << "ik fabrik, pole " << (side == 0 ? "+x" : "-x") << ": head marker at (" << blob.x << ", " << blob.y << "), target projects to (" << at.x
                          << ", " << at.y << "), " << error << " px\n";
                assert(blob.count > 10 && error < 1.5f);
                pictures[side] = p;
                if (side == 0) {
                    first = {blob.x, blob.y};
                }
                dump(p, std::string{"ik_fabrik_"} + std::to_string(side));
            }
            // The head did not move with the pole; the body did (the chain's bulge turned).
            assert(pictures[0].rgba != pictures[1].rgba);
            (void)first;
        }

        // Limits: the knee marker with the pole back and the left foot on a 0.2 m step. The marker is on the CPU's knee both ways; with the hinge and
        // 25 degrees of hip twist the knee is in front of the line hip-ankle, without it behind: seen from the camera at +z (in front) as the knee
        // marker's pixels being nearer the camera: its screen y is lower (larger) for the knee in front when the camera is above... read from the CPU.
        for (const bool limited : {false, true}) {
            const ik_scene::Marked m = ik_scene::make_marked(r3d, B::knee_l);
            kin::EcsWorld world;
            kin::WorldRenderState3D state{world};
            kin::Animator3D animator;
            kin::IKConstraint feet = ik_scene::feet_constraint(
                [](Vec3f from, Vec3f to) -> std::optional<kin::GroundHit> {
                    const float h = from.x > 0.0f ? 0.2f : 0.0f;
                    if (from.y < h || to.y > h) {
                        return std::nullopt;
                    }
                    return kin::GroundHit{{from.x, h, from.z}, {0, 1, 0}};
                },
                false);
            feet.pole_vector = {0.0f, 0.0f, -1.0f};
            if (limited) {
                feet.two_bone.hinge = true;
                feet.two_bone.hinge_axis = {1, 0, 0};
                feet.two_bone.max_twist_degrees = 25.0f;
                feet.two_bone.max_bend_degrees = 150.0f;
            }
            animator.add_ik(feet);
            const flecs::entity e = world.raw().entity().set(kin::Transform3D{}).set(kin::SkinnedMeshRenderer{.model = m.model}).set(animator);
            Pixels p;
            for (int i = 0; i < 3; ++i) {
                p = ik_scene::frame(renderer, r3d, view, state);
            }
            const kin::Animator3D& a = *e.get<kin::Animator3D>();
            const Vec3f knee = ik_scene::joint_world(a, B::knee_l, Mat4f{});
            const Vec3f hip = ik_scene::joint_world(a, B::hip_l, Mat4f{}), ankle = ik_scene::joint_world(a, B::ankle_l, Mat4f{});
            const ik_scene::Blob blob = ik_scene::red_blob(p);
            const kin::Vec2f at = ik_scene::on_screen(knee);
            const float error = std::hypot(blob.x - at.x, blob.y - at.y);
            const Vec3f axis = normalize(ankle - hip);
            const float in_front = dot(knee - hip - axis * dot(knee - hip, axis), Vec3f{0, 0, 1});
            std::cerr << "ik knee, pole back, " << (limited ? "hinge + 25 degrees of twist" : "no limits") << ": knee marker " << error << " px from the CPU's knee, " << in_front
                      << " m in front of the hip-ankle line, ankle height " << ankle.y << "\n";
            assert(blob.count > 10 && error < 1.5f && std::fabs(ankle.y - 0.25f) < 2e-3f);
            assert(limited ? in_front > 0.02f : in_front < -0.02f);
            dump(p, limited ? "ik_knee_limited" : "ik_knee_free");
        }

        // Toes: the marker is on the toe's tip (0.1 m along its own z): drawn where the CPU's toe tilted it, on a 25 degree ramp under the ball of the
        // foot; and with the toes off (no toe constraint joints) it is somewhere else.
        {
            const ik_scene::Marked m = ik_scene::make_marked(r3d, B::toe_l, {0.0f, 0.0f, 0.1f});
            const auto ground = [](Vec3f from, Vec3f to) -> std::optional<kin::GroundHit> {
                const float t = std::tan(25.0f * 3.14159265f / 180.0f);
                const float h = from.z > 0.1f ? (from.z - 0.1f) * t : 0.0f;
                if (from.y < h || to.y > h) {
                    return std::nullopt;
                }
                return kin::GroundHit{{from.x, h, from.z}, from.z > 0.1f ? normalize(Vec3f{0, 1, -t}) : Vec3f{0, 1, 0}};
            };
            kin::Vec2f with_toes{}, without{};
            for (const bool toes : {true, false}) {
                kin::EcsWorld world;
                kin::WorldRenderState3D state{world};
                kin::Animator3D animator;
                animator.add_ik(ik_scene::feet_constraint(ground, toes));
                const flecs::entity e = world.raw().entity().set(kin::Transform3D{}).set(kin::SkinnedMeshRenderer{.model = m.model}).set(animator);
                Pixels p;
                for (int i = 0; i < 3; ++i) {
                    p = ik_scene::frame(renderer, r3d, view, state);
                }
                const kin::Animator3D& a = *e.get<kin::Animator3D>();
                const Vec3f tip = ik_scene::joint_world(a, B::toe_l, Mat4f{}, {0.0f, 0.0f, 0.1f});
                const ik_scene::Blob blob = ik_scene::red_blob(p);
                const kin::Vec2f at = ik_scene::on_screen(tip);
                const float error = std::hypot(blob.x - at.x, blob.y - at.y);
                const Vec3f toe_up = normalize(a.ik_world[B::toe_l].apply_vector({0, 1, 0}));
                std::cerr << "ik toes " << (toes ? "on" : "off") << ": tip marker at (" << blob.x << ", " << blob.y << "), " << error << " px from the CPU's, toe tilted "
                          << std::acos(std::clamp(toe_up.y, -1.0f, 1.0f)) * 57.29578f << " degrees, tip height " << tip.y << "\n";
                assert(blob.count > 6 && error < 1.5f);
                (toes ? with_toes : without) = {blob.x, blob.y};
                if (toes) {
                    assert(std::fabs(std::acos(toe_up.y) * 57.29578f - 25.0f) < 0.2f && tip.y > 0.02f);
                } else {
                    assert(std::acos(std::clamp(toe_up.y, -1.0f, 1.0f)) * 57.29578f < 0.1f);
                }
                dump(p, toes ? "ik_toes_on" : "ik_toes_off");
            }
            assert(std::hypot(with_toes.x - without.x, with_toes.y - without.y) > 0.3f);
        }

        // Locking: the ankle marker while the entity walks along x at 4 cm a frame. Locked, it is where it was put (in the world: the same pixel), for
        // the frames before the release; unlocked, it goes with the body (a pixel or more a frame).
        {
            const ik_scene::Marked m = ik_scene::make_marked(r3d, B::ankle_l);
            for (const bool lock : {true, false}) {
                kin::EcsWorld world;
                kin::WorldRenderState3D state{world};
                kin::Animator3D animator;
                kin::IKConstraint feet = ik_scene::feet_constraint(
                    [](Vec3f from, Vec3f to) -> std::optional<kin::GroundHit> {
                        if (from.y < 0.0f || to.y > 0.0f) {
                            return std::nullopt;
                        }
                        return kin::GroundHit{{from.x, 0.0f, from.z}, {0, 1, 0}};
                    },
                    false);
                feet.foot.lock_feet = lock;
                feet.foot.lock_release_distance = 0.5f; // held for all of the walk (the leg stretches to it)
                animator.add_ik(feet);
                const flecs::entity e = world.raw().entity().set(kin::Transform3D{}).set(kin::SkinnedMeshRenderer{.model = m.model}).set(animator);
                kin::Vec2f start{};
                float worst = 0.0f;
                for (int i = 0; i < 8; ++i) {
                    e.get_mut<kin::Transform3D>()->position = {0.04f * static_cast<float>(i), 0.0f, 0.0f};
                    const Pixels p = ik_scene::frame(renderer, r3d, view, state);
                    const ik_scene::Blob blob = ik_scene::red_blob(p);
                    const kin::Animator3D& a = *e.get<kin::Animator3D>();
                    const kin::Vec2f cpu = ik_scene::on_screen(ik_scene::joint_world(a, B::ankle_l, e.get<kin::Transform3D>()->matrix()));
                    assert(blob.count >= 4 && std::hypot(blob.x - cpu.x, blob.y - cpu.y) < 1.5f);
                    if (i == 0) {
                        start = {blob.x, blob.y};
                    }
                    worst = std::max(worst, std::hypot(blob.x - start.x, blob.y - start.y));
                    if (i == 7) {
                        dump(p, lock ? "ik_lock_on" : "ik_lock_off");
                    }
                }
                std::cerr << "ik foot lock " << (lock ? "on" : "off") << ": the ankle marker moves " << worst << " px over 28 cm of walking\n";
                assert(lock ? worst < 1.5f : worst > 6.0f);
            }
        }
    });
}

// Everything on with skinned copies: cascades 4, ambient occlusion, contact shadows, temporal anti-aliasing, with a job system and without,
// over several frames of moving poses: a picture, the same one both ways, with every pass counted.
void test_gpu_skinned_everything() {
    with_gpu("test_gpu_skinned_everything", [](kin::Renderer2D& renderer) {
        kin::JobSystem jobs;
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes") || lacks(r3d.capabilities().temporal_aa && r3d.capabilities().ambient_occlusion, "taa and ao")) {
            return;
        }
        const kin::Mesh floor = make_box(r3d, {80.0f, 0.2f, 80.0f});
        SkinRig rig{r3d, 9, 8};
        kin::View3D serial_view = r3d.create_view({160, 120}, {.samples = 1, .pick = true});
        kin::View3D worker_view = r3d.create_view({160, 120}, {.samples = 1, .pick = true});
        for (const int count : {400, 2600}) {
            serial_view.reset_history();
            worker_view.reset_history();
            Pixels last;
            for (int f = 0; f < 4; ++f) {
                kin::Scene3D scene;
                scene.camera = kin::Camera3D::look_at({0.0f, 30.0f, 26.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
                skin_lights(scene, 4);
                scene.lights.shadow_area = kin::Bounds3{{-40.0f, -0.3f, -40.0f}, {40.0f, 8.0f, 40.0f}};
                scene.lights.shadow_distance = 80.0f;
                scene.ambient_occlusion = {.enabled = true};
                scene.contact_shadows = {.enabled = true};
                scene.taa = {.enabled = true};
                scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
                const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count))));
                for (int i = 0; i < count; ++i) {
                    const float a = static_cast<float>((i * 37 + f * 11) % 160) - 80.0f, b = static_cast<float>((i * 91 + f * 7) % 160) - 80.0f;
                    const float x = (static_cast<float>(i % side) - static_cast<float>(side) * 0.5f) * (60.0f / static_cast<float>(side));
                    const float z = (static_cast<float>(i / side) - static_cast<float>(side) * 0.5f) * (60.0f / static_cast<float>(side));
                    scene.draw_skinned(rig.mesh, rig.matrices(a, b), Mat4f::translation({x, 0.0f, z}) * Mat4f::scaling({0.5f, 0.5f, 0.5f}), {.color = Color::rgb(200, 90, 70)},
                                       kin::colors::white, static_cast<kin::u32>(i + 1));
                }
                renderer.set_job_system(nullptr);
                const Pixels serial = shoot_view(renderer, r3d, serial_view, scene);
                const kin::Render3DStats s1 = r3d.stats();
                renderer.present();
                renderer.set_job_system(&jobs);
                const Pixels parallel = shoot_view(renderer, r3d, worker_view, scene);
                const kin::Render3DStats s2 = r3d.stats();
                renderer.present();
                renderer.set_job_system(nullptr);
                assert(serial.rgba == parallel.rgba);
                assert(s1.skinned_copies == s2.skinned_copies && s1.skinned_copies > 0 && s1.shadow_cascades == 4 && s1.ao_dispatches > 0 && s1.taa_dispatches > 0);
                assert(s1.contact_dispatches > 0 && s1.ao_instances == s1.skinned_copies + 1);
                last = serial;
            }
            assert(pixels_lit(last) > 2000);
            dump(last, "skinned_everything_" + std::to_string(count));
            // A copy's pixel names its id.
            const kin::PickResult r = r3d.pick(worker_view, {80, 60});
            assert(r.hit);
        }
        std::cerr << "skinned everything (cascades 4, ao, contact, taa): serial and workers agree at 400 and 2,600 copies\n";
    });
}


// --- Physics debug lines (P2b): opt-in, off is the picture without them ---

// Debug lines of a World3D drawn as an unlit wireframe: with the switch off the picture is the one without the physics
// state at all, to the byte; on, the awake body's edges are in its colour and (once it sleeps) in the sleeping colour.
void test_gpu_physics_debug_lines() {
    with_gpu("test_gpu_physics_debug_lines", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh slab = make_box(r3d, {6.0f, 0.2f, 6.0f});
        const kin::Mesh crate = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        kin::EcsWorld ecs;
        flecs::world& world = ecs.raw();
        kin::World3DConfig cfg;
        cfg.single_threaded = true;
        kin::World3D physics(cfg);
        kin::WorldPhysicsState3D bodies{ecs, physics};
        kin::WorldRenderState3D state{ecs};
        kin::Collider3D floor_shape;
        floor_shape.half_extents = {3.0f, 0.1f, 3.0f};
        world.entity().set(kin::Transform3D{.position = {0, -0.1f, 0}}).set(kin::MeshRenderer{.mesh = slab, .material = {.color = Color::rgb(60, 60, 60), .unlit = true}}).set(floor_shape);
        const flecs::entity box = world.entity()
                                      .set(kin::Transform3D{.position = {0, 1.0f, 0}})
                                      .set(kin::MeshRenderer{.mesh = crate, .material = {.color = Color::rgb(200, 120, 40), .unlit = true}})
                                      .set(kin::Collider3D{.kind = kin::Collider3D::Kind::Box, .half_extents = {0.5f, 0.5f, 0.5f}})
                                      .set(kin::RigidBody3D{});
        const auto camera = kin::Camera3D::look_at({0.0f, 2.5f, 6.5f}, {0.0f, 0.5f, 0.0f}, 45.0f);
        const auto shoot = [&](bool with_physics_state, bool debug) {
            kin::Scene3D scene;
            scene.camera = camera;
            scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
            scene.lights.ambient = {0.0f, 0.0f, 0.0f};
            state.propagate_transforms();
            state.collect(scene);
            if (with_physics_state) {
                bodies.config().debug_draw = debug;
                bodies.collect_debug(r3d, scene);
            }
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const auto count_near = [](const Pixels& p, Color want, int tolerance) {
            int n = 0;
            for (int y = 0; y < p.size.y; ++y) {
                for (int x = 0; x < p.size.x; ++x) {
                    n += near(p.at(x, y), want, tolerance) ? 1 : 0;
                }
            }
            return n;
        };
        bodies.sync_bodies();
        // Falling: the box is awake.
        bodies.step(1.0f / 60.0f);
        const Pixels plain = shoot(false, false);
        const Pixels off = shoot(true, false);
        const Pixels on = shoot(true, true);
        assert(plain.rgba == off.rgba);
        assert(plain.rgba != on.rgba);
        const kin::DebugDraw3DOptions colours;
        const int awake = count_near(on, colours.dynamic_body, 24);
        const int fixed = count_near(on, colours.static_body, 24);
        std::cerr << "physics debug lines: " << awake << " px of the awake colour, " << fixed << " of the static colour, "
                  << count_near(plain, colours.dynamic_body, 24) << " without them\n";
        dump(on, "physics_debug_lines_awake");
        assert(awake > 15 && fixed > 30);
        assert(count_near(plain, colours.dynamic_body, 24) == 0);
        // At rest and asleep: the box's edges are in the sleeping colour, none in the awake one.
        for (int i = 0; i < 180; ++i) {
            bodies.step(1.0f / 60.0f);
        }
        assert(!physics.is_active(bodies.body_of(box)));
        const Pixels asleep = shoot(true, true);
        const int sleeping = count_near(asleep, colours.sleeping_body, 24);
        std::cerr << "physics debug lines, asleep: " << sleeping << " px of the sleeping colour\n";
        assert(sleeping > 15 && count_near(asleep, colours.dynamic_body, 24) == 0);
        dump(asleep, "physics_debug_lines_asleep");
        // Where the lines are: the box's top front edge (0.5 up, 0.5 in front) is drawn at its projection.
        const kin::Vec2f edge = camera.project({0.0f, 1.0f, 0.5f}, screen)->pos;
        bool found = false;
        for (int dy = -2; dy <= 2 && !found; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                found = found || near(asleep.at(static_cast<int>(edge.x) + dx, static_cast<int>(edge.y) + dy), colours.sleeping_body, 24);
            }
        }
        assert(found);
        // A ray and its hit through the same view.
        bodies.config().debug_draw = true;
        const auto hit = physics.raycast({0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
        assert(hit && std::fabs(hit->point.y - 1.0f) < 0.03f);
        bodies.debug().ray({0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, hit);
        const Pixels ray = shoot(true, true);
        assert(count_near(ray, colours.ray, 24) > 5 || count_near(ray, colours.ray_hit, 24) > 5);
        dump(ray, "physics_debug_lines_ray");
    });
}

// A ragdoll's pose reaches the render: the biped (the IK fixture, a red cube on the left hand) falls as a ragdoll in the physics
// world and lies; the cube's centre in the picture is where the CPU's pose (the animator's world matrix of the hand joint) projects,
// within a pixel and a half, at several moments of the fall, with the entity moved, turned and scaled; and with the share of
// the ragdoll at 0 the picture is the clip's, byte for byte.
void test_gpu_ragdoll_pose_reaches_render() {
    with_gpu("test_gpu_ragdoll_pose_reaches_render", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes")) {
            return;
        }
        const ik_scene::Biped biped = ik_scene::make_biped(r3d);
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(30, 34, 40)});
        struct Case {
            const char* what;
            kin::Transform3D transform;
        };
        const std::vector<Case> cases = {
            {"upright start", {.position = {0.0f, 1.2f, 0.0f}}},
            {"entity turned and scaled", {.position = {-0.3f, 1.0f, 0.0f}, .rotation = kin::Quatf::axis_angle({0, 1, 0}, 40.0f), .scale = {1.2f, 1.2f, 1.2f}}},
        };
        for (const Case& c : cases) {
            kin::EcsWorld ecs;
            kin::World3DConfig cfg;
            cfg.single_threaded = true;
            kin::World3D physics(cfg);
            kin::WorldPhysicsState3D bodies{ecs, physics};
            kin::WorldRenderState3D state{ecs};
            ecs.raw().entity().set(kin::Transform3D{.position = {0.0f, -0.5f, 0.0f}}).set(kin::Collider3D{.kind = kin::Collider3D::Kind::Box, .half_extents = {6.0f, 0.5f, 6.0f}});
            kin::Animator3D animator;
            const flecs::entity e = ecs.raw().entity().set(c.transform).set(kin::SkinnedMeshRenderer{.model = biped.model}).set(animator).set(kin::Ragdoll3D{});
            const auto shoot = [&]() {
                kin::Scene3D scene;
                scene.camera = ik_scene::camera();
                skin_lights(scene, 1);
                state.propagate_transforms();
                state.collect(scene);
                Pixels p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                return p;
            };
            bodies.sync_bodies();
            // Animated: nothing of the ragdoll in the picture.
            bodies.step(1.0f / 60.0f);
            state.animate(1.0f / 60.0f);
            const Pixels clip_pose = shoot();
            assert(e.get<kin::Animator3D>()->pose_override.empty());
            bodies.start_ragdoll(e, {.set_velocity = true, .velocity = {1.5f, 0.0f, 0.0f}});
            float worst = 0.0f;
            int moments = 0;
            for (int i = 1; i <= 90; ++i) {
                bodies.step(1.0f / 60.0f);
                state.animate(1.0f / 60.0f);
                if (i % 15 != 0) {
                    continue;
                }
                const Pixels p = shoot();
                const kin::Animator3D& a = *e.get<kin::Animator3D>();
                assert(!a.pose_override.empty() && a.ik_world.size() == 18);
                const Mat4f m = e.get<kin::Transform3D>()->matrix() * a.ik_world[static_cast<std::size_t>(skin_fixtures::biped::hand_l)];
                const Vec3f hand{m.m[12], m.m[13], m.m[14]};
                // The cube marker is centred a little off the joint (a cube round it): the centre of mass of its red pixels is the
                // projection of the joint's place; where the marker is out of the view or hidden by the body, the moment is skipped.
                const auto where = ik_scene::camera().project(hand, screen);
                const ik_scene::Blob blob = ik_scene::red_blob(p);
                if (!where || blob.count < 20) {
                    continue;
                }
                const float error = std::hypot(blob.x - where->pos.x, blob.y - where->pos.y);
                worst = std::max(worst, error);
                ++moments;
                if (i == 45) {
                    dump(p, std::string{"ragdoll_pose_"} + std::to_string(&c - cases.data()));
                }
            }
            std::cerr << "ragdoll pose, " << c.what << ": " << moments << " moments with the marker in view, the worst marker error " << worst << " px\n";
            assert(moments >= 2 && worst < 1.5f);
            (void)clip_pose;
        }
        // Blend 0: the picture is the clip's.
        {
            kin::EcsWorld ecs;
            kin::World3DConfig cfg;
            cfg.single_threaded = true;
            kin::World3D physics(cfg);
            kin::WorldPhysicsState3D bodies{ecs, physics};
            kin::WorldRenderState3D state{ecs};
            kin::Animator3D animator;
            const flecs::entity with = ecs.raw().entity().set(kin::Transform3D{.position = {0.0f, 1.2f, 0.0f}}).set(kin::SkinnedMeshRenderer{.model = biped.model}).set(animator).set(kin::Ragdoll3D{});
            kin::EcsWorld plain_ecs;
            kin::WorldRenderState3D plain_state{plain_ecs};
            kin::Animator3D animator2;
            plain_ecs.raw().entity().set(kin::Transform3D{.position = {0.0f, 1.2f, 0.0f}}).set(kin::SkinnedMeshRenderer{.model = biped.model}).set(animator2);
            bodies.sync_bodies();
            bodies.step(1.0f / 60.0f);
            state.animate(1.0f / 60.0f);
            plain_state.animate(1.0f / 60.0f);
            const auto shoot_of = [&](kin::WorldRenderState3D& st) {
                kin::Scene3D scene;
                scene.camera = ik_scene::camera();
                skin_lights(scene, 1);
                st.propagate_transforms();
                st.collect(scene);
                Pixels p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
                return p;
            };
            const Pixels a = shoot_of(state);
            const Pixels b = shoot_of(plain_state);
            assert(a.rgba == b.rgba);
            // And with the whole share at 1, held with no physics at all (alpha 1 at once), the bodies' pose is drawn: it differs.
            bodies.start_ragdoll(with, {});
            for (int i = 0; i < 20; ++i) {
                bodies.step(1.0f / 60.0f);
                state.animate(1.0f / 60.0f);
            }
            const Pixels c = shoot_of(state);
            assert(c.rgba != b.rgba);
            // A blend 0 again (the share falls to 0): the clip's picture, byte for byte.
            bodies.animate_ragdoll(with);
            bodies.step(1.0f / 60.0f);
            state.animate(1.0f / 60.0f);
            plain_state.animate(1.0f / 60.0f);
            const Pixels d = shoot_of(state);
            const Pixels f = shoot_of(plain_state);
            assert(d.rgba == f.rgba);
            std::cerr << "ragdoll blend 0: the picture is the clip's byte for byte; the ragdoll's differs\n";
        }
    });
}

// The previous joint matrices include the ragdoll's pose: a ragdoll's fall under TAA gives, frame by frame, the picture of the same poses
// played from a clip (baked from the ragdoll's own poses), whose history is the same as any skinned animation's.
void test_gpu_ragdoll_taa_matches_clip() {
    with_gpu("test_gpu_ragdoll_taa_matches_clip", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().skinning, "skinned meshes") || lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        const ik_scene::Biped biped = ik_scene::make_biped(r3d);
        const kin::Mesh floor = make_box(r3d, {8.0f, 0.2f, 8.0f});
        const int steps = 16;
        const float dt = 1.0f / 30.0f;
        std::vector<kin::Pose> poses;
        std::vector<Pixels> from_ragdoll, from_clip;
        const auto draw = [&](kin::WorldRenderState3D& state, kin::View3D& view) {
            kin::Scene3D scene;
            scene.camera = ik_scene::camera();
            skin_lights(scene, 1);
            scene.taa = {.enabled = true};
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(190, 190, 190)});
            state.propagate_transforms();
            state.collect(scene);
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        {
            kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(30, 34, 40)});
            kin::EcsWorld ecs;
            kin::World3DConfig cfg;
            cfg.single_threaded = true;
            kin::World3D physics(cfg);
            kin::WorldPhysicsState3D bodies{ecs, physics};
            kin::WorldRenderState3D state{ecs};
            kin::Animator3D animator;
            const flecs::entity e = ecs.raw().entity().set(kin::Transform3D{.position = {0.0f, 0.9f, 0.0f}}).set(kin::SkinnedMeshRenderer{.model = biped.model}).set(animator).set(kin::Ragdoll3D{});
            bodies.sync_bodies();
            bodies.start_ragdoll(e, {.set_velocity = true, .velocity = {0.8f, 0.0f, 0.0f}});
            for (int i = 0; i < steps; ++i) {
                bodies.step(dt);
                state.animate(dt);
                state.propagate_transforms();
                state.apply_ik();
                poses.push_back(e.get<kin::Animator3D>()->ik_pose);
                from_ragdoll.push_back(draw(state, view));
                assert(r3d.stats().skinned_copies == 2);
            }
        }
        auto model = std::make_shared<kin::Model>(*biped.model);
        {
            const kin::Skeleton& sk = model->skeletons[0];
            kin::AnimClip3D clip;
            clip.name = "baked";
            clip.duration = static_cast<float>(steps + 1) * dt;
            clip.channels.resize(sk.joint_count());
            for (std::size_t j = 0; j < sk.joint_count(); ++j) {
                kin::JointChannels& ch = clip.channels[j];
                for (int k = 0; k <= steps; ++k) {
                    const kin::JointPose& jp = poses[static_cast<std::size_t>(std::max(k - 1, 0))].joints[j];
                    const float t = static_cast<float>(k) * dt;
                    ch.translation.times.push_back(t);
                    ch.translation.values.push_back(jp.translation);
                    ch.rotation.times.push_back(t);
                    ch.rotation.values.push_back(jp.rotation);
                    ch.scale.times.push_back(t);
                    ch.scale.values.push_back(jp.scale);
                }
            }
            model->clips.push_back(std::move(clip));
        }
        {
            kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(30, 34, 40)});
            kin::EcsWorld ecs;
            kin::WorldRenderState3D state{ecs};
            // The entity at the same place: the poses are model-space, so the entity's transform is the ragdoll entity's.
            ecs.raw().entity().set(kin::Transform3D{.position = {0.0f, 0.9f, 0.0f}}).set(kin::SkinnedMeshRenderer{.model = model}).set(kin::Animator3D{.clip = "baked"});
            for (int i = 0; i < steps; ++i) {
                state.animate(dt);
                from_clip.push_back(draw(state, view));
            }
        }
        int worst = 0;
        for (int i = 0; i < steps; ++i) {
            worst = std::max(worst, pixels_differing(from_ragdoll[static_cast<std::size_t>(i)], from_clip[static_cast<std::size_t>(i)], 4));
        }
        std::cerr << "ragdoll under taa: the ragdoll run and the baked clip differ in at most " << worst << " of 19200 pixels by more than 4 levels\n";
        assert(worst <= 10);
        dump(from_ragdoll.back(), "ragdoll_taa_last");
    });
}

// A rope drawn as a tube: where its points project the pixels are the rope's (an unlit colour, a sliver of tube), nowhere else; off, the
// picture is the one without the rope, byte for byte; on and lit, the rope casts a shadow on the floor.
void test_gpu_rope_tube() {
    with_gpu("test_gpu_rope_tube", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = make_box(r3d, {8.0f, 0.2f, 8.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(10, 12, 16)});
        kin::EcsWorld ecs;
        kin::World3DConfig cfg;
        cfg.single_threaded = true;
        kin::World3D physics(cfg);
        kin::WorldPhysicsState3D bodies{ecs, physics};
        kin::Rope3D rope;
        rope.start = kin::RopeAttach3D::at({-1.5f, 2.5f, 0.0f});
        rope.end = kin::RopeAttach3D::at({1.5f, 2.5f, 0.0f});
        rope.length = 3.4f;
        rope.radius = 0.06f;
        rope.render = false;
        rope.material = {.color = Color::rgb(255, 0, 255), .unlit = true};
        const flecs::entity holder = ecs.raw().entity().set(kin::Transform3D{}).set(rope);
        bodies.sync_bodies();
        for (int i = 0; i < 120; ++i) {
            bodies.step(1.0f / 60.0f);
        }
        const auto camera = kin::Camera3D::look_at({0.0f, 1.5f, 7.0f}, {0.0f, 1.8f, 0.0f}, 45.0f);
        const auto shoot = [&](bool with_ropes, bool floor_too) {
            kin::Scene3D scene;
            scene.camera = camera;
            scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
            scene.lights.ambient = {0.0f, 0.0f, 0.0f};
            if (floor_too) {
                scene.draw(floor, Mat4f::translation({0.0f, -0.2f, 0.0f}), {.color = Color::rgb(60, 60, 60), .unlit = true});
            }
            if (with_ropes) {
                bodies.collect_ropes(r3d, scene);
            }
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const auto magenta = [](const Pixels& p) {
            int n = 0;
            for (int y = 0; y < p.size.y; ++y) {
                for (int x = 0; x < p.size.x; ++x) {
                    const Color c = p.at(x, y);
                    n += c.r > 200 && c.g < 80 && c.b > 200 ? 1 : 0;
                }
            }
            return n;
        };
        // Off by default: nothing drawn, the picture is the empty one.
        const Pixels none = shoot(false, true);
        const Pixels off = shoot(true, true);
        assert(none.rgba == off.rgba && magenta(off) == 0);
        // On: the tube is where the points are.
        holder.get_mut<kin::Rope3D>()->render = true;
        bodies.step(1.0f / 60.0f);
        const Pixels on = shoot(true, false);
        const int count = magenta(on);
        const std::vector<kin::RopePoint3D>& pts = holder.get<kin::Rope3D>()->points();
        assert(pts.size() > 10);
        int hit = 0;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const auto where = camera.project(pts[i].position, screen);
            assert(where.has_value());
            bool found = false;
            for (int dy = -2; dy <= 2 && !found; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const int x = static_cast<int>(where->pos.x) + dx, y = static_cast<int>(where->pos.y) + dy;
                    if (x >= 0 && y >= 0 && x < on.size.x && y < on.size.y) {
                        const Color c = on.at(x, y);
                        found = found || (c.r > 200 && c.g < 80 && c.b > 200);
                    }
                }
            }
            hit += found ? 1 : 0;
        }
        std::cerr << "rope tube: " << count << " magenta pixels, " << hit << " of " << pts.size() << " joints have the tube at their projection\n";
        assert(count > 120 && hit == static_cast<int>(pts.size()));
        dump(on, "rope_tube");
        // Lit, with the sun: it casts a shadow on the floor under it (its own pixels differ with and without `casts_shadow`).
        const auto lit = [&](bool casts) {
            holder.get_mut<kin::Rope3D>()->material = {.color = Color::rgb(200, 160, 100), .casts_shadow = casts};
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 6.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, 45.0f);
            scene.lights.sun = {.direction = {0.1f, 1.0f, 0.2f}, .color = {1.0f, 1.0f, 1.0f}};
            scene.lights.ambient = {0.2f, 0.2f, 0.2f};
            scene.lights.shadow_area = kin::Bounds3{{-6, -1, -6}, {6, 4, 6}};
            scene.lights.shadow_resolution = 1024;
            scene.draw(floor, Mat4f::translation({0.0f, -0.2f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
            bodies.collect_ropes(r3d, scene);
            Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            return p;
        };
        const Pixels shadowed = lit(true);
        const Pixels unshadowed = lit(false);
        const int differing = pixels_differing(shadowed, unshadowed, 8);
        std::cerr << "rope shadow: " << differing << " pixels differ with the rope's shadow on or off\n";
        assert(differing > 40);
        dump(shadowed, "rope_shadow");
    });
}

// --- Sky, fog and time of day (P3a) ---

// The sun's elevation and azimuth at the March equinox (declination 0) are the textbook ones, the sweep through the day
// turns east to west, sunrise and sunset agree with the standard formula (and London's June day is 16 h 38), the polar
// day and night are found, the moon is where its phase puts it, the sun's light reddens toward the horizon and goes out
// under it, and the clock steps and wraps.
void test_time_of_day_formulae() {
    using kin::TimeOfDay3D;
    const auto at = [](float hours, float day, float latitude, float phase = 0.5f) {
        return kin::evaluate(TimeOfDay3D{.hours = hours, .day_of_year = day, .latitude = latitude, .moon_phase = phase});
    };
    for (const float latitude : {0.0f, 20.0f, 45.0f, 60.0f, -35.0f}) {
        const kin::SkyState3D noon = at(12.0f, 81.0f, latitude);
        assert(std::abs(noon.declination) < 0.05f);
        assert(std::abs(noon.sun_elevation - (90.0f - std::abs(latitude))) < 0.05f);
        assert(std::abs(kin::length(noon.sun_direction) - 1.0f) < 1e-5f);
    }
    // Noon: due south (+z) in the north, due north (-z) in the south.
    assert(std::abs(at(12.0f, 81.0f, 45.0f).sun_azimuth - 180.0f) < 0.5f && at(12.0f, 81.0f, 45.0f).sun_direction.z > 0.5f);
    const float south_azimuth = at(12.0f, 81.0f, -35.0f).sun_azimuth;
    assert(std::min(south_azimuth, 360.0f - south_azimuth) < 0.5f && at(12.0f, 81.0f, -35.0f).sun_direction.z < -0.5f);
    // The solstices: the tilt added to, and taken from, the equinox's elevation.
    assert(std::abs(at(12.0f, 172.0f, 45.0f).declination - 23.44f) < 0.1f);
    assert(std::abs(at(12.0f, 172.0f, 45.0f).sun_elevation - (90.0f - 45.0f + 23.44f)) < 0.1f);
    assert(std::abs(at(12.0f, 355.0f, 45.0f).sun_elevation - (90.0f - 45.0f - 23.44f)) < 0.3f);
    // The sweep: east at 6 (+x), west at 18 (-x), the azimuth growing all day, the elevation peaking at noon.
    assert(std::abs(at(6.0f, 81.0f, 45.0f).sun_azimuth - 90.0f) < 0.5f && std::abs(at(6.0f, 81.0f, 45.0f).sun_elevation) < 0.1f);
    assert(std::abs(at(18.0f, 81.0f, 45.0f).sun_azimuth - 270.0f) < 0.5f);
    assert(at(6.0f, 81.0f, 45.0f).sun_direction.x > 0.99f && at(18.0f, 81.0f, 45.0f).sun_direction.x < -0.99f);
    float last_azimuth = -1.0f, last_elevation = -90.0f;
    for (float hours = 6.5f; hours < 18.0f; hours += 0.5f) {
        const kin::SkyState3D s = at(hours, 81.0f, 45.0f);
        assert(s.sun_azimuth > last_azimuth);
        if (hours <= 12.0f) {
            assert(s.sun_elevation > last_elevation);
        } else {
            assert(s.sun_elevation < last_elevation);
        }
        last_azimuth = s.sun_azimuth;
        last_elevation = s.sun_elevation;
    }
    // Sunrise and sunset: the standard hour angle with 0.833 degrees of refraction and the sun's radius.
    const auto standard = [](double latitude, double declination) {
        const double r = 3.14159265358979 / 180.0;
        return std::acos((std::sin(-0.833 * r) - std::sin(latitude * r) * std::sin(declination * r)) / (std::cos(latitude * r) * std::cos(declination * r))) / r / 15.0;
    };
    for (const auto [latitude, day, declination] : {std::array<double, 3>{51.5, 172.0, 23.44}, std::array<double, 3>{0.0, 81.0, 0.0},
                                                    std::array<double, 3>{40.0, 355.0, -23.44}, std::array<double, 3>{-33.0, 172.0, 23.44}}) {
        const kin::SunTimes3D t = kin::sun_times({.day_of_year = static_cast<float>(day), .latitude = static_cast<float>(latitude)});
        const double half = standard(latitude, declination);
        assert(!t.polar_day && !t.polar_night);
        assert(std::abs(t.sunrise - (12.0 - half)) < 0.03 && std::abs(t.sunset - (12.0 + half)) < 0.03);
        // The elevation there is the refraction's -0.833 degrees.
        assert(std::abs(at(t.sunrise, static_cast<float>(day), static_cast<float>(latitude)).sun_elevation + 0.833f) < 0.1f);
        assert(std::abs(at(t.sunset, static_cast<float>(day), static_cast<float>(latitude)).sun_elevation + 0.833f) < 0.1f);
    }
    {
        const kin::SunTimes3D london = kin::sun_times({.day_of_year = 172.0f, .latitude = 51.5f});
        assert(std::abs((london.sunset - london.sunrise) - 16.63f) < 0.1f); // 16 h 38 min
        assert(london.sunrise > 3.5f && london.sunrise < 3.9f);             // solar time: 03:43 UTC (the equation of time is a few minutes)
        const kin::SunTimes3D equator = kin::sun_times({.day_of_year = 81.0f, .latitude = 0.0f});
        assert(std::abs((equator.sunset - equator.sunrise) - 12.1f) < 0.1f);
        assert(kin::sun_times({.day_of_year = 172.0f, .latitude = 80.0f}).polar_day);
        assert(kin::sun_times({.day_of_year = 355.0f, .latitude = 80.0f}).polar_night);
        assert(at(0.0f, 172.0f, 80.0f).sun_elevation > 0.0f); // the midnight sun
    }
    // The sun's light: warm and bright by day, redder low, none under the horizon; the ambient follows it.
    const kin::Sky3D sky{};
    const kin::SkyState3D noon = kin::evaluate({.hours = 12.0f, .day_of_year = 172.0f, .latitude = 45.0f}, sky);
    const kin::SkyState3D low = kin::evaluate({.hours = 19.4f, .day_of_year = 172.0f, .latitude = 45.0f}, sky);
    const kin::SkyState3D night = kin::evaluate({.hours = 0.0f, .day_of_year = 172.0f, .latitude = 45.0f, .moon_phase = 0.0f}, sky);
    assert(low.sun_elevation > 0.5f && low.sun_elevation < 7.0f);
    assert(noon.sun_color.x > noon.sun_color.y && noon.sun_color.y > noon.sun_color.z && noon.sun_color.z > 1.0f);
    assert(low.sun_color.x / low.sun_color.z > 2.0f * (noon.sun_color.x / noon.sun_color.z));
    assert(low.sun_color.x < noon.sun_color.x);
    assert(night.sun_elevation < -10.0f && night.sun_color.x == 0.0f && night.sun_color.y == 0.0f && night.sun_color.z == 0.0f);
    const auto sum = [](Vec3f v) { return v.x + v.y + v.z; };
    assert(sum(night.ambient) < 0.01f * sum(noon.ambient) && sum(night.ambient) > 0.0f);
    assert(sum(noon.ambient) > 0.15f && noon.ambient.z > noon.ambient.x);          // a blue sky's light
    assert(low.ambient.x / low.ambient.z > noon.ambient.x / noon.ambient.z);       // warmer at the end of the day
    assert(noon.daylight == 1.0f && night.daylight == 0.0f && low.daylight < 1.0f);
    assert(noon.fog_density > 0.0f && sum(noon.fog_color) > 0.0f);
    // The moon: opposite the sun at full, with it at new; its light follows its elevation and phase.
    const kin::SkyState3D full = at(0.0f, 81.0f, 45.0f, 0.5f), fresh = at(0.0f, 81.0f, 45.0f, 0.0f);
    assert(kin::dot(full.moon_direction, full.sun_direction) < -0.999f);
    assert(kin::dot(fresh.moon_direction, fresh.sun_direction) > 0.999f);
    assert(full.moon_elevation > 40.0f && full.moon_light > 0.09f && full.moon_color.z > full.moon_color.x);
    assert(fresh.moon_light == 0.0f && fresh.moon_illumination < 0.001f);
    assert(at(12.0f, 81.0f, 45.0f, 0.5f).moon_light == 0.0f); // a full moon at noon is under the horizon
    // Applied: the sun and the moon's fill light, the ambient colours, the sky's moon, the fog's colour.
    kin::Lights3D lights;
    kin::Sky3D sky_in;
    kin::Fog3D fog;
    const kin::SkyState3D applied = kin::apply({.hours = 0.0f, .day_of_year = 81.0f, .latitude = 45.0f, .moon_phase = 0.5f}, lights, sky_in, fog);
    assert(lights.sun.direction == applied.sun_direction && lights.sun.color == applied.sun_color);
    assert(lights.fill.direction == applied.moon_direction && lights.fill.color == applied.moon_color && sum(lights.fill.color) > 0.0f);
    assert(lights.ambient == applied.ambient && lights.ground_ambient.has_value() && *lights.ground_ambient == applied.ground_ambient);
    assert(sky_in.moon && sky_in.moon_direction == applied.moon_direction && sky_in.moon_light == applied.moon_light);
    assert(fog.colour == applied.fog_color);
    kin::apply({.hours = 12.0f, .day_of_year = 81.0f, .latitude = 45.0f, .moon_phase = 0.5f}, lights, sky_in, fog);
    assert(!sky_in.moon && sum(lights.fill.color) == 0.0f);
    // The clock: `speed` game seconds a second, hours wrapping into days and the year.
    TimeOfDay3D clock{.hours = 23.5f, .day_of_year = 365.0f, .speed = 3600.0f}; // a game hour a second
    kin::advance(clock, 1.0f);
    assert(std::abs(clock.hours - 0.5f) < 1e-4f && clock.day_of_year == 1.0f);
    kin::advance(clock, -2.0f);
    assert(std::abs(clock.hours - 22.5f) < 1e-3f && clock.day_of_year == 365.0f);
    clock.speed = 0.0f;
    kin::advance(clock, 100.0f);
    assert(std::abs(clock.hours - 22.5f) < 1e-3f);
    // Pure: the same inputs give the same bits.
    const kin::SkyState3D a = at(15.3f, 200.0f, 37.0f, 0.3f), b = at(15.3f, 200.0f, 37.0f, 0.3f);
    assert(std::memcmp(&a, &b, sizeof(a)) == 0);
}

// The atmosphere on the CPU: blue overhead and white-orange at the horizon by day, brighter toward the sun, the sun's
// disc-side glow, the night's own light, continuity at the horizon (no seam), the ground colour deep below it.
void test_sky_model_cpu() {
    kin::Sky3D sky;
    const Vec3f sun = normalize(Vec3f{0.3f, 0.6f, -0.5f});
    const Vec3f up{0.0f, 1.0f, 0.0f};
    const Vec3f zenith = kin::sky_radiance(sky, sun, up);
    const Vec3f horizon_away = kin::sky_radiance(sky, sun, normalize(Vec3f{-0.3f, 0.0f, 0.5f}));
    assert(zenith.z > zenith.y && zenith.y > zenith.x);                       // blue overhead
    assert(zenith.z / zenith.x > horizon_away.z / horizon_away.x + 1.0f);     // and bluer than the horizon, which is whiter
    assert(horizon_away.x + horizon_away.y + horizon_away.z > zenith.x + zenith.y + zenith.z); // brighter at the horizon
    // Toward the sun (the aureole): brighter than the same elevation on the far side.
    const Vec3f near_sun = kin::sky_radiance(sky, sun, normalize(sun + Vec3f{0.05f, 0.0f, 0.0f}));
    const Vec3f far_side = kin::sky_radiance(sky, sun, normalize(Vec3f{-sun.x, sun.y, -sun.z}));
    assert(near_sun.x + near_sun.y + near_sun.z > 2.0f * (far_side.x + far_side.y + far_side.z));
    // Haze whitens: more turbidity, a smaller blue-to-red ratio overhead and a brighter sky near the sun.
    kin::Sky3D hazy = sky;
    hazy.turbidity = 8.0f;
    const Vec3f hazy_zenith = kin::sky_radiance(hazy, sun, up);
    assert(hazy_zenith.z / hazy_zenith.x < zenith.z / zenith.x);
    // `intensity` scales the atmosphere (and not the night's own light).
    kin::Sky3D doubled = sky;
    doubled.intensity = 2.0f;
    const Vec3f twice = kin::sky_radiance(doubled, sun, up);
    assert(std::abs((twice.y - sky.night_color.y) / (zenith.y - sky.night_color.y) - 2.0f) < 1e-3f);
    // Continuous at the horizon, whatever the sun does, and smooth: the two sides within 0.5% and the slopes alike.
    for (const Vec3f s : {sun, normalize(Vec3f{0.5f, 0.02f, -0.8f}), normalize(Vec3f{0.5f, -0.1f, -0.8f})}) {
        const Vec3f over = normalize(Vec3f{0.6f, 1e-4f, -0.8f}), under = normalize(Vec3f{0.6f, -1e-4f, -0.8f});
        const Vec3f a = kin::sky_color(sky, s, over), b = kin::sky_color(sky, s, under);
        for (const float d : {a.x - b.x, a.y - b.y, a.z - b.z}) {
            assert(std::abs(d) < 0.005f * std::max({a.x, a.y, a.z, 1e-3f}));
        }
        // Over the 9 degrees below it the colour goes smoothly (no step larger than a tenth of the whole change) to the ground's.
        Vec3f previous = a;
        float biggest = 0.0f;
        const Vec3f ground = kin::sky_ground_light(sky, s);
        const float total = std::abs(a.y - ground.y);
        for (int i = 1; i <= 90; ++i) {
            const float elevation = -static_cast<float>(i) * 0.1f * 3.14159265f / 180.0f;
            const Vec3f d{0.6f * std::cos(elevation), std::sin(elevation), -0.8f * std::cos(elevation)};
            const Vec3f c = kin::sky_color(sky, s, d);
            biggest = std::max(biggest, std::abs(c.y - previous.y));
            previous = c;
        }
        assert(biggest < 0.1f * std::max(total, 1e-4f) + 1e-5f);
    }
    // Deep under the horizon: the ground's colour, lit by the sun and the zenith's light.
    const Vec3f ground = kin::sky_color(sky, sun, normalize(Vec3f{0.2f, -0.9f, 0.1f}));
    const Vec3f lit = kin::sky_ground_light(sky, sun);
    assert(std::abs(ground.x - lit.x) < 1e-6f && std::abs(ground.y - lit.y) < 1e-6f && std::abs(ground.z - lit.z) < 1e-6f);
    assert(lit.x > lit.z && lit.x > 0.0f); // the default ground colour is a warm grey
    kin::Sky3D black_ground = sky;
    black_ground.ground_color = {};
    assert(kin::sky_ground_light(black_ground, sun).x == 0.0f);
    // The sun's light, per colour, through the atmosphere: dimmer and redder with the elevation, cut under the horizon.
    const Vec3f high = kin::sky_sun_light(sky, normalize(Vec3f{0.0f, 1.0f, 0.1f}));
    const Vec3f lower = kin::sky_sun_light(sky, normalize(Vec3f{1.0f, 0.1f, 0.0f}));
    const Vec3f below = kin::sky_sun_light(sky, normalize(Vec3f{1.0f, -0.2f, 0.0f}));
    assert(high.x > lower.x && lower.x / lower.z > high.x / high.z && below.x == 0.0f);
    assert(high.x < sky.sun_intensity && high.x > 0.7f * sky.sun_intensity);
    // At night only the stars' own faint light and the moon's glow: the moon brightens its side of the sky.
    kin::Sky3D night = sky;
    night.moon = true;
    night.moon_direction = normalize(Vec3f{0.5f, 0.5f, 0.0f});
    night.moon_light = 0.1f;
    const Vec3f midnight_sun = normalize(Vec3f{0.0f, -0.8f, 0.6f});
    const Vec3f toward_moon = kin::sky_radiance(night, midnight_sun, normalize(night.moon_direction + Vec3f{0.0f, 0.1f, 0.0f}));
    const Vec3f away = kin::sky_radiance(night, midnight_sun, normalize(Vec3f{-0.5f, 0.5f, 0.0f}));
    assert(toward_moon.z > 3.0f * away.z && away.z > 0.0f);
    kin::Sky3D dark = night;
    dark.moon = false;
    assert(kin::sky_radiance(dark, midnight_sun, up).z < toward_moon.z);
    // Pure.
    const Vec3f once = kin::sky_color(sky, sun, normalize(Vec3f{0.3f, 0.2f, 0.9f})), twice_again = kin::sky_color(sky, sun, normalize(Vec3f{0.3f, 0.2f, 0.9f}));
    assert(once == twice_again);
    // The cosine-weighted irradiance over pi: of a flat sky of colour c it is c (the quadrature's own test).
    kin::Sky3D flat;
    flat.intensity = 0.0f;
    flat.ground_color = {};
    flat.night_color = {0.25f, 0.5f, 1.0f};
    const Vec3f flat_light = kin::sky_irradiance(flat, sun, up);
    assert(std::abs(flat_light.x - 0.25f) < 1e-3f && std::abs(flat_light.y - 0.5f) < 1e-3f && std::abs(flat_light.z - 1.0f) < 1e-3f);
    assert(kin::sky_irradiance(flat, sun, normalize(Vec3f{1.0f, 1.0f, 0.3f})).y > 0.0f);
}

// The fog's optical depth: the closed form equals a numerical integral of the density along the ray (Simpson, 20000 steps),
// for rays up, down and level, from above and under the base, with a start distance, to a distance and to infinity; the
// opacity is 1 - e^-tau capped by max_opacity; the sun's in-scattering adds a Henyey-Greenstein lobe.
void test_fog_math_cpu() {
    const auto integrate = [](const kin::Fog3D& fog, Vec3f eye, Vec3f dir, float distance) {
        // The density at distance t from the eye along dir, integrated from `start` to `distance`.
        const double a = fog.start, b = distance;
        if (b <= a) {
            return 0.0;
        }
        const int n = 20000;
        const double h = (b - a) / n;
        const auto density = [&](double t) {
            const double y = eye.y + dir.y * t;
            return static_cast<double>(fog.density) +
                   static_cast<double>(fog.height_density) * std::exp(-static_cast<double>(fog.height_falloff) * (y - fog.base_height));
        };
        double sum = density(a) + density(b);
        for (int i = 1; i < n; ++i) {
            sum += density(a + i * h) * (i % 2 ? 4.0 : 2.0);
        }
        return sum * h / 3.0;
    };
    kin::Fog3D fog{.enabled = true, .density = 0.002f, .height_density = 0.03f, .height_falloff = 0.08f, .base_height = 10.0f, .start = 5.0f};
    for (const Vec3f eye : {Vec3f{0.0f, 2.0f, 0.0f}, Vec3f{0.0f, 10.0f, 0.0f}, Vec3f{0.0f, 80.0f, 0.0f}}) {
        for (const Vec3f raw : {Vec3f{0.0f, 1.0f, 3.0f}, Vec3f{0.0f, 0.0f, 1.0f}, Vec3f{1.0f, -0.4f, 2.0f}, Vec3f{0.0f, -1.0f, 0.1f}, Vec3f{0.0f, 0.02f, 1.0f}}) {
            const Vec3f dir = normalize(raw);
            for (const float distance : {3.0f, 20.0f, 150.0f, 900.0f}) {
                const double want = integrate(fog, eye, dir, distance);
                const double got = kin::fog_optical_depth(fog, eye, dir, distance);
                assert(std::abs(got - want) <= 2e-4 * std::max(want, 1e-3) + 1e-6);
            }
        }
    }
    // To infinity: the height term alone, going up it is finite and equals the closed integral d e^{-k (y0 - base)} / (k dy).
    {
        kin::Fog3D layer{.enabled = true, .density = 0.5f, .height_density = 0.02f, .height_falloff = 0.1f, .base_height = 0.0f};
        const Vec3f dir = normalize(Vec3f{0.0f, 0.5f, 1.0f});
        const float want = 0.02f * std::exp(-0.1f * 4.0f) / (0.1f * dir.y);
        assert(std::abs(kin::fog_optical_depth(layer, {0.0f, 4.0f, 0.0f}, dir, std::numeric_limits<float>::infinity()) - want) < 1e-4f * want);
        // Horizontal and downward: nothing ends it (a ray that stays in the layer, or goes into it): a large depth.
        assert(kin::fog_optical_depth(layer, {0.0f, 4.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, std::numeric_limits<float>::infinity()) > 1e6f);
    }
    // Opacity: 1 - e^-tau, capped; zero density is zero fog.
    {
        kin::Fog3D plain{.enabled = true, .density = 0.01f};
        const Vec3f eye{}, dir{0.0f, 0.0f, 1.0f};
        for (const float distance : {10.0f, 100.0f, 400.0f}) {
            assert(std::abs(kin::fog_opacity(plain, eye, dir, distance) - (1.0f - std::exp(-0.01f * distance))) < 1e-6f);
        }
        plain.max_opacity = 0.6f;
        assert(kin::fog_opacity(plain, eye, dir, 1000.0f) == 0.6f);
        plain.max_opacity = 1.0f;
        plain.density = 0.0f;
        assert(kin::fog_opacity(plain, eye, dir, 1000.0f) == 0.0f);
    }
    // Colour: the fixed one, or the sky's; the sun adds a lobe that peaks toward it and is the same all round at g = 0.
    {
        kin::Sky3D sky;
        const Vec3f sun = normalize(Vec3f{0.0f, 0.2f, -1.0f});
        kin::Fog3D coloured{.enabled = true, .density = 0.01f, .colour = {0.2f, 0.3f, 0.4f}};
        assert(kin::fog_color(coloured, sky, sun, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}) == Vec3f(0.2f, 0.3f, 0.4f));
        coloured.colour_from_sky = true;
        assert(kin::fog_color(coloured, sky, sun, {}, normalize(Vec3f{0.0f, 0.3f, 1.0f})) == kin::sky_radiance(sky, sun, normalize(Vec3f{0.0f, 0.3f, 1.0f})));
        coloured.sun_scatter = 0.5f;
        coloured.sun_anisotropy = 0.7f;
        const Vec3f light{2.0f, 1.0f, 0.5f};
        const Vec3f toward = kin::fog_color(coloured, sky, sun, light, sun), away = kin::fog_color(coloured, sky, sun, light, -sun);
        const Vec3f base = kin::sky_radiance(sky, sun, sun), base_away = kin::sky_radiance(sky, sun, -sun);
        assert((toward.x - base.x) > 10.0f * (away.x - base_away.x) && (toward.x - base.x) / (toward.z - base.z) > 3.9f); // the sun's own, warm colour
        coloured.sun_anisotropy = 0.0f;
        const Vec3f even = kin::fog_color(coloured, sky, sun, light, -sun);
        assert(std::abs((even.x - base_away.x) - 0.5f * 2.0f) < 1e-4f);
        // Applied: the surface's colour mixed toward the fog's by the opacity.
        kin::Fog3D mix{.enabled = true, .density = 0.01f, .colour = {1.0f, 0.0f, 0.0f}};
        const Vec3f result = kin::fog_apply(mix, sky, sun, {}, {}, {0.0f, 0.0f, 1.0f}, 100.0f, {0.0f, 0.0f, 1.0f});
        const float opacity = 1.0f - std::exp(-1.0f);
        assert(std::abs(result.x - opacity) < 1e-5f && std::abs(result.z - (1.0f - opacity)) < 1e-5f);
    }
}

// --- The sky, the fog and the sky's light on the GPU ---

Vec3f linear_of(Color c) {
    return {kin::srgb_to_linear(static_cast<float>(c.r) / 255.0f), kin::srgb_to_linear(static_cast<float>(c.g) / 255.0f),
            kin::srgb_to_linear(static_cast<float>(c.b) / 255.0f)};
}

// What a gamma 8-bit target holds for linear light `c` (an 8-bit view has no exposure or tonemap).
Color stored_of(Vec3f c) {
    const auto one = [](float v) {
        return static_cast<kin::u8>(std::lround(255.0f * kin::linear_to_srgb(std::clamp(v, 0.0f, 1.0f))));
    };
    return Color::rgb(one(c.x), one(c.y), one(c.z));
}

int largest_difference(Color a, Color b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)), std::abs(int(a.b) - int(b.b))});
}

// The largest and the mean difference over two pictures, leaving out a square (a sun disc's neighbourhood) if asked.
struct Spread {
    int largest = 0;
    double mean = 0.0;
};
// Only the rows above `end_row` (the sky's, in a scene whose ground begins lower) if given.
Spread compare_pictures(const Pixels& a, const Pixels& b, int skip_x = -1, int skip_y = -1, int skip_radius = 0, int end_row = 1 << 20) {
    Spread s;
    double sum = 0.0;
    int count = 0;
    for (int y = 0; y < std::min(a.size.y, end_row); ++y) {
        for (int x = 0; x < a.size.x; ++x) {
            if (skip_radius > 0 && std::abs(x - skip_x) <= skip_radius && std::abs(y - skip_y) <= skip_radius) {
                continue;
            }
            const int d = largest_difference(a.at(x, y), b.at(x, y));
            s.largest = std::max(s.largest, d);
            sum += d;
            ++count;
        }
    }
    s.mean = count > 0 ? sum / count : 0.0;
    return s;
}

bool same_picture(const Pixels& a, const Pixels& b) { return a.size == b.size && a.rgba == b.rgba; }

// A daytime sky with the sun's disc left out (a hundredth of a degree), nothing else drawn: what the CPU model says
// each pixel shows. The sun is 17 degrees up, so a level camera sees it.
kin::Scene3D clear_sky_scene(Vec3f eye, Vec3f target, float fov, Vec3f sun = {0.45f, 0.30f, -0.84f}) {
    kin::Scene3D scene;
    scene.camera = kin::Camera3D::look_at(eye, target, fov);
    scene.lights.sun = {.direction = normalize(sun), .color = {1.0f, 1.0f, 1.0f}};
    scene.sky.enabled = true;
    scene.sky.turbidity = 3.0f;
    scene.sky.sun_intensity = 1.0f; // the sky stays under 1: an 8-bit view does not clip it
    scene.sky.sun_size = 0.01f;
    return scene;
}

// The direction the camera sees through `pixel` (its centre) of the 160 x 120 window.
Vec3f pixel_direction(const kin::Camera3D& camera, int x, int y) {
    return camera.ray({static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f}, screen).direction;
}

// The sky pass shows the CPU model's colour for each pixel's direction: a level view, one tilted up, one tilted down (the
// ground below the horizon, with the 9 degrees of blend) and a wide one; the sun's own neighbourhood is left out.
void test_gpu_sky_matches_cpu() {
    with_gpu("test_gpu_sky_matches_cpu", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const Vec3f eye{0.0f, 1.0f, 0.0f};
        int worst = 0, count = 0;
        const std::array<std::pair<Vec3f, float>, 4> views{std::pair<Vec3f, float>{{0.0f, 1.0f, -10.0f}, 60.0f}, {{4.0f, 14.0f, -10.0f}, 70.0f},
                                                           {{0.0f, -9.0f, -10.0f}, 50.0f}, {{-10.0f, 3.0f, 2.0f}, 90.0f}};
        int index = 0;
        for (const auto& [target, fov] : views) {
            kin::Scene3D scene = clear_sky_scene(eye, target, fov);
            // The last two with the intensity and the haze changed (a factor applied twice, or the haze ignored, shows).
            scene.sky.intensity = index >= 2 ? 0.55f : 1.0f;
            scene.sky.turbidity = index == 3 ? 7.0f : 3.0f;
            ++index;
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            assert(r3d.stats().sky_drawn && !r3d.stats().fog_applied);
            const Vec3f sun = normalize(scene.lights.sun.direction);
            for (int y = 2; y < 120; y += 7) {
                for (int x = 3; x < 160; x += 9) {
                    const Vec3f d = pixel_direction(scene.camera, x, y);
                    if (kin::dot(d, sun) > 0.985f) {
                        continue; // a few degrees of the sun: its glare is steep and the dither's
                    }
                    const Color want = stored_of(kin::sky_color(scene.sky, sun, d));
                    const int off = largest_difference(p.at(x, y), want);
                    worst = std::max(worst, off);
                    ++count;
                    if (off > 4) {
                        std::cerr << "sky at (" << x << ", " << y << "): got " << p.at(x, y) << ", the CPU model says " << want << '\n';
                    }
                    assert(off <= 4);
                }
            }
            renderer.present();
        }
        std::cerr << "sky: " << count << " pixels against the CPU model, worst " << worst << " of 255\n";
    });
}

// No seam at the horizon: down a column of a level view the colour runs smoothly through the horizon line (no step
// larger than the steepest step above it, in a sky whose sun sits on the horizon, where the gradient is steepest), and
// far below it, the ground's colour.
void test_gpu_sky_horizon_is_smooth() {
    with_gpu("test_gpu_sky_horizon_is_smooth", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        for (const Vec3f sun : {Vec3f{0.45f, 0.30f, -0.84f}, normalize(Vec3f{-0.5f, 0.04f, 0.8f}), normalize(Vec3f{0.0f, -0.12f, -1.0f})}) {
            // Looking level from the ground (a fov of 70: the horizon is the picture's middle row, ground below, sky above).
            kin::Scene3D scene = clear_sky_scene({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -10.0f}, 70.0f, sun);
            scene.sky.ground_color = {0.35f, 0.3f, 0.2f};
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            const int x = 120;
            // The steps down the column are all small: the horizon is no edge.
            int biggest = 0;
            for (int y = 1; y < 119; ++y) {
                biggest = std::max(biggest, largest_difference(p.at(x, y), p.at(x, y + 1)));
            }
            int around = 0;
            for (int y = 50; y < 70; ++y) {
                around = std::max(around, largest_difference(p.at(x, y), p.at(x, y + 1)));
            }
            std::cerr << "horizon: largest step in the column " << biggest << ", within 10 rows of the horizon " << around << "; column:";
            for (int y = 40; y < 100; y += 3) {
                std::cerr << ' ' << int(p.at(x, y).g);
            }
            std::cerr << '\n';
            // No seam: the largest step between neighbouring rows is a small part of the whole change from the sky to the ground
            // (the blend runs over 9 degrees), and the rows at the horizon itself step no more than their neighbours.
            const int top = p.at(x, 20).g, bottom = p.at(x, 118).g;
            assert(biggest <= static_cast<int>(0.3f * static_cast<float>(std::abs(top - bottom))) + 3);
            // Far below it (more than the 9 degrees of blend) the sky is the ground's colour.
            const Vec3f down = pixel_direction(scene.camera, x, 118);
            assert(down.y < -0.4f);
            const Color ground = stored_of(kin::sky_ground_light(scene.sky, scene.lights.sun.direction));
            expect(p.at(x, 118), ground, "the ground colour far below the horizon", 4);
            // Black ground: black, whatever the sun.
            scene.sky.ground_color = {};
            expect(shoot_view(renderer, r3d, view, scene).at(x, 118), kin::colors::black, "a black ground", 1);
            renderer.present();
        }
    });
}

// The sun disc's centre is where the sun projects, within a pixel, at several camera orientations and fields of view
// (its light centroid, which is sub-pixel exact), with the sky itself left black so that only the disc shows.
void test_gpu_sky_sun_position() {
    with_gpu("test_gpu_sky_sun_position", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const Vec3f sun = normalize(Vec3f{0.45f, 0.30f, -0.84f});
        struct Config {
            Vec3f offset;
            float fov;
        };
        double worst = 0.0;
        int checked = 0;
        for (const Config c : {Config{{0.0f, 0.0f, 0.0f}, 60.0f}, {{0.15f, 0.05f, 0.0f}, 60.0f}, {{-0.2f, 0.1f, 0.1f}, 90.0f},
                               {{0.05f, -0.15f, 0.0f}, 30.0f}, {{0.0f, 0.25f, 0.0f}, 100.0f}, {{0.3f, 0.0f, 0.2f}, 45.0f}}) {
            const Vec3f eye{3.0f, 1.0f, -2.0f};
            kin::Scene3D scene = clear_sky_scene(eye, eye + normalize(sun + c.offset) * 10.0f, c.fov);
            scene.sky.intensity = 0.0f;
            scene.sky.night_color = {};
            scene.sky.ground_color = {};
            scene.sky.sun_size = 2.0f;
            scene.sky.sun_disc = 1.0f; // 1 x the sun's light: the disc stays under white
            const auto at = scene.camera.project(eye + sun * 1000.0f, screen);
            assert(at && at->pos.x > 8.0f && at->pos.x < 152.0f && at->pos.y > 8.0f && at->pos.y < 112.0f);
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            double sum = 0.0, sx = 0.0, sy = 0.0;
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    const double w = linear_of(p.at(x, y)).y;
                    sum += w;
                    sx += w * (x + 0.5);
                    sy += w * (y + 0.5);
                }
            }
            assert(sum > 1.0); // something drawn: a disc of at least a few pixels
            const double dx = sx / sum - at->pos.x, dy = sy / sum - at->pos.y;
            const double off = std::sqrt(dx * dx + dy * dy);
            worst = std::max(worst, off);
            ++checked;
            if (off > 0.6) {
                std::cerr << "sun disc centroid (" << sx / sum << ", " << sy / sum << ") against (" << at->pos.x << ", " << at->pos.y << ") fov " << c.fov << '\n';
            }
            assert(off < 0.6); // the brief's bound is a pixel; the centroid of a smooth disc is better
            renderer.present();
        }
        std::cerr << "sun disc: " << checked << " views, centroid off by at most " << worst << " px\n";
    });
}

// The sky draws only where nothing was drawn: a near box and a far one (3 km, past where a finite far plane would be)
// show their own colour all over, with 1 sample and with 4; and it draws behind blended meshes.
void test_gpu_sky_stays_behind_geometry() {
    with_gpu("test_gpu_sky_stays_behind_geometry", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        for (const kin::u32 samples : {1u, 4u}) {
            kin::View3D view = r3d.create_view({160, 120}, {.samples = samples});
            kin::Scene3D scene = clear_sky_scene({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, -10.0f}, 60.0f);
            const kin::Material3D red{.color = Color::rgb(200, 40, 40), .unlit = true};
            const kin::Material3D green{.color = Color::rgb(40, 200, 40), .unlit = true};
            scene.draw(box, Mat4f::translation({0.0f, 1.0f, -20.0f}) * Mat4f::scaling({6.0f, 6.0f, 1.0f}), red);
            scene.draw(box, Mat4f::translation({800.0f, 0.0f, -3000.0f}) * Mat4f::scaling({1200.0f, 1200.0f, 1.0f}), green);
            // A blue glass in front of the sky and the box, drawn after: the sky is under it.
            const kin::Material3D glass{.color = Color::rgba(0, 0, 255, 128), .unlit = true, .blend = kin::Blend3D::Alpha};
            scene.draw(box, Mat4f::translation({-5.0f, 1.0f, -10.0f}) * Mat4f::scaling({3.0f, 8.0f, 0.2f}), glass);
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            const auto near_spot = scene.camera.project({0.0f, 1.0f, -19.0f}, screen);
            const auto far_spot = scene.camera.project({800.0f, 0.0f, -2999.0f}, screen);
            const auto glass_spot = scene.camera.project({-5.0f, 1.0f, -10.0f}, screen);
            assert(near_spot && far_spot && glass_spot && far_spot->pos.x < 150.0f);
            for (int dy = -8; dy <= 8; dy += 4) {
                for (int dx = -8; dx <= 8; dx += 4) {
                    expect(p.at(static_cast<int>(near_spot->pos.x) + dx, static_cast<int>(near_spot->pos.y) + dy), red.color, "the near box, the sky not over it", 1);
                }
            }
            expect(p.at(far_spot->pos), green.color, "the far box", 1);
            // The glass over the sky: the sky's blue-ish colour and the glass's blue, mixed half and half.
            const Color through = p.at(glass_spot->pos);
            assert(through.b > 120 && through.r < 160);
            // Sky elsewhere, as the CPU model has it.
            expect(p.at(5, 5), stored_of(kin::sky_color(scene.sky, scene.lights.sun.direction, pixel_direction(scene.camera, 5, 5))), "the sky in the corner", 4);
            assert(r3d.stats().sky_drawn);
            renderer.present();
        }
    });
}

// Off is off: with the sky disabled (its settings set), or the fog disabled or its densities zero, the picture is the one a
// scene without any of it draws, byte for byte; and sky pixels cover the background where on.
void test_gpu_sky_and_fog_off_are_unchanged() {
    with_gpu("test_gpu_sky_and_fog_off_are_unchanged", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        for (const bool hdr : {false, true}) {
            kin::View3D view = r3d.create_view({160, 120}, {.samples = 4, .background = Color::rgb(20, 50, 90), .hdr = hdr});
            const auto make_plain = [&] {
                kin::Scene3D plain;
                plain.camera = kin::Camera3D::look_at({0.0f, 1.0f, 5.0f}, {0.0f, 0.5f, 0.0f}, 60.0f);
                plain.lights.sun = {.direction = {0.4f, 1.0f, 0.5f}, .color = {1.0f, 0.95f, 0.9f}};
                plain.draw(box, Mat4f::translation({0.0f, 0.5f, 0.0f}), {.color = Color::rgb(200, 120, 60)});
                plain.draw(box, Mat4f::translation({0.0f, -0.5f, 0.0f}) * Mat4f::scaling({20.0f, 0.2f, 20.0f}), {.color = Color::rgb(120, 130, 120)});
                plain.draw(box, Mat4f::translation({1.5f, 0.5f, 1.0f}), {.color = Color::rgb(60, 120, 220), .blend = kin::Blend3D::Alpha});
                return plain;
            };
            const kin::Scene3D plain = make_plain();
            const Pixels base = shoot_view(renderer, r3d, view, plain);
            renderer.present();
            // The sky and the fog configured, switched off.
            kin::Scene3D off = make_plain();
            off.sky = {.enabled = false, .turbidity = 6.0f, .sun_intensity = 9.0f, .stars = true, .moon = true, .moon_light = 1.0f};
            off.fog = {.enabled = false, .density = 0.5f, .height_density = 0.5f, .colour_from_sky = true, .sun_scatter = 2.0f};
            assert(same_picture(shoot_view(renderer, r3d, view, off), base));
            assert(!r3d.stats().sky_drawn && !r3d.stats().fog_applied);
            renderer.present();
            // Fog enabled with nothing in it, or a capacity of nothing.
            off.fog = {.enabled = true, .density = 0.0f, .height_density = 0.0f};
            assert(same_picture(shoot_view(renderer, r3d, view, off), base));
            off.fog = {.enabled = true, .density = 0.5f, .max_opacity = 0.0f};
            assert(same_picture(shoot_view(renderer, r3d, view, off), base));
            assert(!r3d.stats().fog_applied);
            renderer.present();
            // The sky's light on lights the scene from the sky's settings, drawn or not (a picture of its own); off, no panorama is made.
            off.fog = {};
            off.lights.sky_lights_scene = true;
            off.sky = {.enabled = false, .turbidity = 3.0f};
            const Pixels lit = shoot_view(renderer, r3d, view, off);
            assert(r3d.stats().sky_env_generations >= 1 && !r3d.stats().sky_drawn && !same_picture(lit, base));
            renderer.present();
            off.lights.sky_lights_scene = false;
            assert(same_picture(shoot_view(renderer, r3d, view, off), base));
            assert(!r3d.stats().sky_env_regenerated);
            renderer.present();
            // On: the background gives way to the sky where nothing is drawn (and nowhere else).
            kin::Scene3D on = make_plain();
            on.sky.enabled = true;
            on.sky.turbidity = 3.0f;
            const Pixels with_sky = shoot_view(renderer, r3d, view, on);
            assert(r3d.stats().sky_drawn);
            expect(with_sky.at(80, 60), base.at(80, 60), "the box under the sky (an HDR view's output is dithered by a step)", hdr ? 1 : 0); // the box
            assert(with_sky.at(4, 4) != base.at(4, 4) && with_sky.at(4, 4).b > with_sky.at(4, 4).r);
            renderer.present();
        }
    });
}


// The fog: a wall of a known colour at known distances and heights is the colour the CPU formula says, through the
// view's pixels (the ray's length, not its depth; heights above and below the base; a start distance; the cap; the sky's
// own colour and the sun's lobe). Unlit green wall, an 8-bit view: the tolerance is 4 of 255.
void test_gpu_fog_matches_formula() {
    with_gpu("test_gpu_fog_matches_formula", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        struct Case {
            const char* name;
            Vec3f eye;
            Vec3f target;
            float fov;
            float wall;  // the wall's front face, z
            kin::Fog3D fog;
        };
        const kin::Fog3D none{};
        const std::array<Case, 8> cases{{
            {"10 m", {0, 1, 0}, {0, 1, -1}, 60.0f, -10.0f, {.enabled = true, .density = 0.02f}},
            {"40 m", {0, 1, 0}, {0, 1, -1}, 60.0f, -40.0f, {.enabled = true, .density = 0.02f, .colour = {0.8f, 0.5f, 0.3f}}},
            {"160 m", {0, 1, 0}, {0, 1, -1}, 60.0f, -160.0f, {.enabled = true, .density = 0.006f}},
            {"wide view: the ray's length", {0, 1, 0}, {0, 1, -1}, 110.0f, -30.0f, {.enabled = true, .density = 0.03f}},
            {"start and cap", {0, 1, 0}, {0, 1, -1}, 60.0f, -120.0f, {.enabled = true, .density = 0.03f, .start = 20.0f, .max_opacity = 0.7f}},
            {"height fog from near its base", {0, 1.7f, 0}, {0, 1.7f, -1}, 60.0f, -70.0f,
             {.enabled = true, .height_density = 0.04f, .height_falloff = 0.12f, .base_height = 0.0f}},
            {"height fog from above its base", {0, 60, 0}, {0, 30, -80}, 70.0f, -80.0f,
             {.enabled = true, .density = 0.001f, .height_density = 0.05f, .height_falloff = 0.07f, .base_height = 20.0f, .start = 5.0f}},
            {"the sky's colour and the sun's lobe", {0, 2, 0}, {0, 2, -1}, 90.0f, -90.0f,
             {.enabled = true, .density = 0.012f, .colour_from_sky = true, .sun_scatter = 0.4f, .sun_anisotropy = 0.6f}},
        }};
        const kin::Material3D wall{.color = Color::rgb(60, 180, 90), .unlit = true};
        const Vec3f wall_linear = linear_of(wall.color);
        int worst = 0;
        for (const Case& c : cases) {
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at(c.eye, c.target, c.fov);
            scene.lights.sun = {.direction = normalize(Vec3f{0.2f, 0.25f, -1.0f}), .color = {1.2f, 1.0f, 0.8f}};
            scene.sky.turbidity = 3.0f; // not drawn: the fog takes its parameters
            scene.fog = c.fog;
            scene.draw(box, Mat4f::translation({0.0f, c.eye.y, c.wall - 0.5f}) * Mat4f::scaling({4000.0f, 4000.0f, 1.0f}), wall);
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            assert(r3d.stats().fog_applied && !r3d.stats().sky_drawn);
            for (int y = 6; y < 120; y += 21) {
                for (int x = 6; x < 160; x += 19) {
                    const kin::Ray3 ray = scene.camera.ray({static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f}, screen);
                    const float t = (c.wall - ray.origin.z) / ray.direction.z;
                    const Vec3f hit = ray.origin + ray.direction * t;
                    const Vec3f to = hit - scene.camera.position;
                    const float distance = kin::length(to);
                    const Vec3f want = kin::fog_apply(scene.fog, scene.sky, scene.lights.sun.direction, scene.lights.sun.color,
                                                      scene.camera.position, to / distance, distance, wall_linear);
                    const Color expected = stored_of(want);
                    const int off = largest_difference(p.at(x, y), expected);
                    worst = std::max(worst, off);
                    if (off > 4) {
                        std::cerr << c.name << " at (" << x << ", " << y << "): got " << p.at(x, y) << ", the formula says " << expected << " (distance "
                                  << distance << ")\n";
                    }
                    assert(off <= 4);
                }
            }
            renderer.present();
        }
        std::cerr << "fog: " << cases.size() << " cases against the formula, worst " << worst << " of 255\n";
        static_cast<void>(none);
    });
}

// Blended and additive meshes are fogged too, as particles will be: a glass of alpha a shows a*fogged colour over the
// window's black; an additive glow dims by the opacity (what it lies over has the fog already). A custom material shader
// runs its own code and is not fogged (documented): its picture is the same with the fog on.
void test_gpu_fog_blended_and_custom() {
    with_gpu("test_gpu_fog_blended_and_custom", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, -1.0f}, 60.0f);
        scene.lights.sun.color = {};
        scene.fog = {.enabled = true, .density = 0.02f, .colour = {0.7f, 0.8f, 1.0f}};
        const float opacity = 1.0f - std::exp(-0.02f * 50.0f);
        // An alpha glass 50 m away (its face at z = -50), over nothing.
        const kin::Material3D glass{.color = Color::rgba(255, 60, 20, 128), .unlit = true, .blend = kin::Blend3D::Alpha};
        scene.draw(box, Mat4f::translation({0.0f, 1.0f, -50.5f}) * Mat4f::scaling({60.0f, 60.0f, 1.0f}), glass);
        Pixels p = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const Vec3f glass_linear = linear_of(glass.color);
        const float alpha = 128.0f / 255.0f;
        const Vec3f fog_colour{0.7f, 0.8f, 1.0f};
        // A point on the face: the centre ray's, 50 m.
        const Vec3f fogged = glass_linear + (fog_colour - glass_linear) * opacity;
        // The shader encodes the colour and then applies the alpha (premultiplied), and the target blends encoded values.
        const Color encoded = stored_of(fogged);
        const Color half = Color::rgb(static_cast<kin::u8>(std::lround(encoded.r * alpha)), static_cast<kin::u8>(std::lround(encoded.g * alpha)),
                                      static_cast<kin::u8>(std::lround(encoded.b * alpha)));
        expect(p.at(80, 60), half, "a glass seen through the fog", 4);
        // Additive: the colour dims by (1 - opacity), no fog colour added.
        scene.clear();
        const kin::Material3D glow{.color = Color::rgb(220, 140, 60), .unlit = true, .blend = kin::Blend3D::Additive};
        scene.draw(box, Mat4f::translation({0.0f, 1.0f, -50.5f}) * Mat4f::scaling({60.0f, 60.0f, 1.0f}), glow);
        p = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        expect(p.at(80, 60), stored_of(linear_of(glow.color) * (1.0f - opacity)), "an additive glow through the fog", 4);
        // A cut-out (Blend3D::Mask): where its alpha is under the cutoff nothing is drawn, fogged or not; where over, it is
        // opaque and fogged as any opaque surface.
        scene.clear();
        scene.draw(box, Mat4f::translation({0.0f, 1.0f, -50.5f}) * Mat4f::scaling({60.0f, 60.0f, 1.0f}),
                   {.color = Color::rgba(250, 40, 40, 60), .unlit = true, .blend = kin::Blend3D::Mask, .alpha_cutoff = 0.5f});
        scene.draw(box, Mat4f::translation({0.0f, 1.0f, -60.5f}) * Mat4f::scaling({4.0f, 4.0f, 1.0f}), {.color = Color::rgb(40, 250, 40), .unlit = true});
        p = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const float far_opacity = 1.0f - std::exp(-0.02f * 60.0f);
        const Vec3f green = linear_of(Color::rgb(40, 250, 40));
        // The wall is cut away wherever (alpha 60/255 < 0.5): the green box behind it shows, fogged at 60 m.
        expect(p.at(80, 60), stored_of(green + (fog_colour - green) * far_opacity), "behind a cut-away wall, fogged", 4);
        scene.clear();
        scene.draw(box, Mat4f::translation({0.0f, 1.0f, -50.5f}) * Mat4f::scaling({60.0f, 60.0f, 1.0f}),
                   {.color = Color::rgba(250, 40, 40, 220), .unlit = true, .blend = kin::Blend3D::Mask, .alpha_cutoff = 0.5f});
        p = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const Vec3f red = linear_of(Color::rgb(250, 40, 40));
        expect(p.at(80, 60), stored_of(red + (fog_colour - red) * opacity), "a cut-out that stays, opaque and fogged", 4);
        // Custom material shader: not fogged.
        if (kin::shader_compiler_available()) {
            const std::filesystem::path dir{KIN_TEST_MATERIAL_SHADERS};
            std::string error;
            const kin::MaterialShader3D flat = r3d.compile_material_shader(dir / "custom_flat.frag.glsl", &error);
            assert(flat.valid());
            kin::Material3D custom{.shader = flat};
            custom.shader_params[4] = 0.1f;
            custom.shader_params[5] = 0.7f;
            custom.shader_params[6] = 0.2f;
            scene.clear();
            scene.draw(box, Mat4f::translation({0.0f, 1.0f, -50.5f}) * Mat4f::scaling({60.0f, 60.0f, 1.0f}), custom);
            scene.draw(box, Mat4f::translation({0.0f, 1.0f, -60.0f}) * Mat4f::scaling({4.0f, 4.0f, 1.0f}), {.color = Color::rgb(200, 60, 60), .unlit = true});
            const Pixels fogged_picture = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            scene.fog.enabled = false;
            const Pixels clear_picture = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            // The custom wall in front is the same either way (the red box behind it is hidden by it).
            assert(same_picture(fogged_picture, clear_picture));
        }
    });
}

// Far geometry melts into the sky at the horizon: with the fog's colour taken from the sky, a ground that runs to the
// horizon, seen level, is the sky's own colour for the last rows of it, and there is no seam where the two meet. Fog off:
// the ground keeps its colour to the horizon.
void test_gpu_fog_melts_into_the_sky() {
    with_gpu("test_gpu_fog_melts_into_the_sky", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene = clear_sky_scene({0.0f, 2.0f, 0.0f}, {0.0f, 2.0f, -10.0f}, 60.0f);
        scene.lights.sun.direction = normalize(Vec3f{-0.6f, 0.35f, -0.7f});
        scene.draw(box, Mat4f::translation({0.0f, -0.5f, -9000.0f}) * Mat4f::scaling({40000.0f, 1.0f, 20000.0f}),
                   {.color = Color::rgb(90, 140, 60), .unlit = true});
        // Without fog: a hard line, the green ground against the sky.
        const Pixels hard = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const int horizon = 60; // the level view's middle row: the sky above it, the ground below
        assert(largest_difference(hard.at(80, horizon + 2), hard.at(80, horizon - 3)) > 40);
        // Fog from the sky, thick enough that 100 m is gone: the ground's last rows are the sky's colour.
        scene.fog = {.enabled = true, .density = 0.05f, .colour_from_sky = true};
        const Pixels fogged = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const Color above = fogged.at(80, horizon - 3), below = fogged.at(80, horizon + 2);
        std::cerr << "fog at the horizon: sky " << above << ", ground " << below << '\n';
        assert(largest_difference(above, below) <= 8);
        // And it matches the model: the fog colour is the sky's in that direction (the ground's pixel 2 rows under the horizon).
        const Vec3f d = pixel_direction(scene.camera, 80, horizon + 2);
        const Vec3f sky_there = kin::sky_radiance(scene.sky, scene.lights.sun.direction, d);
        const float distance = 2.0f / std::max(-d.y, 1e-4f);
        const Vec3f want = kin::fog_apply(scene.fog, scene.sky, scene.lights.sun.direction, scene.lights.sun.color, scene.camera.position, d, distance,
                                          linear_of(Color::rgb(90, 140, 60)));
        expect(below, stored_of(want), "the ground's last rows", 4);
        assert(largest_difference(below, stored_of(sky_there)) <= 10);
        // The sky itself, with the fog on, is unchanged where it is far from the ground (distance fog is not applied to it).
        const Pixels sky_only = [&] {
            kin::Scene3D s = clear_sky_scene({0.0f, 2.0f, 0.0f}, {0.0f, 2.0f, -10.0f}, 60.0f);
            s.lights.sun.direction = scene.lights.sun.direction;
            return shoot_view(renderer, r3d, view, s);
        }();
        renderer.present();
        assert(largest_difference(fogged.at(80, 10), sky_only.at(80, 10)) <= 1);
    });
}

// The sky and the fog under temporal anti-aliasing: a still camera's frames settle and stay (no flicker: the consecutive
// frames differ by a level or two, the sun's disc not at all in where it sits), and a turning camera's sky is
// reprojected by the camera's turn, not smeared: the disc stays a disc where the sun projects, and the whole picture is
// within a few levels of the same frame drawn without temporal anti-aliasing. Both paths: the main pass writes the
// velocity, and (with ambient occlusion) the pre-pass does, where the sky's turn is a pass of its own.
void test_gpu_sky_under_taa() {
    with_gpu("test_gpu_sky_under_taa", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        const Vec3f sun = normalize(Vec3f{0.45f, 0.30f, -0.84f});
        const auto make_scene = [&](float yaw_degrees, bool taa, bool ao) {
            const float yaw = yaw_degrees * 3.14159265f / 180.0f;
            const Vec3f eye{0.0f, 1.0f, 0.0f};
            kin::Scene3D scene;
            // The camera turns about +y from facing the sun.
            const Vec3f facing = normalize(Vec3f{sun.x * std::cos(yaw) - sun.z * std::sin(yaw), 0.18f, sun.x * std::sin(yaw) + sun.z * std::cos(yaw)});
            scene.camera = kin::Camera3D::look_at(eye, eye + facing * 10.0f, 60.0f);
            scene.lights.sun = {.direction = sun, .color = {1.0f, 0.95f, 0.9f}};
            scene.sky.enabled = true;
            scene.sky.sun_intensity = 1.0f;
            scene.sky.sun_size = 2.0f;
            scene.sky.sun_disc = 2.0f; // the disc is at about 1.6 (HDR): above white where it counts
            scene.fog = {.enabled = true, .density = 0.004f, .colour_from_sky = true, .sun_scatter = 0.1f};
            scene.draw(box, Mat4f::translation({-2.0f, 0.0f, -14.0f}), {.color = Color::rgb(200, 120, 60)});
            scene.draw(box, Mat4f::translation({0.0f, -0.6f, 0.0f}) * Mat4f::scaling({60.0f, 0.2f, 60.0f}), {.color = Color::rgb(120, 130, 120)});
            scene.taa.enabled = taa;
            scene.ambient_occlusion.enabled = ao;
            return scene;
        };
        for (const bool ao : {false, true}) {
            if (ao && lacks(r3d.capabilities().ambient_occlusion, "ambient occlusion")) {
                continue;
            }
            kin::View3D view = r3d.create_view({160, 120}, {.samples = 4});
            kin::View3D reference = r3d.create_view({160, 120}, {.samples = 1});
            // Still: let it settle, then compare consecutive frames.
            const kin::Scene3D still = make_scene(0.0f, true, ao);
            Pixels last;
            double biggest_mean = 0.0;
            int biggest = 0, in_disc = 0;
            const auto sun_screen = still.camera.project(still.camera.position + sun * 1000.0f, screen);
            assert(sun_screen);
            for (int frame = 0; frame < 24; ++frame) {
                const Pixels p = shoot_view(renderer, r3d, view, still);
                renderer.present();
                if (frame >= 16) {
                    // The sky's rows (the ground and the box are below row 72: their edges, high contrast, are temporal
                    // anti-aliasing's own business, a few tens of levels a frame between the pixels of an edge).
                    const Spread s = compare_pictures(p, last, static_cast<int>(sun_screen->pos.x), static_cast<int>(sun_screen->pos.y), 10, 72);
                    const Spread whole = compare_pictures(p, last, -1, -1, 0, 72);
                    biggest = std::max(biggest, s.largest);
                    biggest_mean = std::max(biggest_mean, s.mean);
                    in_disc = std::max(in_disc, whole.largest);
                }
                last = p;
            }
            std::cerr << "sky under TAA" << (ao ? " (pre-pass velocity)" : " (main-pass velocity)") << ": still frames differ by at most " << biggest
                      << " levels (mean " << biggest_mean << ") away from the sun disc, " << in_disc << " with it\n";
            assert(biggest <= 2 && biggest_mean < 0.25);
            // Turning: 0.7 degrees a frame; the last frame against the same view drawn without temporal anti-aliasing.
            view.reset_history();
            Pixels turned;
            kin::Scene3D last_scene;
            for (int frame = 0; frame < 30; ++frame) {
                last_scene = make_scene(static_cast<float>(frame) * 0.7f, true, ao);
                turned = shoot_view(renderer, r3d, view, last_scene);
                renderer.present();
            }
            kin::Scene3D truth = make_scene(29.0f * 0.7f, false, ao);
            const Pixels exact = shoot_view(renderer, r3d, reference, truth);
            renderer.present();
            const auto sun_at = last_scene.camera.project(last_scene.camera.position + sun * 1000.0f, screen);
            assert(sun_at && sun_at->pos.x > 10.0f && sun_at->pos.x < 150.0f);
            // The whole picture, the sun's neighbourhood left out (its edge is anti-aliased by the history, which is the point).
            const Spread s = compare_pictures(turned, exact, static_cast<int>(sun_at->pos.x), static_cast<int>(sun_at->pos.y), 9, 72);
            // The disc: where it is (centroid) and that it is still bright (not smeared away).
            const auto centroid = [&](const Pixels& p) {
                double sum = 0.0, sx = 0.0, sy = 0.0;
                for (int y = std::max(0, static_cast<int>(sun_at->pos.y) - 12); y < std::min(120, static_cast<int>(sun_at->pos.y) + 12); ++y) {
                    for (int x = std::max(0, static_cast<int>(sun_at->pos.x) - 12); x < std::min(160, static_cast<int>(sun_at->pos.x) + 12); ++x) {
                        const double w = std::max(0.0, linear_of(p.at(x, y)).y - 0.5);
                        sum += w;
                        sx += w * (x + 0.5);
                        sy += w * (y + 0.5);
                    }
                }
                return std::array<double, 3>{sum > 0.0 ? sx / sum : 0.0, sum > 0.0 ? sy / sum : 0.0, sum};
            };
            const auto c_turned = centroid(turned), c_exact = centroid(exact);
            std::cerr << "sky under TAA, turning" << (ao ? " (pre-pass)" : " (main pass)") << ": picture differs by at most " << s.largest << ", mean " << s.mean
                      << "; sun centroid (" << c_turned[0] << ", " << c_turned[1] << ") against projected (" << sun_at->pos.x << ", " << sun_at->pos.y
                      << "), disc light " << c_turned[2] << " against " << c_exact[2] << '\n';
            assert(s.mean < 1.5 && s.largest <= 24);
            assert(c_turned[2] > 0.5 * c_exact[2]);
            assert(std::abs(c_turned[0] - sun_at->pos.x) < 1.0 && std::abs(c_turned[1] - sun_at->pos.y) < 1.0);
        }
    });
}

// The sky lights the scene (Lights3D::sky_lights_scene): a white diffuse sphere with no sun and no ambient takes its
// light from the sky's panorama, so its colour follows the time of day (blue-white at noon, warm at the end of the day,
// near black at night) and is the CPU estimate (the cosine-weighted sky over each pixel's normal) within a tenth.
void test_gpu_sky_lights_the_scene() {
    with_gpu("test_gpu_sky_lights_the_scene", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().ambient_occlusion, "compute (the environment's filter)")) {
            return;
        }
        const kin::Mesh sphere = r3d.create_mesh(kin::meshes::sphere(1.0f, 64, 32));
        const kin::Mesh slab = make_box(r3d, {30.0f, 0.2f, 30.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        struct Result {
            Vec3f mean;
            Vec3f estimate;
            Vec3f up;
            Vec3f up_estimate;
        };
        const auto shoot = [&](float hours, float rotation = 0.0f) {
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, 30.0f);
            scene.sky.enabled = false; // the light only: the picture shows the sphere
            scene.sky.turbidity = 3.0f;
            scene.sky.stars = false;
            scene.sky.ground_color = {0.3f, 0.28f, 0.25f};
            kin::apply({.hours = hours, .day_of_year = 172.0f, .latitude = 45.0f, .moon_phase = 0.5f}, scene.lights, scene.sky, scene.fog);
            scene.lights.sun.color = {}; // only the sky's light
            scene.lights.fill.color = {};
            scene.lights.ambient = {1.0f, 0.0f, 1.0f}; // not used with an environment
            scene.lights.sky_lights_scene = true;
            scene.lights.environment_rotation = rotation;
            scene.draw(sphere, Mat4f::identity(), {.color = Color::rgb(255, 255, 255), .metallic = 0.0f, .roughness = 1.0f});
            // The panorama follows the sky after `light_interval` renders: a few frames of the same time, as a game's are.
            Pixels p;
            for (int frame = 0; frame < 6; ++frame) {
                p = shoot_view(renderer, r3d, view, scene);
                renderer.present();
            }
            Result r{};
            double count = 0.0;
            for (int y = 0; y < 120; y += 2) {
                for (int x = 0; x < 160; x += 2) {
                    const kin::Ray3 ray = scene.camera.ray({static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f}, screen);
                    // The sphere of radius 1 at the origin.
                    const Vec3f oc = ray.origin;
                    const float b = kin::dot(oc, ray.direction), c = kin::dot(oc, oc) - 1.0f;
                    const float disc = b * b - c;
                    if (disc <= 0.0f) {
                        continue;
                    }
                    const Vec3f hit = ray.origin + ray.direction * (-b - std::sqrt(disc));
                    const Vec3f n = normalize(hit);
                    if (n.z < 0.35f) {
                        continue; // not the grazing rim: the specular's Fresnel is the model's own
                    }
                    r.mean += linear_of(p.at(x, y));
                    r.estimate += kin::sky_irradiance(scene.sky, scene.lights.sun.direction, n, 24, 12);
                    count += 1.0;
                }
            }
            assert(count > 100.0);
            r.mean = r.mean / static_cast<float>(count);
            r.estimate = r.estimate / static_cast<float>(count);
            return r;
        };
        const auto sum = [](Vec3f v) { return v.x + v.y + v.z; };
        const Result noon = shoot(12.0f);
        assert(r3d.stats().sky_env_generations >= 1);
        const Result dusk = shoot(19.4f);
        const Result night = shoot(0.5f);
        for (const auto& [name, r] : {std::pair<const char*, Result>{"noon", noon}, {"dusk", dusk}, {"night", night}}) {
            std::cerr << "sky light on a white sphere, " << name << ": drawn (" << r.mean.x << ", " << r.mean.y << ", " << r.mean.z << "), estimated ("
                      << r.estimate.x << ", " << r.estimate.y << ", " << r.estimate.z << ")\n";
        }
        // The colour follows the CPU estimate: within 12% of it per channel (the sphere also reflects the sky a little).
        for (const Result& r : {noon, dusk}) {
            for (const auto& [got, want] : {std::pair<float, float>{r.mean.x, r.estimate.x}, {r.mean.y, r.estimate.y}, {r.mean.z, r.estimate.z}}) {
                assert(std::abs(got - want) < 0.12f * want + 0.004f);
            }
        }
        assert(std::abs(sum(night.mean) - sum(night.estimate)) < 0.2f * sum(night.estimate) + 0.01f);
        // The panorama is in the world's space: the light's own rotation (for a picture's environment) does not turn it.
        const Result rotated = shoot(12.0f, 90.0f);
        assert(std::abs(rotated.mean.x - noon.mean.x) < 0.01f * noon.mean.x && std::abs(rotated.mean.z - noon.mean.z) < 0.01f * noon.mean.z);
        // Warmer at the end of the day than at noon, and the night dark.
        assert(dusk.mean.x / dusk.mean.z > noon.mean.x / noon.mean.z);
        assert(sum(night.mean) < 0.05f * sum(noon.mean));
        assert(noon.mean.z > noon.mean.x); // a blue sky's light
    });
}

// The panorama is made again when the sky has moved, and not otherwise: the first render makes it, the same sky
// does not, a change under the threshold or inside the interval does not, a change over it does (after the interval),
// the cost is recorded; and with the option off the scene's ambient light is used as before.
void test_gpu_sky_environment_regeneration() {
    with_gpu("test_gpu_sky_environment_regeneration", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().ambient_occlusion, "compute (the environment's filter)")) {
            return;
        }
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 1.0f, 4.0f}, {0.0f, 0.0f, 0.0f}, 50.0f);
        scene.lights.sun = {.direction = normalize(Vec3f{0.4f, 0.8f, 0.3f}), .color = {1.0f, 1.0f, 1.0f}};
        scene.lights.ambient = {0.6f, 0.0f, 0.0f}; // red: the fallback's colour, which the sky's light would not have
        scene.sky.enabled = false;
        scene.sky.light_threshold = 0.02f;
        scene.sky.light_interval = 3;
        scene.draw(box, Mat4f::identity(), {.color = Color::rgb(220, 220, 220)});
        const auto render = [&](const kin::Scene3D& s) {
            const Pixels p = shoot_view(renderer, r3d, view, s);
            renderer.present();
            return p;
        };
        const Pixels fallback = render(scene);
        assert(!r3d.stats().sky_env_regenerated && r3d.stats().sky_env_generations == 0);
        assert(fallback.at(80, 60).r > fallback.at(80, 60).b + 20); // the red hemisphere ambient
        scene.lights.sky_lights_scene = true;
        const Pixels first = render(scene);
        assert(r3d.stats().sky_env_regenerated && r3d.stats().sky_env_generations == 1);
        assert(r3d.stats().sky_env_cpu_ms > 0.0 && r3d.stats().environment_prefilters == 0); // the sky's own filter, not a user environment's
        assert(first.at(80, 60).b + 10 >= first.at(80, 60).r); // a blue sky: not the red ambient
        // The same sky: kept, and the picture is the same.
        for (int i = 0; i < 6; ++i) {
            const Pixels again = render(scene);
            assert(!r3d.stats().sky_env_regenerated && r3d.stats().sky_env_generations == 1);
            assert(same_picture(again, first));
        }
        // Moved under the threshold (0.005 of a unit of direction): kept.
        scene.lights.sun.direction = normalize(Vec3f{0.4f, 0.8f, 0.3f} + Vec3f{0.004f, 0.0f, 0.0f});
        for (int i = 0; i < 6; ++i) {
            render(scene);
            assert(!r3d.stats().sky_env_regenerated);
        }
        // Moved over it: made again, but not before the interval (3 renders) has passed since the last one.
        scene.lights.sun.direction = normalize(Vec3f{-0.6f, 0.5f, 0.3f});
        int renders_until = 0;
        do {
            render(scene);
            ++renders_until;
        } while (renders_until < 8 && !r3d.stats().sky_env_regenerated);
        assert(r3d.stats().sky_env_regenerated && r3d.stats().sky_env_generations == 2);
        std::cerr << "sky environment: made again " << renders_until << " render(s) after the sun moved, recording took " << r3d.stats().sky_env_cpu_ms << " ms\n";
        // Right after one, a new move waits out the interval (3 renders kept, the 4th makes it).
        scene.lights.sun.direction = normalize(Vec3f{0.5f, 0.2f, -0.6f});
        int waited = 0;
        do {
            render(scene);
            ++waited;
        } while (waited < 8 && !r3d.stats().sky_env_regenerated);
        assert(r3d.stats().sky_env_regenerated && waited == 4);
        // The haze changing counts too; the sun's own light too (each after the interval has passed).
        for (int i = 0; i < 6; ++i) {
            render(scene);
        }
        const kin::u32 before = r3d.stats().sky_env_generations;
        scene.sky.turbidity = 9.0f;
        for (int i = 0; i < 6; ++i) {
            render(scene);
        }
        assert(r3d.stats().sky_env_generations == before + 1);
        scene.sky.sun_intensity = 6.0f;
        for (int i = 0; i < 6; ++i) {
            render(scene);
        }
        assert(r3d.stats().sky_env_generations == before + 2);
        // Off again: the hemisphere ambient, as at the start.
        scene.lights.sky_lights_scene = false;
        scene.lights.sun.direction = normalize(Vec3f{0.4f, 0.8f, 0.3f});
        scene.sky.turbidity = 2.5f;
        scene.sky.sun_intensity = 3.0f;
        assert(same_picture(render(scene), fallback));
    });
}

// The image sky shows the environment's panorama as it lights: the six-colour sky's red at +z, green at +x, white
// above; the intensity scales it and the rotation turns it, as for the light.
void test_gpu_sky_image_mode() {
    with_gpu("test_gpu_sky_image_mode", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Vec2i size{256, 128};
        const kin::Environment3D environment = r3d.create_environment(size, six_colour_sky(size));
        if (!environment.valid()) {
            std::cerr << "no compute: environment not made, skipped\n";
            return;
        }
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const auto look = [&](Vec3f toward, float intensity = 1.0f, float rotation = 0.0f) {
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, toward, 40.0f);
            scene.lights.environment = environment;
            scene.lights.environment_intensity = intensity;
            scene.lights.environment_rotation = rotation;
            scene.sky.enabled = true;
            scene.sky.mode = kin::SkyMode3D::Image;
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            assert(r3d.stats().sky_drawn);
            return p.at(80, 60);
        };
        const Color red = look({0.0f, 0.0f, 1.0f}), green = look({1.0f, 0.0f, 0.0f}), up = look({0.0f, 1.0f, 0.3f});
        expect(red, stored_of({0.8f, 0.0f, 0.0f}), "the panorama's +z", 6);
        expect(green, stored_of({0.0f, 0.8f, 0.0f}), "the panorama's +x", 6);
        expect(up, stored_of({0.8f, 0.8f, 0.8f}), "the panorama's up", 6);
        expect(look({0.0f, 0.0f, 1.0f}, 0.25f), stored_of({0.2f, 0.0f, 0.0f}), "a quarter of the intensity", 6);
        // A quarter turn about +y takes the red off +z, onto +x or -x.
        const Color turned_z = look({0.0f, 0.0f, 1.0f}, 1.0f, 90.0f);
        const Color turned_x = look({1.0f, 0.0f, 0.0f}, 1.0f, 90.0f), turned_nx = look({-1.0f, 0.0f, 0.0f}, 1.0f, 90.0f);
        std::cerr << "image sky turned 90: +z " << turned_z << ", +x " << turned_x << ", -x " << turned_nx << '\n';
        assert(turned_z.r < 100 && (turned_x.r > 200 || turned_nx.r > 200));
        // Without an environment the image sky draws nothing (the background shows); the procedural one is not asked.
        kin::Scene3D bare;
        bare.sky.enabled = true;
        bare.sky.mode = kin::SkyMode3D::Image;
        kin::View3D plain = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(30, 60, 90)});
        expect(shoot_view(renderer, r3d, plain, bare).at(80, 60), Color::rgb(30, 60, 90), "no panorama: the background", 1);
        assert(!r3d.stats().sky_drawn);
        renderer.present();
    });
}

// Stars fade in as the sun sinks (none above -3 degrees, more by -6, all by -12) and the moon disc sits where its
// direction projects (from a time of day's own moon, opposite the sun at full): with the sky's light off, so that only
// they show.
void test_gpu_sky_stars_and_moon() {
    with_gpu("test_gpu_sky_stars_and_moon", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const auto stars_at = [&](float sun_elevation_degrees) {
            const float e = sun_elevation_degrees * 3.14159265f / 180.0f;
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, -0.4f}, 100.0f);
            scene.lights.sun = {.direction = {std::cos(e), std::sin(e), 0.0f}, .color = {1.0f, 1.0f, 1.0f}};
            scene.sky.enabled = true;
            scene.sky.intensity = 0.0f;
            scene.sky.night_color = {};
            scene.sky.ground_color = {};
            scene.sky.sun_size = 0.01f;
            scene.sky.stars = true;
            scene.sky.star_brightness = 8.0f;
            const Pixels p = shoot_view(renderer, r3d, view, scene);
            renderer.present();
            int count = 0;
            double light = 0.0;
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    const int v = p.at(x, y).g;
                    light += linear_of(p.at(x, y)).y;
                    count += v > 24 ? 1 : 0;
                }
            }
            return std::pair<int, double>{count, light};
        };
        const auto day = stars_at(30.0f), civil = stars_at(-1.0f), nautical = stars_at(-6.0f), night = stars_at(-15.0f);
        std::cerr << "stars (pixels over 24, their light): day " << day.first << "/" << day.second << ", sun at -1: " << civil.first << "/" << civil.second
                  << ", at -6: " << nautical.first << "/" << nautical.second << ", at -15: " << night.first << "/" << night.second << '\n';
        assert(day.first == 0 && day.second == 0.0 && civil.first == 0 && civil.second == 0.0);
        assert(night.first >= 15 && night.second > 1.5 * nautical.second && nautical.second > 0.0);
        // The moon: a disc where the time of day's moon is, nowhere else; the sun's below the horizon.
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 50.0f);
        scene.sky.enabled = true;
        scene.sky.intensity = 0.0f;
        scene.sky.night_color = {};
        scene.sky.ground_color = {};
        scene.sky.sun_size = 0.01f;
        scene.sky.moon_size = 3.0f;
        kin::SkyState3D state{};
        // Midnight at the equinox, 45 N: the full moon is high in the south (+z).
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f},
                                              kin::evaluate({.hours = 0.0f, .day_of_year = 81.0f, .latitude = 45.0f, .moon_phase = 0.5f}).moon_direction * 10.0f +
                                                  Vec3f{0.3f, -0.4f, 0.0f},
                                              50.0f);
        state = kin::apply({.hours = 0.0f, .day_of_year = 81.0f, .latitude = 45.0f, .moon_phase = 0.5f}, scene.lights, scene.sky, scene.fog);
        scene.sky.moon_light = 0.04f; // under white
        assert(state.moon_elevation > 40.0f && scene.sky.moon);
        const auto at = scene.camera.project(state.moon_direction * 1000.0f, screen);
        assert(at && at->pos.x > 10.0f && at->pos.x < 150.0f && at->pos.y > 10.0f && at->pos.y < 110.0f);
        const Pixels p = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        double sum = 0.0, sx = 0.0, sy = 0.0;
        for (int y = 0; y < 120; ++y) {
            for (int x = 0; x < 160; ++x) {
                const double w = linear_of(p.at(x, y)).y;
                sum += w;
                sx += w * (x + 0.5);
                sy += w * (y + 0.5);
            }
        }
        assert(sum > 3.0);
        const double dx = sx / sum - at->pos.x, dy = sy / sum - at->pos.y;
        std::cerr << "moon disc centroid off by (" << dx << ", " << dy << ") px\n";
        assert(std::sqrt(dx * dx + dy * dy) < 0.8);
        // Moon off: a black sky.
        scene.sky.moon = false;
        const Pixels none = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        expect(none.at(static_cast<int>(at->pos.x), static_cast<int>(at->pos.y)), kin::colors::black, "no moon", 2);
    });
}

// The overlay is never fogged (a gun in the hand) and the sky stays behind it; the fog of the world still applies to the
// world; warm_up makes the sky's and the fog's pipelines (and the dither's) so that the first render makes none.
void test_gpu_sky_overlay_and_warm_up() {
    with_gpu("test_gpu_sky_overlay_and_warm_up", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 4, .hdr = true, .bloom = 0.2f});
        kin::Scene3D scene = clear_sky_scene({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, -10.0f}, 60.0f);
        scene.fog = {.enabled = true, .density = 0.3f, .colour = {1.0f, 1.0f, 1.0f}}; // 99.99% at 30 m; 36% at the gun's 1.5 m
        scene.taa.enabled = false;
        scene.draw(box, Mat4f::translation({0.0f, 1.0f, -30.0f}) * Mat4f::scaling({6.0f, 6.0f, 1.0f}), {.color = Color::rgb(200, 40, 40), .unlit = true});
        kin::Scene3D& gun = scene.overlay();
        gun.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 60.0f);
        gun.draw(box, Mat4f::translation({0.4f, -0.3f, -1.5f}) * Mat4f::scaling({0.5f, 0.5f, 0.5f}), {.color = Color::rgb(40, 200, 40), .unlit = true});
        assert(r3d.warm_up(view, scene));
        render_ok(r3d, view, scene);
        assert(r3d.stats().pipelines_made == 0);
        const Pixels p = shoot_view(renderer, r3d, view, scene);
        renderer.present();
        const auto gun_at = gun.camera.project({0.4f, -0.3f, -1.2f}, screen);
        assert(gun_at);
        // The gun's green is its own (tonemapped, unfogged: green dominates, and it is not washed to white by the fog's white).
        const Color g = p.at(gun_at->pos);
        assert(g.g > 150 && g.r < 60 && g.b < 60); // the gun's own green (tonemapped); 36% of white fog in it would lift red and blue over 100
        // The world's box, 30 m away through the fog 0.3 (all white), is the fog's colour.
        const Color far_box = p.at(80, 60);
        assert(far_box.r > 200 && far_box.g > 200 && far_box.b > 200);
        assert(r3d.stats().fog_applied && r3d.stats().sky_drawn);
    });
}



// A sub-pixel sun disc (0.45 degrees, in a black sky: only it shows) is the point to judge the sky's temporal handling by:
// still, its centroid sits where the sun projects (within 0.15 px) and holds there; turning at 1.2 degrees a frame
// (2.6 px) it follows the camera within 0.3 px on average and 0.6 at worst, and keeps its light (over half of the
// same frame drawn without temporal anti-aliasing): a sky with no velocity of its own would be rejected by the history's
// clamp frame after frame and lose nearly all of it. Both ways the velocity is made: by the main pass (no pre-pass) and
// by the pre-pass (with ambient occlusion), where the sky's turn is a pass of its own.
void test_gpu_sky_sun_under_taa() {
    with_gpu("test_gpu_sky_sun_under_taa", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        if (lacks(r3d.capabilities().temporal_aa, "temporal anti-aliasing")) {
            return;
        }
        const kin::Mesh box = make_box(r3d, {1.0f, 1.0f, 1.0f});
        const Vec3f sun = normalize(Vec3f{0.45f, 0.30f, -0.84f});
        // The disc's light, over a threshold above the dither's floor, and its centroid.
        struct Disc {
            double x = 0.0, y = 0.0, light = 0.0;
        };
        const auto measure = [](const Pixels& p) {
            Disc d;
            double sx = 0.0, sy = 0.0;
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    const double w = std::max(0.0, linear_of(p.at(x, y)).y - 0.004);
                    d.light += w;
                    sx += w * (x + 0.5);
                    sy += w * (y + 0.5);
                }
            }
            if (d.light > 0.0) {
                d.x = sx / d.light;
                d.y = sy / d.light;
            }
            return d;
        };
        for (const bool ao : {false, true}) {
            if (ao && lacks(r3d.capabilities().ambient_occlusion, "ambient occlusion")) {
                continue;
            }
            for (const float step : {0.0f, 1.2f}) {
                kin::View3D view = r3d.create_view({160, 120}, {.samples = 4});
                kin::View3D reference = r3d.create_view({160, 120}, {.samples = 1});
                const auto make_scene = [&](float yaw_degrees, bool taa) {
                    const float yaw = yaw_degrees * 3.14159265f / 180.0f;
                    const Vec3f eye{0.0f, 1.0f, 0.0f};
                    kin::Scene3D scene;
                    // Turning about +y, the sun staying at the same height on the screen.
                    const Vec3f facing = normalize(Vec3f{sun.x * std::cos(yaw) - sun.z * std::sin(yaw), sun.y, sun.x * std::sin(yaw) + sun.z * std::cos(yaw)});
                    scene.camera = kin::Camera3D::look_at(eye, eye + facing * 10.0f, 60.0f);
                    scene.lights.sun = {.direction = sun, .color = {1.0f, 0.95f, 0.9f}};
                    scene.sky.enabled = true;
                    scene.sky.intensity = 0.0f;
                    scene.sky.night_color = {};
                    scene.sky.ground_color = {};
                    scene.sky.sun_intensity = 1.0f;
                    scene.sky.sun_size = 0.45f;
                    scene.sky.sun_disc = 1.0f; // under white
                    // Something to draw (a box far below the view), so that the pre-pass has its storage.
                    scene.draw(box, Mat4f::translation({0.0f, -40.0f, 0.0f}), {.color = Color::rgb(120, 130, 120)});
                    scene.taa.enabled = taa;
                    scene.ambient_occlusion.enabled = ao;
                    return scene;
                };
                double worst = 0.0, sum_x = 0.0, sum_y = 0.0, lowest_light = 1e9, highest_light = 0.0;
                double wobble_x = 0.0, wobble_y = 0.0;
                std::vector<std::array<double, 2>> offsets;
                for (int frame = 0; frame < 26; ++frame) {
                    const kin::Scene3D scene = make_scene(static_cast<float>(frame) * step, true);
                    const Pixels p = shoot_view(renderer, r3d, view, scene);
                    renderer.present();
                    if (frame < 14) {
                        continue;
                    }
                    const kin::Scene3D exact_scene = make_scene(static_cast<float>(frame) * step, false);
                    const Pixels exact = shoot_view(renderer, r3d, reference, exact_scene);
                    renderer.present();
                    const auto at = scene.camera.project(scene.camera.position + sun * 1000.0f, screen);
                    assert(at && at->pos.x > 6.0f && at->pos.x < 154.0f);
                    const Disc d = measure(p), e = measure(exact);
                    offsets.push_back({d.x - at->pos.x, d.y - at->pos.y});
                    worst = std::max(worst, std::hypot(d.x - at->pos.x, d.y - at->pos.y));
                    assert(e.light > 0.1);
                    lowest_light = std::min(lowest_light, d.light / e.light);
                    highest_light = std::max(highest_light, d.light / e.light);
                }
                for (const auto& o : offsets) {
                    sum_x += o[0];
                    sum_y += o[1];
                }
                const double mean_x = sum_x / static_cast<double>(offsets.size()), mean_y = sum_y / static_cast<double>(offsets.size());
                for (const auto& o : offsets) {
                    wobble_x += (o[0] - mean_x) * (o[0] - mean_x);
                    wobble_y += (o[1] - mean_y) * (o[1] - mean_y);
                }
                const double wobble = std::sqrt((wobble_x + wobble_y) / static_cast<double>(offsets.size()));
                std::cerr << "sun disc under TAA, " << (ao ? "pre-pass" : "main pass") << ", " << step << " deg a frame: centroid off by (" << mean_x << ", "
                          << mean_y << ") on average, worst " << worst << ", wobble " << wobble << " px; light " << lowest_light << " to " << highest_light
                          << " of the same frame without TAA\n";
                assert(std::abs(mean_x) < (step > 0.0f ? 0.3 : 0.15) && std::abs(mean_y) < (step > 0.0f ? 0.3 : 0.15));
                assert(worst < (step > 0.0f ? 0.6 : 0.2) && wobble < 0.3);
                assert(lowest_light > 0.55 && highest_light < 1.3); // a sub-pixel dot keeps 0.7 to 1.1 of its flux; with no velocity of its own the sky kept 0.06
            }
        }
    });
}

const char* test_filter = nullptr;
bool selected(const char* name) { return !test_filter || std::strstr(name, test_filter) != nullptr; }
#define RUN(test) \
    do { \
        if (selected(#test)) test; \
    } while (false)

int main(int argc, char** argv) {
    test_filter = argc > 1 ? argv[1] : nullptr;
    RUN(test_meshes_face_out());
    RUN(test_mesh_lod_builder());
    RUN(test_mesh_validation_and_append());
    RUN(test_scene_merges_draws());
    RUN(test_cameras());
    RUN(test_cascade_splits());
    RUN(test_cascade_fit());
    RUN(test_ecs_entities());
    RUN(test_sdl_renderer_has_no_3d());
    RUN(test_gpu_reports_3d());
    RUN(test_gpu_depth_and_colour());
    RUN(test_gpu_lighting());
    RUN(test_gpu_instances());
    RUN(test_gpu_instances_on_workers());
    RUN(test_gpu_frustum_culling());
    RUN(test_gpu_overlay());
    RUN(test_gpu_point_lights());
    RUN(test_gpu_shadows());
    RUN(test_gpu_blending());
    RUN(test_gpu_hdr_bloom());
    RUN(test_gpu_texture());
    RUN(test_gpu_model());
    RUN(test_gpu_culling());
    RUN(test_gpu_interleaves_with_2d());
    RUN(test_gpu_linear_hdr());
    RUN(test_gpu_meshes_outlive_renderer());
    RUN(test_gpu_pick());
    RUN(test_gpu_wireframe());
    RUN(test_gpu_fill_and_ambient());
    RUN(test_gpu_receives_no_shadow());
    RUN(test_gpu_large_mesh());
    RUN(test_gpu_shadow_faces());
    RUN(test_gpu_shadows_into_view());
    RUN(test_gpu_cascades());
    RUN(test_gpu_cascade_casters());
    RUN(test_gpu_ambient_occlusion());
    RUN(test_gpu_contact_shadows());
    RUN(test_gpu_occlusion_speedups());
    RUN(test_taa_jitter());
    RUN(test_gpu_taa());
    RUN(test_gpu_taa_over_detail());
    RUN(test_gpu_taa_crowd());
    RUN(test_gpu_taa_overlay());
    RUN(test_gpu_taa_main_velocity());
    RUN(test_gpu_shadow_cache());
    RUN(test_gpu_taa_with_occlusion());
    RUN(test_gpu_taa_pick());
    RUN(test_gpu_taa_hdr());
    RUN(test_gpu_taa_ids_after_reset());
    RUN(test_gpu_taa_match_on_workers());
    RUN(test_gpu_sun_off_no_shadow_pass());
    RUN(test_gpu_lod_selection());
    RUN(test_gpu_lod_off_is_unchanged());
    RUN(test_gpu_lod_same_level_everywhere());
    RUN(test_gpu_lod_picture_bound());
    RUN(test_gpu_lod_pick());
    RUN(test_gpu_lod_on_workers());
    RUN(test_gpu_lod_taa());
    for (const bool gpu : {false, true}) {
        RUN(test_gpu_clusters_off_is_unchanged(gpu));
    }
    for (const bool gpu : {false, true}) {
        RUN(test_gpu_clusters_cut(gpu));
    }
    for (const bool gpu : {false, true}) {
        RUN(test_gpu_clusters_same_cut_everywhere(gpu));
    }
    for (const bool gpu : {false, true}) {
        RUN(test_gpu_clusters_pick(gpu));
    }
    for (const bool gpu : {false, true}) {
        RUN(test_gpu_clusters_on_workers(gpu));
    }
    for (const bool gpu : {false, true}) {
        RUN(test_gpu_clusters_taa(gpu));
    }
    RUN(test_gpu_cut_equals_cpu_selection());
    RUN(test_gpu_cut_picture_equals_cpu());
    RUN(test_gpu_cut_is_deterministic());
    RUN(test_gpu_cut_culls_by_pass());
    RUN(test_gpu_cut_overflow_clamps());
    RUN(test_gpu_cut_falls_back());
    RUN(test_gpu_cut_many_copies());
    RUN(test_gpu_cut_chunks());
    RUN(test_gpu_occlusion_picture_and_stats());
    RUN(test_gpu_occlusion_moving());
    RUN(test_gpu_occlusion_gaps_and_edges());
    RUN(test_gpu_occlusion_history());
    RUN(test_gpu_occlusion_is_deterministic());
    RUN(test_gpu_occlusion_keeps_shadows());
    RUN(test_gpu_occlusion_is_ignored());
    RUN(test_gpu_occlusion_with_overflow());
    RUN(test_gpu_feature_release());
    RUN(test_gpu_occlusion_release());
    RUN(test_gpu_warm_up());
    RUN(test_gpu_metal_and_roughness());
    RUN(test_gpu_normal_map());
    RUN(test_gpu_alpha_mask());
    RUN(test_gpu_inverse_square());
    RUN(test_gpu_model_materials());
    RUN(test_gpu_wrap_modes());
    RUN(test_gpu_environment());
    RUN(test_gpu_environment_levels_written());
    RUN(test_gpu_material_shader());
    RUN(test_gpu_skinned_matches_cpu());
    RUN(test_gpu_skinned_shadow_out_of_view());
    RUN(test_gpu_skinned_cutout_shadow());
    RUN(test_gpu_skinned_poses_and_mix());
    RUN(test_gpu_skinned_culling());
    RUN(test_gpu_skinned_off_path());
    RUN(test_gpu_skinned_on_workers());
    RUN(test_gpu_skinned_overlay());
    RUN(test_gpu_skinned_with_features());
    RUN(test_gpu_skinned_many_copies());
    RUN(test_gpu_skinned_velocity());
    RUN(test_gpu_skinned_velocity_new_and_reordered());
    RUN(test_gpu_skinned_prepass_features());
    RUN(test_gpu_skinned_occluder());
    RUN(test_gpu_skinned_pick());
    RUN(test_gpu_skinned_shadow_cache());
    RUN(test_gpu_skinned_shared_range());
    RUN(test_gpu_ecs_character());
    RUN(test_gpu_ik_hand_at_target());
    RUN(test_gpu_ik_not_culled());
    RUN(test_gpu_ik_taa_matches_clip());
    RUN(test_gpu_ik_shadow_cache());
    RUN(test_gpu_ik_on_workers());
    RUN(test_gpu_ik_limits_fabrik_locking_toes());
    RUN(test_gpu_skinned_everything());
    RUN(test_gpu_physics_debug_lines());
    RUN(test_gpu_ragdoll_pose_reaches_render());
    RUN(test_gpu_ragdoll_taa_matches_clip());
    RUN(test_gpu_rope_tube());
    RUN(test_time_of_day_formulae());
    RUN(test_sky_model_cpu());
    RUN(test_fog_math_cpu());
    RUN(test_gpu_sky_matches_cpu());
    RUN(test_gpu_sky_horizon_is_smooth());
    RUN(test_gpu_sky_sun_position());
    RUN(test_gpu_sky_stays_behind_geometry());
    RUN(test_gpu_sky_and_fog_off_are_unchanged());
    RUN(test_gpu_fog_matches_formula());
    RUN(test_gpu_fog_blended_and_custom());
    RUN(test_gpu_fog_melts_into_the_sky());
    RUN(test_gpu_sky_under_taa());
    RUN(test_gpu_sky_sun_under_taa());
    RUN(test_gpu_sky_lights_the_scene());
    RUN(test_gpu_sky_environment_regeneration());
    RUN(test_gpu_sky_image_mode());
    RUN(test_gpu_sky_stars_and_moon());
    RUN(test_gpu_sky_overlay_and_warm_up());
    return 0;
}
