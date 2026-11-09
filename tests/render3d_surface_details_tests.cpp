// Decals (Scene3D::draw_decal), auto exposure (ViewOptions3D::auto_exposure) and morph targets
// (MeshData3D::morph_targets, Renderer3D::set_morph_weights): the CPU parts always, the pictures where a GPU 3D device
// is (77, skipped, otherwise; 1 with KIN_REQUIRE_GPU_TESTS).
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kin/assets/image.hpp>
#include <kin/ecs/scene3d.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/animation3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scene/scene_loader3d.hpp>
#include <vector>

using namespace kin;
namespace {
constexpr Vec2i extent{96, 96};
constexpr Rectf rect{0, 0, 96, 96};

bool near(f32 a, f32 b, f32 tolerance = 1e-5f) {
    return std::abs(a - b) <= tolerance;
}
bool near(Vec3f a, Vec3f b, f32 tolerance = 1e-5f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

// A quad facing +Z, `size` across, centred on the origin, with a target moving it by `offset`.
MeshData3D quad(f32 size, Vec3f offset = {}) {
    MeshData3D m = meshes::plane({size, size}, 1);
    for (MeshVertex3D& v : m.vertices) { // the plane faces +Y: turned to face +Z
        v.position = {v.position.x, -v.position.z, v.position.y};
        v.normal = {0, 0, 1};
    }
    if (length_squared(offset) > 0)
        m.morph_targets.push_back({"slide", std::vector<Vec3f>(m.vertices.size(), offset), {}});
    return m;
}

void cpu_morphs() {
    MeshData3D m = quad(2);
    MorphTarget3D up{"up", std::vector<Vec3f>(m.vertices.size(), Vec3f{0, 0, 1}), {}};
    MorphTarget3D turn{"turn", std::vector<Vec3f>(m.vertices.size(), Vec3f{}),
                       std::vector<Vec3f>(m.vertices.size(), Vec3f{1, 0, -1})};
    m.morph_targets = {up, turn};
    assert(m.validate());
    std::vector<MeshVertex3D> out;
    const std::array<f32, 2> half{0.5f, 0.0f};
    blend_morph_targets(m, half, out);
    for (std::size_t i = 0; i < out.size(); ++i)
        assert(near(out[i].position, m.vertices[i].position + Vec3f{0, 0, 0.5f}) && near(out[i].normal, {0, 0, 1}));
    const std::array<f32, 2> turned{0.0f, 1.0f};
    blend_morph_targets(m, turned, out);
    assert(near(out[0].normal, {1, 0, 0})); // (0, 0, 1) + (1, 0, -1), made unit
    // Bounds hold any weights within [-1, 1].
    const Bounds3D b = morph_bounds(m);
    assert(near(b.min.z, -1) && near(b.max.z, 1) && near(b.min.x, -1) && near(b.max.x, 1));
    // Validation: counts and finiteness.
    MeshData3D bad = m;
    bad.morph_targets[0].positions.pop_back();
    assert(!bad.validate());
    bad = m;
    bad.morph_weights = {1};
    assert(!bad.validate());
    bad = m;
    bad.morph_targets[1].normals[0].x = std::nanf("");
    assert(!bad.validate());
    // Appending meets targets by name; the other side's vertices do not move with a target it lacks.
    MeshData3D a = quad(1), c = quad(1);
    a.morph_targets.push_back({"up", std::vector<Vec3f>(a.vertices.size(), Vec3f{0, 1, 0}), {}});
    c.morph_targets.push_back({"left", std::vector<Vec3f>(c.vertices.size(), Vec3f{-1, 0, 0}), {}});
    c.morph_weights = {0.25f};
    a.append(c, Mat4f::scaling({2, 2, 2}));
    assert(a.validate() && a.morph_targets.size() == 2 && a.morph_weights == (std::vector<f32>{0, 0.25f}));
    assert(near(a.morph_targets[0].positions.back(), {}) && near(a.morph_targets[1].positions.front(), {}));
    assert(near(a.morph_targets[1].positions.back(), {-2, 0, 0})); // turned (scaled) with the shape

    // Weight channels: step, linear and cubic spline, held outside the keys.
    MorphChannel3D linear{.parts = {0}, .targets = 2, .times = {0, 1}, .values = {0, 1, 1, 0}};
    std::vector<f32> w;
    sample_morph_channel(linear, 0.25f, w);
    assert(w.size() == 2 && near(w[0], 0.25f) && near(w[1], 0.75f));
    sample_morph_channel(linear, 5, w);
    assert(near(w[0], 1) && near(w[1], 0));
    MorphChannel3D step = linear;
    step.interpolation = Interpolation3D::Step;
    sample_morph_channel(step, 0.9f, w);
    assert(near(w[0], 0) && near(w[1], 1));
    // A cubic spline with zero tangents: smoothstep between the values.
    MorphChannel3D cubic{.parts = {0},
                         .targets = 1,
                         .interpolation = Interpolation3D::CubicSpline,
                         .times = {0, 1},
                         .values = {0, 0, 0, 0, 1, 0}};
    sample_morph_channel(cubic, 0.5f, w);
    assert(w.size() == 1 && near(w[0], 0.5f));
    sample_morph_channel(cubic, 0.25f, w);
    assert(near(w[0], 0.25f * 0.25f * (3 - 2 * 0.25f)));
    MorphChannel3D broken = linear;
    broken.values.pop_back();
    sample_morph_channel(broken, 0.5f, w);
    assert(w.size() == 2 && w[0] == 0 && w[1] == 0);
    AnimClip3D clip;
    clip.morphs = {linear};
    assert(near(clip.key_duration(), 1));
    std::cout << "surface details: morph blending, bounds, validation, append and weight channels\n";
}

// AnimationPlayer3D blends the clips' morph weights as it blends their poses.
void player_morphs() {
    Skeleton3D sk;
    sk.names = {"root"};
    sk.parents = {-1};
    sk.rest = {JointPose3D{}};
    sk.inverse_bind = {Mat4f{}};
    const auto clip = [](const char* name, std::vector<f32> values, std::vector<f32> times = {0, 1}) {
        AnimClip3D c;
        c.name = name;
        c.channels.resize(1);
        c.morphs = {MorphChannel3D{.parts = {2}, .targets = 1, .times = std::move(times), .values = std::move(values)}};
        c.duration = c.key_duration();
        return c;
    };
    const AnimClip3D low = clip("low", {0, 0}), high = clip("high", {1, 1}), rise = clip("rise", {0, 1});
    AnimationPlayer3D player{&sk};
    player.update(0.1f);
    assert(player.morph_weights().empty()); // nothing playing moves weights
    player.play(&low);
    player.update(0.1f);
    assert(player.morph_weights().size() == 3 && player.morph_weights()[0].empty() &&
           near(player.morph_weights()[2][0], 0));
    player.crossfade_to(&high, 1.0f);
    player.update(0.5f);
    assert(near(player.morph_weights()[2][0], 0.5f, 1e-4f));
    player.update(0.6f); // the fade is over
    assert(near(player.morph_weights()[2][0], 1));
    // A layer at half weight toward 0; an additive one adds its change from its first frame.
    player.layer(0) = {.clip = &low, .weight = 0.5f};
    player.update(0.1f);
    assert(near(player.morph_weights()[2][0], 0.5f, 1e-4f));
    player.layer(0) = {.clip = &rise, .time = 0.0f, .weight = 0.5f, .loop = false, .additive = true};
    player.update(0.5f); // rise at 0.5: +0.5 from its first frame, at half weight
    assert(near(player.morph_weights()[2][0], 1.25f, 1e-4f));
    player.clear_layers();
    player.play(nullptr);
    player.update(0.1f);
    assert(player.morph_weights().empty());
    std::cout << "surface details: the player blends morph weights through fades and layers\n";
}

// MorphWeights3D's authored form and the components in the registry.
void registry() {
    const auto parsed = parse_morph_weights3d(" smile = 0.5, blink=1 ");
    assert(parsed && parsed->size() == 2 && (*parsed)[0].target == "smile" && near((*parsed)[0].weight, 0.5f) &&
           (*parsed)[1].target == "blink");
    assert(format_morph_weights3d(*parsed) == "smile=0.5, blink=1");
    assert(parse_morph_weights3d("") && parse_morph_weights3d("")->empty());
    for (const char* bad : {"smile", "smile=2", "=1", "smile=x", "smile=0.5,,blink"})
        assert(!parse_morph_weights3d(bad));
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    auto entity = world.raw().entity().set(MorphWeights3D{});
    kin::EcsEntity e{entity};
    assert(world.components().patch_field(e, "MorphWeights3D", "weights", std::string("jaw=-0.25")));
    assert(e.get<MorphWeights3D>()->weights == (std::vector<MorphWeight3D>{{"jaw", -0.25f}}));
    assert(!world.components().patch_field(e, "MorphWeights3D", "weights", std::string("jaw=3")));
    auto decal = world.raw().entity().set(DecalRenderer3D{});
    kin::EcsEntity d{decal};
    assert(world.components().patch_field(d, "DecalRenderer3D", "size", Vec3f{2, 1, 3}) &&
           world.components().patch_field(d, "DecalRenderer3D", "normal_fade", 0.5f) &&
           !world.components().patch_field(d, "DecalRenderer3D", "roughness", 2.0f));
    assert(d.get<DecalRenderer3D>()->size.z == 3 && d.get<DecalRenderer3D>()->decal.normal_fade == 0.5f);
    std::cout << "surface details: morph weights and decals in the component registry\n";
}

struct Fixture {
    Renderer2D& r;
    Renderer3D r3d{r};

    std::vector<u8> read(const View3D& view) {
        r.clear(colors::black);
        r.draw_texture(view.texture(), rect);
        std::vector<u8> p;
        Vec2i size;
        assert(r.read_rgba(rect, p, size) && size == extent);
        r.present();
        return p;
    }
    static std::array<int, 4> at(const std::vector<u8>& p, int x, int y) {
        const std::size_t i = (static_cast<std::size_t>(y) * extent.x + x) * 4;
        return {p[i], p[i + 1], p[i + 2], p[i + 3]};
    }
};

// A white wall facing the camera, lit by a light from all round only (the colour on screen is its albedo times that).
Scene3D wall_scene(Mesh3D& wall, f32 ambient) {
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 4}, {0, 0, 0}, 60);
    scene.lights.sun.color = {0, 0, 0};
    scene.lights.ambient = {ambient, ambient, ambient};
    (void)scene.draw(wall, Mat4f::identity(), Material3D{.color = Color::rgb(128, 128, 128), .roughness = 1});
    return scene;
}

