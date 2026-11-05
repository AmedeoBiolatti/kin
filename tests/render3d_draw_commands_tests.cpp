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
constexpr Vec2i extent{160, 120};
constexpr Rectf rect{0, 0, 160, 120};
struct Capture {
    std::vector<u8> pixels;
    RenderDebugStats3D stats;
    u32 pick = 0;
};

void check(Renderer2D& renderer) {
    Renderer3D r{renderer};
    const auto box = r.create_mesh(meshes::box({.6f, .6f, .6f})).value_or({});
    auto skin = meshes::box({.6f, .6f, .6f});
    skin.skin.resize(skin.vertices.size());
    const auto skinned = r.create_mesh(skin).value_or({});
    auto view = r.create_view(extent, {.samples = 1, .background = colors::black, .pick = true}).value_or({});
    auto second = r.create_view(extent, {.samples = 4, .background = colors::black}).value_or({});
    auto third = r.create_view(extent, {.samples = 1, .background = colors::black}).value_or({});
    auto hdr = r.create_view(extent, {.samples = 1, .background = colors::black, .hdr = true}).value_or({});
    assert(box && skinned && view && second && third && hdr);
    std::array<u8, 8 * 8 * 4> texels{};
    for (u32 i = 0; i < 64; ++i) {
        texels[4 * i] = 255;
        texels[4 * i + 1] = i % 2 ? 80 : 255;
        texels[4 * i + 2] = 255;
        texels[4 * i + 3] = i % 2 ? 0 : 255;
    }
    const auto texture = renderer.create_texture_from_rgba(texels.data(), {8, 8});
    assert(texture);
    std::array<Material3D, 4> materials;
    for (u32 i = 0; i < materials.size(); ++i) {
        auto& m = materials[i];
        m.texture = texture;
        m.color = Color::rgb(u8(230 - i * 45), u8(40 + i * 60), u8(70 + i * 30));
        m.metallic = i % 2 ? .75f : 0;
        m.roughness = .2f + i * .2f;
        if (i == 2) {
            m.blend = Blend3D::Mask;
            m.alpha_cutoff = .7f;
        }
        if (i == 3)
            m.set_emission({.15f, .7f, .3f}, .4f);
    }
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 1, 14}, {});
    scene.lights.ambient = {.3f, .3f, .3f};
    scene.lights.sun.direction = normalize(Vec3f{.5f, 1, .4f});
    scene.lights.cache_shadows = false;
    const auto populate = [&](bool edit) {
        scene.clear();
        for (u32 i = 0; i < 384; ++i) {
            auto m = materials[(i + u32(edit)) % materials.size()];
            // Texture/sampling and culling changes split consecutive runs.
            if (i % 91 == 0)
                m.double_sided = true;
            if (i % 71 == 0)
                m.texture_samplers[0] = TextureSampler3D{};
            assert(scene.draw(
                box, Mat4f::translation({f32(i % 24) * .65f - 7.5f, f32(i / 24) * .65f - 5.f, edit ? -.5f : 0}), m,
                colors::white, i + 1));
        }
        // Same mesh and textures, different scalar material values, equal depth:
        // the later source must still win, including inside a merged draw.
        assert(scene.draw(box, Mat4f::scaling({2, 2, 2}), materials[0], colors::white, 900));
        assert(scene.draw(box, Mat4f::scaling({2, 2, 2}), materials[1], colors::white, 901));
        assert(scene.draw(box, Mat4f::translation({0, 3, 1}), {.color = colors::white, .unlit = true}));
        const std::array pose{Mat4f::identity()};
        assert(scene.draw_skinned(skinned, pose, Mat4f::translation({-2, 2, 1}), materials[1]));
        assert(scene.draw(box, Mat4f::translation({0, 0, 2}),
                          {.color = Color::rgba(30, 90, 220, 90), .blend = Blend3D::Alpha}));
    };
    const auto read = [&](const View3D& target) {
        renderer.clear(colors::black);
        renderer.draw_texture(target.texture(), rect);
        Vec2i size;
        std::vector<u8> pixels;
        assert(renderer.read_rgba(rect, pixels, size) && size == extent);
        return pixels;
    };
    const auto shoot = [&](bool cached, View3D& target) {
        scene.debug.cache_draw_commands = cached;
        renderer.clear(colors::black);
        assert(r.render(target, scene));
        Capture result{{}, r.debug_stats()};
        result.pixels = read(target);
        if (&target == &view)
            for (u32 i = 0; i < result.pixels.size() / 4; ++i)
                if (result.pixels[4 * i] || result.pixels[4 * i + 1] || result.pixels[4 * i + 2]) {
                    const auto pick = r.pick(view, {i32(i % extent.x), i32(i / extent.x)});
                    assert(pick);
                    result.pick = pick->id;
                    break;
                }
        renderer.present();
        return result;
    };
    const auto compare = [&](View3D& target, bool merged = true) {
        shoot(true, target); // establish an unchanged command revision
        const auto before = shoot(false, target);
        const auto after = shoot(true, target);
        assert(before.pixels == after.pixels && before.pick == after.pick);
        assert(before.stats.instances == after.stats.instances && before.stats.triangles == after.stats.triangles &&
               before.stats.local_shadow_casters == after.stats.local_shadow_casters &&
               before.stats.local_shadow_triangles == after.stats.local_shadow_triangles &&
               before.stats.local_shadow_draws == after.stats.local_shadow_draws);
        assert(before.stats.draws == after.stats.draws + after.stats.main_merged_draws);
        if (merged)
            assert(after.stats.main_merged_draws > 200 && after.stats.main_batched_draws > 0);
        assert(after.stats.device_waits == 0 && after.stats.draw_material_gpu_bytes <= 4 * 1024 * 1024 &&
               after.stats.draw_plan_bytes <= 4 * 1024 * 1024);
        return after;
    };
    populate(false);
    assert(r.warm_up(view, scene));
    shoot(true, view);
    shoot(true, view);
    compare(view);
    auto warm = shoot(true, view);
    assert(warm.stats.draw_plan_hits == 1 && warm.stats.draw_plan_builds == 0 && warm.stats.draw_pipeline_misses == 0 &&
           warm.stats.draw_pipeline_hits > 0 && warm.stats.draw_material_upload_bytes == 0);
    compare(second); // different sample count, same retained source
    compare(hdr);    // different target format and output transform
    compare(view);
    scene.sky.enabled = true;
    compare(view);
    scene.fog.density = .025f;
    scene.fog.enabled = true;
    compare(view); // fog shader with a material table
    scene.lights.spots.push_back(
        {.position = {2, 4, 8}, .direction = normalize(Vec3f{-2, -4, -8}), .radius = 30, .shadow_enabled = true});
    scene.lights.max_shadowed_spots = 1;
    scene.lights.local_shadow_resolution = 128;
    assert(r.warm_up(view, scene));
    compare(view); // light+fog shader
    scene.fog.density = 0;
    scene.fog.enabled = false;
    compare(view); // light shader
    const std::array pose{Mat4f::translation({.5f, 0, 0})};
    assert(scene.update_joint_matrices(0, pose));
    compare(view);
    scene.ambient_occlusion.enabled = scene.contact_shadows.enabled = true;
    compare(view);
    scene.ambient_occlusion.enabled = scene.contact_shadows.enabled = false;
    // Changing a texture in place or changing its legacy sampler does not
    // change the source revision. Bindings must still observe it immediately.
    for (auto mode : {ScaleMode::Linear, ScaleMode::Mipmapped, ScaleMode::Nearest}) {
        renderer.set_scale_mode(texture, mode);
        compare(view);
    }
    for (u32 i = 0; i < 64; ++i)
        texels[4 * i + 3] = 255;
    assert(renderer.update_texture(texture, {}, {8, 8}, texels.data()));
    warm = shoot(true, view);
    assert(warm.stats.draw_plan_hits == 1 && warm.stats.draw_material_upload_bytes == 0);
    assert(warm.pixels == shoot(false, view).pixels);
    populate(true);
    shoot(true, view);
    compare(view);
    scene.debug.draw_command_budget = 1;
    auto fallback = compare(view, false);
    assert(fallback.stats.draw_plan_bytes == 0 && fallback.stats.draw_material_gpu_bytes == 0 &&
           fallback.stats.main_merged_draws == 0);
    scene.debug.draw_command_budget = 4 * 1024 * 1024;
    compare(view);
    scene.lod.enabled = true;
    assert(compare(view, false).stats.main_merged_draws == 0);
    scene.lod.enabled = false;
    compare(view);
    scene.taa.enabled = true;
    scene.debug.freeze_jitter = true;
    shoot(true, view);
    assert(shoot(true, view).stats.main_merged_draws == 0);
    scene.taa.enabled = false;
    scene.sky.enabled = false;
    auto exposed = scene.add_instances(box, 1, materials[1]).value_or({});
    assert(exposed.size() == 1);
    exposed[0] = {Mat4f::translation({0, 0, 3}), colors::white, 999};
    assert(compare(view, false).stats.main_merged_draws == 0);
    exposed[0].transform = Mat4f::translation({4, 0, 3});
    compare(view, false);
    populate(false);
    shoot(true, view);
    shoot(true, view);
    auto& overlay = scene.overlay();
    overlay.camera = scene.camera;
    for (u32 i = 0; i < 384; ++i)
        assert(overlay.draw(box, Mat4f::translation({f32(i % 24) * .6f - 7.f, f32(i / 24) * .6f - 4.f, 0}),
                            materials[i % materials.size()]));
    shoot(true, view);
    shoot(true, view);
    compare(view);
    overlay.clear();
    const auto held = scene.draw_lifetime().lock();
    Scene3D moved = std::move(scene);
    assert(held && *held == 0);
    scene = std::move(moved);
    shoot(true, view);
    shoot(true, view);
    compare(view);
    // A scene resubmitted every render stays on the immediate path, without
    // rebuilding a retained command plan that would never get a cache hit.
    for (u32 edit = 0; edit < 3; ++edit) {
        populate(edit % 2);
        const auto immediate = shoot(true, view);
        assert(immediate.stats.draw_plan_hits == 0 && immediate.stats.draw_plan_builds == 0 &&
               immediate.stats.main_merged_draws == 0 && immediate.stats.draw_material_upload_bytes == 0);
    }
    shoot(true, view);
    compare(view);
    // Cache disable/source edits in later views cannot change an earlier
    // recorded view, including when CPU cache payload is freed immediately.
    const auto reference = shoot(true, view);
    renderer.clear(colors::black);
    assert(r.render(view, scene));
    populate(true);
    scene.debug.cache_draw_commands = false;
    assert(r.render(second, scene));
    populate(false);
    scene.debug.cache_draw_commands = true;
    assert(r.render(third, scene));
    assert(read(view) == reference.pixels && read(third) == reference.pixels);
    renderer.present();
    // Instance retention can fit while the independent material budget cannot.
    // That source must still draw completely with per-draw uniforms.
    scene = Scene3D{};
    scene.camera = Camera3D::look_at({0, 0, 13}, {});
    std::vector<MeshInstance3D> outside(4097, {Mat4f::translation({1000, 0, 0})});
    outside[0] = {Mat4f::identity(), colors::white, 1234};
    for (u32 i = 0; i < 32; ++i)
        assert(scene.draw_instanced(box, outside, materials[i % materials.size()]));
    shoot(true, view);
    shoot(true, view);
    fallback = compare(view, false);
    assert(fallback.stats.direct_retained_instances > 0 && fallback.stats.main_merged_draws == 0 &&
           fallback.stats.draw_material_gpu_bytes == 0 && fallback.pick == 1234);
    // Metadata can fit while its camera-index scratch would cross a reduced
    // CPU cap. Many distinct bindings put the plan near that boundary.
    scene = Scene3D{};
    scene.cull = false;
    scene.camera = Camera3D::look_at({0, 0, 13}, {});
    const std::array<MeshInstance3D, 3> copies{{{Mat4f::identity(), colors::white, 7},
                                                {Mat4f::identity(), colors::white, 7},
                                                {Mat4f::identity(), colors::white, 7}}};
    for (u32 i = 0; i < 3073; ++i) {
        Material3D material;
        for (u32 map = 0; map < material.texture_samplers.size(); ++map) {
            const u32 bits = (i >> (3 * map)) & 7;
            TextureSampler3D sampler;
            sampler.min = bits & 1 ? TextureFilter3D::Nearest : TextureFilter3D::Linear;
            sampler.mag = bits & 2 ? TextureFilter3D::Nearest : TextureFilter3D::Linear;
            sampler.wrap_u = bits & 4 ? Wrap3D::Repeat : Wrap3D::Clamp;
            material.texture_samplers[map] = sampler;
        }
        assert(scene.draw_instanced(box, copies, material));
    }
    shoot(true, view);
    shoot(true, view);
    const auto boundary = shoot(true, view);
    assert(boundary.stats.main_batched_draws > 0 && boundary.stats.draw_plan_bytes > 0);
    scene.debug.draw_command_budget = static_cast<u32>(boundary.stats.draw_plan_bytes - 1);
    const auto capped = shoot(true, view);
    assert(capped.stats.draw_plan_bytes <= scene.debug.draw_command_budget && capped.stats.main_batched_draws == 0);
    assert(capped.pixels == shoot(false, view).pixels);
    std::cout << "draw commands: exact pixels/picks/order, heterogeneous materials, sampling/texture edits, "
                 "fog/lights/MSAA/AO, poses, overlays, source moves, raw spans, budgets and same-frame views passed\n";
}
} // namespace

int main() {
    const char* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
    bool ready = false;
    try {
        App app;
        auto& window =
            app.create_window({.title = "draw commands", .width = extent.x, .height = extent.y, .hidden = true});
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
