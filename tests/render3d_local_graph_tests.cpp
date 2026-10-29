#include "local_graph_shaders.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
using namespace kin;
namespace {
constexpr Vec2i size{160, 120};
constexpr Rectf rect{0, 0, 160, 120};
struct Fixture {
    Renderer2D& r;
    Renderer3D r3d{r};
    SurfaceTargets3D target{r.create_render_target(size),
                            r.create_render_attachment(size, TextureFormat::Depth32Float)};
    Texture array = r.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 20);
    View3D view = r3d.create_view(size, {.samples = 1, .background = colors::black}).value_or({});
    Mesh3D box = r3d.create_mesh(meshes::box({1, 1, 1})).value_or({});
    Mesh3D ground = r3d.create_mesh(meshes::box({8, .1f, 6})).value_or({});
    MaterialShader3D hook;
    ShaderHandle probe;
    Scene3D scene;
    Fixture(Renderer2D& renderer) : r(renderer) {
        const auto bytes = local_graph_shaders("custom_direct_light.frag.spv");
        hook = r3d.create_material_shader(bytes).value_or({});
        const auto p = local_graph_shaders("shadow_probe.frag.spv");
        probe = r.create_shader(
            {.spirv = {p.data(), static_cast<u32>(p.size())}, .num_samplers = 1, .num_uniform_buffers = 1});
        assert(target.colour && target.depth && array && view && box && ground && hook && probe);
        scene.camera = Camera3D::look_at({3, 3, 6}, {0, .3f, 0}, 45);
        scene.lights.sun.color = {};
        scene.lights.local_shadow_resolution = 128;
        scene.lights.points.push_back(
            {.position = {-2, 3, 2}, .color = {10, 8, 6}, .radius = 12, .shadow_enabled = true});
        scene.lights.spots.push_back({.position = {2, 4, 2},
                                      .direction = {-.3f, -1, -.4f},
                                      .color = {12, 14, 16},
                                      .radius = 12,
                                      .shadow_enabled = true});
        objects();
    }
    void clear_draws() {
        auto points = std::move(scene.lights.points);
        auto spots = std::move(scene.lights.spots);
        scene.clear();
        scene.lights.points = std::move(points);
        scene.lights.spots = std::move(spots);
    }
    void objects(Material3D material = {}) {
        clear_draws();
        assert(scene.draw(ground, Mat4f::translation({0, -.1f, 0}), material));
        assert(scene.draw(box, Mat4f::translation({0, .5f, 0}), material));
    }
    std::vector<u8> read(const Texture& texture) {
        r.draw_texture(texture, rect);
        std::vector<u8> pixels;
        Vec2i actual;
        assert(r.read_rgba(rect, pixels, actual) && actual == size);
        r.present();
        return pixels;
    }
    std::vector<u8> stock() {
        r.clear(colors::black);
        assert(r3d.render(view, scene));
        return read(view.texture());
    }
    std::vector<u8> custom(std::span<const LocalShadowRequest3D> requests, bool shadows = true) {
        r.clear(colors::black);
        const auto prepared = r3d.prepare(scene).value_or({});
        assert(prepared);
        const auto result = r3d.render_local_shadows(array, prepared, requests, size);
        if (!result)
            std::cerr << diagnostics3d_text(result.diagnostics) << '\n';
        const auto maps = result.value_or({});
        assert(maps.depth && r3d.debug_stats().local_shadow_cached == 0 &&
               r3d.debug_stats().particle_shadow_draws == 0 && r3d.debug_stats().bounds_instance_tests == 0);
        assert(r3d.render_surface(target, prepared,
                                  {.background = colors::black, .local_shadows = shadows ? &maps : nullptr}));
        assert(r3d.debug_stats().local_shadow_draws == 0 && r3d.debug_stats().shadow_draws == 0 &&
               r3d.debug_stats().device_waits == 0 && r3d.debug_stats().taa_dispatches == 0);
        return read(target.colour.texture());
    }
};
void stock_without_shadow_work(Fixture& f) {
    const auto lights = f.scene.lights;
    f.scene.lights.spots.clear();
    f.scene.lights.points = {{.position = {-100, 0, 0},
                              .color = {10, 10, 10},
                              .radius = 200,
                              .shadow_enabled = true,
                              .shadow_resolution = 128,
                              .shadow_distance = 1}};
    f.r.clear(colors::black);
    // First render on this backend: every cube face is outside the camera,
    // so no local caster buffers have ever been required or allocated.
    assert(f.r3d.render(f.view, f.scene));
    assert(f.r3d.debug_stats().local_shadow_drawn == 0);
    f.r.present();
    f.scene.lights = lights;
    std::cout << "stock local shadows: all faces culled before caster-buffer allocation is a successful no-op\n";
}

