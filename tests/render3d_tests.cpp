#include <kin/core/jobs.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/shadow_cascades.hpp>
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
#include <memory>
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
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}));
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
                renderer.clear(kin::colors::black);
                render_ok(r3d, view, scene);
                assert(r3d.stats().draws == 2 && r3d.stats().instances == 2 && r3d.stats().triangles == 24);
                renderer.draw_texture(view.texture(), screen);
                const Pixels p = read(renderer);
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
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}));
        kin::View3D view = r3d.create_view({160, 120});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({3.0f, 1.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, 50.0f);
        scene.lights.sun = {.direction = {1.0f, 0.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}};
        scene.lights.ambient = {0.0f, 0.0f, 0.0f};
        scene.draw(box, Mat4f::identity(), {.color = Color::rgb(180, 180, 180)});
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        renderer.draw_texture(view.texture(), screen);
        Pixels p = read(renderer);
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
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        renderer.draw_texture(view.texture(), screen);
        p = read(renderer);
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
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({0.8f, 0.8f, 0.8f}));
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
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        assert(r3d.stats().draws == 2 && r3d.stats().instances == 5);
        renderer.draw_texture(view.texture(), screen);
        const Pixels p = read(renderer);
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
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({0.8f, 0.8f, 0.8f}));
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
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        assert(r3d.stats().instances == 12300 && r3d.stats().draws == 302);
        renderer.draw_texture(view.texture(), screen);
        const Pixels p = read(renderer);
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
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({0.8f, 0.8f, 0.8f}));
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            assert(r3d.stats().instances == 4 && r3d.stats().culled == 11997 && r3d.stats().draws == 1);
            renderer.draw_texture(view.texture(), screen);
            const Pixels p = read(renderer);
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
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}));
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
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        assert(r3d.stats().draws == 2 && r3d.stats().instances == 2);
        renderer.draw_texture(view.texture(), screen);
        const Pixels p = read(renderer);
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
        const kin::Mesh wall = r3d.create_mesh(kin::meshes::box({20.0f, 20.0f, 0.2f}));
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 8.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
        scene.lights.ambient = {0.0f, 0.0f, 0.0f};
        scene.draw(wall, Mat4f::identity(), {.color = Color::rgb(200, 200, 200)});
        scene.add_light({.position = {-2.0f, 0.0f, 1.0f}, .color = {2.0f, 1.0f, 0.5f}, .radius = 3.0f});
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        assert(r3d.stats().lights == 1);
        renderer.draw_texture(view.texture(), screen);
        Pixels p = read(renderer);
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
        const kin::Mesh floor = r3d.create_mesh(kin::meshes::box({20.0f, 0.2f, 20.0f}));
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({1.5f, 1.5f, 1.5f}));
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            return read(renderer);
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
        const kin::Mesh quad = r3d.create_mesh(kin::meshes::box({2.0f, 2.0f, 0.01f}));
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
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        renderer.draw_texture(view.texture(), screen);
        const Pixels p = read(renderer);
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
        const kin::Mesh cube = r3d.create_mesh(kin::meshes::box({0.6f, 0.6f, 0.6f}));
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.draw(cube, Mat4f::identity(), {.color = Color::rgb(255, 140, 60), .emissive = 6.0f, .unlit = true});
        const auto shot = [&](const kin::View3DOptions& options) {
            kin::View3D view = r3d.create_view({160, 120}, options);
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            return read(renderer);
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
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        renderer.draw_texture(view.texture(), screen);
        const Pixels p = read(renderer);
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
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        renderer.draw_texture(view.texture(), screen);
        const Pixels p = read(renderer);
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            expect(read(renderer).at(80, 60), double_sided ? Color::rgb(90, 200, 90) : kin::colors::black,
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
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({10.0f, 10.0f, 1.0f}));
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
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}));
        kin::View3D view = r3d.create_view({160, 120});
        if (!renderer.set_color_space(kin::ColorSpace::Linear, true)) {
            std::cerr << "[SKIP] test_gpu_linear_hdr: no linear HDR pipeline\n";
            return;
        }
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f});
        scene.draw(box, Mat4f::identity(), {.color = Color::rgb(200, 40, 40), .unlit = true});
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        renderer.draw_texture(view.texture(), screen);
        expect(read(renderer).at(80, 60), Color::rgb(200, 40, 40), "a linear HDR scene", 4);
        renderer.present();
    });
}