void auto_exposure(Fixture& f) {
    Mesh3D wall = f.r3d.create_mesh(quad(20)).value_or({});
    assert(wall);
    const ViewOptions3D metered{.samples = 1, .hdr = true, .auto_exposure = {.enabled = true}};
    const auto shot = [&](View3D& view, Scene3D& scene) {
        assert(f.r3d.render(view, scene));
        return Fixture::at(f.read(view), 48, 48)[0];
    };
    // A dark wall and one sixteen times brighter (four stops), each in a new view (which starts at its target): the
    // metering brings both to the same grey. Without it they differ.
    Scene3D dark = wall_scene(wall, 0.05f), bright = wall_scene(wall, 0.8f);
    View3D a = f.r3d.create_view(extent, metered).value_or({}), b = f.r3d.create_view(extent, metered).value_or({});
    assert(a && b);
    const int dark_metered = shot(a, dark), bright_metered = shot(b, bright);
    assert(f.r3d.debug_stats().exposure_dispatches == 3); // a new view: cleared, then metered and adapted
    // Mid-grey through ACES and sRGB: about 142 of 255 (the histogram's bins are a quarter of a stop).
    assert(std::abs(dark_metered - bright_metered) <= 10 && std::abs(dark_metered - 142) <= 14);
    View3D plain = f.r3d.create_view(extent, {.samples = 1, .hdr = true}).value_or({});
    const int dark_plain = shot(plain, dark), bright_plain = shot(plain, bright);
    assert(bright_plain > dark_plain + 60 && f.r3d.debug_stats().exposure_dispatches == 0);

    // Adapting: view `a`, settled on the dark wall, sees the bright one. A tenth of a second later it is still
    // brighter than settled (falling at speed_down 3: 26% of the way in stops); after a few seconds it has settled.
    bright.time = 0.1f;
    const int first = shot(a, bright);
    assert(first > bright_metered + 20 && f.r3d.debug_stats().exposure_dispatches == 2);
    int last = first;
    for (int i = 2; i <= 40; ++i) {
        bright.time = 0.1f * static_cast<f32>(i);
        last = shot(a, bright);
    }
    assert(std::abs(last - bright_metered) <= 3);
    // Back into the dark: rising at speed_up 1 it is slower than falling was.
    dark.time = bright.time + 0.1f;
    const int dim = shot(a, dark);
    assert(dim < dark_metered - 20);
    // reset_history() starts again from the target.
    a.reset_history();
    dark.time += 0.1f;
    assert(std::abs(shot(a, dark) - dark_metered) <= 3);

    // Compensation: a stop brighter; a range that cannot reach the target clamps the exposure.
    View3D brighter =
        f.r3d.create_view(extent, {.samples = 1, .hdr = true, .auto_exposure = {.enabled = true, .compensation_ev = 1}})
            .value_or({});
    assert(shot(brighter, dark) > dark_metered + 25);
    View3D clamped =
        f.r3d.create_view(extent, {.samples = 1, .hdr = true, .auto_exposure = {.enabled = true, .max_ev = 0}})
            .value_or({});
    assert(std::abs(shot(clamped, dark) - dark_plain) <= 2); // the most it may expose is 2^0: as the plain view
    // Invalid options are refused.
    for (const AutoExposure3D bad :
         {AutoExposure3D{.enabled = true, .key = 0}, AutoExposure3D{.enabled = true, .min_ev = 2, .max_ev = 1},
          AutoExposure3D{.enabled = true, .low = 0.9f, .high = 0.5f}, AutoExposure3D{.enabled = true, .speed_up = -1}})
        assert(!f.r3d.create_view(extent, {.hdr = true, .auto_exposure = bad}));
    // Read back without waiting: after a few presented frames the adapted exposure is there (0.18 / 0.0108, about
    // 16.7, within a bin); a view without auto exposure has none.
    View3D readable = f.r3d.create_view(extent, metered).value_or({});
    assert(!f.r3d.exposure(readable));
    for (int i = 0; i < 4; ++i)
        (void)shot(readable, dark);
    const auto adapted = f.r3d.exposure(readable);
    assert(adapted && *adapted > 13 && *adapted < 21);
    assert(!f.r3d.exposure(plain));
    std::cout << "surface details: auto exposure meters, adapts at its speeds, resets, compensates and clamps\n";
}

