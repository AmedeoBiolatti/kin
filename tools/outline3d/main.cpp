// Public graph: opaque/cutout geometry -> depth/normal outline shader -> output.
#include "outline3d_shader.hpp"

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
        App app{{.max_fps = 60}};
        auto& window =
            app.create_window({.title = "Kin depth/normal graph", .width = 640, .height = 448, .hidden = test});
        Renderer2D renderer{make_render_backend(window, false, true)};
        Renderer3D r3d{renderer};
        const auto features = available_graph_features(renderer);
        if (!r3d.capabilities().geometry_pass || !(features & graph_feature::attachments)) {
            std::cerr << "Outline graph requires standalone geometry and sampled float/depth attachments\n";
            return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
        }
        const auto bytes = outline3d_shader("outline.frag.spv");
        const auto shader = renderer.create_shader(
            {.spirv = {bytes.data(), static_cast<u32>(bytes.size())}, .num_samplers = 2, .num_uniform_buffers = 1});
        check(bool(shader), "outline shader unavailable");
        auto cube = r3d.create_mesh(meshes::box({2, 2, 2})).value_or({});
        check(bool(cube), "mesh unavailable");
        ResourceGraphDesc graph;
        graph.resources = {
            {.name = "depth",
             .kind = GraphResourceKind::RenderAttachment,
             .format = TextureFormat::Depth32Float,
             .size = {640, 448}},
            {.name = "normals",
             .kind = GraphResourceKind::RenderAttachment,
             .format = TextureFormat::Rg16Float,
             .size = {640, 448}},
            {.name = "outline", .kind = GraphResourceKind::ColorTarget, .size = {640, 448}},
            {.name = "output", .kind = GraphResourceKind::ColorTarget, .size = {640, 448}, .exported = true}};
        graph.passes = {
            {.name = "geometry",
             .writes = {0, 1},
             .required_features = graph_feature::rendering3d | graph_feature::attachments},
            {.name = "outline", .reads = {0, 1}, .writes = {2}, .required_features = graph_feature::shaders},
            {.name = "output", .reads = {2}, .writes = {3}, .required_features = graph_feature::raster}};
        ResourceGraphCompiler compiler;
        GraphResources resources;
        Scene3D scene;
        scene.camera = Camera3D::look_at({4, 3, 6}, {});
        ShaderParams params;
        params.uniforms[0] = 2;
        params.uniforms[1] = 0.04f;
        params.uniforms[2] = 0.8f;
        f32 angle = 20;
        const auto picture = [&](bool reference) {
            scene.clear();
            check(bool(scene.draw(cube, Mat4f::rotation_y(angle))), "scene draw rejected");
            renderer.clear(colors::black);
            const auto& plan =
                compiler.compile(graph, features, {.cull_unused = !reference, .reuse_slots = !reference});
            check(resources.prepare(renderer, graph, plan), "graph resource allocation failed");
            const std::array<std::function<bool()>, 3> operations{
                [&] { return bool(r3d.render_geometry({resources.texture(0), resources.texture(1)}, scene)); },
                [&] {
                    auto target = renderer.scoped_render_target(resources.target(2));
                    const std::array sources{resources.texture(0), resources.texture(1)};
                    renderer.draw_shader_surface({0, 0, 640, 448}, shader, params, sources);
                    return true;
                },
                [&] {
                    auto target = renderer.scoped_render_target(resources.target(3));
                    renderer.draw_texture(resources.texture(2), {0, 0, 640, 448});
                    return true;
                }};
            check(execute_resource_graph(plan, operations), "graph recording failed");
            renderer.draw_texture(resources.texture(3), {0, 0, 640, 448});
        };
        if (test) {
            picture(false);
            std::vector<u8> a, b;
            Vec2i size;
            check(renderer.read_rgba({0, 0, 640, 448}, a, size), "optimized readback failed");
            renderer.present();
            picture(true);
            check(renderer.read_rgba({0, 0, 640, 448}, b, size), "reference readback failed");
            renderer.present();
            check(a == b, "optimized/reference outline pixels differ");
            u32 filled = 0, outlined = 0;
            for (std::size_t i = 0; i < a.size(); i += 4) {
                filled += a[i + 2] > 40;
                outlined += a[i] < 5 && a[i + 2] < 10;
            }
            check(filled > 1000 && outlined > 300, "outline image is missing surfaces or edges");
            picture(false);
            renderer.present();
            const auto depth = resources.texture(0), normals = resources.texture(1);
            const auto allocations = renderer.backend_stats().gpu_memory.allocations;
            const auto compilations = compiler.stats().compilations;
            for (int i = 0; i < 4; ++i) {
                picture(false);
                renderer.present();
            }
            check(resources.texture(0) == depth && resources.texture(1) == normals, "stable attachments changed");
            check(renderer.backend_stats().gpu_memory.allocations == allocations,
                  "stable graph allocated GPU resources");
            check(compiler.stats().compilations == compilations, "stable graph recompiled");
            check(r3d.debug_stats().geometry_draws > 0 && r3d.stats().draws == 0 && r3d.stats().shadow_draws == 0 &&
                      r3d.debug_stats().device_waits == 0,
                  "standalone pass recorded unrelated work or waited");
            std::cout
                << "Outline graph: exact reference pixels, cached plans, stable allocations; no stock colour pass\n";
        } else
            app.run([&](f32 dt) { angle += dt * 20; },
                    [&](f32) {
                        picture(false);
                        renderer.present();
                    });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