// What is under a pixel: the id given with the draw and the point hit, in
// the world; nothing over the background, the overlay or a wireframe. 2D
// drawn after a pick (the frame submitted under it) still shows.
void test_gpu_pick() {
    with_gpu("test_gpu_pick", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}));
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
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({2.0f, 2.0f, 2.0f}));
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
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        renderer.draw_texture(view.texture(), screen);
        const Pixels p = read(renderer);
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
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}));
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
        scene.lights.ambient = {0.0f, 0.0f, 0.0f};
        const auto shot = [&](Vec3f eye) {
            scene.clear();
            scene.camera = kin::Camera3D::look_at(eye, {0.0f, 0.0f, 0.0f}, 50.0f);
            scene.draw(box, Mat4f::identity(), {.color = kin::colors::white});
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            const Color c = read(renderer).at(80, 60);
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
        const kin::Mesh floor = r3d.create_mesh(kin::meshes::box({20.0f, 0.2f, 20.0f}));
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({1.5f, 1.5f, 1.5f}));
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 12.0f, 0.01f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.lights.sun = {.direction = {1.0f, 1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}};
        scene.lights.ambient = {0.15f, 0.15f, 0.15f};
        scene.lights.shadow_area = kin::Bounds3{{-10.0f, -0.2f, -10.0f}, {10.0f, 4.0f, 10.0f}};
        scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(180, 180, 180), .receives_shadow = false});
        scene.draw(box, Mat4f::translation({0.0f, 2.5f, 0.0f}), {.color = Color::rgb(180, 60, 60)});
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        assert(r3d.stats().shadow_draws == 2);
        renderer.draw_texture(view.texture(), screen);
        const Pixels p = read(renderer);
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            expect(read(renderer).at(scene.camera.project({0.0f, -0.3f, 0.0f}, screen)->pos), Color::rgb(220, 30, 30), what);
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
        const kin::Mesh floor = r3d.create_mesh(kin::meshes::box({20.0f, 0.2f, 20.0f}));
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            const Pixels p = read(renderer);
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
        const kin::Mesh floor = r3d.create_mesh(kin::meshes::box({20.0f, 0.2f, 20.0f}));
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}));
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
        const kin::Mesh floor = r3d.create_mesh(kin::meshes::box({300.0f, 0.2f, 300.0f}));
        const kin::Mesh block = r3d.create_mesh(kin::meshes::box({4.0f, 4.0f, 4.0f}));
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            Pixels p = read(renderer);
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

// Ambient occlusion (G2b): the foot of a wall is darker than with it off and
// open floor is not; only the ambient light is dimmed (the sun is not); off,
// the pre-pass and the compute are not run; the overlay and blended meshes
// are not in it.
void test_gpu_ambient_occlusion() {
    with_gpu("test_gpu_ambient_occlusion", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = r3d.create_mesh(kin::meshes::box({20.0f, 0.2f, 20.0f}));
        const kin::Mesh wall = r3d.create_mesh(kin::meshes::box({20.0f, 4.0f, 0.2f}));
        const kin::Mesh pane = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 0.01f}));
        const kin::Mesh block = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}));
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
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
            scene.draw(wall, Mat4f::translation({0.0f, 2.0f, -2.1f}), {.color = Color::rgb(200, 200, 200)});
            if (pane_in_front) {
                scene.draw(pane, Mat4f::translation({0.0f, 0.5f, 0.5f}),
                           {.color = Color::rgba(255, 255, 255, 128), .blend = kin::Blend3D::Alpha});
            }
            if (gun) {
                kin::Scene3D& hands = scene.overlay();
                hands.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 70.0f);
                hands.draw(block, Mat4f::translation({0.5f, -0.3f, -2.0f}), {.color = Color::rgb(200, 120, 60)});
            }
            renderer.clear(kin::colors::black);
            render_ok(r3d, target ? *target : view, scene);
            renderer.draw_texture((target ? *target : view).texture(), screen);
            Pixels p = read(renderer);
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
        const kin::Mesh floor = r3d.create_mesh(kin::meshes::box({20.0f, 0.2f, 20.0f}));
        const kin::Mesh pole = r3d.create_mesh(kin::meshes::box({0.2f, 1.0f, 0.2f}));
        const kin::Mesh plate = r3d.create_mesh(kin::meshes::box({0.24f, 0.02f, 0.24f}));
        const kin::Mesh block = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}));
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
            scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
            scene.draw(pole, Mat4f::translation({0.0f, 0.5f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
            if (with_plate) {
                scene.draw(plate, Mat4f::translation(plate_at), {.color = Color::rgb(200, 200, 200)});
            }
            if (gun) {
                kin::Scene3D& hands = scene.overlay();
                hands.camera = kin::Camera3D::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 70.0f);
                hands.draw(block, Mat4f::translation({0.5f, -0.3f, -2.0f}), {.color = Color::rgb(200, 120, 60)});
            }
            renderer.clear(kin::colors::black);
            render_ok(r3d, target ? *target : view, scene);
            renderer.draw_texture((target ? *target : view).texture(), screen);
            Pixels p = read(renderer);
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
        // Without ambient occlusion: the pre-pass of both meshes, the march and two blurs.
        assert(r3d.stats().ao_draws == 2 && r3d.stats().ao_instances == 2 && r3d.stats().ao_dispatches == 0 &&
               r3d.stats().contact_dispatches == 3);
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            Pixels p = read(renderer);
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            const Pixels p = read(renderer);
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
        const kin::Mesh floor = r3d.create_mesh(kin::meshes::box({20.0f, 0.2f, 20.0f}));
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            Pixels p = read(renderer);
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
        const kin::Mesh wall = r3d.create_mesh(kin::meshes::box({40.0f, 40.0f, 0.2f}));
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1});
        const auto level = [&](float distance, float radius) {
            kin::Scene3D scene;
            scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 30.0f}, {0.0f, 0.0f, 0.0f}, 20.0f);
            scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
            scene.lights.ambient = {0.0f, 0.0f, 0.0f};
            scene.draw(wall, Mat4f::translation({0.0f, 0.0f, -0.1f}), {.color = Color::rgb(200, 200, 200)});
            scene.add_light({.position = {0.0f, 0.0f, distance}, .color = {3.0f, 3.0f, 3.0f}, .radius = radius});
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            const Pixels p = read(renderer);
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
        renderer.clear(kin::colors::black);
        render_ok(r3d, view, scene);
        renderer.draw_texture(view.texture(), screen);
        const Pixels p = read(renderer);
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            const Pixels p = read(renderer);
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            Pixels p = read(renderer);
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            Pixels p = read(renderer);
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
    const float c = static_cast<float>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