// A white wall lit only by ambient light, and decals on it.
void decals(Fixture& f) {
    Mesh3D wall = f.r3d.create_mesh(quad(20)).value_or({});
    assert(wall);
    std::vector<u8> pixels(16 * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        pixels[i] = 255, pixels[i + 3] = 255;
    const Texture red = f.r.create_texture_from_rgba(pixels.data(), 4, 4);
    assert(red);
    const Quatf onto_wall = Quatf::axis_angle({1, 0, 0}, 90); // the box's +Y toward +Z: projected onto the wall
    View3D view = f.r3d.create_view(extent, {.samples = 1}).value_or({});
    assert(view);
    const auto picture = [&](std::span<const Decal3D> list, bool taa = false) {
        Scene3D scene = wall_scene(wall, 1.0f);
        scene.taa.enabled = taa;
        for (const Decal3D& d : list)
            assert(scene.draw_decal(d));
        assert(f.r3d.render(view, scene));
        return f.read(view);
    };
    const auto none = picture({});
    const auto plain = Fixture::at(none, 48, 48);
    assert(plain[0] > 100 && std::abs(plain[0] - plain[2]) <= 2);
    // A red decal one unit across at the centre: red there (the wall's light on a red surface), untouched outside.
    const Decal3D centre{.transform = Mat4f::trs({0, 0, 0}, onto_wall, {1, 1, 1}), .texture = red};
    auto p = picture(std::span{&centre, 1});
    auto c = Fixture::at(p, 48, 48);
    // (Green and blue only from the ambient light's reflection off the paint, a few per cent: lifted by the sRGB.)
    assert(c[0] > 200 && c[0] > c[1] + 120 && std::abs(c[1] - c[2]) <= 2);
    assert(Fixture::at(p, 5, 5) == Fixture::at(none, 5, 5));
    assert(f.r3d.debug_stats().decals_drawn == 1);
    // Its colour times the texture's; half alpha blends half.
    Decal3D tinted = centre;
    tinted.color = Color::rgba(255, 255, 255, 128);
    c = Fixture::at(picture(std::span{&tinted, 1}), 48, 48);
    assert(c[0] > plain[0] + 30 && c[1] < plain[1] - 15 && c[1] > 30);
    // Projected edge-on (the box's +Y along the wall): the surface faces across, so the normal fade takes it all.
    const Decal3D edge_on{.transform = Mat4f::trs({0, 0, 0}, Quatf::identity(), {1, 1, 1}), .texture = red};
    assert(Fixture::at(picture(std::span{&edge_on, 1}), 48, 48) == plain);
    // A box the wall is not in draws nothing.
    const Decal3D away{.transform = Mat4f::trs({0, 0, 2}, onto_wall, {1, 1, 1}), .texture = red};
    assert(Fixture::at(picture(std::span{&away, 1}), 48, 48) == plain);
    // The camera inside the box (its back faces are drawn then).
    const Decal3D around{.transform = Mat4f::trs({0, 0, 2}, onto_wall, {3, 10, 3}), .texture = red};
    c = Fixture::at(picture(std::span{&around, 1}), 48, 48);
    assert(c[0] > 200 && c[0] > c[1] + 120);
    // Mirrored boxes draw too (the winding turns over).
    const Decal3D mirrored{.transform = Mat4f::trs({0, 0, 0}, onto_wall, {-1, 1, 1}), .texture = red};
    c = Fixture::at(picture(std::span{&mirrored, 1}), 48, 48);
    assert(c[0] > 200 && c[0] > c[1] + 120);
    // In order: the second over the first.
    std::vector<u8> green(16 * 4);
    for (std::size_t i = 0; i < green.size(); i += 4)
        green[i + 1] = 255, green[i + 3] = 255;
    const Texture green_texture = f.r.create_texture_from_rgba(green.data(), 4, 4);
    const std::array<Decal3D, 2> both{centre, Decal3D{.transform = centre.transform, .texture = green_texture}};
    c = Fixture::at(picture(both), 48, 48);
    assert(c[1] > 200 && c[1] > c[0] + 120);
    // With temporal anti-aliasing (the pre-pass's velocity variant and a float picture): still there.
    View3D taa_view = f.r3d.create_view(extent, {.samples = 1}).value_or({});
    if (f.r3d.capabilities().temporal_aa) {
        Scene3D scene = wall_scene(wall, 1.0f);
        scene.taa.enabled = true;
        assert(scene.draw_decal(centre));
        for (int i = 0; i < 4; ++i)
            assert(f.r3d.render(taa_view, scene));
        c = Fixture::at(f.read(taa_view), 48, 48);
        assert(c[0] > 200 && c[0] > c[1] + 120);
    }
    // Refused: a flat box, values that are not finite.
    Scene3D scene;
    assert(!scene.draw_decal({.transform = Mat4f::scaling({1, 0, 1})}));
    Scene3D other;
    assert(!other.draw_decal({.roughness = std::nanf("")}));
    std::cout << "surface details: decals project, blend, fade edge-on, draw from inside, in order and with TAA\n";
}

void morphs(Fixture& f) {
    // A small unlit quad at the centre and a target that slides it right by half the view.
    Mesh3D quad_mesh = f.r3d.create_mesh(quad(0.5f, {1.5f, 0, 0})).value_or({});
    assert(quad_mesh && quad_mesh.morph_target_count() == 1 && quad_mesh.morph_target_name(0) == "slide" &&
           quad_mesh.find_morph_target("slide") == 0 && quad_mesh.find_morph_target("none") == -1);
    assert(quad_mesh.bounds().max.x >= 1.75f - 1e-5f); // room for the slide
    View3D view = f.r3d.create_view(extent, {.samples = 1}).value_or({});
    const auto picture = [&](const Mesh3D& mesh) {
        Scene3D scene;
        scene.camera = Camera3D::look_at({0, 0, 4}, {0, 0, 0}, 60);
        (void)scene.draw(mesh, Mat4f::identity(), Material3D{.unlit = true});
        assert(f.r3d.render(view, scene));
        return f.read(view);
    };
    // At rest, then slid: the white moves from the centre to the right (about 1.5 units: 1.5 / (4 tan 30) of half the
    // view, 31 pixels).
    auto p = picture(quad_mesh);
    assert(Fixture::at(p, 48, 48)[0] > 200 && Fixture::at(p, 79, 48)[0] < 20);
    const std::array<f32, 1> one{1.0f};
    assert(f.r3d.set_morph_weights(quad_mesh, one));
    assert(quad_mesh.morph_weights()[0] == 1.0f);
    p = picture(quad_mesh);
    assert(Fixture::at(p, 48, 48)[0] < 20 && Fixture::at(p, 79, 48)[0] > 200);
    // A copy has weights of its own (it starts at the mesh's).
    Mesh3D copy = f.r3d.copy_mesh(quad_mesh).value_or({});
    assert(copy && copy.morph_weights()[0] == 1.0f);
    const std::array<f32, 1> zero{0.0f};
    assert(f.r3d.set_morph_weights(copy, zero));
    assert(Fixture::at(picture(copy), 48, 48)[0] > 200);
    assert(Fixture::at(picture(quad_mesh), 48, 48)[0] < 20);
    // Refused: the wrong count, a weight outside [-1, 1] or not finite, a mesh without targets.
    const std::array<f32, 2> two{0, 0};
    const std::array<f32, 1> big{1.5f}, nan{std::nanf("")};
    assert(!f.r3d.set_morph_weights(quad_mesh, two) && !f.r3d.set_morph_weights(quad_mesh, big) &&
           !f.r3d.set_morph_weights(quad_mesh, nan));
    Mesh3D still = f.r3d.create_mesh(quad(0.5f)).value_or({});
    assert(still && still.morph_target_count() == 0 && !f.r3d.set_morph_weights(still, one) && !f.r3d.copy_mesh(still));
    // Skinned and morphed: blended, then skinned (the joint moves the slid quad up).
    MeshData3D skinned = quad(0.5f, {1.5f, 0, 0});
    skinned.skin.assign(skinned.vertices.size(), MeshSkinVertex3D{});
    skinned.morph_weights = {1.0f};
    Mesh3D both = f.r3d.create_mesh(skinned).value_or({});
    assert(both && both.skinned() && both.morph_weights()[0] == 1.0f);
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 0, 4}, {0, 0, 0}, 60);
    const std::array<Mat4f, 1> joint{Mat4f::translation({0, 1, 0})};
    assert(scene.draw_skinned(both, joint, Mat4f::identity(), Material3D{.unlit = true}));
    assert(f.r3d.render(view, scene));
    p = f.read(view);
    assert(Fixture::at(p, 79, 48)[0] < 20 && Fixture::at(p, 79, 27)[0] > 200);
    // Blended where the device blends (the GPU) as on the CPU: a lit sphere with position and normal offsets against
    // the same shape blended by blend_morph_targets, drawn as a plain mesh.
    MeshData3D ball = meshes::sphere(1.0f, 32, 16);
    MorphTarget3D bulge{"bulge", {}, {}}, tilt{"tilt", {}, {}};
    for (const MeshVertex3D& v : ball.vertices) {
        bulge.positions.push_back(v.normal * (0.3f * std::max(0.0f, v.normal.x)));
        tilt.positions.push_back({0, 0.2f * v.position.x, 0});
        tilt.normals.push_back({0.4f, 0, 0});
    }
    ball.morph_targets = {bulge, tilt};
    const std::array<f32, 2> pose{0.8f, -0.6f};
    Mesh3D blended = f.r3d.create_mesh(ball).value_or({});
    assert(blended && f.r3d.set_morph_weights(blended, pose));
    MeshData3D baked = ball;
    blend_morph_targets(ball, pose, baked.vertices);
    baked.morph_targets.clear();
    Mesh3D reference = f.r3d.create_mesh(baked).value_or({});
    const auto lit = [&](const Mesh3D& mesh) {
        Scene3D scene;
        scene.camera = Camera3D::look_at({0, 0, 4}, {0, 0, 0}, 60);
        scene.lights.sun = {.direction = {0.5f, 0.6f, 0.6f}, .color = {1, 1, 1}};
        (void)scene.draw(mesh, Mat4f::identity(), Material3D{.roughness = 0.5f});
        assert(f.r3d.render(view, scene));
        return f.read(view);
    };
    const auto a = lit(blended), b = lit(reference);
    int worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
    assert(worst <= 2);
    // With temporal anti-aliasing: the velocity pre-pass takes a changing shape's previous positions (the shape the
    // frame before drew), and its own vertices when the shape did not change; the picture follows the shape.
    if (f.r3d.capabilities().temporal_aa) {
        View3D smooth = f.r3d.create_view(extent, {.samples = 1}).value_or({});
        Mesh3D slider = f.r3d.create_mesh(quad(0.5f, {1.5f, 0, 0})).value_or({});
        const auto frame = [&](f32 weight) {
            const std::array<f32, 1> w{weight};
            assert(f.r3d.set_morph_weights(slider, w));
            Scene3D scene;
            scene.taa.enabled = true;
            scene.camera = Camera3D::look_at({0, 0, 4}, {0, 0, 0}, 60);
            (void)scene.draw(slider, Mat4f::identity(), Material3D{.unlit = true});
            assert(f.r3d.render(smooth, scene));
            const auto drawn = f.r3d.debug_stats().morph_velocity_draws;
            return std::pair{f.read(smooth), drawn};
        };
        for (int i = 1; i <= 10; ++i)
            assert(frame(0.1f * f32(i)).second == 1); // a new shape each frame
        std::pair<std::vector<u8>, u32> still;
        for (int i = 0; i < 8; ++i)
            still = frame(1.0f); // the same shape: no previous positions needed
        assert(still.second == 0);
        assert(Fixture::at(still.first, 79, 48)[0] > 200 && Fixture::at(still.first, 48, 48)[0] < 20);
    }
    // Asking for levels of detail or clusters gives a mesh without them.
    Mesh3D levels = f.r3d.create_mesh(quad(0.5f, {1, 0, 0}), MeshOptions3D{.levels = 4, .clusters = true}).value_or({});
    assert(levels && levels.level_count() == 1 && !levels.has_clusters() && levels.morph_target_count() == 1);
    std::cout << "surface details: morph weights move meshes, copies keep their own, skinned ones blend first\n";
}
} // namespace