void parity(Fixture& f) {
    // Non-stock layer placement exercises explicit mappings rather than private slot layouts.
    const std::array requests{LocalShadowRequest3D{.kind = LocalShadowKind3D::Spot, .light = 0, .first_layer = 9},
                              LocalShadowRequest3D{.kind = LocalShadowKind3D::Point, .light = 0, .first_layer = 1}};
    const auto expected = f.stock();
    const auto actual = f.custom(requests);
    assert(actual == expected);
    const auto lit = f.custom(requests, false);
    assert(lit != actual);
    // The requested shadow is independent of the scene's stock-selection flags.
    f.scene.lights.points[0].shadow_enabled = false;
    f.scene.lights.spots[0].shadow_enabled = false;
    assert(f.custom(requests) == actual && f.stock() == lit);
    f.scene.lights.points[0].shadow_enabled = true;
    f.scene.lights.spots[0].shadow_enabled = true;
    // Smaller viewports use only a share of each full array layer.
    f.scene.lights.points[0].shadow_resolution = 64;
    auto small = requests;
    small[1].resolution = 64;
    assert(f.custom(small) == f.stock());
    f.scene.lights.points[0].shadow_resolution = 0;
    // Cached stock maps survive public recording into a different projection.
    f.scene.lights.cache_shadows = true;
    for (int i = 0; i < 3; ++i)
        f.stock();
    assert(f.r3d.debug_stats().local_shadow_cached > 0);
    const auto cached = f.stock();
    f.custom(requests);
    assert(f.stock() == cached && f.r3d.debug_stats().local_shadow_cached > 0);
    f.scene.lights.cache_shadows = false;
    // Data layers are fully initialized, including holes and an empty request set.
    for (const bool empty : {false, true}) {
        f.r.clear(colors::black);
        const auto prepared = f.r3d.prepare(f.scene).value_or({});
        assert(f.r3d.render_local_shadows(f.array, prepared, empty ? std::span<const LocalShadowRequest3D>{} : requests,
                                          size));
        for (const u32 layer : {0u, 7u, 8u, 10u, 19u}) {
            ShaderParams p;
            p.uniforms[0] = static_cast<f32>(layer);
            f.r.draw_shader_surface(rect, f.probe, p, f.array);
            std::vector<u8> pixels;
            Vec2i actual_size;
            assert(f.r.read_rgba(rect, pixels, actual_size));
            for (std::size_t i = 0; i < pixels.size(); i += 4)
                assert(pixels[i] == 0);
        }
        f.r.present();
    }
    std::cout
        << "local graph: stock pixel parity, explicit layers/selection, viewport scaling, private cache and clears\n";
}
void variants(Fixture& f) {
    const std::array requests{LocalShadowRequest3D{.kind = LocalShadowKind3D::Spot, .first_layer = 0},
                              LocalShadowRequest3D{.kind = LocalShadowKind3D::Point, .first_layer = 1}};
    for (const bool clusters : {false, true}) {
        const auto mesh =
            f.r3d.create_mesh(meshes::sphere(1, 24, 12), {.levels = 3, .clusters = clusters}).value_or({});
        for (const bool gpu : {false, true}) {
            f.clear_draws();
            f.scene.lod.enabled = true;
            f.scene.debug.gpu_culling = gpu;
            f.scene.debug.force_level = 0;
            assert(mesh && f.scene.draw(mesh, Mat4f::scaling({1.5f, .7f, 1})));
            assert(f.custom(requests) == f.stock());
        }
    }
    f.scene.lod.enabled = false;
    f.scene.debug.force_level = -1;
    auto data = meshes::box({1, 1, 1});
    data.skin.assign(data.vertices.size(), {});
    const auto skinned = f.r3d.create_mesh(data).value_or({});
    auto batch =
        InstanceBatch3D::create(f.box, std::vector<MeshInstance3D>{{.transform = Mat4f::translation({-1, .5f, 0})},
                                                                   {.transform = Mat4f::translation({1, .5f, 0})}})
            .value_or({});
    f.clear_draws();
    const std::array pose{Mat4f::translation({0, .5f, .5f})};
    assert(skinned && batch && f.scene.draw_skinned(skinned, pose, {}) && f.scene.draw_instanced(batch));
    assert(f.custom(requests) == f.stock());
    f.objects();
    // Mask coverage, translucent ordering and overlays reuse the existing recorder.
    const std::array<u8, 8> rgba{255, 255, 255, 0, 255, 255, 255, 255};
    const auto mask = f.r.create_texture_from_rgba(rgba.data(), {2, 1});
    assert(mask && f.scene.draw(f.box, Mat4f::translation({1, .5f, 1}), {.blend = Blend3D::Mask, .texture = mask}));
    assert(f.scene.draw(f.box, Mat4f::translation({-1, .5f, 1}),
                        {.color = Color::rgba(50, 200, 100, 100), .blend = Blend3D::Alpha}));
    assert(f.scene.overlay().draw(f.box, Mat4f::scaling({.15f, .15f, .15f}), {.unlit = true}));
    assert(f.custom(requests) == f.stock());
    f.objects();
    // Seed stale particle-caster scratch; public local shadows remain mesh-only.
    Scene3D particles;
    particles.camera = f.scene.camera;
    particles.lights = f.scene.lights;
    const std::array sprites{ParticleSprite3D{.position = {0, 1, 0}, .size = 2}};
    assert(particles.draw_particles({.casts_shadow = true}, sprites));
    f.r.clear(colors::black);
    assert(f.r3d.render(f.view, particles));
    f.r.present();
    const auto mesh_only = f.custom(requests);
    assert(mesh_only == f.stock());
    // Steady recording allocates no geometry/local-shadow resources or staging growth.
    const auto frame = [&] {
        f.r.clear(colors::black);
        const auto p = f.r3d.prepare(f.scene).value_or({});
        const auto maps = f.r3d.render_local_shadows(f.array, p, requests, size).value_or({});
        assert(maps.depth && f.r3d.render_surface(f.target, p, {.local_shadows = &maps}));
        f.r.present();
    };
    for (int i = 0; i < 3; ++i)
        frame();
    const auto before = f.r.backend_stats();
    for (int i = 0; i < 8; ++i)
        frame();
    const auto after = f.r.backend_stats();
    assert(after.gpu_memory.allocations - before.gpu_memory.allocations ==
           after.upload_staging_allocations - before.upload_staging_allocations);
    assert(after.upload_staging_bytes <= before.upload_staging_bytes);
    std::cout << "local graph: LOD/CPU-GPU cuts, skinning/retained/mask/transparency/overlay, particle exclusion and "
                 "stable resources\n";
}