kin::u8 encode_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    const float e = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<kin::u8>(std::lround(e * 255.0f));
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
        const kin::Mesh slab = r3d.create_mesh(kin::meshes::box({6.0f, 4.0f, 0.1f}));
        const kin::Mesh block = r3d.create_mesh(kin::meshes::box({2.0f, 2.0f, 0.1f}));
        const kin::Mesh other = r3d.create_mesh(kin::meshes::box({2.0f, 2.0f, 0.2f}));
        const Color black = Color::rgb(0, 0, 0);
        const kin::Material3D white{.color = Color::rgb(255, 255, 255), .unlit = true};
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = black});
        kin::Scene3D scene;
        scene.camera = flat_camera();
        const auto shoot = [&](kin::View3D& target, const kin::Scene3D& s) {
            renderer.clear(kin::colors::black);
            render_ok(r3d, target, s);
            renderer.draw_texture(target.texture(), screen);
            Pixels p = read(renderer);
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
        const kin::Mesh back = r3d.create_mesh(kin::meshes::box({20.0f, 14.0f, 0.1f}));
        const kin::Mesh cell = r3d.create_mesh(kin::meshes::box({0.1f, 0.1f, 0.1f}));
        const kin::Mesh big_cell = r3d.create_mesh(kin::meshes::box({0.4f, 0.4f, 0.1f}));
        const kin::Mesh block = r3d.create_mesh(kin::meshes::box({2.0f, 2.0f, 0.1f}));
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, target, s);
            renderer.draw_texture(target.texture(), screen);
            Pixels p = read(renderer);
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
        const kin::Mesh block = r3d.create_mesh(kin::meshes::box({1.6f, 1.6f, 0.2f}));
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        kin::View3D alone_view = r3d.create_view({160, 120}, {.samples = 1, .background = Color::rgb(0, 0, 0)});
        kin::Scene3D scene;
        scene.camera = flat_camera(-5.03f, 0.0f);
        const kin::Material3D white{.color = Color::rgb(255, 255, 255), .unlit = true};
        const auto shoot = [&](kin::View3D& target) {
            renderer.clear(kin::colors::black);
            render_ok(r3d, target, scene);
            renderer.draw_texture(target.texture(), screen);
            Pixels p = read(renderer);
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
        const kin::Mesh gun = r3d.create_mesh(kin::meshes::box({3.0f, 2.0f, 0.5f}));
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, target, scene);
            renderer.draw_texture(target.texture(), screen);
            Pixels p = read(renderer);
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

// Ambient occlusion and contact shadows with temporal anti-aliasing: the
// pre-pass is drawn once and shared (jittered like the main pass), each
// effect runs, the foot of the wall is dark and the picture settles.
void test_gpu_taa_with_occlusion() {
    with_gpu("test_gpu_taa_with_occlusion", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh floor = r3d.create_mesh(kin::meshes::box({20.0f, 0.2f, 20.0f}));
        const kin::Mesh wall = r3d.create_mesh(kin::meshes::box({20.0f, 4.0f, 0.2f}));
        const kin::Mesh pole = r3d.create_mesh(kin::meshes::box({0.2f, 1.0f, 0.2f}));
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
                scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = Color::rgb(200, 200, 200)});
                scene.draw(wall, Mat4f::translation({0.0f, 2.0f, -2.1f}), {.color = Color::rgb(200, 200, 200)});
                scene.draw(pole, Mat4f::translation({1.0f, 0.5f, 1.0f}), {.color = Color::rgb(200, 200, 200)});
                renderer.clear(kin::colors::black);
                render_ok(r3d, view, scene);
                renderer.draw_texture(view.texture(), screen);
                last = std::move(p);
                p = read(renderer);
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
        // And TAA alone shares the pre-pass without occlusion.
        render(true, false, false, 2);
        assert(stats.ao_draws == 3 && stats.ao_dispatches == 0 && stats.contact_dispatches == 0 && stats.taa_dispatches == 1);
        render(false, false, false, 1);
        assert(stats.ao_draws == 0 && stats.taa_dispatches == 0);
    });
}

