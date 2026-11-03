#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <stdexcept>

using namespace kin;

namespace {
constexpr Vec2i extent{128, 96};
constexpr Rectf rect{0, 0, 128, 96};

struct Capture {
    std::vector<u8> pixels;
    RenderDebugStats3D stats;
    u32 pick_id = 0;
};

void check(Renderer2D& renderer) {
    Renderer3D r3d{renderer};
    assert(r3d.available());
    const auto mesh = r3d.create_mesh(meshes::box({.6f, .6f, .6f})).value_or({});
    auto skin = meshes::box({.6f, .6f, .6f});
    skin.skin.resize(skin.vertices.size());
    const auto skinned = r3d.create_mesh(skin).value_or({});
    auto view = r3d.create_view(extent, {.samples = 1, .background = colors::black, .pick = true}).value_or({});
    auto second_view = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    auto third_view = r3d.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    assert(mesh && skinned && view && second_view && third_view);
    std::array<u8, 4 * 4 * 4> texels{};
    for (u32 i = 0; i < 16; ++i) {
        texels[4 * i] = texels[4 * i + 1] = texels[4 * i + 2] = 255;
        texels[4 * i + 3] = i % 2 ? 0 : 255;
    }
    const auto mask = renderer.create_texture_from_rgba(texels.data(), {4, 4});
    assert(mask);
    const std::array<Material3D, 3> materials{
        Material3D{.color = Color::rgb(220, 40, 65)},
        Material3D{.color = Color::rgb(30, 190, 95)},
        Material3D{.color = colors::white, .blend = Blend3D::Mask, .texture = mask}};
    auto explicit_batch = InstanceBatch3D::create(
        mesh, std::vector<MeshInstance3D>(16, {Mat4f::translation({-6, -3, 0})})).value_or({});
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 1, 13}, {});
    scene.camera.z_far = 60;
    scene.lights.sun.direction = normalize(Vec3f{.5f, 1, .4f});
    scene.lights.ambient = {.2f, .2f, .2f};
    scene.lights.cache_shadows = false;
    const auto populate = [&](bool changed) {
        scene.clear();
        for (u32 i = 0; i < 512; ++i) {
            const Vec3f at{f32(i % 32) * .65f - 10.f, f32(i / 32) * .65f - 5.f, changed ? -.4f : 0.f};
            assert(scene.draw(mesh, Mat4f::translation(at), materials[(i + u32(changed)) % 3], colors::white,
                              i + 1));
        }
        // Equal-depth, differently coloured surfaces must retain their source order.
        assert(scene.draw(mesh, Mat4f::scaling({2, 2, 2}), materials[0], colors::white, 900));
        assert(scene.draw(mesh, Mat4f::scaling({2, 2, 2}), materials[1], colors::white, 901));
        assert(scene.draw_instanced(explicit_batch, materials[0]));
        const std::array pose{Mat4f::identity()};
        assert(scene.draw_skinned(skinned, pose, Mat4f::translation({0, 3, 1}), materials[0]));
        assert(scene.draw(mesh, Mat4f::translation({0, 0, 2}),
                          {.color = Color::rgba(40, 80, 220, 90), .blend = Blend3D::Alpha}));
        scene.lights.spots.push_back({.position = {2, 4, 8},
                                      .direction = normalize(Vec3f{-2, -4, -8}),
                                      .color = {1, .8f, .5f},
                                      .radius = 30,
                                      .shadow_enabled = true});
        scene.lights.max_shadowed_spots = 1;
        scene.lights.local_shadow_resolution = 128;
    };
    const auto shoot = [&](bool retained) {
        scene.debug.retain_scene_instances = retained;
        renderer.clear(colors::black);
        assert(r3d.render(view, scene));
        Capture result{{}, r3d.debug_stats()};
        renderer.draw_texture(view.texture(), rect);
        Vec2i size;
        assert(renderer.read_rgba(rect, result.pixels, size) && size == extent);
        for (u32 p = 0; p < result.pixels.size() / 4; ++p)
            if (result.pixels[4 * p] || result.pixels[4 * p + 1] || result.pixels[4 * p + 2]) {
                const auto hit = r3d.pick(view, {i32(p % extent.x), i32(p / extent.x)});
                assert(hit);
                result.pick_id = hit->id;
                break;
            }
        renderer.present();
        return result;
    };
    const auto compare = [&]() {
        const auto reference = shoot(false);
        const auto actual = shoot(true);
        assert(actual.pixels == reference.pixels);
        assert(actual.pick_id == reference.pick_id);
        assert(actual.stats.draws == reference.stats.draws && actual.stats.instances == reference.stats.instances &&
               actual.stats.triangles == reference.stats.triangles &&
               actual.stats.shadow_casters == reference.stats.shadow_casters &&
               actual.stats.local_shadow_casters == reference.stats.local_shadow_casters);
        assert(actual.stats.direct_retained_instances == 514 && actual.stats.device_waits == 0);
        return actual;
    };

    populate(false);
    shoot(true); // establish the source revision before admission
    const auto initial = compare();
    assert(initial.stats.direct_upload_bytes > 0 && initial.stats.direct_bounds_rebuilt == 514 && initial.pick_id > 0);
    auto stable = compare();
    assert(stable.stats.direct_cache_hits == 1 && stable.stats.direct_upload_bytes == 0 &&
           stable.stats.direct_bounds_rebuilt == 0 && stable.stats.bounds_instance_tests == 2);
    for (u32 cascades : {1u, 4u}) {
        scene.lights.shadow_area = {{-12, -7, -3}, {12, 7, 4}};
        scene.lights.shadow_resolution = 128;
        scene.lights.shadow_cascades = cascades;
        scene.camera = Camera3D::look_at({2, 2, 12}, {0, 0, 0});
        stable = compare();
        assert(stable.stats.direct_upload_bytes == 0 && stable.stats.direct_bounds_rebuilt == 0);
    }
    // Pose-only updates do not repack static records or their bounds.
    const std::array moved_pose{Mat4f::translation({.5f, 0, 0})};
    assert(scene.update_joint_matrices(0, moved_pose));
    stable = compare();
    assert(stable.stats.direct_upload_bytes == 0 && stable.stats.direct_bounds_rebuilt == 0);
    scene.ambient_occlusion.enabled = true;
    scene.contact_shadows.enabled = true;
    compare();
    scene.ambient_occlusion.enabled = scene.contact_shadows.enabled = false;
    // Reusing source capacity after clear must still replace both records and bounds.
    populate(true);
    const auto edited = compare();
    assert(edited.stats.direct_upload_bytes > 0 && edited.stats.direct_bounds_rebuilt == 514);
    assert(edited.pixels != initial.pixels);
    scene.lod.enabled = true;
    assert(shoot(true).stats.direct_retained_instances == 0);
    scene.lod.enabled = false;
    compare();
    scene.taa.enabled = true;
    assert(shoot(true).stats.direct_retained_instances == 0);
    scene.taa.enabled = false;

    // Old add_instances spans may change without modified_instances(). They
    // remain live reads, including after an explicit preparation notification.
    auto exposed = scene.add_instances(mesh, 1, materials[1]).value_or({});
    assert(exposed.size() == 1);
    exposed[0] = {Mat4f::translation({0, 0, 3}), colors::white, 999};
    const auto raw_first = shoot(true);
    assert(raw_first.stats.direct_retained_instances == 0);
    exposed[0].transform = Mat4f::translation({5, 0, 3});
    scene.modified_instances();
    const auto raw_second = shoot(true);
    assert(raw_second.stats.direct_retained_instances == 0 && raw_first.pixels != raw_second.pixels);
    assert(raw_second.pixels == shoot(false).pixels);
    populate(false);
    shoot(false);
    compare();

    // A second scene table uses independent source rows and storage bindings.
    auto& overlay = scene.overlay();
    overlay.camera = scene.camera;
    for (u32 i = 0; i < 512; ++i)
        assert(overlay.draw(mesh, Mat4f::translation({f32(i % 32) * .6f - 9.f, f32(i / 32) * .6f - 4.f, 0}),
                            materials[i % 2]));
    shoot(true);
    const auto overlay_reference = shoot(false);
    const auto overlay_cached = shoot(true);
    assert(overlay_cached.pixels == overlay_reference.pixels && overlay_cached.stats.direct_retained_instances == 1026);
    overlay.clear();
    compare();

    // Moving invalidates the old weak identity even while its token is held.
    auto old_identity = scene.draw_lifetime().lock();
    const auto old_id = scene.draw_identity();
    Scene3D moved = std::move(scene);
    assert(old_identity && *old_identity == 0 && moved.draw_identity() != old_id);
    scene = std::move(moved);
    shoot(true);
    compare();
    assert(r3d.debug_stats().retained_gpu_bytes < 2 * 1024 * 1024);

    // A later upload in the same GPU frame must preserve the earlier view.
    const auto before_edit = shoot(false);
    scene.debug.retain_scene_instances = true;
    renderer.clear(colors::black);
    assert(r3d.render(view, scene));
    populate(true);
    assert(r3d.render(second_view, scene));
    populate(false);
    assert(r3d.render(third_view, scene));
    assert(r3d.debug_stats().direct_retained_instances == 0); // a second edit in this GPU frame streams
    const auto read_view = [&](const View3D& target) {
        renderer.clear(colors::black);
        renderer.draw_texture(target.texture(), rect);
        std::vector<u8> pixels;
        Vec2i size;
        assert(renderer.read_rgba(rect, pixels, size) && size == extent);
        return pixels;
    };
    const auto after_edit = read_view(second_view);
    assert(read_view(view) == before_edit.pixels);
    assert(read_view(third_view) == before_edit.pixels);
    renderer.present();
    populate(true);
    assert(after_edit == shoot(false).pixels);

    // Oversized direct tables stream within the existing 32 MiB budget.
    scene = Scene3D{};
    scene.camera = Camera3D::look_at({0, 0, 10}, {});
    std::vector<MeshInstance3D> outside(8193, {Mat4f::translation({1000, 0, 0})});
    outside[0] = {Mat4f::identity(), colors::white, 1234};
    for (u32 i = 0; i < 32; ++i)
        assert(scene.draw_instanced(mesh, outside, materials[i % 2]));
    const auto oversized_reference = shoot(true);
    const auto oversized = shoot(true);
    assert(oversized.stats.direct_retained_instances == 0 && oversized.stats.retained_refused == 1 &&
           oversized.stats.retained_gpu_bytes <= 32 * 1024 * 1024);
    assert(oversized.pixels == oversized_reference.pixels && oversized.pick_id == 1234 &&
           oversized_reference.pick_id == 1234);
    std::cout << "direct scene cache: exact pixels/order, mixed records, camera/cascades, pose edits, raw spans, "
                 "move identity, same-frame views, budget fallback and warmed zero uploads/bounds passed\n";
}
} // namespace

int main() {
    const char* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
    bool ready = false;
    try {
        App app;
        auto& window = app.create_window({.title = "direct scene cache", .width = extent.x,
                                          .height = extent.y, .hidden = true});
        auto backend = make_render_backend(window, false, true);
        if (!backend || backend->name() != "SDL_GPU")
            throw std::runtime_error("SDL_GPU unavailable");
        Renderer2D renderer{std::move(backend)};
        ready = true;
        check(renderer);
        return 0;
    } catch (const std::exception& error) {
        const bool failed = ready || (required && required[0] && required[0] != '0');
        std::cerr << (failed ? "[FAIL] " : "[SKIP] ") << error.what() << '\n';
        return failed ? 1 : 77;
    }
}