void cube_and_camera(Fixture& f) {
    const auto lights = f.scene.lights;
    const auto camera = f.scene.camera;
    f.clear_draws();
    f.scene.lights.spots.clear();
    f.scene.lights.points = {{.position = {}, .color = {1, 1, 1}, .radius = 10}};
    static constexpr Vec3f axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto axis : axes)
        assert(f.scene.draw(f.box, Mat4f::translation(axis * 2.f)));
    f.r.clear(colors::black);
    const auto p = f.r3d.prepare(f.scene).value_or({});
    const std::array cube{LocalShadowRequest3D{.kind = LocalShadowKind3D::Point}};
    assert(f.r3d.render_local_shadows(f.array, p, cube, size));
    assert(f.r3d.debug_stats().local_shadow_maps == 6);
    for (u32 face = 0; face < 6; ++face) {
        ShaderParams params;
        params.uniforms[0] = static_cast<f32>(face);
        f.r.draw_shader_surface(rect, f.probe, params, f.array);
        std::vector<u8> pixels;
        Vec2i actual;
        assert(f.r.read_rgba(rect, pixels, actual));
        u32 filled = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4)
            filled += pixels[i] > 0;
        assert(filled > 100);
    }
    f.r.present();
    // A caster beyond the camera's right edge still shadows a visible receiver.
    f.clear_draws();
    f.scene.camera = Camera3D::look_at({0, 2.5f, 6}, {0, .3f, 0}, 45);
    f.scene.lights.points = {{.position = {6, 3, 0}, .color = {20, 20, 20}, .radius = 12, .shadow_enabled = true}};
    assert(f.scene.draw(f.ground, Mat4f::translation({0, -.1f, 0})));
    assert(f.scene.draw(f.box, Mat4f::translation({5, 2, 0})));
    const auto shadowed = f.custom(cube);
    assert(f.r3d.stats().culled >= 1 && shadowed == f.stock());
    assert(shadowed != f.custom(cube, false));
    f.scene.camera = camera;
    f.scene.lights = lights;
    f.objects();
    std::cout << "local graph: all six cube faces contain casters and off-camera casters shadow visible receivers\n";
}
void grid_and_history(Fixture& f) {
    const auto lights = f.scene.lights;
    const std::array requests{LocalShadowRequest3D{.kind = LocalShadowKind3D::Spot},
                              LocalShadowRequest3D{.kind = LocalShadowKind3D::Point, .first_layer = 1}};
    // Surface-local bindings must reach the storage-buffer light grid too.
    for (u32 i = 0; i < 20; ++i)
        f.scene.lights.points.push_back(
            {.position = {float(i % 5) - 2, 2, float(i / 5) - 2}, .color = {.1f, .15f, .2f}, .radius = 8});
    for (const bool grid : {false, true}) {
        f.scene.debug.light_grid = grid;
        assert(f.custom(requests) == f.stock());
    }
    f.scene.lights = lights;
    f.scene.taa.enabled = true;
    f.scene.ambient_occlusion.enabled = true;
    auto a = f.r3d.create_view(size, {.samples = 1, .background = colors::black, .hdr = true}).value_or({});
    auto b = f.r3d.create_view(size, {.samples = 1, .background = colors::black, .hdr = true}).value_or({});
    assert(a && b);
    const auto shot = [&](View3D& view) {
        f.r.clear(colors::black);
        assert(f.r3d.render(view, f.scene));
        return f.read(view.texture());
    };
    for (int frame = 0; frame < 8; ++frame) {
        f.clear_draws();
        assert(f.scene.draw(f.ground, Mat4f::translation({0, -.1f, 0})));
        assert(f.scene.draw(f.box, Mat4f::translation({float(frame) * .03f, .5f, 0})));
        const auto expected = shot(a);
        f.r.clear(colors::black);
        const auto prepared = f.r3d.prepare(f.scene).value_or({});
        auto maps = f.r3d.render_local_shadows(f.array, prepared, requests, size).value_or({});
        assert(maps.depth);
        for (auto& map : maps.maps)
            for (auto& projection : map.projection)
                projection.view_projection.m[12] += 1000;
        assert(f.r3d.render_surface(f.target, prepared, {.local_shadows = &maps}));
        f.r.present();
        assert(shot(b) == expected);
    }
    f.scene.taa.enabled = false;
    f.scene.ambient_occlusion.enabled = false;
    f.objects();
    const auto unshadowed = f.custom(requests, false);
    f.r.clear(colors::black);
    const auto prepared = f.r3d.prepare(f.scene).value_or({});
    auto maps = f.r3d.render_local_shadows(f.array, prepared, requests, size).value_or({});
    assert(maps.depth);
    for (auto& map : maps.maps)
        for (auto& projection : map.projection)
            projection.view_projection.m[12] += 1000;
    assert(f.r3d.render_surface(f.target, prepared, {.background = colors::black, .local_shadows = &maps}));
    assert(f.read(f.target.colour.texture()) == unshadowed);
    std::cout
        << "local graph: light-grid parity, explicit projection consumption and independent stock TAA/AO history\n";
}

