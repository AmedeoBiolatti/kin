#include <kin/core/jobs.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
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
    assert(!r3d.render(view, scene)); // nothing drawn, but the background is there
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
                assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene));
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
            assert(r3d.render(view, scene));
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
        assert(r3d.render(view, all) && r3d.stats().instances == 1 && r3d.stats().culled == 0);
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
        assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene) && r3d.stats().lights == 1);
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
        assert(r3d.render(view, scene) && r3d.stats().lights == 16);
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
            assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene));
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
            assert(r3d.render(view, scene));
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
        assert(r3d.render(dark, scene));
        renderer.draw_texture(dark.texture(), screen);
        const Color background = read(renderer).at(4, 4);
        assert(background.r <= 18 && background.b <= 36 && background.b > background.g && background.g > background.r);
        renderer.present();
        // Over a transparent background the glow still shows on what is under it.
        kin::View3D clear_view = r3d.create_view({160, 120}, {.samples = 1, .background = kin::colors::transparent, .bloom = 1.0f});
        renderer.clear(Color::rgb(0, 0, 120));
        assert(r3d.render(clear_view, scene));
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
        assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene));
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
            assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene));
        renderer.draw_texture(view.texture(), {0.0f, 0.0f, 40.0f, 40.0f});
        scene.clear();
        scene.draw(box, Mat4f::identity(), {.color = Color::rgb(30, 220, 30), .unlit = true});
        assert(r3d.render(view, scene));
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
        assert(r3d.render(clear_view, scene));
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
        assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene));
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
        assert(r3d.render(plain, scene));
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
        assert(r3d.render(view, scene));
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
            assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene) && r3d.stats().shadow_draws == 2);
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
            assert(r3d.render(view, scene));
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
            assert(r3d.render(view, scene));
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
        assert(r3d.render(view, scene));
        assert(r3d.stats().culled == 3);
        assert(r3d.stats().shadow_casters == 2); // the floor, and the box casting into view
        renderer.draw_texture(view.texture(), screen);
        const Pixels p = read(renderer);
        const auto at = [&](Vec3f w) { return p.at(scene.camera.project(w, screen)->pos); };
        assert(brightness(at({3.5f, 0.0f, 0.0f})) * 2 < brightness(at({-2.0f, 0.0f, 0.0f})));
        renderer.present();
    });
}

} // namespace

int main() {
    test_meshes_face_out();
    test_mesh_validation_and_append();
    test_scene_merges_draws();
    test_cameras();
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
    return 0;
}
