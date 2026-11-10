// Reflection probes (docs/rendering_3d.md, "Reflection probes"): their weights and box projection on the CPU, and on
// the GPU a capture read by a mirror floor, parallax-corrected. And a scene written out for a path tracer
// (write_scene_snapshot: its bakes face the right way, its sky's panorama).
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <kin/assets/image.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/snapshot3d.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <tuple>

using namespace kin;

extern "C" { // (kin's .hdr decoder, in the engine)
float* kin_hdr_load(const unsigned char* bytes, int size, int* width, int* height);
void kin_hdr_free(float* pixels);
}

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
    assert(!r3d.save_reflection_capture(capture, std::filesystem::temp_directory_path() / "kin-probe-empty.hdr"));
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
    // Baked: the capture written as a Radiance .hdr panorama and loaded back reads the same (the red patch where the
    // parallax puts it), without a capture.
    const auto file = std::filesystem::temp_directory_path() / "kin-probe-room.hdr";
    assert(r3d.save_reflection_capture(capture, file));
    {
        std::ifstream in(file, std::ios::binary);
        std::string head(10, '\0');
        assert(in.read(head.data(), 10) && head == "#?RADIANCE");
    }
    ReflectionCapture3D loaded = r3d.load_reflection_capture(file).value_or({});
    assert(loaded && loaded.backend()->generation() == 1);
    capture = loaded;
    const auto [baked, unused_baked, third] = frame(true);
    std::cerr << "probes: baked " << baked.r << " " << baked.g << " " << baked.b << "\n";
    assert(third.reflection_filters == 1 && baked.r > 150 && baked.g < 80);
    // A missing picture is refused.
    assert(!r3d.load_reflection_capture(std::filesystem::temp_directory_path() / "kin-probe-missing.hdr"));
    std::filesystem::remove(file);
    std::cout << "probes: baked to disk\n";
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
        std::vector<Rgb> extra; // the pixels at `probe_points` of the last frame
        std::vector<Vec3f> probe_points;
        const auto frame = [&](bool ssr, Material3D floor = {.color = colors::white, .metallic = 1, .roughness = 0}) {
            Scene3D scene;
            scene.camera = Camera3D::look_at({0, 1, 4}, floor_point, 60);
            scene.lights.sun.color = {};
            scene.lights.ambient = {0.05f, 0.05f, 0.05f};
            scene.ssr.enabled = ssr;
            assert(scene.draw(plane, Mat4f::trs({0, 0, 0}, {}, {20, 1, 20}), floor));
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
            const auto pixel = [&](Vec3f p) {
                const auto at = scene.camera.project(p, rect);
                assert(at);
                const u8* c = &pixels[(std::size_t(at->pos.y) * std::size_t(extent.x) + std::size_t(at->pos.x)) * 4];
                return Rgb{c[0], c[1], c[2]};
            };
            extra.clear();
            for (const Vec3f p : probe_points)
                extra.push_back(pixel(p));
            return std::pair{pixel(floor_point), traces};
        };
        const auto [first, first_traces] = frame(true);
        assert(first_traces == 0); // nothing to read yet
        const auto [seen, traces] = frame(true);
        std::cerr << "ssr (" << (hdr ? "hdr" : "ldr") << "): " << seen.r << " " << seen.g << " " << seen.b << "\n";
        assert(traces == 1 && seen.r > 120 && seen.g < 60);
        // A glossy floor (roughness 0.4, under max_roughness): the red, blurred (a level of the picture's pyramid as
        // wide as the lobe), not the grey.
        const Material3D glossy{.color = colors::white, .metallic = 1, .roughness = 0.4f};
        (void)frame(true, glossy);
        const auto [blurred, glossy_traces] = frame(true, glossy);
        std::cerr << "ssr glossy (" << (hdr ? "hdr" : "ldr") << "): " << blurred.r << " " << blurred.g << " "
                  << blurred.b << "\n";
        assert(glossy_traces == 1 && blurred.r > blurred.g + 40);
        // A floor whose normal map turns every normal far toward +X: its reflection there goes past the box (it follows
        // the mapped normal, not the pre-pass's flat one), so the red is gone from that spot.
        const u8 bent[]{230, 128, 204, 255};
        const Texture tilt = r.create_texture({1, 1}, TextureFormat::Rgba8, bent);
        assert(tilt);
        const Material3D mapped{.color = colors::white, .metallic = 1, .roughness = 0, .normal = tilt};
        (void)frame(true, mapped);
        const auto [turned, turned_traces] = frame(true, mapped);
        std::cerr << "ssr normal map (" << (hdr ? "hdr" : "ldr") << "): " << turned.r << " " << turned.g << " "
                  << turned.b << "\n";
        assert(turned_traces == 1 && turned.r < turned.g + 40);
        // A mild one (8 degrees toward the tangent): the reflection turns about 16 and its hit, about 1.3 away, moves
        // about 0.36. The floor's points whose flat reflections meet the box's face 0.05 in from its sides and bottom
        // and 0.1 in from its top (the face's points mirrored under the floor, the lines to them from the eye crossing
        // it; the top's 0.05 is under a pixel of this view from its edge, which a trace a texel a pixel can miss):
        // flat, all red; bumped, some now pass beside, over or under the box (which, by the tangent's way).
        probe_points = {{-0.3f, 0, 5.0f / 3.0f}, {0.3f, 0, 5.0f / 3.0f}, {0, 0, 2.158f}, {0, 0, 0.667f}};
        const auto red = [](Rgb c) { return c.r > c.g + 80; };
        (void)frame(true);
        (void)frame(true);
        for (const Rgb& c : extra)
            assert(red(c));
        const u8 mild_bent[]{146, 128, 254, 255};
        const Texture mild = r.create_texture({1, 1}, TextureFormat::Rgba8, mild_bent);
        const Material3D bumped{.color = colors::white, .metallic = 1, .roughness = 0, .normal = mild};
        (void)frame(true, bumped);
        (void)frame(true, bumped);
        std::cerr << "ssr mild normal map:";
        u32 lost = 0;
        for (const Rgb& c : extra) {
            std::cerr << " " << c.r << "," << c.g;
            lost += red(c) ? 0 : 1;
        }
        std::cerr << "\n";
        assert(lost >= 1);
        probe_points.clear();
        const auto [plain, none] = frame(false);
        std::cerr << "no ssr (" << (hdr ? "hdr" : "ldr") << "): " << plain.r << " " << plain.g << " " << plain.b
                  << " traces " << none << "\n";
        assert(none == 0 && plain.r < 120 && std::abs(plain.r - plain.g) < 20); // the grey ambient
    }
    std::cout << "ssr: reflections\n";
}