void hooks(Fixture& f) {
    const std::array requests{LocalShadowRequest3D{.kind = LocalShadowKind3D::Spot, .first_layer = 0},
                              LocalShadowRequest3D{.kind = LocalShadowKind3D::Point, .first_layer = 1}};
    f.objects();
    const auto stock = f.custom(requests);
    Material3D material;
    material.shader = f.hook;
    f.objects(material);
    const auto identity = f.custom(requests);
    assert(identity.size() == stock.size());
    for (std::size_t i = 0; i < stock.size(); ++i)
        assert(std::abs(int(identity[i]) - int(stock[i])) <= 1);
    material.shader_params[0] = 1;
    f.objects(material);
    const auto ignore = f.custom(requests);
    assert(ignore != identity && ignore == f.custom(requests, false));
    material.shader_params[0] = 2;
    f.objects(material);
    const auto kind = f.custom(requests);
    assert(kind != identity && kind != ignore);
    const auto ambient = f.scene.lights.ambient;
    const auto ground_ambient = f.scene.lights.ground_ambient;
    f.scene.lights.ambient = {};
    f.scene.lights.ground_ambient.color = {};
    f.scene.lights.spots[0].color = {};
    const auto red = f.custom(requests);
    const std::size_t centre = (60 * 160 + 80) * 4;
    assert(red[centre] > 80 && red[centre + 1] == 0 && red[centre + 2] == 0);
    material.shader_params[0] = 4;
    f.objects(material);
    const auto distance = f.custom(requests);
    assert(distance[centre] > 0 && std::abs(int(distance[centre]) - int(distance[centre + 1])) <= 1 &&
           distance[centre + 2] > distance[centre]);
    material.shader_params[0] = 2;
    f.objects(material);
    f.scene.lights.spots[0].color = {12, 14, 16};
    f.scene.lights.points[0].color = {};
    const auto green = f.custom(requests);
    assert(green[centre] == 0 && green[centre + 1] > 80 && green[centre + 2] == 0);
    f.scene.lights.ambient = ambient;
    f.scene.lights.ground_ambient = ground_ambient;
    // A wrapped diffuse hook sees a sun behind the shading normal too.
    f.scene.lights.points.clear();
    f.scene.lights.spots.clear();
    f.scene.lights.sun.color = {1, 1, 1};
    f.scene.lights.sun.direction = {1, .1f, 0};
    material.shader_params[0] = 3;
    f.objects(material);
    const auto wrapped = f.custom({});
    material.shader_params[0] = 0;
    f.objects(material);
    assert(wrapped != f.custom({}));
    std::cout << "material hook: stock response parity, independent visibility, light kind and wrapped directional "
                 "response\n";
}
void validation(Fixture& f) {
    f.scene.lights.sun.color = {};
    f.scene.lights.points = {{.position = {0, 3, 2}, .color = {10, 10, 10}, .radius = 10}};
    f.scene.lights.spots = {{.position = {2, 3, 2}, .direction = {0, -1, 0}}};
    f.objects();
    f.r.clear(colors::black);
    const auto prepared = f.r3d.prepare(f.scene).value_or({});
    const auto rejected = [&](std::initializer_list<LocalShadowRequest3D> requests) {
        const auto result = f.r3d.render_local_shadows(f.array, prepared, requests, size);
        assert(!result && !result.diagnostics.empty());
    };
    rejected({{.kind = LocalShadowKind3D::Point, .first_layer = 18}});
    rejected({{.light = 8}});
    rejected({{.first_layer = 0}, {.first_layer = 1}});
    rejected({{.resolution = 129}});
    rejected(
        {{.kind = LocalShadowKind3D::Spot, .first_layer = 0}, {.kind = LocalShadowKind3D::Point, .first_layer = 0}});
    rejected({{.kind = static_cast<LocalShadowKind3D>(99)}});
    const auto map =
        f.r3d.render_local_shadows(f.array, prepared, std::array{LocalShadowRequest3D{}}, size).value_or({});
    assert(map.depth);
    auto bad = map;
    bad.maps[0].projection[0].view_projection.m[0] = std::numeric_limits<f32>::quiet_NaN();
    assert(!f.r3d.render_surface(f.target, prepared, {.local_shadows = &bad}));
    bad = map;
    bad.maps[0].source.first_layer = 20;
    assert(!f.r3d.render_surface(f.target, prepared, {.local_shadows = &bad}));
    bad = map;
    bad.maps[0].projection[0].far_plane = bad.maps[0].projection[0].near_plane;
    assert(!f.r3d.render_surface(f.target, prepared, {.local_shadows = &bad}));
    bad = map;
    bad.maps[0].softness = 0;
    assert(!f.r3d.render_surface(f.target, prepared, {.local_shadows = &bad}));
    bad = map;
    bad.maps.push_back(bad.maps[0]);
    assert(!f.r3d.render_surface(f.target, prepared, {.local_shadows = &bad}));
    bad = map;
    bad.depth = f.target.depth;
    assert(!f.r3d.render_surface(f.target, prepared, {.local_shadows = &bad}));
    assert(!f.r3d.render_local_shadows(f.target.depth, prepared, {}, size));
    f.r.present();
    assert(!f.r3d.render_local_shadows(f.array, prepared, {}, size));

    // Historical map binding: producer retention ended in the previous frame.
    // Drop the last application handle after surface recording, before submit.
    f.objects({.color = Color::rgb(80, 120, 160)});
    LocalShadows3D historical;
    f.r.clear(colors::black);
    {
        const auto p = f.r3d.prepare(f.scene).value_or({});
        const auto depth = f.r.create_render_attachment({128, 128}, TextureFormat::Depth32Float, 7);
        historical = f.r3d.render_local_shadows(depth, p, std::array{LocalShadowRequest3D{}}, size).value_or({});
        assert(historical.depth);
    }
    f.r.present();
    f.r.clear(colors::black);
    {
        const auto p = f.r3d.prepare(f.scene).value_or({});
        const SurfaceTargets3D temporary{f.r.create_render_target(size),
                                         f.r.create_render_attachment(size, TextureFormat::Depth32Float)};
        assert(f.r3d.render_surface(temporary, p, {.background = colors::black, .local_shadows = &historical}));
        f.r.draw_texture(temporary.colour.texture(), rect);
        historical = {};
    }
    std::vector<u8> pixels;
    Vec2i actual;
    assert(f.r.read_rgba(rect, pixels, actual));
    f.r.present();
    assert(std::any_of(pixels.begin(), pixels.end(), [](u8 x) { return x > 0 && x < 255; }));
    // Exercise the complete 8-spot/2-point profile (all twenty layers).
    f.scene.lights.spots.resize(9, f.scene.lights.spots[0]);
    f.scene.lights.points.resize(3, f.scene.lights.points[0]);
    std::vector<LocalShadowRequest3D> maximum;
    for (u32 i = 0; i < 8; ++i)
        maximum.push_back({.light = i, .first_layer = i});
    for (u32 i = 0; i < 2; ++i)
        maximum.push_back({.kind = LocalShadowKind3D::Point, .light = i, .first_layer = 8 + 6 * i});
    f.r.clear(colors::black);
    const auto p = f.r3d.prepare(f.scene).value_or({});
    const auto all = f.r3d.render_local_shadows(f.array, p, maximum, size).value_or({});
    assert(all.maps.size() == 10 && f.r3d.debug_stats().local_shadow_maps == 20 &&
           f.r3d.render_surface(f.target, p, {.local_shadows = &all}));
    const std::array too_many_points{
        LocalShadowRequest3D{.kind = LocalShadowKind3D::Point, .light = 0},
        LocalShadowRequest3D{.kind = LocalShadowKind3D::Point, .light = 1, .first_layer = 6},
        LocalShadowRequest3D{.kind = LocalShadowKind3D::Point, .light = 2, .first_layer = 12}};
    assert(!f.r3d.render_local_shadows(f.array, p, too_many_points, size));
    maximum.clear();
    for (u32 i = 0; i < 9; ++i)
        maximum.push_back({.light = i, .first_layer = i});
    assert(!f.r3d.render_local_shadows(f.array, p, maximum, size));
    f.r.present();
    std::cout << "local graph: validation, historical/queued lifetime and the full bounded profile\n";
}
} // namespace
int main() {
    App app;
    auto& window = app.create_window({.title = "local graph", .width = 160, .height = 120, .hidden = true});
    Renderer2D r{make_render_backend(window, false, true)};
    Renderer3D check{r};
    if (!check.capabilities().local_shadow_pass || !check.capabilities().surface_pass)
        return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
    Fixture f{r};
    stock_without_shadow_work(f);
    parity(f);
    variants(f);
    cube_and_camera(f);
    grid_and_history(f);
    hooks(f);
    validation(f);
}