// The ECS: a decal entity, a mesh that does not receive decals, entities with weights of their own, and an authored
// decal from a scene file.
void ecs(Fixture& f) {
    Mesh3D wall = f.r3d.create_mesh(quad(20)).value_or({});
    Mesh3D small = f.r3d.create_mesh(quad(0.3f)).value_or({});
    Mesh3D sliding = f.r3d.create_mesh(quad(0.5f, {1.5f, 0, 0})).value_or({});
    assert(wall && small && sliding);
    std::vector<u8> pixels(16 * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        pixels[i] = 255, pixels[i + 3] = 255;
    const Texture red = f.r.create_texture_from_rgba(pixels.data(), 4, 4);
    View3D view = f.r3d.create_view(extent, {.samples = 1}).value_or({});
    const auto render = [&](kin::EcsWorld& world, WorldRenderState3D& state) {
        Scene3D scene;
        scene.camera = Camera3D::look_at({0, 0, 4}, {0, 0, 0}, 60);
        scene.lights.sun.color = {0, 0, 0};
        scene.lights.ambient = {1, 1, 1};
        state.propagate_transforms();
        state.apply_morphs(f.r3d);
        state.collect(scene);
        assert(f.r3d.render(view, scene));
        return f.read(view);
    };
    const Quatf onto_wall = Quatf::axis_angle({1, 0, 0}, 90);
    for (const bool receives : {true, false}) {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        world.raw().entity().set(Transform3D{}).set(MeshRenderer3D{.mesh = wall});
        world.raw()
            .entity()
            .set(Transform3D{.position = {0, 0, 0.2f}})
            .set(MeshRenderer3D{.mesh = small,
                                .material = {.color = Color::rgb(0, 255, 0), .receives_decals = receives}});
        world.raw()
            .entity()
            .set(Transform3D{.rotation = onto_wall})
            .set(DecalRenderer3D{.decal = {.texture = red}, .size = {1, 1, 1}});
        const auto p = render(world, state);
        const auto centre = Fixture::at(p, 48, 48), beside = Fixture::at(p, 48 + 7, 48); // (the quad: 3 pixels round)
        assert(beside[0] > beside[1] + 120); // the wall under the decal: red
        if (receives)
            assert(centre[0] > centre[1] + 120); // the small quad too
        else
            assert(centre[1] > centre[0] + 120); // drawn over the decal: its own green
        assert(f.r3d.debug_stats().decals_drawn == 1);
    }
    {
        // Two entities of one model: the one with weights draws its own slid copy; the shared mesh is untouched.
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        auto model = std::make_shared<Model3D>();
        model->parts.push_back({.name = "q", .mesh = sliding});
        auto own = world.raw()
                       .entity()
                       .set(Transform3D{.position = {0, 1, 0}})
                       .set(ModelRenderer3D{.model = model})
                       .set(MorphWeights3D{.weights = {{"slide", 1.0f}}});
        world.raw().entity().set(Transform3D{.position = {0, -1, 0}}).set(ModelRenderer3D{.model = model});
        const auto p = render(world, state);
        // One unit up is 21 pixels; 1.5 right, 31.
        assert(Fixture::at(p, 79, 27)[0] > 200 && Fixture::at(p, 48, 27)[0] < 20); // slid
        assert(Fixture::at(p, 48, 69)[0] > 200 && Fixture::at(p, 79, 69)[0] < 20); // at rest
        const auto* m = own.get<MorphWeights3D>();
        assert(m->meshes.size() == 1 && m->meshes[0] && m->meshes[0].morph_weights()[0] == 1.0f &&
               sliding.morph_weights()[0] == 0.0f);
        // Back to the model's own weight once the name is gone; a copied component makes copies of its own.
        own.get_mut<MorphWeights3D>()->erase("slide");
        auto twin = world.raw().entity().set(Transform3D{.position = {0, -1, 0}}).set(*own.get<ModelRenderer3D>());
        twin.set(MorphWeights3D{*own.get<MorphWeights3D>()});
        twin.get_mut<MorphWeights3D>()->set("slide", 1.0f);
        const auto q = render(world, state);
        assert(Fixture::at(q, 48, 27)[0] > 200 && Fixture::at(q, 79, 69)[0] > 200);
        assert(twin.get<MorphWeights3D>()->meshes[0] != own.get<MorphWeights3D>()->meshes[0]);
    }
    {
        // Authored: a .kinscene names the decal's picture; instantiate_scene3d binds it.
        const auto root =
            std::filesystem::temp_directory_path() /
            ("kin-surface-details-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        Image image;
        image.size = {4, 4};
        image.rgba = pixels;
        assert(save_image(image, root / "red.png"));
        {
            std::ofstream(root / "decal.kinscene") << R"({"name":"Decal","entities":[
                {"id":"mark","components":{"Transform3D":{},
                 "DecalRenderer3D":{"texture":"red.png","size":[2,1,2],"normal_fade":0.5},
                 "MorphWeights3D":{"weights":"smile=0.5"}}}]})";
        }
        kin::EcsWorld world;
        register_builtin_editor_components(world);
        Assets3D assets{f.r3d, root};
        SceneAssetRuntime source;
        assert(source.load(root / "decal.kinscene", root, world.components()));
        assert(instantiate_scene3d(source, world, assets));
        const auto mark = source.instance().by_authored_id.at("mark");
        const auto* d = mark.get<DecalRenderer3D>();
        assert(d && d->decal.texture && d->bound_texture == "red.png" && d->size.x == 2 &&
               d->decal.normal_fade == 0.5f);
        assert(mark.get<MorphWeights3D>()->get("smile") == 0.5f);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::cout << "surface details: decal and morph components, receives_decals, per-entity copies, scene files\n";
}

int main() {
    cpu_morphs();
    player_morphs();
    registry();
    App app;
    auto& window =
        app.create_window({.title = "surface details", .width = extent.x, .height = extent.y, .hidden = true});
    Renderer2D r{make_render_backend(window, false, true)};
    Fixture f{r};
    const auto caps = f.r3d.capabilities();
    if (!caps.decals || !caps.auto_exposure || !caps.morph_targets)
        return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
    auto_exposure(f);
    decals(f);
    morphs(f);
    ecs(f);
}