// A probe's diffuse light (ReflectionProbeSource3D::diffuse): a rough white box in the dark, a big red panel at its +X
// side, a probe captured at its centre (the box left out). With the probe's diffuse light, the box's +X face is lit
// red by what the probe saw that way; without (reflections only: a rough dielectric's faint sheen), it stays dark.
void test_diffuse(Renderer2D& r, Renderer3D& r3d) {
    View3D view = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    const Mesh3D box = r3d.create_mesh(meshes::box({1, 1, 1})).value_or({});
    const Mesh3D plane = r3d.create_mesh(meshes::plane({1, 1})).value_or({});
    assert(view && box && plane);
    const auto room = [&](Scene3D& scene) {
        scene.lights.sun.color = {};
        scene.lights.ambient = {};
        assert(scene.draw(box, Mat4f::identity(), {.color = colors::white, .roughness = 0.9f}, colors::white, 9));
        assert(scene.draw(plane, Mat4f::trs({3, 0, 0}, Quatf::axis_angle({0, 0, 1}, 90), {8, 1, 8}),
                          {.color = Color::rgb(240, 20, 20), .unlit = true, .double_sided = true}));
    };
    ReflectionCapture3D capture = r3d.create_reflection_capture(32).value_or({});
    Scene3D scene;
    room(scene);
    assert(capture && r3d.capture_reflection(capture, scene, {0, 0, 0}, {.hidden_id = 9}));
    const auto face = [&](bool diffuse) {
        Scene3D lit;
        room(lit);
        lit.camera = Camera3D::look_at({3, 0.6f, 2.5f}, {0, 0, 0}, 50);
        lit.lights.reflection_probes.push_back(
            {.extent = {4, 4, 4}, .blend = 0.1f, .parallax = false, .diffuse = diffuse, .capture = capture});
        r.clear(colors::black);
        assert(r3d.render(view, lit));
        r.draw_texture(view.texture(), rect);
        std::vector<u8> pixels;
        Vec2i size;
        assert(r.read_rgba(rect, pixels, size) && size == extent);
        r.present();
        const auto at = lit.camera.project({0.5f, 0, 0}, rect);
        assert(at);
        const u8* c = &pixels[(std::size_t(at->pos.y) * std::size_t(extent.x) + std::size_t(at->pos.x)) * 4];
        return Rgb{c[0], c[1], c[2]};
    };
    const Rgb lit = face(true), unlit = face(false);
    std::cerr << "probe diffuse: " << lit.r << " " << lit.g << " " << lit.b << " / reflections only: " << unlit.r << " "
              << unlit.g << " " << unlit.b << "\n";
    assert(lit.r > 80 && lit.g < lit.r / 3);
    assert(unlit.r < lit.r / 2);
    std::cout << "probes: diffuse light\n";
}

