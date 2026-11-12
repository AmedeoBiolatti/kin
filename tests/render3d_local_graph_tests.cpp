#include "../engine/src/renderer/gpu/gpu_light_grid3d.hpp"
#include "../engine/src/renderer/gpu/gpu_pipeline_cache.hpp"
#include "../engine/src/renderer/gpu/gpu_renderer3d.hpp"
#include "../engine/src/renderer/gpu/gpu_shadow_mesh.hpp"
#include "../engine/src/renderer/gpu/gpu_sphere_cull4.hpp"
#include "local_graph_shaders.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/material_graph3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/shader_compiler.hpp>
#include <limits>
#include <random>
namespace kin::gpu {
// Test-only inspection of stock maps. Public graph maps intentionally remain
// uncached and are not a substitute for checking the stock atlas coordinates.
struct LocalShadowDepthProbe {
    static void invalidate_full(GpuRenderer3D& renderer) {
        for (auto& cache : renderer._local_cache)
            cache.valid = false;
    }
    static GpuShader full_redraw(GpuRenderer3D& renderer) {
        invalidate_full(renderer);
        return std::move(renderer._shadow_restore_shader);
    }
    static void restore(GpuRenderer3D& renderer, GpuShader shader) {
        renderer._shadow_restore_shader = std::move(shader);
    }
    static bool packed_origin(const GpuRenderer3D& renderer) {
        return std::any_of(renderer._local_views.begin(), renderer._local_views.end(),
                           [](const auto& v) { return v.wanted && (v.x || v.y); });
    }
    static std::vector<u8> read(GpuRenderer3D& renderer) {
        auto& device = *renderer._device;
        const auto bytes = local_graph_shaders("shadow_cache_probe.frag.spv");
        auto shader = GpuShader::from_bytes(device, SDL_GPU_SHADERSTAGE_FRAGMENT, SDL_GPU_SHADERFORMAT_SPIRV,
                                            {bytes.data(), bytes.size()}, 1, 1);
        GpuPipelineCache pipelines;
        pipelines.init(device.handle());
        auto* pipeline = pipelines.get(GpuPipelineKey{.vertex = renderer._fullscreen_shader.handle(),
                                                      .fragment = shader.handle(),
                                                      .blend = GpuBlendMode::Replace,
                                                      .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                                      .layout = GpuVertexLayout::None});
        const SDL_GPUSamplerCreateInfo info{};
        auto* sampler = SDL_CreateGPUSampler(device.handle(), &info);
        assert(shader && pipeline && sampler);
        std::vector<u8> result;
        bool covered = false;
        for (const auto& view : renderer._local_views) {
            if (!view.wanted || view.moon || view.gi_sun)
                continue;
            const std::array<u32, 4> metadata{view.layer, view.x, view.y, view.size};
            const auto* raw = reinterpret_cast<const u8*>(metadata.data());
            result.insert(result.end(), raw, raw + sizeof(metadata));
            auto colour = device.create_target(view.size, view.size, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                               SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            for (const i32 shift : {0, 24}) {
                auto frame = device.begin_frame();
                const SDL_GPUColorTargetInfo target{
                    .texture = colour.handle(), .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE};
                auto* pass = SDL_BeginGPURenderPass(frame.command_buffer(), &target, 1, nullptr);
                assert(pass);
                const SDL_GPUViewport viewport{0, 0, f32(view.size), f32(view.size), 0, 1};
                const SDL_Rect scissor{0, 0, int(view.size), int(view.size)};
                SDL_SetGPUViewport(pass, &viewport);
                SDL_SetGPUScissor(pass, &scissor);
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                const SDL_GPUTextureSamplerBinding source{renderer._local_map.handle(), sampler};
                SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
                const std::array<i32, 4> region{int(view.x), int(view.y), int(view.layer), shift};
                SDL_PushGPUFragmentUniformData(frame.command_buffer(), 0, region.data(), sizeof(region));
                SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
                SDL_EndGPURenderPass(pass);
                frame.submit();
                std::vector<u8> pixels;
                assert(device.read_texture_rgba(colour, pixels));
                for (std::size_t i = 0; i < pixels.size(); i += 4)
                    covered = covered || pixels[i] || pixels[i + 1] || pixels[i + 2];
                result.insert(result.end(), pixels.begin(), pixels.end());
            }
        }
        SDL_ReleaseGPUSampler(device.handle(), sampler);
        assert(covered);
        return result;
    }
};
} // namespace kin::gpu
using namespace kin;
namespace {
constexpr Vec2i size{160, 120};
constexpr Rectf rect{0, 0, 160, 120};
// Independent cell-centric oracle: directly test every light's box at each
// cell and append source IDs in order, without the builder's axis masks.
void grid_lists() {
    using namespace kin::gpu;
    LightGridLists grid;
    const auto check = [&](std::span<const LightGridBox> boxes, u32 across, u32 down, u32 slices) {
        std::vector<u32> expected, indices;
        bool fits = true;
        if (boxes.empty()) {
            expected.push_back(0);
        } else {
            expected.resize(across * down * slices);
            u32 cell = 0;
            for (u32 z = 0; z < slices && fits; ++z)
                for (u32 y = 0; y < down && fits; ++y)
                    for (u32 x = 0; x < across && fits; ++x) {
                        const u32 first = static_cast<u32>(indices.size());
                        for (u32 i = 0; i < boxes.size(); ++i) {
                            const auto& b = boxes[i];
                            if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1 && z >= b.z0 && z <= b.z1)
                                indices.push_back(i);
                        }
                        expected[cell++] = (first << 12) | (static_cast<u32>(indices.size()) - first);
                        fits = indices.size() < LightGridLists::max_indices;
                    }
            if (fits)
                expected.insert(expected.end(), indices.begin(), indices.end());
            else
                expected.assign(1, 0);
        }
        assert(grid.build(boxes, across, down, slices) == fits);
        assert(grid.data == expected);
    };
    check({}, 1, 1, 1);
    for (u32 n : {1u, 31u, 32u, 63u, 64u, 65u, 127u, 128u, 255u, 256u, 257u, 511u, 512u}) {
        std::vector<LightGridBox> boxes(n, {0, 6, 0, 4, 0, 8});
        check(boxes, 7, 5, 9); // dense coverage and every mask-word boundary
        for (u32 i = 0; i < n; ++i)
            boxes[i] = i % 3 ? LightGridBox{i % 7, i % 7, i % 5, i % 5, i % 9, i % 9} : LightGridBox{};
        check(boxes, 7, 5, 9); // empty boxes, holes and isolated boundary cells
    }
    std::mt19937 random(0x19d63a);
    for (u32 trial = 0; trial < 160; ++trial) {
        const u32 across = 1 + random() % 21, down = 1 + random() % 13, slices = 1 + random() % 32;
        std::vector<LightGridBox> boxes(random() % 513);
        for (auto& b : boxes)
            if (random() % 5) {
                b.x0 = random() % across;
                b.x1 = b.x0 + random() % (across - b.x0);
                b.y0 = random() % down;
                b.y1 = b.y0 + random() % (down - b.y0);
                b.z0 = random() % slices;
                b.z1 = b.z0 + random() % (slices - b.z0);
            }
        check(boxes, across, down, slices); // reuse across changing dimensions/counts
    }
    std::vector<LightGridBox> limit(512, {0, 0, 0, 0, 0, 2047});
    limit.back().z1 = 2046;
    check(limit, 1, 1, 2048); // exactly max_indices - 1 is admitted
    limit.back().z1 = 2047;
    check(limit, 1, 1, 2048); // equality selects the original overflow fallback
    check(limit, 2, 1, 2048); // uncovered trailing cells cannot change overflow
    limit.back().x1 = 1;
    check(limit, 2, 1, 2048); // above the global index limit also falls back
    limit.assign(512, {});
    check(limit, 2, 3, 4); // nonempty light input, all projected boxes empty
    check({}, 1, 1, 1);    // no stale headers/IDs after overflow and reuse
    std::cout << "light grid: exact packed lists, mask boundaries, reuse and overflow passed\n";
}

// Check both the original pointer API and reusable packets against independent
// scalar decisions, including partial group proofs and ordered NaN comparisons.
void shadow_packets() {
    using namespace kin::gpu::sphere_cull4;
    const std::array<Vec4f, 7> planes{{{1, 0, 0, 1},
                                       {-1, 0, 0, 1},
                                       {0, 1, 0, 1},
                                       {0, -1, 0, 1},
                                       {0, 0, 1, 1},
                                       {0, 0, -1, 1},
                                       {.13f, -.27f, .63f, .41f}}};
    const Vec3f centre{0, -.2f, .1f};
    constexpr f32 radius = .75f;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN(), inf = std::numeric_limits<f32>::infinity();
    const std::array edge{Vec4f{0, 0, 0, 0},
                          Vec4f{1, 1, 1, 0},
                          Vec4f{2, 0, 0, 1},
                          Vec4f{std::nextafter(2.f, inf), 0, 0, 1},
                          Vec4f{nan, 0, 0, 1},
                          Vec4f{0, nan, 0, 1},
                          Vec4f{0, 0, 0, nan},
                          Vec4f{inf, 0, 0, 1},
                          Vec4f{-inf, 0, 0, 1},
                          Vec4f{0, 0, 0, inf},
                          Vec4f{-0.f, 0, -0.f, -0.f},
                          Vec4f{1, -.2f, .1f, .25f},
                          Vec4f{std::nextafter(1.f, inf), -.2f, .1f, .25f}};
    std::mt19937 random(0x431bf);
    std::uniform_real_distribution<f32> coordinate(-3.f, 3.f);
    for (u32 count = 0; count <= planes.size(); ++count)
        for (const bool bounded : {false, true}) {
            const Test test{{planes.data(), count}, centre, radius, bounded};
            for (u32 trial = 0; trial < 512; ++trial) {
                const u8 active = static_cast<u8>(random() & ((1u << count) - 1));
                const bool test_radius = random() % 2;
                std::array<Vec4f, 4> spheres;
                for (u32 i = 0; i < spheres.size(); ++i)
                    spheres[i] = trial < 4 * edge.size() ? edge[(trial + i) % edge.size()]
                                                         : Vec4f{coordinate(random), coordinate(random),
                                                                 coordinate(random), std::abs(coordinate(random))};
                const auto reference = [&](u8 mask, bool reach) {
                    u32 result = 0;
                    for (u32 i = 0; i < spheres.size(); ++i) {
                        const auto& sp = spheres[i];
                        const Vec3f c{sp.x, sp.y, sp.z};
                        if (bounded && reach) {
                            const Vec3f d = c - centre;
                            const f32 limit = sp.w + radius;
                            if (dot(d, d) > limit * limit)
                                continue;
                        }
                        bool inside = true;
                        for (u32 k = 0; k < count; ++k) {
                            const auto& p = planes[k];
                            if ((mask & (1u << k)) && p.x * c.x + p.y * c.y + p.z * c.z + p.w < -sp.w) {
                                inside = false;
                                break;
                            }
                        }
                        if (inside)
                            result |= 1u << i;
                    }
                    return result;
                };
                const Packet packet{spheres.data()};
                assert(test.mask(packet, active, test_radius) == reference(active, test_radius));
                const u32 full = reference(static_cast<u8>((1u << count) - 1), true);
                assert(test.mask(packet) == full && test.mask(spheres.data()) == full);
            }
        }
    std::cout
        << "shadow packets: pointer/packet/scalar parity, partial planes, radius proofs, boundaries and NaNs passed\n";
}

void shadow_mesh_topology() {
    const auto bits = [](Vec3f p) {
        return std::array{std::bit_cast<u32>(p.x), std::bit_cast<u32>(p.y), std::bit_cast<u32>(p.z)};
    };
    const auto check = [&](const MeshData3D& source, const MeshLodChain3D* chain = nullptr) {
        const auto shadow = gpu::build_shadow_mesh(source, chain);
        std::vector<Vec3f> expected;
        if (source.indices.empty()) {
            for (const auto& vertex : source.vertices)
                expected.push_back(vertex.position);
        } else {
            for (const u32 index : source.indices)
                expected.push_back(source.vertices[index].position);
        }
        for (std::size_t l = 1; chain && l < chain->levels.size(); ++l)
            for (const u32 index : chain->levels[l].indices)
                expected.push_back(index < source.vertices.size()
                                       ? source.vertices[index].position
                                       : chain->extra_vertices[index - source.vertices.size()].position);
        assert(shadow.indices.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            assert(shadow.indices[i] < shadow.positions.size());
            assert(bits(shadow.positions[shadow.indices[i]]) == bits(expected[i]));
        }
        return shadow;
    };
    auto box = meshes::box({1, 1, 1});
    assert(box.vertices.size() == 24 && box.indices.size() == 36);
    assert(check(box).positions.size() == 8); // Normal/UV seams do not affect opaque shadow vertices.
    MeshData3D unindexed;
    for (const u32 index : box.indices)
        unindexed.vertices.push_back(box.vertices[index]);
    assert(check(unindexed).positions.size() == 8);
    box.indices.insert(box.indices.end(), {0, 0, 1}); // Keep degenerates and repeated triangles in their source order.
    assert(check(box).indices.size() == 39);
    MeshLodChain3D chain;
    chain.extra_vertices = {box.vertices[0], {{2, 2, 2}}};
    chain.levels.resize(3);
    chain.levels[1].indices = {0, 1, 24, 24, 1, 25};
    chain.levels[2].indices = {25, 24, 0};
    assert(check(box, &chain).positions.size() == 9);
    MeshData3D close;
    for (const Vec3f position : std::array{Vec3f{0, 0, 0}, Vec3f{-0.f, 0, 0}, Vec3f{1, 0, 0},
                                           Vec3f{std::nextafter(1.f, 2.f), 0, 0}, Vec3f{1, 0, 0}})
        close.vertices.push_back({position});
    close.indices = {0, 1, 2, 3, 4, 0};
    assert(check(close).positions.size() == 4); // Signed zero and adjacent floats must not be merged.
    chain.levels[1].indices[0] = 26;
    assert(gpu::build_shadow_mesh(box, &chain).indices.empty());
    box.indices[0] = static_cast<u32>(box.vertices.size());
    assert(gpu::build_shadow_mesh(box).indices.empty());
    std::cout << "shadow topology: exact ordered positions across seams, implicit indices, LODs and float boundaries\n";
}

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

void shadow_position_depth(Fixture& f) {
    // Opaque white cutouts use the original full vertex/index stream. Compare
    // its depth bits with the same geometry through the welded position stream;
    // stock/public comparisons alone would run the new recorder on both sides.
    const std::array<u8, 4> rgba{255, 255, 255, 255};
    const auto white = f.r.create_texture_from_rgba(rgba.data(), {1, 1});
    auto raw = meshes::box({.6f, .8f, .7f});
    MeshData3D unindexed;
    for (const u32 index : raw.indices)
        unindexed.vertices.push_back(raw.vertices[index]);
    const auto flat = f.r3d.create_mesh(unindexed).value_or({});
    const auto levels = f.r3d.create_mesh(meshes::sphere(.4f, 24, 12), {.levels = 3}).value_or({});
    // This index payload exceeds the small-mesh pool's admission limit.
    const auto dedicated = f.r3d.create_mesh(meshes::plane({.7f, .7f}, 80)).value_or({});
    const auto batch =
        InstanceBatch3D::create(f.box, std::vector<MeshInstance3D>{{.transform = Mat4f::translation({-.8f, .6f, .7f})},
                                                                   {.transform = Mat4f::translation({.8f, .6f, .7f})}})
            .value_or({});
    assert(white && flat && levels && dedicated && batch);
    const auto* pooled_backend = dynamic_cast<const gpu::GpuMesh3D*>(flat.backend().get());
    const auto* dedicated_backend = dynamic_cast<const gpu::GpuMesh3D*>(dedicated.backend().get());
    assert(pooled_backend && pooled_backend->has_shadow_mesh() && pooled_backend->shadow_allocation);
    assert(pooled_backend->shadow_allocation.vertices.offset > 0);
    assert(pooled_backend->shadow_allocation.vertices.bytes < unindexed.vertices.size() * 3 * sizeof(f32));
    assert(dedicated_backend && dedicated_backend->has_shadow_mesh() && !dedicated_backend->shadow_allocation);
    const auto lod = f.scene.lod;
    const auto debug = f.scene.debug;
    const std::array requests{LocalShadowRequest3D{.kind = LocalShadowKind3D::Spot},
                              LocalShadowRequest3D{.kind = LocalShadowKind3D::Point, .first_layer = 1}};
    const auto capture = [&](bool original, bool reverse, i32 level) {
        f.clear_draws();
        f.scene.lod.enabled = true;
        f.scene.debug.force_level = level;
        Material3D material{.double_sided = true};
        if (original) {
            material.blend = Blend3D::Mask;
            material.texture = white;
        }
        for (u32 i = 0; i < 5; ++i) {
            const u32 object = reverse ? 4 - i : i;
            if (object == 0)
                assert(f.scene.draw(flat, Mat4f::translation({-1, .6f, -.5f}), material));
            else if (object == 1)
                assert(f.scene.draw(levels, Mat4f::translation({1, .6f, -.5f}) * Mat4f::scaling({-.8f, 1.1f, .7f}),
                                    material));
            else if (object == 2)
                assert(f.scene.draw_instanced(batch, material));
            else if (object == 3)
                assert(f.scene.draw(dedicated, Mat4f::translation({0, .4f, 0}), material));
            else
                assert(f.scene.draw(f.box, Mat4f::translation({0, 1.2f, -.8f}),
                                    {.blend = Blend3D::Mask, .texture = white})); // Switch back to the full stream.
        }
        f.r.clear(colors::black);
        const auto prepared = f.r3d.prepare(f.scene).value_or({});
        assert(prepared && f.r3d.render_local_shadows(f.array, prepared, requests, size));
        std::vector<u8> result;
        bool covered = false;
        for (u32 layer = 0; layer < 7; ++layer)
            for (const f32 shift : {0.f, 24.f}) {
                ShaderParams params;
                params.uniforms[0] = static_cast<f32>(layer);
                params.uniforms[1] = 1;
                params.uniforms[2] = shift;
                f.r.draw_shader_surface(rect, f.probe, params, f.array);
                std::vector<u8> pixels;
                Vec2i actual;
                assert(f.r.read_rgba(rect, pixels, actual) && actual == size);
                for (std::size_t i = 0; i < pixels.size(); i += 4)
                    covered = covered || pixels[i] || pixels[i + 1] || pixels[i + 2];
                result.insert(result.end(), pixels.begin(), pixels.end());
            }
        f.r.present();
        assert(covered);
        return result;
    };
    for (const i32 level : {0, 1, 2}) {
        const auto reference = capture(true, false, level);
        for (const bool reverse : {false, true}) {
            assert(capture(false, reverse, level) == reference);
            assert(capture(true, reverse, level) == reference);
        }
    }
    f.scene.lod = lod;
    f.scene.debug = debug;
    f.objects();
    std::cout << "shadow positions: all depth bits match original-stream cutouts across LODs, batches and seven maps\n";
}

void static_shadow_depth(Fixture& f) {
    const auto lights = f.scene.lights;
    f.scene.lights.cache_shadows = true;
    f.scene.lights.spots.resize(3, f.scene.lights.spots.front());
    for (u32 i = 0; i < 3; ++i) {
        f.scene.lights.spots[i].position.x += .2f * i;
        f.scene.lights.spots[i].shadow_resolution = 64;
    }
    f.objects();
    std::array moving{MeshInstance3D{.transform = Mat4f::translation({-.8f, .6f, .7f})}};
    auto batch = InstanceBatch3D::create(f.box, {moving.begin(), moving.end()}).value_or({});
    auto skin = meshes::box({.6f, .6f, .6f});
    skin.skin.assign(skin.vertices.size(), {});
    const auto skinned = f.r3d.create_mesh(skin).value_or({});
    std::array pose{Mat4f::translation({.8f, .7f, 0})};
    assert(batch && skinned && f.scene.draw_instanced(batch) && f.scene.draw_skinned(skinned, pose, {}));
    const std::array<u8, 8> rgba{255, 255, 255, 0, 255, 255, 255, 255};
    const auto mask = f.r.create_texture_from_rgba(rgba.data(), {2, 1});
    assert(mask && f.scene.draw(f.box, Mat4f::translation({1, .5f, 1}), {.blend = Blend3D::Mask, .texture = mask}));
    gpu::GpuRenderer3D* owner = nullptr;
    {
        const auto prepared = f.r3d.prepare(f.scene).value_or({});
        const auto* backend = dynamic_cast<const gpu::GpuPreparedScene3D*>(prepared.backend());
        assert(backend);
        owner = backend->owner;
    }
    u32 frame = 0, hits = 0;
    const auto check = [&](bool require_hit) {
        moving[0].transform = Mat4f::translation({-.8f + .02f * ++frame, .6f, .7f});
        pose[0] = Mat4f::translation({.8f, .7f + .01f * frame, 0});
        const auto revision = f.scene.draw_revision();
        assert(batch.update(0, moving) && f.scene.update_joint_matrices(0, pose));
        assert(f.scene.draw_revision() == revision);
        gpu::LocalShadowDepthProbe::invalidate_full(*owner);
        const auto optimized = f.stock();
        const auto stats = f.r3d.debug_stats();
        if (require_hit) {
            assert(stats.local_shadow_static_cached > 0 && stats.local_shadow_static_skipped_triangles > 0);
            assert(stats.local_shadow_static_bytes > 0 && stats.local_shadow_static_bytes <= 64ull * 1024 * 1024);
            ++hits;
        }
        const auto depth = gpu::LocalShadowDepthProbe::read(*owner);
        auto shader = gpu::LocalShadowDepthProbe::full_redraw(*owner);
        const auto reference = f.stock();
        const auto reference_stats = f.r3d.debug_stats();
        const auto reference_depth = gpu::LocalShadowDepthProbe::read(*owner);
        gpu::LocalShadowDepthProbe::restore(*owner, std::move(shader));
        assert(optimized == reference && depth == reference_depth);
        assert(reference_stats.local_shadow_static_cached == 0);
        if (stats.local_shadow_triangles + stats.local_shadow_static_skipped_triangles !=
            reference_stats.local_shadow_triangles)
            std::cerr << "static depth frame " << frame << ": actual=" << stats.local_shadow_triangles
                      << " skipped=" << stats.local_shadow_static_skipped_triangles
                      << " reference=" << reference_stats.local_shadow_triangles
                      << " whole=" << stats.local_shadow_cached << " builds=" << stats.local_shadow_static_builds
                      << '\n';
        assert(stats.local_shadow_triangles + stats.local_shadow_static_skipped_triangles ==
               reference_stats.local_shadow_triangles);
    };
    check(false); // Observe the mixed maps.
    check(false); // Bake static geometry at its original atlas coordinates.
    check(true);
    assert(gpu::LocalShadowDepthProbe::packed_origin(*owner));
    check(true); // Batch/pose changes keep static geometry reusable.
    // Changing a fixed caster invalidates the retained depth.
    assert(f.scene.set_draw_material(1, {.double_sided = true}));
    check(false);
    check(false);
    check(true);
    // Projection changes and packed atlas reassignment must not revive stale tiles.
    f.scene.lights.spots[0].position.x += .4f;
    f.scene.lights.spots[1].shadow_resolution = 128;
    check(false);
    check(false);
    check(true);
    f.scene.lights.spots.erase(f.scene.lights.spots.begin());
    check(false);
    check(false);
    check(true);
    f.scene.cull = !f.scene.cull;
    check(false);
    check(false);
    check(true);
    f.scene.cull = !f.scene.cull;
    f.clear_draws();
    assert(f.scene.draw(f.ground, Mat4f::translation({0, -.1f, 0})));
    assert(f.scene.draw_instanced(batch) && f.scene.draw_skinned(skinned, pose, {}));
    check(false); // Removing a fixed caster must not leave its depth behind.
    check(false);
    check(true);
    const auto warm = [&] {
        moving[0].transform = Mat4f::translation({-.7f + .01f * ++frame, .6f, .7f});
        assert(batch.update(0, moving));
        f.r.clear(colors::black);
        assert(f.r3d.render(f.view, f.scene));
        f.r.present();
        assert(f.r3d.debug_stats().local_shadow_static_cached > 0);
    };
    for (int i = 0; i < 3; ++i)
        warm();
    const auto before = f.r.backend_stats();
    for (int i = 0; i < 8; ++i)
        warm();
    const auto after = f.r.backend_stats();
    assert(after.gpu_memory.allocations - before.gpu_memory.allocations ==
           after.upload_staging_allocations - before.upload_staging_allocations);
    assert(after.upload_staging_bytes <= before.upload_staging_bytes);
    f.scene.lights.cache_shadows = false;
    check(false);
    assert(f.r3d.debug_stats().local_shadow_static_bytes == 0);
    assert(hits == 7);
    f.scene.lights = lights;
    f.objects();
    std::cout << "static local shadows: exact RGBA/all depth bits, moving batches/poses, masks, invalidation, "
                 "packed origins/reassignment and cache-off passed\n";
}

void recording_ranges(Fixture& f) {
    // Independent one-object maps establish the coverage union. The combined
    // map crosses nonzero list offsets, pipelines and direct cluster ranges;
    // stock/public parity alone would exercise the same recorder twice.
    const std::array requests{LocalShadowRequest3D{.kind = LocalShadowKind3D::Spot}};
    const auto clustered = f.r3d.create_mesh(meshes::sphere(.45f, 16, 8), {.clusters = true}).value_or({});
    auto data = meshes::box({.7f, .7f, .7f});
    data.skin.assign(data.vertices.size(), {});
    const auto skinned = f.r3d.create_mesh(data).value_or({});
    const std::array<u8, 8> rgba{255, 255, 255, 0, 255, 255, 255, 255};
    const auto mask = f.r.create_texture_from_rgba(rgba.data(), {2, 1});
    const std::array<u8, 8> inverse_rgba{255, 255, 255, 255, 255, 255, 255, 0};
    const auto inverse = f.r.create_texture_from_rgba(inverse_rgba.data(), {2, 1});
    const auto batch =
        InstanceBatch3D::create(f.box, std::vector<MeshInstance3D>{{.transform = Mat4f::translation({-.6f, .5f, 1})},
                                                                   {.transform = Mat4f::translation({.6f, .5f, 1})}})
            .value_or({});
    assert(clustered && skinned && mask && inverse && batch);
    Material3D raised, higher, masked_left, masked_right;
    if (shader_compiler_available()) {
        const auto dir =
            std::filesystem::temp_directory_path() /
            ("kin-shadow-recording-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        const auto file = dir / "offset.kingraph.json";
        std::ofstream{file} << R"({"schema":"kin.material-graph/1","name":"shadow_recording",
            "parameters":[{"name":"rise","type":"float","default":0.2},
                          {"name":"mask","type":"texture","slot":"emissive"}],
            "nodes":[{"id":"lift","type":"multiply","inputs":{"a":[0,1,0],"b":"rise"}},
                     {"id":"sample","type":"texture","texture":"mask"}],
            "outputs":{"offset":"lift","alpha":"sample.a"}})";
        const auto graph = f.r3d.compile_material_graph(file);
        assert(graph && graph->graph.offset && graph->graph.cutout);
        raised.surface = graph->surface;
        raised.shader_params = material_graph_params3d(graph->graph);
        higher = raised;
        higher.shader_params = material_graph_params3d(graph->graph, {{"rise", {.75f}}});
        std::ofstream{file} << R"({"schema":"kin.material-graph/1","name":"shadow_mask",
            "parameters":[{"name":"mask","type":"texture","slot":"emissive"}],
            "nodes":[{"id":"sample","type":"texture","texture":"mask"}],
            "outputs":{"alpha":"sample.a"}})";
        const auto cutout = f.r3d.compile_material_graph(file);
        assert(cutout && cutout->graph.cutout && !cutout->graph.offset);
        masked_left = raised;
        masked_left.surface = cutout->surface;
        masked_left.shader_params = material_graph_params3d(cutout->graph);
        masked_left.blend = Blend3D::Mask;
        masked_left.emissive_texture = mask;
        masked_right = masked_left;
        masked_right.emissive_texture = inverse;
        std::filesystem::remove_all(dir);
    }
    std::vector<int> objects{0, 1, 2, 3, 4, 9};
    if (raised.surface)
        objects.insert(objects.end(), {5, 6, 7, 8});
    const auto lod = f.scene.lod;
    const auto debug = f.scene.debug;
    f.scene.lod.enabled = true;
    f.scene.debug.force_level = 0;
    f.scene.debug.gpu_culling = false;
    const auto add = [&](int object) {
        switch (object) {
        case 0:
            assert(f.scene.draw(f.box, Mat4f::translation({-1, .5f, -.8f})));
            break;
        case 1:
            assert(f.scene.draw(clustered, Mat4f::translation({0, 1.1f, -.8f}), {.double_sided = true}));
            break;
        case 2:
            assert(f.scene.draw(f.box, Mat4f::translation({1, .5f, -.8f}), {.blend = Blend3D::Mask, .texture = mask}));
            break;
        case 3: {
            const std::array pose{Mat4f::translation({0, 1.7f, .5f})};
            assert(f.scene.draw_skinned(skinned, pose, {}));
            break;
        }
        case 4:
            assert(f.scene.draw_instanced(batch));
            break;
        case 5:
            assert(f.scene.draw(f.box, Mat4f::translation({-.5f, 1.7f, -.5f}), raised));
            break;
        case 6:
            assert(f.scene.draw(f.box, Mat4f::translation({1, 1.5f, .5f}), higher));
            break;
        case 7:
            assert(f.scene.draw(f.box, Mat4f::translation({-1, .5f, .5f}), masked_left));
            break;
        case 8:
            assert(f.scene.draw(f.box, Mat4f::translation({1, .5f, .5f}), masked_right));
            break;
        case 9:
            assert(f.scene.draw(f.box, Mat4f::translation({-1, .5f, 1}),
                                {.blend = Blend3D::Mask, .texture = mask, .uv_offset = {1, 0}}));
            break;
        }
    };
    const auto coverage = [&] {
        f.r.clear(colors::black);
        const auto prepared = f.r3d.prepare(f.scene).value_or({});
        assert(prepared && f.r3d.render_local_shadows(f.array, prepared, requests, size));
        f.r.draw_shader_surface(rect, f.probe, {}, f.array);
        std::vector<u8> pixels;
        Vec2i actual;
        assert(f.r.read_rgba(rect, pixels, actual) && actual == size);
        f.r.present();
        return pixels;
    };
    std::array<std::vector<u8>, 10> reference;
    for (const int object : objects) {
        f.clear_draws();
        add(object);
        const auto pixels = coverage();
        assert(std::count(pixels.begin(), pixels.end(), u8{255}) > size.x * size.y + 30);
        reference[object] = pixels;
    }
    const auto check = [&](std::span<const int> group) {
        std::vector<u8> expected(reference[0].size());
        for (const int object : group)
            for (std::size_t i = 0; i < expected.size(); ++i)
                expected[i] = std::max(expected[i], reference[object][i]);
        for (const bool reverse : {false, true}) {
            f.clear_draws();
            for (std::size_t i = 0; i < group.size(); ++i)
                add(group[reverse ? group.size() - 1 - i : i]);
            assert(coverage() == expected);
        }
    };
    check(objects);
    // Isolate each compatibility distinction so another caster cannot hide it.
    for (const auto pair : {std::array{0, 5}, std::array{5, 6}, std::array{7, 8}, std::array{2, 9}})
        if (!reference[pair[0]].empty() && !reference[pair[1]].empty())
            check(pair);
    if (raised.surface) {
        // Fill complete retained windows, including the surface refs, then
        // exercise compiled shadow-plan reuse and material revision changes.
        f.scene.lod.enabled = false;
        f.scene.debug.force_level = -1;
        f.scene.debug.cache_draw_commands = false; // isolate shadow-plan reuse
        f.clear_draws();
        std::size_t surface_ref = 0;
        for (int i = 0; i < 256; ++i) {
            if (i == 32) {
                surface_ref = f.scene.draws().size();
                for (const int object : {5, 6, 7, 8, 9})
                    add(object);
            }
            // Different colours keep Scene3D from coalescing the submissions;
            // they still share one compatible opaque shadow state.
            assert(f.scene.draw(f.box, Mat4f::translation({-1, .5f, -.8f}),
                                {.color = i % 2 ? Color::rgb(255, 0, 0) : colors::white}));
        }
        const std::array full_requests{LocalShadowRequest3D{.kind = LocalShadowKind3D::Spot},
                                       LocalShadowRequest3D{.kind = LocalShadowKind3D::Point, .first_layer = 1}};
        const auto warm = [&] {
            for (int i = 0; i < 4; ++i)
                f.stock();
            assert(f.r3d.debug_stats().direct_retained_instances > 0);
            assert(f.r3d.debug_stats().local_sort_cache_hits > 0);
            assert(f.r3d.debug_stats().local_shadow_draws < 64);
            const auto expected = f.custom(full_requests);
            assert(f.stock() == expected && f.r3d.debug_stats().local_sort_cache_hits > 0);
        };
        warm();
        assert(f.scene.set_draw_material(surface_ref, higher));
        warm();
    }
    // Ninety-six distinct cutout states repeat across locality windows. Compare
    // every depth bit against a small independent map, including special paths
    // as barriers. Repeated coincident copies cannot change the depth result.
    f.scene.lod.enabled = false;
    f.scene.debug.force_level = -1;
    const auto depth = [&](u32 repeats, bool reverse) {
        f.clear_draws();
        for (u32 repeat = 0; repeat < repeats; ++repeat) {
            for (u32 row = 0; row < 96; ++row) {
                const u32 id = reverse ? 95 - row : row;
                assert(f.scene.draw(f.box,
                                    Mat4f::translation({f32(id % 12) * .25f - 1.4f, .2f + f32(id / 12) * .2f, -.4f}),
                                    {.color = Color::rgba(255, 255, 255, id % 2 ? 128 : 255),
                                     .double_sided = id % 3 == 0,
                                     .blend = Blend3D::Mask,
                                     .texture = mask,
                                     .alpha_cutoff = .1f + f32(id) * .008f}));
            }
            if (repeat == repeats / 2)
                for (const int object : objects)
                    add(object);
        }
        f.r.clear(colors::black);
        const auto prepared = f.r3d.prepare(f.scene).value_or({});
        assert(prepared && f.r3d.render_local_shadows(f.array, prepared, requests, size));
        assert(f.r3d.debug_stats().local_shadow_draws <= 192 + objects.size());
        std::vector<u8> bits;
        for (const f32 shift : {0.f, 24.f}) {
            ShaderParams params;
            params.uniforms[1] = 1;
            params.uniforms[2] = shift;
            f.r.draw_shader_surface(rect, f.probe, params, f.array);
            std::vector<u8> pixels;
            Vec2i actual;
            assert(f.r.read_rgba(rect, pixels, actual) && actual == size);
            bits.insert(bits.end(), pixels.begin(), pixels.end());
        }
        f.r.present();
        return bits;
    };
    const auto independent = depth(1, false);
    for (const bool reverse : {false, true})
        assert(depth(11, reverse) == independent);
    std::cout << "local graph: map-wide stock batches preserve all depth bits across windows and special barriers\n";
    f.scene.lod = lod;
    f.scene.debug = debug;
    f.objects();
    std::cout << "local graph: combined shadow ranges match independent opaque/mask/skin/cluster/batch coverage\n";
    if (raised.surface)
        std::cout << "local graph: shared-mesh surface params/maps and cutout UVs survive batching and cached plans\n";
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
    // One cube more than max_point_shadow_maps.
    f.scene.lights.points.resize(max_point_shadow_maps + 1, f.scene.lights.points[0]);
    const auto more = f.r3d.prepare(f.scene).value_or({});
    std::vector<LocalShadowRequest3D> too_many_points;
    for (u32 i = 0; i <= max_point_shadow_maps; ++i)
        too_many_points.push_back({.kind = LocalShadowKind3D::Point, .light = i, .first_layer = 6 * i});
    assert(!f.r3d.render_local_shadows(f.array, more, too_many_points, size));
    maximum.clear();
    for (u32 i = 0; i < 9; ++i)
        maximum.push_back({.light = i, .first_layer = i});
    assert(!f.r3d.render_local_shadows(f.array, p, maximum, size));
    f.r.present();
    std::cout << "local graph: validation, historical/queued lifetime and the full bounded profile\n";
}
} // namespace
int main() {
    grid_lists();
    shadow_packets();
    shadow_mesh_topology();
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
    shadow_position_depth(f);
    static_shadow_depth(f);
    recording_ranges(f);
    cube_and_camera(f);
    grid_and_history(f);
    hooks(f);
    validation(f);
}
