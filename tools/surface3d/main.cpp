// Public graph: shadows + compute lighting data -> toon material -> GPU ASCII -> presentation.
#include "surface3d_shader.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/graph_resources.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <stdexcept>
using namespace kin;
int main(int argc, char** argv) {
    const bool test = argc > 1 && std::string_view(argv[1]) == "--test";
    try {
        const auto check = [](bool ok, const char* message) {
            if (!ok)
                throw std::runtime_error(message);
        };
        constexpr Vec2i size{640, 448};
        constexpr Rectf rect{0, 0, 640, 448};
        App app{{.max_fps = 60}};
        auto& window =
            app.create_window({.title = "Kin shadow/surface graph", .width = size.x, .height = size.y, .hidden = test});
        Renderer2D renderer{make_render_backend(window, false, true)};
        Renderer3D r3d{renderer};
        const auto features = available_graph_features(renderer);
        if (!r3d.capabilities().surface_pass || !r3d.capabilities().sun_shadow_pass ||
            !r3d.capabilities().local_shadow_pass || !r3d.capabilities().material_inputs ||
            !(features & graph_feature::attachment_arrays) || !(features & graph_feature::compute) ||
            !(features & graph_feature::buffers)) {
            std::cerr
                << "Surface graph requires native material inputs, shadows, attachment arrays and compute buffers\n";
            return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
        }
        const auto cube = r3d.create_mesh(meshes::box({2, 2, 2})).value_or({});
        const auto ground = r3d.create_mesh(meshes::box({12, .1f, 12})).value_or({});
        const auto ascii = renderer.builtin_shader(BuiltinShader::Ascii);
        const auto bytes = surface3d_shader("toon.frag.spv");
        const auto toon = r3d.create_material_shader(bytes).value_or({});
        const auto field_bytes = surface3d_shader("field.comp.spv");
        const auto field_shader =
            renderer.create_compute_shader({field_bytes.data(), static_cast<u32>(field_bytes.size())});
        check(cube && ground && ascii && toon && field_shader, "surface graph resources unavailable");
        ResourceGraphDesc graph;
        graph.resources = {{.name = "sun",
                            .kind = GraphResourceKind::RenderAttachment,
                            .format = TextureFormat::Depth32Float,
                            .size = {512, 512},
                            .array_layers = 4},
                           {.name = "colour", .kind = GraphResourceKind::ColorTarget, .size = size},
                           {.name = "depth",
                            .kind = GraphResourceKind::RenderAttachment,
                            .format = TextureFormat::Depth32Float,
                            .size = size},
                           {.name = "glyphs", .kind = GraphResourceKind::ColorTarget, .size = size},
                           {.name = "output", .kind = GraphResourceKind::ColorTarget, .size = size, .exported = true},
                           {.name = "local",
                            .kind = GraphResourceKind::RenderAttachment,
                            .format = TextureFormat::Depth32Float,
                            .size = {256, 256},
                            .array_layers = 7},
                           {.name = "palette", .kind = GraphResourceKind::Buffer, .bytes = sizeof(Vec4f)},
                           {.name = "light_pattern",
                            .kind = GraphResourceKind::StorageTexture,
                            .format = TextureFormat::Rgba16Float,
                            .size = {64, 64}}};
        graph.passes = {{.name = "material_data",
                         .writes = {6, 7},
                         .required_features = graph_feature::compute | graph_feature::buffers},
                        {.name = "sun",
                         .writes = {0},
                         .required_features = graph_feature::rendering3d | graph_feature::attachment_arrays},
                        {.name = "local",
                         .writes = {5},
                         .required_features = graph_feature::rendering3d | graph_feature::attachment_arrays},
                        {.name = "surface",
                         .reads = {0, 5, 6, 7},
                         .writes = {1, 2},
                         .required_features = graph_feature::rendering3d},
                        {.name = "ascii", .reads = {1}, .writes = {3}, .required_features = graph_feature::shaders},
                        {.name = "output", .reads = {3}, .writes = {4}, .required_features = graph_feature::raster}};
        ResourceGraphCompiler compiler;
        GraphResources resources;
        Scene3D scene;
        scene.camera = Camera3D::look_at({4, 3, 6}, {});
        scene.lights.shadow_cascades = 4;
        scene.lights.shadow_area = {{-8, -2, -8}, {8, 6, 8}};
        const std::array local_requests{LocalShadowRequest3D{.kind = LocalShadowKind3D::Spot, .first_layer = 0},
                                        LocalShadowRequest3D{.kind = LocalShadowKind3D::Point, .first_layer = 1}};
        Material3D ground_material{.shader = toon, .shader_params = {4}};
        Material3D cube_material{.color = Color::rgb(230, 140, 60), .shader = toon, .shader_params = {4}};
        ShaderParams params;
        params.uniforms[0] = 80;
        params.uniforms[1] = 32;
        params.uniforms[2] = 1;
        params.uniforms[3] = 1.5f;
        MaterialInputs3D inputs;
        Texture bound_pattern;
        std::shared_ptr<IDataBufferBackend> bound_palette;
        f32 angle = 20;
        const auto picture = [&](bool reference) {
            scene.clear();
            scene.lights.points.push_back({.position = {-2, 3, 2}, .color = {8, 4, 2}, .radius = 12});
            scene.lights.spots.push_back(
                {.position = {3, 4, 2}, .direction = {-.5f, -1, -.5f}, .color = {4, 6, 10}, .radius = 12});

            renderer.clear(colors::black);
            const auto& plan =
                compiler.compile(graph, features, {.cull_unused = !reference, .reuse_slots = !reference});
            check(resources.prepare(renderer, graph, plan), "graph allocation failed");
            if (bound_pattern != resources.texture(7) || bound_palette != resources.buffer(6).backend()) {
                const std::array textures{resources.texture(7)};
                const std::array buffers{resources.buffer(6)};
                inputs = r3d.create_material_inputs(toon, textures, buffers).value_or({});
                check(bool(inputs), "material input layout/ownership rejected");
                bound_pattern = textures[0];
                bound_palette = buffers[0].backend();
            }
            ground_material.inputs = cube_material.inputs = inputs;
            check(bool(scene.draw(ground, Mat4f::translation({0, -1.1f, 0}), ground_material)), "ground rejected");
            check(bool(scene.draw(cube, Mat4f::rotation_y(angle), cube_material)), "cube rejected");

            auto prepared = r3d.prepare(scene).value_or({});
            check(bool(prepared), "scene preparation failed");
            SunShadow3D shadow;
            LocalShadows3D local;
            const std::array<std::function<bool()>, 6> operations{
                [&] {
                    const std::array outputs{resources.texture(7)};
                    const std::array buffers{resources.buffer(6)};
                    ShaderParams params;
                    params.uniforms[0] = angle / 20.f;
                    return renderer.dispatch_compute(
                        field_shader, {64, 64}, {.outputs = outputs, .output_buffers = buffers, .params = &params});
                },
                [&] {
                    auto result = r3d.render_sun_shadow(resources.texture(0), prepared, size);
                    if (!result)
                        return false;
                    shadow = *result;
                    return true;
                },
                [&] {
                    auto result = r3d.render_local_shadows(resources.texture(5), prepared, local_requests, size);
                    if (!result)
                        return false;
                    local = std::move(*result);
                    return true;
                },
                [&] {
                    return bool(r3d.render_surface(
                        {resources.target(1), resources.texture(2)}, prepared,
                        {.background = Color::rgb(20, 25, 35), .sun_shadow = &shadow, .local_shadows = &local}));
                },
                [&] {
                    auto target = renderer.scoped_render_target(resources.target(3));
                    renderer.clear(colors::black);
                    renderer.draw_shader_surface(rect, ascii, params, resources.texture(1));
                    return true;
                },
                [&] {
                    auto target = renderer.scoped_render_target(resources.target(4));
                    renderer.clear(colors::black);
                    renderer.draw_texture(resources.texture(3), rect);
                    return true;
                }};
            check(execute_resource_graph(plan, operations), "graph recording failed");
            renderer.draw_texture(resources.texture(4), rect);
        };
        if (test) {
            picture(false);
            std::vector<u8> a, b;
            Vec2i actual;
            check(renderer.read_rgba(rect, a, actual), "optimized readback failed");
            renderer.present();
            picture(true);
            check(renderer.read_rgba(rect, b, actual), "reference readback failed");
            renderer.present();
            check(a == b, "optimized/reference surface pixels differ");
            check(std::count_if(a.begin(), a.end(), [](u8 x) { return x > 20 && x < 255; }) > 1000, "missing glyphs");
            picture(false);
            renderer.present();
            const auto allocations = renderer.backend_stats().gpu_memory.allocations;
            const auto compilations = compiler.stats().compilations;
            for (int i = 0; i < 8; ++i) {
                picture(false);
                renderer.present();
            }
            check(renderer.backend_stats().gpu_memory.allocations == allocations,
                  "stable frames allocated GPU handles");
            check(compiler.stats().compilations == compilations, "stable graph recompiled");
            const auto& stats = r3d.debug_stats();
            check(stats.device_waits == 0 && stats.shadow_draws == 0 && stats.taa_dispatches == 0 &&
                      stats.gi_dispatches == 0,
                  "surface recorded implicit effects or waited");
            const auto array = resources.texture(0);
            check(renderer.set_color_space(ColorSpace::Linear, true), "HDR unavailable");
            for (auto& resource : graph.resources)
                if (resource.kind == GraphResourceKind::ColorTarget)
                    resource.hdr = true;
            picture(false);
            renderer.present();
            check(resources.texture(0) == array, "HDR transition replaced data array");
            std::cout << "Surface graph: exact reference pixels, cached plans, stable allocations, HDR, explicit "
                         "sun/local shadows, GPU material texture/buffer inputs, toon lighting and ASCII\n";
        } else
            app.run([&](f32 dt) { angle += dt * 20; },
                    [&](f32) {
                        picture(false);
                        renderer.present();
                    });
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