// A capture lit by a view's global illumination (ReflectionCaptureOptions3D::global_illumination): a closed white room
// lit only by a bright green emitter on its floor (no sun, no sky, no ambient light), seen by a view until its GI has
// light. A probe captured at the room's middle borrowing that view's GI sees the walls lit green by the emitter's
// bounce; one without (a capture's own GI would start dark) sees them black.
void test_gi_capture(Renderer2D& r, Renderer3D& r3d) {
    if (!r3d.capabilities().global_illumination) {
        std::cout << "probes: GI capture skipped (no GI here)\n";
        return;
    }
    View3D view = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    const Mesh3D box = r3d.create_mesh(meshes::box({1, 1, 1})).value_or({});
    assert(view && box);
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 1.5f, 1.5f}, {0, 1.0f, -2}, 70);
    scene.lights.sun.color = {};
    scene.lights.ambient = {};
    scene.global_illumination = {.enabled = true, .area = {{-3, -1, -3}, {3, 4, 3}}, .cell = 0.15f};
    scene.global_illumination.probes.per_render = 256;
    Material3D wall{.color = colors::white, .roughness = 0.9f};
    wall.double_sided = true;
    const auto room = [&] {
        scene.clear();
        assert(scene.draw(box, Mat4f::trs({0, 1.5f, 0}, {}, {4, 3, 4}), wall));
        assert(scene.draw(box, Mat4f::trs({0, 0.1f, 0}, {}, {1, 0.2f, 1}),
                          {.color = Color::rgb(40, 255, 40), .emissive = 20}));
    };
    room();
    for (int i = 0; i < 40; ++i) {
        r.clear(colors::black);
        assert(r3d.render(view, scene));
        r.present();
    }
    assert(r3d.global_illumination_lit(view));
    // The captures' panoramas' mean green, the emitter's bright texels left out: the walls' light.
    const auto walls = [&](const ReflectionCapture3D& capture, const char* name) {
        const auto file = std::filesystem::temp_directory_path() / name;
        assert(r3d.save_reflection_capture(capture, file));
        std::ifstream in(file, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(in), {}};
        int w = 0, h = 0;
        float* rgb = kin_hdr_load(reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()), &w, &h);
        assert(rgb);
        double sum = 0;
        int n = 0;
        for (int i = 0; i < w * h; ++i)
            if (rgb[i * 3 + 1] < 2.0f) {
                sum += rgb[i * 3 + 1];
                ++n;
            }
        kin_hdr_free(rgb);
        std::filesystem::remove(file);
        return n ? sum / n : 0.0;
    };
    ReflectionCapture3D lit = r3d.create_reflection_capture(32).value_or({});
    ReflectionCapture3D dark = r3d.create_reflection_capture(32).value_or({});
    assert(lit && dark);
    room();
    assert(r3d.capture_reflection(lit, scene, {0, 1.5f, 0}, {.global_illumination = &view}));
    assert(r3d.debug_stats().reflection_gi_captures == 1);
    assert(r3d.capture_reflection(dark, scene, {0, 1.5f, 0}));
    assert(r3d.debug_stats().reflection_gi_captures == 0);
    const double with_gi = walls(lit, "kin-probe-gi.hdr"), without = walls(dark, "kin-probe-nogi.hdr");
    std::cerr << "probes: walls' green with the view's GI " << with_gi << ", without " << without << "\n";
    assert(with_gi > 0.02 && with_gi > 4.0 * without);
    // The view's own GI is as it was: it still renders lit.
    r.clear(colors::black);
    assert(r3d.render(view, scene));
    r.present();
    assert(r3d.global_illumination_lit(view));
    std::cout << "probes: captures lit by a view's global illumination\n";

    // A second view rendered lit by the first's GI (Renderer3D::render(view, scene, lit_by): a mirror's or a portal's
    // picture): its walls green from the first's probes, none of its own made; it may render part of itself (GI
    // borrowed, not built round its whole picture); lit by a view without light yet, no GI at all (not one of its own).
    View3D other = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    View3D unlit_view = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    assert(other && unlit_view);
    assert(!r3d.can_render_part(other, scene) && r3d.can_render_part(other, scene, view));
    const auto wall_green = [&](View3D& into, const View3D* lit_by, Vec2i part) {
        into.set_render_size(part);
        r.clear(colors::black);
        assert(lit_by ? r3d.render(into, scene, *lit_by) : r3d.render(into, scene));
        r.draw_texture(into.texture(), rect);
        std::vector<u8> pixels;
        Vec2i size;
        assert(r.read_rgba(rect, pixels, size) && size == extent);
        r.present();
        // (A wall at the left of the picture, in the part rendered: its top-left quarter.)
        double green = 0;
        for (int y = 10; y < 20; ++y)
            for (int x = 10; x < 20; ++x)
                green += pixels[(std::size_t(y) * std::size_t(extent.x) + std::size_t(x)) * 4 + 1];
        return green / 100.0;
    };
    const double borrowed = wall_green(other, &view, {}), in_part = wall_green(other, &view, {80, 60});
    assert(!r3d.global_illumination_lit(other)); // (none of its own)
    const double without_light = wall_green(other, &unlit_view, {});
    assert(!r3d.global_illumination_lit(other) && !r3d.global_illumination_lit(unlit_view));
    std::cerr << "probes: a view lit by another's GI: wall green " << borrowed << " (in part " << in_part
              << "), lit by one without light " << without_light << "\n";
    assert(borrowed > 8.0 && borrowed > 3.0 * (without_light + 1.0) && std::abs(in_part - borrowed) < 0.25 * borrowed);
    std::cout << "probes: views lit by another view's global illumination, in part too\n";
}

} // namespace