// Pick is exact: the same pixel gives the same answer every frame with the
// jitter on, and the one without it.
void test_gpu_taa_pick() {
    with_gpu("test_gpu_taa_pick", [](kin::Renderer2D& renderer) {
        kin::Renderer3D r3d{renderer};
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({2.0f, 2.0f, 2.0f}));
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
        const kin::Mesh cube = r3d.create_mesh(kin::meshes::box({0.6f, 0.6f, 0.6f}));
        kin::Scene3D scene;
        scene.camera = kin::Camera3D::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f);
        scene.draw(cube, Mat4f::identity(), {.color = Color::rgb(255, 140, 60), .emissive = 6.0f, .unlit = true});
        kin::View3D view = r3d.create_view({160, 120}, {.samples = 4, .bloom = 1.0f});
        const auto side = scene.camera.project({0.55f, 0.0f, 0.3f}, screen);
        assert(side);
        Pixels p;
        for (int f = 0; f < 12; ++f) {
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            p = read(renderer);
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
        const kin::Mesh block = r3d.create_mesh(kin::meshes::box({3.0f, 3.0f, 0.2f}));
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, views[with_ids ? 0 : 1], scene);
            renderer.draw_texture(views[with_ids ? 0 : 1].texture(), screen);
            Pixels p = read(renderer);
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
        const kin::Mesh block = r3d.create_mesh(kin::meshes::box({0.12f, 0.12f, 0.12f}));
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
                renderer.clear(kin::colors::black);
                render_ok(r3d, view, scene);
                renderer.draw_texture(view.texture(), screen);
                shots[workers ? 1 : 0].push_back(read(renderer));
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
        const kin::Mesh floor = r3d.create_mesh(kin::meshes::box({20.0f, 0.2f, 20.0f}));
        const kin::Mesh box = r3d.create_mesh(kin::meshes::box({1.5f, 1.5f, 1.5f}));
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
            renderer.clear(kin::colors::black);
            render_ok(r3d, view, scene);
            renderer.draw_texture(view.texture(), screen);
            Pixels p = read(renderer);
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

int main() {
    test_meshes_face_out();
    test_mesh_validation_and_append();
    test_scene_merges_draws();
    test_cameras();
    test_cascade_splits();
    test_cascade_fit();
    test_ecs_entities();
    test_sdl_renderer_has_no_3d();
    test_gpu_reports_3d();
    test_gpu_depth_and_colour();
    test_gpu_lighting();
    test_gpu_instances();
    test_gpu_instances_on_workers();
    test_gpu_frustum_culling();
    test_gpu_overlay();
    test_gpu_point_lights();
    test_gpu_shadows();
    test_gpu_blending();
    test_gpu_hdr_bloom();
    test_gpu_texture();
    test_gpu_model();
    test_gpu_culling();
    test_gpu_interleaves_with_2d();
    test_gpu_linear_hdr();
    test_gpu_meshes_outlive_renderer();
    test_gpu_pick();
    test_gpu_wireframe();
    test_gpu_fill_and_ambient();
    test_gpu_receives_no_shadow();
    test_gpu_large_mesh();
    test_gpu_shadow_faces();
    test_gpu_shadows_into_view();
    test_gpu_cascades();
    test_gpu_ambient_occlusion();
    test_gpu_contact_shadows();
    test_taa_jitter();
    test_gpu_taa();
    test_gpu_taa_over_detail();
    test_gpu_taa_crowd();
    test_gpu_taa_overlay();
    test_gpu_taa_with_occlusion();
    test_gpu_taa_pick();
    test_gpu_taa_hdr();
    test_gpu_taa_ids_after_reset();
    test_gpu_taa_match_on_workers();
    test_gpu_sun_off_no_shadow_pass();
    test_gpu_metal_and_roughness();
    test_gpu_normal_map();
    test_gpu_alpha_mask();
    test_gpu_inverse_square();
    test_gpu_model_materials();
    test_gpu_wrap_modes();
    test_gpu_environment();
    test_gpu_material_shader();
    return 0;
}
