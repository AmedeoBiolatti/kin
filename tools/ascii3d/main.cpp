// Public-API custom pipeline: stock 3D output -> GPU ASCII glyphs -> presentation.
// --test checks the optimized/reference images and exits; otherwise a live demo.
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
        const auto check = [](bool condition, const char* message) {
            if (!condition)
                throw std::runtime_error(message);
        };
        App app{{.max_fps = 60}};
        auto& window = app.create_window({.title = "Kin GPU ASCII graph", .width = 640, .height = 448, .hidden = test});
        Renderer2D renderer{make_render_backend(window, false, true)};
        const auto features = available_graph_features(renderer);
        if ((features & (graph_feature::raster | graph_feature::shaders | graph_feature::rendering3d)) !=
            (graph_feature::raster | graph_feature::shaders | graph_feature::rendering3d)) {
            std::cerr << "ASCII pipeline requires raster targets, custom shaders and 3D\n";
            return std::getenv("KIN_REQUIRE_GPU_TESTS") ? 1 : 77;
        }
        Renderer3D r3d{renderer};
        auto view = r3d.create_view({640, 448}, {.samples = 1, .background = Color::rgb(20, 25, 35)}).value_or({});
        auto cube = r3d.create_mesh(meshes::box({2, 2, 2})).value_or({});
        if (!view || !cube)
            throw std::runtime_error("3D resources unavailable");
        const auto shader = renderer.builtin_shader(BuiltinShader::Ascii);
        if (!shader)
            throw std::runtime_error("ASCII shader unavailable");
        ShaderParams params;
        params.uniforms[0] = 80;
        params.uniforms[1] = 32;
        params.uniforms[2] = 1;
        params.uniforms[3] = 1.5f;
        ResourceGraphDesc graph;
        graph.resources = {
            {.name = "view", .kind = GraphResourceKind::ColorTarget, .size = {640, 448}, .imported = true},
            {.name = "copy", .kind = GraphResourceKind::ColorTarget, .size = {640, 448}},
            {.name = "glyphs", .kind = GraphResourceKind::ColorTarget, .size = {640, 448}},
            {.name = "graded", .kind = GraphResourceKind::ColorTarget, .size = {640, 448}},
            {.name = "output", .kind = GraphResourceKind::ColorTarget, .size = {640, 448}, .exported = true}};
        graph.passes = {{.name = "copy", .reads = {0}, .writes = {1}, .required_features = graph_feature::raster},
                        {.name = "glyphs", .reads = {1}, .writes = {2}, .required_features = graph_feature::shaders},
                        {.name = "grade", .reads = {2}, .writes = {3}, .required_features = graph_feature::raster},
                        {.name = "output", .reads = {3}, .writes = {4}, .required_features = graph_feature::raster}};
        ResourceGraphCompiler compiler;
        GraphResources resources;
        Scene3D scene;
        f32 angle = 20;
        scene.camera = Camera3D::look_at({4, 3, 6}, {});
        const auto picture = [&](bool reference) {
            scene.clear();
            if (!scene.draw(cube, Mat4f::rotation_y(angle), {.color = Color::rgb(230, 140, 60)}))
                throw std::runtime_error("scene draw rejected");
            renderer.clear(colors::black);
            if (!r3d.render(view, scene))
                throw std::runtime_error("3D render failed");
            const auto& plan =
                compiler.compile(graph, features, {.cull_unused = !reference, .reuse_slots = !reference});
            const std::array imports{GraphImport{.resource = 0, .texture = view.texture()}};
            if (!resources.prepare(renderer, graph, plan, imports))
                throw std::runtime_error(resources.diagnostics().front());
            const std::array<std::function<bool()>, 4> operations{
                {[&] {
                     auto target = renderer.scoped_render_target(resources.target(1));
                     renderer.clear(colors::black);
                     renderer.draw_texture(resources.texture(0), {0, 0, 640, 448});
                     return true;
                 },
                 [&] {
                     auto target = renderer.scoped_render_target(resources.target(2));
                     renderer.clear(colors::black);
                     renderer.draw_shader_surface({0, 0, 640, 448}, shader, params, resources.texture(1));
                     return true;
                 },
                 [&] {
                     auto target = renderer.scoped_render_target(resources.target(3));
                     renderer.clear(colors::black);
                     renderer.draw_texture(resources.texture(2), {0, 0, 640, 448});
                     return true;
                 },
                 [&] {
                     auto target = renderer.scoped_render_target(resources.target(4));
                     renderer.clear(colors::black);
                     renderer.draw_texture(resources.texture(3), {0, 0, 640, 448});
                     return true;
                 }}};
            if (test) {
                check(plan.slots.size() == (reference ? 4u : 3u), "unexpected graph slot count");
                check((resources.texture(1) == resources.texture(3)) == !reference,
                      "physical target reuse differs from plan");
            }
            if (!execute_resource_graph(plan, operations))
                throw std::runtime_error("graph execution failed");
            renderer.draw_texture(resources.texture(4), {0, 0, 640, 448});
        };
        if (test) {
            std::vector<u8> optimized, reference;
            Vec2i size;
            picture(false);
            if (!renderer.read_rgba({0, 0, 640, 448}, optimized, size))
                return 1;
            renderer.present();
            picture(true);
            if (!renderer.read_rgba({0, 0, 640, 448}, reference, size))
                return 1;
            renderer.present();
            check(optimized == reference, "optimized/reference pixels differ");
            const auto bright =
                std::count_if(optimized.begin(), optimized.end(), [](u8 x) { return x > 20 && x < 255; });
            check(bright > 1000, "ASCII image has insufficient visible glyphs");
            picture(false);
            renderer.present();
            const auto allocations = renderer.backend_stats().gpu_memory.allocations;
            for (int i = 0; i < 8; ++i) {
                picture(false);
                renderer.present();
            }
            check(compiler.stats().cache_hits >= 8, "stable frames did not reuse the compiled plan");
            // Recording stable graph targets itself must not allocate new resource handles.
            check(renderer.backend_stats().gpu_memory.allocations == allocations,
                  "stable frames allocated resource handles");
            check(renderer.set_color_space(ColorSpace::Linear, true), "HDR working space unavailable");
            const auto& stale = compiler.compile(graph, features);
            const std::array hdr_imports{GraphImport{.resource = 0, .texture = view.texture()}};
            check(!resources.prepare(renderer, graph, stale, hdr_imports), "stale HDR declaration accepted");
            for (auto& resource : graph.resources)
                resource.hdr = true;
            picture(false);
            renderer.present();
            const auto& hdr_plan = compiler.compile(graph, features);
            check(hdr_plan.slot_payload_bytes == 3 * 640 * 448 * 8, "HDR payload estimate is wrong");
            std::cout << "GPU ASCII: exact optimized/reference pixels, cached plans and stable resource allocations\n";
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
