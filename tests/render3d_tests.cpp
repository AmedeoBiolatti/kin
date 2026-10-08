#include <kin/core/jobs.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <cassert>
#include <cmath>
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
// (for shapes round the origin) facing out.
void check_winding(const MeshData& m, std::string_view name, bool outward) {
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
        if (kin::dot(n, vn) <= 0.0f || (outward && kin::dot(n, centroid(m, t)) <= 0.0f)) {
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

} // namespace

int main() {
    test_meshes_face_out();
    test_mesh_validation_and_append();
    test_scene_merges_draws();
    test_cameras();
    test_sdl_renderer_has_no_3d();
    test_gpu_reports_3d();
    test_gpu_depth_and_colour();
    test_gpu_lighting();
    test_gpu_instances();
    test_gpu_instances_on_workers();
    test_gpu_culling();
    test_gpu_interleaves_with_2d();
    test_gpu_linear_hdr();
    test_gpu_meshes_outlive_renderer();
    return 0;
}