// A textured floor (baked into a page) and a plain box (its constants), under a sky: the files, the floor's normal
// baked facing up (+Y), its texture's two colours in the base page, its UVs in its mesh file pointing at them.
void test_snapshot(Renderer2D& r, Renderer3D& r3d) {
    const Mesh3D plane = r3d.create_mesh(meshes::plane({4, 4})).value_or({});
    const Mesh3D box = r3d.create_mesh(meshes::box({1, 1, 1})).value_or({});
    const u8 checks[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 255, 0, 0, 255};
    const Texture checker = r.create_texture({2, 2}, TextureFormat::Rgba8, checks);
    assert(plane && box && checker);
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 3, 5}, {0, 0, 0});
    scene.sky.enabled = true;
    scene.lights.sky_lights_scene = true;
    assert(scene.draw(plane, Mat4f::identity(), Material3D{.texture = checker}).ok());
    assert(scene.draw(box, Mat4f::translation({0, 0.5f, 0}), Material3D{.color = Color::rgb(40, 90, 200)}).ok());
    const auto folder = std::filesystem::temp_directory_path() / "kin-snapshot";
    std::filesystem::remove_all(folder);
    const auto written = write_scene_snapshot(
        r3d, scene, extent, folder, {.texels_per_unit = 16, .max_cell = 128, .page_size = 256, .environment_side = 16});
    assert(written.ok());
    assert(written->objects == 2 && written->baked == 1 && written->pages == 1 && written->triangles == 14);
    assert(std::filesystem::exists(folder / "scene.json") && std::filesystem::exists(folder / "sky.hdr"));
    const Image normal = load_image(folder / "pages" / "page0_normal.png");
    const Image base = load_image(folder / "pages" / "page0_base.png");
    assert(normal.valid() && base.valid() && normal.size == (Vec2i{256, 256}));
    // The floor's mesh file: its corners' UVs (u right, v down over the page) and page; at each, its bakes.
    std::ifstream in(folder / "meshes" / "00000.bin", std::ios::binary);
    std::vector<f32> corners;
    f32 value = 0;
    while (in.read(reinterpret_cast<char*>(&value), sizeof value))
        corners.push_back(value);
    assert(corners.size() == 6 * 13); // (two triangles, unindexed)
    int red = 0, green = 0;
    for (std::size_t k = 0; k < 6; ++k) {
        const f32* v = &corners[k * 13];
        assert(v[12] == 0.0f); // its page
        // A little inside the triangle, toward its middle: the texel there.
        const std::size_t t = k / 3 * 3;
        const f32 u =
            0.8f * v[6] + 0.1f * (corners[t * 13 + 6] + corners[(t + 1) * 13 + 6] + corners[(t + 2) * 13 + 6] - v[6]);
        const f32 w =
            0.8f * v[7] + 0.1f * (corners[t * 13 + 7] + corners[(t + 1) * 13 + 7] + corners[(t + 2) * 13 + 7] - v[7]);
        const std::size_t texel = (std::size_t(w * 256) * 256 + std::size_t(u * 256)) * 4;
        // Facing up: (0, 1, 0) stored as (128, 255, 128).
        assert(std::abs(int(normal.rgba[texel]) - 128) < 6 && normal.rgba[texel + 1] > 245 &&
               std::abs(int(normal.rgba[texel + 2]) - 128) < 6);
        red += base.rgba[texel] > 150 && base.rgba[texel + 1] < 100;
        green += base.rgba[texel + 1] > 150 && base.rgba[texel] < 100;
    }
    assert(red > 0 && green > 0); // (the checker's corners: red at (0, 0) and (1, 1), green at the others)
    std::filesystem::remove_all(folder);
    std::cout << "snapshot: bakes face up, the texture in the page, the sky written\n";
}

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
    test_diffuse(r, r3d);
    test_gi_capture(r, r3d);
    test_ssr(r, r3d);
    test_snapshot(r, r3d);
}
