// Reflection probes (docs/rendering_3d.md, "Reflection probes"): their weights and box projection on the CPU, and on
// the GPU a capture read by a mirror floor, parallax-corrected.
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <tuple>

using namespace kin;

namespace {

constexpr Vec2i extent{160, 120};
constexpr Rectf rect{0, 0, 160, 120};

bool near(f32 a, f32 b, f32 eps = 1e-4f) {
    return std::abs(a - b) <= eps;
}
bool same_way(Vec3f a, Vec3f b) {
    return near(a.x, b.x, 1e-3f) && near(a.y, b.y, 1e-3f) && near(a.z, b.z, 1e-3f);
}

void test_weight() {
    ReflectionProbeSource3D probe{.position = {1, 0, 0}, .extent = {2, 1, 1}, .blend = 0.5f};
    assert(near(reflection_probe_weight(probe, {1, 0, 0}), 1));
    assert(near(reflection_probe_weight(probe, {2.75f, 0, 0}), 0.5f)); // a quarter from the +X face
    assert(near(reflection_probe_weight(probe, {3.5f, 0, 0}), 0));     // outside
    assert(near(reflection_probe_weight(probe, {1, 0.9f, 0}), 0.2f));
    // The boxes turned a quarter about Y: their X along world -Z.
    probe.rotation = Quatf::axis_angle({0, 1, 0}, 90);
    assert(near(reflection_probe_weight(probe, {1, 0, -1.9f}), 0.2f));
    assert(near(reflection_probe_weight(probe, {2.5f, 0, 0}), 0)); // its Z half extent is 1
    probe.blend = 0;
    assert(near(reflection_probe_weight(probe, {1, 0.99f, 0}), 1));
    // Offset boxes.
    ReflectionProbeSource3D offset{.position = {0, 0, 0}, .box_offset = {0, 5, 0}, .extent = {1, 1, 1}};
    assert(reflection_probe_weight(offset, {0, 0, 0}) == 0 && reflection_probe_weight(offset, {0, 5, 0}) > 0);
}

void test_direction() {
    const ReflectionProbeSource3D probe{.position = {0, 0, 0}, .extent = {5, 5, 5}};
    // From where it was captured, the way itself.
    assert(same_way(*reflection_probe_direction(probe, {0, 0, 0}, {1, 0, 0}), {1, 0, 0}));
    // From elsewhere, toward where the ray leaves the box: (2, 0, 5) seen from the origin.
    const Vec3f toward = normalize(Vec3f{2, 0, 5});
    assert(same_way(*reflection_probe_direction(probe, {2, 0, 0}, {0, 0, 1}), toward));
    // A smaller parallax box (half 1 in Y): a floor at y = -1 seen from 3 along X.
    ReflectionProbeSource3D room = probe;
    room.parallax_extent = {5, 1, 5};
    const Vec3f down = normalize(Vec3f{3, -1, 0});
    assert(same_way(*reflection_probe_direction(room, {0, 0, 0}, normalize(Vec3f{3, -1, 0})), down));
    assert(same_way(*reflection_probe_direction(room, {2, 0, 0}, {0, -1, 0}), normalize(Vec3f{2, -1, 0})));
    // Outside the parallax box: nothing; without parallax: the way itself.
    assert(!reflection_probe_direction(room, {0, 3, 0}, {0, 1, 0}));
    room.parallax = false;
    assert(same_way(*reflection_probe_direction(room, {2, 0, 0}, {0, -1, 0}), {0, -1, 0}));
    // Turned boxes: the same room a quarter round Y gives the same answer for a ray along Y.
    ReflectionProbeSource3D turned = probe;
    turned.rotation = Quatf::axis_angle({0, 1, 0}, 90);
    turned.parallax_extent = {1, 5, 5}; // its X (world -Z) short
    assert(same_way(*reflection_probe_direction(turned, {0, 0, 0}, {0, 0, -1}), {0, 0, -1}));
    assert(same_way(*reflection_probe_direction(turned, {2, 0, 0}, {0, 0, -1}), normalize(Vec3f{2, 0, -1})));
    std::cout << "probes: weights and directions\n";
}

struct Rgb {
    int r, g, b;
};

// A room 2 high (floor y = 0, a grey ceiling at y = 2 with a red patch over (3, 1)), its floor a mirror; a probe
// captured at (0, 1, 0) whose parallax box is the room. A camera at (-3, 1, 0) sees the patch's reflection on the floor
// at
// (-1, 0, 1/3) (the patch mirrored to (3, -2, 1), the line to it crossing the floor there): the probe,
// parallax-corrected, shows it there (not on the other side: the views are not mirrored); read as if infinitely far, it
// shows the grey ceiling there instead.
void test_parallax(Renderer2D& r, Renderer3D& r3d) {
    View3D view = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    const Mesh3D plane = r3d.create_mesh(meshes::plane({1, 1})).value_or({});
    assert(view && plane);
    const Material3D grey{.color = Color::rgb(90, 90, 90), .unlit = true, .double_sided = true};
    const Material3D red{.color = Color::rgb(240, 20, 20), .unlit = true, .double_sided = true};
    const Material3D mirror{.color = colors::white, .metallic = 1, .roughness = 0};
    const auto room = [&](Scene3D& scene) {
        scene.lights.sun.color = {};
        scene.lights.ambient = {};
        assert(scene.draw(plane, Mat4f::trs({0, 0, 0}, {}, {12, 1, 12}), mirror));
        assert(scene.draw(plane, Mat4f::trs({0, 2, 0}, {}, {12, 1, 12}), grey));
        assert(scene.draw(plane, Mat4f::trs({3, 1.98f, 1}, {}, {0.8f, 1, 0.8f}), red));
    };
    ReflectionCapture3D capture = r3d.create_reflection_capture(64).value_or({});
    assert(capture);
    Scene3D scene;
    room(scene);
    assert(r3d.capture_reflection(capture, scene, {0, 1, 0}));
    assert(r3d.debug_stats().reflection_captures == 1);
    const Vec3f floor_point{-1, 0, 1.0f / 3.0f};
    const auto frame = [&](bool parallax) {
        Scene3D lit;
        room(lit);
        lit.camera = Camera3D::look_at({-3, 1, 0}, floor_point, 60);
        lit.lights.reflection_probes.push_back({.position = {0, 1, 0},
                                                .extent = {5, 1.5f, 5},
                                                .blend = 0.1f,
                                                .parallax_extent = {5, 1, 5},
                                                .parallax = parallax,
                                                .capture = capture});
        r.clear(colors::black);
        assert(r3d.render(view, lit));
        r.draw_texture(view.texture(), rect);
        std::vector<u8> pixels;
        Vec2i size;
        assert(r.read_rgba(rect, pixels, size) && size == extent);
        r.present();
        const auto at = lit.camera.project(floor_point, rect);
        assert(at);
        const auto pixel = [&](Vec2f p) {
            const u8* c = &pixels[(std::size_t(p.y) * std::size_t(extent.x) + std::size_t(p.x)) * 4];
            return Rgb{c[0], c[1], c[2]};
        };
        // The other side, mirrored across the camera's line: where a left-right flipped view would put the patch.
        const auto other = lit.camera.project({-1, 0, -1.0f / 3.0f}, rect);
        assert(other);
        return std::tuple{pixel(at->pos), pixel(other->pos), r3d.debug_stats()};
    };
    const auto [corrected, flipped, first] = frame(true);
    assert(first.reflection_probes == 1 && first.reflection_filters == 1);
    std::cout << "probes: corrected " << corrected.r << " " << corrected.g << " " << corrected.b << "\n";
    assert(corrected.r > 150 && corrected.g < 80);                   // the red patch
    assert(flipped.r < 140 && std::abs(flipped.r - flipped.g) < 30); // and the ceiling on the other side
    const auto [infinite, unused, second] = frame(false);
    assert(second.reflection_filters == 0); // filtered once, kept
    std::cout << "probes: infinite " << infinite.r << " " << infinite.g << " " << infinite.b << "\n";
    assert(infinite.r < 140 && std::abs(infinite.r - infinite.g) < 30); // the grey ceiling
    std::cout << "probes: parallax\n";
}

// Camera3D::hidden_id: the copies with that pick id are not drawn by the camera (a shiny thing in its own probe's
// capture); the others are.
void test_hidden(Renderer2D& r, Renderer3D& r3d) {
    View3D view = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    const Mesh3D box = r3d.create_mesh(meshes::box({1, 1, 1})).value_or({});
    assert(view && box);
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 6}, {0, 0, 0});
    const Material3D red{.color = Color::rgb(230, 20, 20), .unlit = true};
    const Material3D green{.color = Color::rgb(20, 230, 20), .unlit = true};
    assert(scene.draw(box, Mat4f::translation({-1.5f, 0, 0}), red, colors::white, 7));
    assert(scene.draw(box, Mat4f::translation({1.5f, 0, 0}), green, colors::white, 8));
    const auto seen = [&](u32 hidden) {
        scene.camera.hidden_id = hidden;
        r.clear(colors::black);
        assert(r3d.render(view, scene));
        r.draw_texture(view.texture(), rect);
        std::vector<u8> pixels;
        Vec2i size;
        assert(r.read_rgba(rect, pixels, size) && size == extent);
        r.present();
        const auto pixel = [&](Vec3f p) {
            const auto at = scene.camera.project(p, rect);
            assert(at);
            return pixels[(std::size_t(at->pos.y) * std::size_t(extent.x) + std::size_t(at->pos.x)) * 4 +
                          (p.x < 0 ? 0 : 1)];
        };
        return std::pair{pixel({-1.5f, 0, 0.5f}) > 150, pixel({1.5f, 0, 0.5f}) > 150};
    };
    assert((seen(0) == std::pair{true, true}));
    assert((seen(7) == std::pair{false, true}));
    assert((seen(8) == std::pair{true, false}));
    std::cout << "probes: hidden copies\n";
}

