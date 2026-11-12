#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/backend.hpp>
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
        Material3D{.color = Color::rgb(220, 40, 65)}, Material3D{.color = Color::rgb(30, 190, 95)},
        Material3D{.color = colors::white, .blend = Blend3D::Mask, .texture = mask}};
    auto explicit_batch =
        InstanceBatch3D::create(mesh, std::vector<MeshInstance3D>(16, {Mat4f::translation({-6, -3, 0})})).value_or({});
    Scene3D scene;
    scene.debug.cache_draw_commands = false; // isolate instance/local selection caches in this fixture
    scene.camera = Camera3D::look_at({0, 1, 13}, {});
    scene.camera.z_far = 60;
    scene.lights.sun.direction = normalize(Vec3f{.5f, 1, .4f});
    scene.lights.ambient = {.2f, .2f, .2f};
    scene.lights.cache_shadows = false;
    const auto populate = [&](bool changed) {
        scene.clear();
        for (u32 i = 0; i < 512; ++i) {
            if (i == 128 || i == 384)
                assert(scene.draw_instanced(explicit_batch, materials[0]));
            const Vec3f at{f32(i % 32) * .65f - 10.f, f32(i / 32) * .65f - 5.f, changed ? -.4f : 0.f};
            assert(scene.draw(mesh, Mat4f::translation(at), materials[(i + u32(changed)) % 3], colors::white, i + 1));
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
               actual.stats.local_shadow_casters == reference.stats.local_shadow_casters &&
               actual.stats.local_shadow_triangles == reference.stats.local_shadow_triangles &&
               actual.stats.local_shadow_draws == reference.stats.local_shadow_draws);
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
    stable = compare();
    assert(stable.stats.local_gather_cache_hits == 1 && stable.stats.local_gather_cached_copies > 0 &&
           stable.stats.local_gather_cached_tests > 0 && stable.stats.local_sort_cache_hits > 0 &&
           stable.stats.local_gather_cache_bytes <= 4 * 1024 * 1024);
    // Dynamic refs occur between static rows. They may leave/re-enter a map
    // without invalidating the static selection or reusing stale sort keys.
    for (Vec3f at : {Vec3f{1000, 0, 0}, Vec3f{0, 0, 3}, Vec3f{-6, -3, 0}}) {
        const std::vector<MeshInstance3D> copies(16, {Mat4f::translation(at)});
        assert(explicit_batch.update(0, copies));
        stable = compare();
        assert(stable.stats.local_gather_cache_hits == 1 && stable.stats.direct_bounds_rebuilt == 0);
    }
    // A prior batch slot is only a hint: one shared batch can use different
    // colour tables, with its bounds moving between those tables each render.
    u32 batch_draw = 0;
    while (!scene.draws()[batch_draw].batch)
        ++batch_draw;
    for (Color color : {materials[1].color, colors::white, materials[0].color}) {
        Material3D edited = materials[0];
        edited.color = color;
        assert(scene.set_draw_material(batch_draw, edited));
        compare();
        stable = compare();
        assert(stable.stats.bounds_cache_rebuilt == 0 && stable.stats.direct_upload_bytes == 0);
    }
    // Geometry keys include the actual light projection and reach, independently
    // of the source's draw revision or camera.
    // Bulk resident metadata must keep mixed caster flags current, and a
    // single fresh view still needs static pieces while another view hits.
    auto second_spot = scene.lights.spots.front();
    second_spot.position.x = -3;
    second_spot.direction = normalize(Vec3f{3, -4, -8});
    scene.lights.spots.push_back(second_spot);
    scene.lights.max_shadowed_spots = 2;
    std::array<Material3D, 8> original_casters;
    for (u32 i = 0; i < original_casters.size(); ++i)
        original_casters[i] = scene.draws()[37 + i].material;
    for (bool casts : {false, true}) {
        for (u32 i = 0; i < original_casters.size(); ++i) {
            Material3D edited = original_casters[i];
            edited.casts_shadow = casts;
            assert(scene.set_draw_material(37 + i, edited));
        }
        compare();
        compare();
        stable = compare();
        assert(stable.stats.local_gather_cache_hits == 2 && stable.stats.direct_bounds_rebuilt == 0);
    }
    scene.lights.spots[1].position.x -= 1;
    stable = compare();
    assert(stable.stats.local_gather_cache_hits == 1 && stable.stats.local_gather_cache_misses == 1);
    scene.lights.spots.pop_back();
    scene.lights.max_shadowed_spots = 1;
    // Entirely visible/hidden camera decisions must not change the flat-row
    // mapping or retain stale scratch when local views leave and re-enter.
    const Camera3D original_camera = scene.camera;
    scene.camera = Camera3D::look_at({0, 0, 13}, {});
    scene.camera.projection = Projection3D::Orthographic;
    scene.camera.height = 32;
    scene.camera.z_far = 60;
    const auto all_seen = compare();
    assert(all_seen.stats.culled == 0);
    scene.camera = Camera3D::look_at({0, 0, 13}, {0, 0, 14});
    scene.camera.z_far = 60;
    const auto hidden = compare();
    assert(hidden.stats.instances == 0 && hidden.pick_id == 0);
    // Repeated empty output rows can retain unused offsets, but count/shape
    // changes must initialize them before any later view makes them visible.
    const f32 sun_strength = scene.lights.sun_shadow_strength;
    for (u32 cascades : {0u, 0u, 1u, 4u, 0u, 0u}) {
        scene.lights.sun_shadow_strength = cascades ? sun_strength : 0.f;
        scene.lights.shadow_cascades = std::max(cascades, 1u);
        scene.lights.shadow_resolution = 128;
        scene.camera = Camera3D::look_at({0, 0, 13}, {0, 0, 14});
        const auto empty = shoot(true);
        const auto still_empty = shoot(true);
        assert(empty.stats.instances == 0 && still_empty.stats.instances == 0 && empty.pixels == still_empty.pixels &&
               still_empty.pick_id == 0);
        scene.camera = original_camera;
        const auto exposed_again = compare();
        assert(exposed_again.stats.instances > 0 && exposed_again.pick_id > 0);
        scene.cull = false;
        assert(compare().stats.culled == 0);
        scene.cull = true;
    }
    scene.lights.sun_shadow_strength = sun_strength;
    scene.lights.shadow_cascades = 1;
    scene.camera = original_camera;
    compare();
    const auto original_spot = scene.lights.spots.front();
    for (u32 edit = 0; edit < 4; ++edit) {
        auto& spot = scene.lights.spots.front();
        if (edit == 0)
            spot.position.x += 2;
        if (edit == 1)
            spot.radius = 12;
        if (edit == 2)
            spot.shadow_near_plane = .4f;
        if (edit == 3)
            spot.direction = normalize(Vec3f{-1, -2, -8});
        const auto changed = compare();
        assert(changed.stats.local_gather_cache_hits == 0 && changed.stats.local_gather_cache_misses == 1);
        compare(); // admit the unchanged projection
        assert(compare().stats.local_gather_cache_hits == 1);
    }
    scene.lights.spots.front() = original_spot;
    compare();
    compare();
    // Budget exhaustion and a disabled cache use the live path with the same
    // records. Restoring the budget admits the source again.
    scene.debug.local_shadow_gather_budget = 1;
    auto fallback = compare();
    assert(fallback.stats.local_gather_cache_hits == 0 && fallback.stats.local_gather_cache_bytes <= 1);
    scene.debug.local_shadow_gather_budget = 4 * 1024 * 1024;
    compare();
    assert(compare().stats.local_gather_cache_hits == 1);
    scene.debug.cache_local_shadow_gather = false;
    fallback = compare();
    assert(fallback.stats.local_gather_cache_hits == 0 && fallback.stats.local_gather_cache_bytes == 0);
    scene.debug.cache_local_shadow_gather = true;
    compare();
    assert(compare().stats.local_gather_cache_hits == 1);
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
    assert(stable.stats.direct_upload_bytes == 0 && stable.stats.direct_bounds_rebuilt == 0 &&
           stable.stats.local_gather_cache_hits == 1);
    // Texture content is live: selection may hit while the whole-map cache
    // must redraw a mask changed in place.
    scene.lights.cache_shadows = true;
    shoot(true);
    assert(shoot(true).stats.local_shadow_cached > 0);
    for (u32 i = 0; i < 16; ++i)
        texels[4 * i + 3] = 255;
    assert(renderer.update_texture(mask, {}, {4, 4}, texels.data()));
    const auto mask_changed = shoot(true);
    assert(mask_changed.stats.local_gather_cache_hits == 1 && mask_changed.stats.local_shadow_drawn > 0);
    scene.lights.cache_shadows = false;
    assert(mask_changed.pixels == shoot(false).pixels);
    compare();
    // Cube faces have independent slots; camera-dependent wanted faces can
    // change without treating a previous camera's selection as a new light.
    scene.lights.points.push_back({.position = {0, 0, 4}, .radius = 30, .shadow_enabled = true});
    scene.lights.max_shadowed_points = 1;
    compare();
    compare();
    assert(compare().stats.local_gather_cache_hits > 1);
    for (Vec3f eye : {Vec3f{0, 0, -13}, Vec3f{12, 2, 0}, Vec3f{2, 2, 12}}) {
        scene.camera = Camera3D::look_at(eye, {});
        compare();
        compare();
        assert(compare().stats.local_gather_cache_hits > 1);
    }
    scene.lights.points.clear();
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
    shoot(true); // both retained sources and the main scene's selection program are warm
    const auto overlay_reference = shoot(false);
    const auto overlay_cached = shoot(true);
    assert(overlay_cached.pixels == overlay_reference.pixels &&
           overlay_cached.stats.direct_retained_instances == 1026 &&
           overlay_cached.stats.local_gather_cache_hits == 1 &&
           overlay_cached.stats.local_shadow_draws == overlay_reference.stats.local_shadow_draws &&
           overlay_cached.stats.local_shadow_casters == overlay_reference.stats.local_shadow_casters &&
           overlay_cached.stats.local_shadow_triangles == overlay_reference.stats.local_shadow_triangles);
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

    // With no dynamic casters, a hit produces no gather tasks at all. Empty
    // selections must also be cached, without inventing a row or copy span.
    scene = Scene3D{};
    scene.debug.cache_draw_commands = false;
    scene.camera = Camera3D::look_at({0, 0, 13}, {});
    scene.lights.cache_shadows = false;
    scene.lights.spots.push_back(original_spot);
    scene.lights.max_shadowed_spots = 1;
    scene.lights.local_shadow_resolution = 128;
    const std::vector<MeshInstance3D> stationary(16, {Mat4f::identity()});
    for (u32 i = 0; i < 32; ++i)
        assert(scene.draw_instanced(mesh, stationary, materials[i % 3]));
    const auto only_static_reference = shoot(false);
    for (u32 i = 0; i < 3; ++i)
        shoot(true);
    const auto only_static = shoot(true);
    assert(only_static.pixels == only_static_reference.pixels && only_static.stats.local_gather_cache_hits == 1 &&
           only_static.stats.local_instance_tests == 0 && only_static.stats.local_task_bytes == 0 &&
           only_static.stats.local_shadow_casters == only_static_reference.stats.local_shadow_casters);
    // Resident accounting and budget changes also apply when no light maps
    // are selected, without waiting for another local-shadow render.
    scene.lights.spots.clear();
    assert(only_static.stats.local_gather_cache_bytes > 0 &&
           shoot(true).stats.local_gather_cache_bytes == only_static.stats.local_gather_cache_bytes);
    scene.debug.local_shadow_gather_budget = 0;
    assert(shoot(true).stats.local_gather_cache_bytes == 0);
    scene.debug.local_shadow_gather_budget = 4 * 1024 * 1024;
    scene.lights.spots.push_back(original_spot);
    scene.lights.spots.front().direction = {0, 0, 1};
    const auto empty_reference = shoot(false);
    for (u32 i = 0; i < 2; ++i)
        shoot(true);
    const auto empty = shoot(true);
    assert(empty.pixels == empty_reference.pixels && empty.stats.local_gather_cache_hits == 1 &&
           empty.stats.local_gather_cached_copies == 0 && empty.stats.local_shadow_casters == 0 &&
           empty.stats.local_instance_tests == 0 && empty.stats.local_task_bytes == 0);

    // Explicit batches must not permanently starve a larger static source of
    // its table (and therefore its local-shadow gather cache) at the slot cap.
    scene = Scene3D{};
    scene.debug.cache_draw_commands = false;
    scene.camera = Camera3D::look_at({0, 0, 13}, {});
    scene.lights.cache_shadows = false;
    scene.lights.spots.push_back(original_spot);
    scene.lights.max_shadowed_spots = 1;
    scene.lights.local_shadow_resolution = 128;
    std::vector<InstanceBatch3D> crowded;
    for (u32 i = 0; i < 128; ++i) {
        crowded.push_back(InstanceBatch3D::create(mesh, stationary).value_or({}));
        assert(crowded.back() && scene.draw_instanced(crowded.back(), materials[i % 3]));
    }
    for (u32 i = 0; i < 32; ++i)
        assert(scene.draw_instanced(mesh, stationary, materials[i % 3]));
    const auto crowded_reference = shoot(false);
    for (u32 i = 0; i < 5; ++i)
        shoot(true);
    const auto crowded_cached = shoot(true);
    assert(crowded_cached.stats.direct_retained_instances == 512 && crowded_cached.stats.direct_cache_hits == 1 &&
           crowded_cached.stats.local_gather_cache_hits == 1 && crowded_cached.stats.retained_refused > 0 &&
           crowded_cached.stats.retained_gpu_bytes <= 32 * 1024 * 1024 && crowded_cached.stats.device_waits == 0);
    assert(crowded_cached.pixels == crowded_reference.pixels && crowded_cached.pick_id == crowded_reference.pick_id &&
           crowded_cached.stats.draws == crowded_reference.stats.draws &&
           crowded_cached.stats.instances == crowded_reference.stats.instances &&
           crowded_cached.stats.local_shadow_draws == crowded_reference.stats.local_shadow_draws &&
           crowded_cached.stats.local_shadow_casters == crowded_reference.stats.local_shadow_casters &&
           crowded_cached.stats.local_shadow_triangles == crowded_reference.stats.local_shadow_triangles);
    // Both a streamed and a retained dynamic batch stay live under pressure.
    for (u32 index : {0u, 127u}) {
        const std::vector<MeshInstance3D> copies(16, {Mat4f::translation({f32(index % 3), 0, 2})});
        assert(crowded[index].update(0, copies));
        const auto updated = shoot(true);
        const auto reference = shoot(false);
        assert(updated.pixels == reference.pixels && updated.pick_id == reference.pick_id &&
               updated.stats.local_shadow_casters == reference.stats.local_shadow_casters &&
               updated.stats.local_shadow_triangles == reference.stats.local_shadow_triangles);
    }

    // Structural layout reuse does not prove material eligibility. Recompile
    // resident runs after command revisions, including streamed/transparent
    // interruptions inside a previously resident run.
    scene = Scene3D{};
    scene.debug.cache_draw_commands = false;
    scene.camera = Camera3D::look_at({0, 0, 13}, {});
    for (u32 i = 0; i < 1031; ++i) {
        const Vec3f at{f32(i % 41) * .25f - 5, f32(i / 41) * .25f - 3, 0};
        const std::array copy{MeshInstance3D{Mat4f::translation(at), colors::white, i + 1}};
        assert(scene.draw_instanced(mesh, copy, materials[i % 3]));
    }
    assert(scene.draws().size() == 1031);
    for (u32 edit = 0; edit < 3; ++edit) {
        if (edit) {
            Material3D alpha = materials[257 % 3], edge = materials[511 % 3];
            alpha.blend = edit == 1 ? Blend3D::Alpha : Blend3D::Additive;
            alpha.color.a = 137;
            edge.wireframe = edit == 1;
            assert(scene.set_draw_material(257, alpha));
            assert(scene.set_draw_material(511, edge));
        }
        const auto layout = shoot(true);
        if (edit)
            assert(layout.stats.prepared_reused == 1);
        for (u32 warmup = 0; warmup < 3; ++warmup)
            shoot(true);
        const auto actual = shoot(true);
        const auto reference = shoot(false);
        assert(actual.pixels == reference.pixels && actual.pick_id == reference.pick_id &&
               actual.stats.instances == reference.stats.instances &&
               actual.stats.triangles == reference.stats.triangles && actual.stats.culled == reference.stats.culled &&
               actual.stats.camera_instance_tests == reference.stats.camera_instance_tests &&
               actual.stats.direct_retained_instances == 1031 - (edit == 1   ? 2
                                                                 : edit == 2 ? 1
                                                                             : 0));
    }

    // A persistent source beyond the structural-descriptor cap can still reuse
    // its revision-proven layout, without allocating more descriptor payload.
    scene = Scene3D{};
    scene.debug.cache_draw_commands = false;
    scene.camera = Camera3D::look_at({0, 0, 13}, {});
    const auto large_source = [&](bool changed) {
        scene.clear();
        for (u32 i = 0; i < 8193; ++i)
            assert(scene.draw(
                mesh, Mat4f::translation({f32(i % 96) * .4f - 19, f32(i / 96) * .4f - 17, changed ? -.5f : 0.f}),
                materials[(i + u32(changed)) % 3], colors::white, i + 1));
        const std::array pose{Mat4f::identity()};
        assert(scene.draw_skinned(skinned, pose, Mat4f::translation({0, 0, 2}), materials[0]));
    };
    const auto fresh_layout = [&] {
        Scene3D moved = std::move(scene);
        scene = std::move(moved);
        const auto result = shoot(false);
        assert(result.stats.prepared_reused == 0);
        return result;
    };
    large_source(false);
    const auto large_reference = shoot(false);
    const auto large_cached = shoot(false);
    assert(large_reference.stats.prepared_reused == 0 && large_cached.stats.prepared_reused == 1 &&
           large_cached.stats.prepared_draws > 8192 && large_cached.stats.prepared_cache_bytes < 4096 &&
           large_cached.stats.prepared_layout_bytes <= 16 * 1024 * 1024);
    assert(large_cached.pixels == large_reference.pixels && large_cached.pick_id == large_reference.pick_id &&
           large_cached.stats.instances == large_reference.stats.instances &&
           large_cached.stats.triangles == large_reference.stats.triangles);
    scene.camera = Camera3D::look_at({2, 1, 13}, {});
    assert(scene.update_joint_matrices(0, std::array{Mat4f::translation({.5f, 0, 0})}));
    const auto posed = shoot(false);
    assert(posed.stats.prepared_reused == 1 && posed.pixels == fresh_layout().pixels);
    // Clear/resubmit into the same storage must rebuild without descriptors.
    large_source(true);
    assert(shoot(false).stats.prepared_reused == 0);
    const auto resubmitted = shoot(false);
    assert(resubmitted.stats.prepared_reused == 1 && resubmitted.pixels != large_reference.pixels &&
           resubmitted.pixels == fresh_layout().pixels);
    // Unannounced writes through a valid exposed span still refresh content.
    auto large_span = scene.add_instances(mesh, 1, materials[1]).value_or({});
    assert(large_span.size() == 1);
    large_span[0] = {Mat4f::translation({0, 0, 3}), colors::white, 9000};
    shoot(false);
    assert(shoot(false).stats.prepared_reused == 1);
    large_span[0].transform = Mat4f::translation({3, 0, 3});
    const auto raw_large = shoot(false);
    assert(raw_large.stats.prepared_reused == 1 && raw_large.pixels == fresh_layout().pixels);
    // The existing piece/layout budgets still force a live rebuild beyond them.
    for (u32 i = 0; i < 8193; ++i)
        assert(scene.draw(mesh, Mat4f::translation({1000, 0, 0}), materials[i % 3]));
    for (u32 i = 0; i < 2; ++i) {
        const auto oversized_layout = shoot(false);
        assert(oversized_layout.stats.prepared_draws > 16384 && oversized_layout.stats.prepared_reused == 0 &&
               oversized_layout.pixels == raw_large.pixels);
    }

    // Multiple shadow-order windows mix multi-copy static rows, masked state
    // and live batch rows around window boundaries. Views can be cached or
    // redrawn independently, with the same union slots and final pixels.
    scene = Scene3D{};
    scene.debug.cache_draw_commands = false;
    scene.camera = Camera3D::look_at({0, 0, 13}, {});
    auto mixed_batch = InstanceBatch3D::create(mesh, stationary).value_or({});
    assert(mixed_batch);
    for (u32 row = 0; row < 1025; ++row) {
        std::vector<MeshInstance3D> copies;
        for (u32 i = 0; i < 2; ++i) {
            const u32 n = (row * 2 + i) % 512;
            copies.push_back(
                {Mat4f::translation({f32(n % 32) * .65f - 10, f32(n / 32) * .65f - 5, 0}), colors::white, n + 1});
        }
        auto material = materials[row % 3];
        material.double_sided = row % 5 == 0;
        // Keep cut-outs as distinct depth commands so ordinary two-buffer
        // records interleave with the general recorder across many entries.
        if (material.blend == Blend3D::Mask)
            material.uv_offset = {.13f, .27f};
        assert(scene.draw_instanced(mesh, copies, material));
        if (row == 63 || row == 127 || row == 128 || row == 255) {
            assert(scene.draw_instanced(mixed_batch, materials[0]));
            const std::array pose{Mat4f::identity()};
            assert(scene.draw_skinned(skinned, pose, Mat4f::translation({0, 3, 1}), materials[0]));
        }
    }
    for (f32 x : {-6.f, 6.f})
        scene.lights.spots.push_back(
            {.position = {x, 4, 6}, .direction = normalize(Vec3f{0, -4, -6}), .radius = 30, .shadow_enabled = true});
    scene.lights.max_shadowed_spots = 2;
    scene.lights.local_shadow_resolution = 128;
    const auto move_mixed = [&](f32 x) {
        std::vector<MeshInstance3D> copies;
        for (u32 i = 0; i < 16; ++i)
            copies.push_back({Mat4f::translation({x + f32(i) * .01f, 0, 2}), colors::white, 2000 + i});
        assert(mixed_batch.update(0, copies));
    };
    move_mixed(-7.8f);
    for (f32 x : {-7.5f, 7.5f, 7.8f}) {
        scene.lights.cache_shadows = true;
        // Admission, static selection and whole-map reuse warm independently.
        for (u32 warmup = 0; warmup < 4; ++warmup)
            shoot(true);
        move_mixed(x);
        const auto cached = shoot(true);
        if (x != 7.5f)
            assert(cached.stats.local_shadow_cached > 0 && cached.stats.local_shadow_drawn > 0);
        scene.lights.cache_shadows = false;
        const auto fresh = shoot(false);
        if (cached.pixels != fresh.pixels || cached.pick_id != fresh.pick_id ||
            cached.stats.local_gather_cache_hits != 2 || cached.stats.local_shadow_maps != 2)
            std::cerr << "mixed cached rows x=" << x << " pixels_equal=" << (cached.pixels == fresh.pixels)
                      << " picks=" << cached.pick_id << '/' << fresh.pick_id
                      << " gather_hits=" << cached.stats.local_gather_cache_hits
                      << " maps=" << cached.stats.local_shadow_maps << '\n';
        assert(cached.pixels == fresh.pixels && cached.pick_id == fresh.pick_id &&
               cached.stats.local_gather_cache_hits == 2 && cached.stats.local_shadow_maps == 2);
    }

    // Immutable cached spans and fresh selections feed the same window stream.
    // Mix a deferred view with a fresh view in both orders; their separate
    // selection/output storage must preserve unread live multi-copy spans.
    scene.lights.cache_shadows = false;
    for (u32 edit = 0; edit < 3; ++edit) {
        for (u32 warmup = 0; warmup < 4; ++warmup)
            shoot(true);
        if (edit)
            scene.lights.spots[edit - 1].shadow_near_plane = .3f + f32(edit) * .1f;
        move_mixed(-7.7f + f32(edit) * 7.7f);
        const auto cached = shoot(true);
        const auto fresh = shoot(false);
        if (!edit)
            assert(cached.stats.local_sort_cache_hits + cached.stats.local_sort_cache_misses > 2);
        assert(cached.pixels == fresh.pixels && cached.pick_id == fresh.pick_id &&
               cached.stats.local_gather_cache_hits == (edit ? 1 : 2) && cached.stats.local_shadow_cached == 0 &&
               cached.stats.local_shadow_drawn == 2 &&
               cached.stats.local_shadow_maps == fresh.stats.local_shadow_maps &&
               cached.stats.local_shadow_draws == fresh.stats.local_shadow_draws &&
               cached.stats.local_shadow_triangles == fresh.stats.local_shadow_triangles &&
               cached.stats.local_shadow_casters == fresh.stats.local_shadow_casters);
    }

    // A changed earlier projection changes the first selected representatives
    // used to order a later, unchanged map. Its compiled static windows must
    // validate those leaders; moving live rows also changes the static prefix.
    for (f32 x : {6.f, -6.f}) {
        move_mixed(-7.7f);
        for (u32 warmup = 0; warmup < 4; ++warmup)
            shoot(true);
        scene.lights.spots[0].position.x = x;
        const auto cached = shoot(true);
        const auto fresh = shoot(false);
        assert(cached.pixels == fresh.pixels && cached.pick_id == fresh.pick_id &&
               cached.stats.local_gather_cache_hits == 1 &&
               cached.stats.local_shadow_draws == fresh.stats.local_shadow_draws &&
               cached.stats.local_shadow_triangles == fresh.stats.local_shadow_triangles &&
               cached.stats.local_shadow_casters == fresh.stats.local_shadow_casters &&
               cached.stats.local_gather_cache_bytes <= 4 * 1024 * 1024);
    }
    // Cube faces leave holes in wanted-view indices. Dense view packets must
    // map back to those exact faces while cached/static and live rows interleave.
    scene.lights.spots.clear();
    scene.lights.max_shadowed_spots = 0;
    scene.lights.max_shadowed_points = 2;
    for (f32 x : {-6.f, 6.f})
        scene.lights.points.push_back(
            {.position = {x, 3, 5}, .color = {4, 3, 2}, .radius = 30, .shadow_enabled = true});
    for (f32 x : {-7.f, 0.f, 7.f}) {
        scene.camera = Camera3D::look_at({x * .2f, 0, 13}, {});
        move_mixed(x);
        for (u32 warmup = 0; warmup < 4; ++warmup)
            shoot(true);
        const auto cached = shoot(true);
        const auto fresh = shoot(false);
        assert(cached.stats.local_shadow_maps >= 2 &&
               cached.stats.local_gather_cache_hits == cached.stats.local_shadow_maps &&
               cached.pixels == fresh.pixels && cached.pick_id == fresh.pick_id &&
               cached.stats.local_shadow_draws == fresh.stats.local_shadow_draws &&
               cached.stats.local_shadow_triangles == fresh.stats.local_shadow_triangles &&
               cached.stats.local_shadow_casters == fresh.stats.local_shadow_casters);
    }
    scene.debug.local_shadow_gather_budget = 0;
    const auto no_program = shoot(true);
    assert(no_program.stats.local_gather_cache_bytes == 0 && no_program.pixels == shoot(false).pixels);

    // Adjacent independent draws cross task boundaries and leave short tails.
    // Camera-edge decisions and interruptions by other submission types must
    // match the live scalar path, including source-order picking and counters.
    scene = Scene3D{};
    scene.debug.cache_draw_commands = false;
    scene.camera = Camera3D::look_at({0, 0, 13}, {});
    scene.camera.projection = Projection3D::Orthographic;
    scene.camera.height = 10;
    scene.camera.z_far = 60;
    // A multi-copy ref leaves a one-copy piece at the next task's start.
    // Keep its general prefix before the independent refs following it.
    std::vector<MeshInstance3D> tail(513);
    for (u32 i = 0; i < tail.size(); ++i)
        tail[i] = {Mat4f::translation({f32(i % 17) - 8, f32(i / 17) * .4f - 6, 1}), colors::white, 5000 + i};
    assert(scene.draw_instanced(mesh, tail, materials[1]));
    for (u32 i = 0; i < 1031; ++i) {
        if (i == 510)
            assert(scene.draw_instanced(explicit_batch, materials[0]));
        if (i == 769) {
            const std::array copies{MeshInstance3D{Mat4f::translation({-2, 0, 0}), colors::white, 3000},
                                    MeshInstance3D{Mat4f::translation({2, 0, 0}), colors::white, 3001}};
            assert(scene.draw_instanced(mesh, copies, materials[1]));
        }
        constexpr std::array x{-7.19f, -6.75f, -6.4f, 0.f, 6.4f, 6.75f, 7.19f};
        constexpr std::array y{-5.52f, -5.5f, 0.f, 5.5f, 5.52f};
        const Vec3f at{x[i % x.size()], y[(i / x.size()) % y.size()], 0};
        const std::array copy{MeshInstance3D{Mat4f::translation(at), colors::white, i + 1}};
        const DrawClip3D clip = i % 17 == 0 ? DrawClip3D::whole : DrawClip3D::camera;
        assert(scene.draw_instanced(mesh, copy, materials[i % 3], clip));
    }
    shoot(true);
    shoot(true);
    for (u32 camera = 0; camera < 5; ++camera) {
        const f32 x = (f32(camera) - 1) * .002f;
        scene.camera = Camera3D::look_at({x, 0, 13}, {x, 0, 0});
        scene.camera.projection = Projection3D::Orthographic;
        scene.camera.height = 10;
        scene.camera.z_far = 60;
        if (camera == 4) {
            scene.camera.clip_plane = {1, 0, 0, 0};
            scene.camera.window = {.25f, 0, .5f, 1};
        }
        scene.cull = camera != 3;
        scene.lights.shadow_area = {{-10, -7, -2}, {10, 7, 4}};
        scene.lights.shadow_resolution = 128;
        scene.lights.sun_shadow_strength = camera % 2 ? 0.f : 1.f;
        scene.lights.shadow_cascades = camera == 4 ? 4 : 1;
        const auto reference = shoot(false);
        const auto actual = shoot(true);
        assert(actual.pixels == reference.pixels && actual.pick_id == reference.pick_id &&
               actual.stats.draws == reference.stats.draws && actual.stats.instances == reference.stats.instances &&
               actual.stats.triangles == reference.stats.triangles &&
               actual.stats.camera_instance_tests == reference.stats.camera_instance_tests &&
               actual.stats.direct_retained_instances == 1546 && actual.stats.device_waits == 0);
        // Both retained and streamed calls can reuse ref metadata. Force a
        // fresh identity to independently check camera/caster flag transitions.
        Scene3D moved = std::move(scene);
        scene = std::move(moved);
        const auto fresh = shoot(false);
        assert(fresh.stats.prepared_reused == 0 && actual.pixels == fresh.pixels && actual.pick_id == fresh.pick_id &&
               actual.stats.instances == fresh.stats.instances &&
               actual.stats.shadow_casters == fresh.stats.shadow_casters &&
               actual.stats.shadow_draws == fresh.stats.shadow_draws &&
               actual.stats.camera_instance_tests == fresh.stats.camera_instance_tests);
        shoot(true);
        shoot(true);
    }
    // Prepared consumers replace the ref bindings with shared records/spheres.
    // Returning to native retention must rebind them even with the same layout.
    const auto native = shoot(true);
    const auto prepared = r3d.prepare(scene).value_or({});
    assert(prepared);
    renderer.clear(colors::black);
    assert(r3d.render_prepared(view, prepared));
    assert(r3d.debug_stats().direct_retained_instances == 0 && r3d.debug_stats().shared_records > 0);
    renderer.draw_texture(view.texture(), rect);
    std::vector<u8> prepared_pixels;
    Vec2i prepared_size;
    assert(renderer.read_rgba(rect, prepared_pixels, prepared_size) && prepared_size == extent);
    renderer.present();
    const auto rebound = shoot(true);
    assert(prepared_pixels == native.pixels && rebound.pixels == native.pixels && rebound.pick_id == native.pick_id &&
           rebound.stats.direct_retained_instances == 1546 && rebound.stats.direct_upload_bytes == 0 &&
           rebound.stats.direct_bounds_rebuilt == 0);

    // Force the general multi-level prefix path to level zero and compare it
    // with the single-level path. Mix retained and streamed refs, including a
    // streamed draw spanning multiple worker pieces and partially visible rows.
    const auto prefix_mesh = r3d.create_mesh(meshes::sphere(.2f, 16, 8), {.levels = 3}).value_or({});
    assert(prefix_mesh && prefix_mesh.level_count() > 1);
    std::vector<MeshInstance3D> prefix_copies;
    for (u32 i = 0; i < 1025; ++i)
        prefix_copies.push_back(
            {Mat4f::translation({f32(i % 41) * .5f - 10, f32(i / 41) * .5f - 6, 0}), colors::white, 4000 + i});
    auto prefix_batch =
        InstanceBatch3D::create(mesh,
                                std::vector<MeshInstance3D>(32, {Mat4f::translation({-2, 0, 2}), colors::white, 6000}))
            .value_or({});
    assert(prefix_batch);
    scene = Scene3D{};
    scene.debug.cache_draw_commands = false;
    scene.camera = Camera3D::look_at({0, 0, 13}, {});
    scene.camera.projection = Projection3D::Orthographic;
    scene.camera.height = 8;
    assert(scene.draw_instanced(prefix_mesh, prefix_copies, materials[0]));
    assert(scene.draw_instanced(prefix_batch, materials[1]));
    assert(scene.draw(mesh, Mat4f::translation({0, -3, 1}), materials[1], colors::white, 6001));
    for (u32 repeat = 0; repeat < 2; ++repeat) {
        scene.lod.enabled = false;
        shoot(false);
        const auto single = shoot(false);
        scene.lod.enabled = true;
        scene.debug.force_level = 0;
        const auto general = shoot(false);
        assert(single.pixels == general.pixels && single.pick_id == general.pick_id &&
               single.stats.instances == general.stats.instances && single.stats.triangles == general.stats.triangles &&
               single.stats.culled == general.stats.culled &&
               single.stats.skinned_copies == general.stats.skinned_copies &&
               single.stats.camera_instance_tests == general.stats.camera_instance_tests &&
               single.stats.retained_instances >= 32 && single.stats.instances > 32 && single.stats.culled > 0);
    }

    // Oversized direct tables stream within the existing 32 MiB budget.
    scene = Scene3D{};
    scene.debug.cache_draw_commands = false;
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
                 "move identity, same-frame views, budget fallback, local gather/sort reuse, moving casters/lights, "
                 "point faces, texture edits and warmed zero uploads/bounds passed\n";
}
} // namespace

int main() {
    const char* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
    bool ready = false;
    try {
        App app;
        auto& window =
            app.create_window({.title = "direct scene cache", .width = extent.x, .height = extent.y, .hidden = true});
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