// Screen-space reflections (Scene3D::ssr): a mirror floor and a red box on it, seen from (0, 1, 4). The box's front
// face's middle (0, 0.5, 0.5), mirrored to (0, -0.5, 0.5), shows on the floor where the line to it crosses it:
// (0, 0, 5/3). From the second frame on (the trace reads the previous one) it is red there with them, and not without
// (no environment, no probe: the hemisphere's grey). An LDR view (its picture sRGB-encoded) and an HDR one.
void test_ssr(Renderer2D& r, Renderer3D& r3d) {
    const Mesh3D plane = r3d.create_mesh(meshes::plane({1, 1})).value_or({});
    const Mesh3D box = r3d.create_mesh(meshes::box({1, 1, 1})).value_or({});
    assert(plane && box);
    const Vec3f floor_point{0, 0, 5.0f / 3.0f};
    for (const bool hdr : {false, true}) {
        View3D view = r3d.create_view(extent, {.samples = 1, .background = colors::black, .hdr = hdr}).value_or({});
        assert(view);
        const auto frame = [&](bool ssr) {
            Scene3D scene;
            scene.camera = Camera3D::look_at({0, 1, 4}, floor_point, 60);
            scene.lights.sun.color = {};
            scene.lights.ambient = {0.05f, 0.05f, 0.05f};
            scene.ssr.enabled = ssr;
            assert(scene.draw(plane, Mat4f::trs({0, 0, 0}, {}, {20, 1, 20}),
                              {.color = colors::white, .metallic = 1, .roughness = 0}));
            assert(
                scene.draw(box, Mat4f::translation({0, 0.5f, 0}), {.color = Color::rgb(240, 20, 20), .unlit = true}));
            r.clear(colors::black);
            assert(r3d.render(view, scene));
            const u32 traces = r3d.debug_stats().ssr_traces;
            r.draw_texture(view.texture(), rect);
            std::vector<u8> pixels;
            Vec2i size;
            assert(r.read_rgba(rect, pixels, size) && size == extent);
            r.present();
            const auto at = scene.camera.project(floor_point, rect);
            assert(at);
            const u8* c = &pixels[(std::size_t(at->pos.y) * std::size_t(extent.x) + std::size_t(at->pos.x)) * 4];
            return std::pair{Rgb{c[0], c[1], c[2]}, traces};
        };
        const auto [first, first_traces] = frame(true);
        assert(first_traces == 0); // nothing to read yet
        const auto [seen, traces] = frame(true);
        std::cerr << "ssr (" << (hdr ? "hdr" : "ldr") << "): " << seen.r << " " << seen.g << " " << seen.b << "\n";
        assert(traces == 1 && seen.r > 120 && seen.g < 60);
        const auto [plain, none] = frame(false);
        std::cerr << "no ssr (" << (hdr ? "hdr" : "ldr") << "): " << plain.r << " " << plain.g << " " << plain.b
                  << " traces " << none << "\n";
        assert(none == 0 && plain.r < 120 && std::abs(plain.r - plain.g) < 20); // the grey ambient
    }
    std::cout << "ssr: reflections\n";
}

} // namespace

int main() {
    test_weight();
    test_direction();
    App app;
    auto& window = app.create_window({.title = "reflection probes", .width = 160, .height = 120, .hidden = true});
    Renderer2D r{make_render_backend(window, false, true)};
    Renderer3D r3d{r};
    const bool require = std::getenv("KIN_REQUIRE_GPU_TESTS") != nullptr;
    if (!r3d.available())
        return require ? 1 : 77;
    test_parallax(r, r3d);
    test_hidden(r, r3d);
    test_ssr(r, r3d);
}
