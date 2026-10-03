// Times drawing many quads a frame on the SDL_GPU backend, three ways:
// draw_texture calls, fill_rect calls, and one draw_sprites batch.
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Two "runs" of a shader drawn as triangles with Max blending (not the
// pipeline create_shader makes by itself): the first keeps its pipeline
// record, the second hands it to prewarm_pipelines before making the shader.
// Run with __GL_SHADER_DISK_CACHE=0 for a cold driver cache.
int pipeline_runs(kin::Window& window) {
    using clock = std::chrono::steady_clock;
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::string record;
    for (int run = 0; run < 2; ++run) {
        std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
        auto renderer = std::make_unique<kin::Renderer2D>(std::move(backend));
        const auto t0 = clock::now();
        renderer->prewarm_pipelines(record);
        const kin::ShaderHandle shader = renderer->builtin_shader(kin::BuiltinShader::Vignette);
        const auto t1 = clock::now();
        const std::array<kin::ShaderVertex, 3> triangle{{{.position = {0, 0}}, {.position = {64, 0}}, {.position = {0, 64}}}};
        const auto frame = [&] {
            renderer->clear(kin::Color::rgb(0, 0, 0));
            const auto blend = renderer->scoped_blend_mode(kin::BlendMode::Max);
            renderer->draw_shader_geometry(triangle, {}, shader, {});
            renderer->present();
        };
        frame();
        const auto t2 = clock::now();
        frame();
        const auto t3 = clock::now();
        std::printf("run %d (%s): prewarm + shader %.2f ms, first frame %.2f ms, second %.2f ms\n", run + 1,
                    run == 0 ? "no record" : "with the first run's record", ms(t0, t1), ms(t1, t2), ms(t2, t3));
        record = renderer->pipeline_record();
    }
    return 0;
}

// Per-object data read by a shader: 1000 objects of 64 floats, from an R32F
// data texture or from a storage buffer; the data uploaded each frame, then
// every object drawn (32 x 32 each), 64 reads a pixel.
int data_reads(kin::Window& window, int frames) {
    using clock = std::chrono::steady_clock;
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
    auto renderer = std::make_unique<kin::Renderer2D>(std::move(backend));
    renderer->set_gpu_timing_enabled(true);
    const auto load = [&](const char* name) {
        std::ifstream file(std::filesystem::path{KIN_TEST_SHADER_DIR} / name, std::ios::binary);
        const std::vector<kin::u8> code{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        kin::ShaderDesc desc{};
        desc.spirv = {code.data(), static_cast<kin::u32>(code.size())};
        return renderer->create_shader(desc);
    };
    const kin::ShaderHandle from_texture = load("bench_read_texture.frag.spv");
    const kin::ShaderHandle from_buffer = load("bench_read_buffer.frag.spv");
    constexpr int objects = 1000;
    std::vector<float> data(objects * 64, 0.5f);
    const kin::Texture texture = renderer->create_texture({64, objects}, kin::TextureFormat::R32Float, data.data());
    const kin::DataBuffer buffer = renderer->create_data_buffer(data.size() * sizeof(float), data.data());
    std::vector<kin::ShaderVertex> vertices;
    for (int i = 0; i < objects; ++i) {
        const float x = static_cast<float>((i % 40) * 32), y = static_cast<float>((i / 40) * 28);
        const std::array<float, 4> custom{static_cast<float>(i), 0, 0, 0};
        for (const kin::Vec2f p : {kin::Vec2f{x, y}, kin::Vec2f{x + 32, y}, kin::Vec2f{x + 32, y + 32},
                                   kin::Vec2f{x, y}, kin::Vec2f{x + 32, y + 32}, kin::Vec2f{x, y + 32}}) {
            vertices.push_back({.position = p, .custom = custom});
        }
    }
    for (int mode = 0; mode < 2; ++mode) {
        std::vector<double> upload, gpu;
        for (int f = 0; f < frames; ++f) {
            const auto t0 = clock::now();
            if (mode == 0) {
                renderer->update_texture(texture, {0, 0}, {64, objects}, reinterpret_cast<const kin::u8*>(data.data()));
            } else {
                renderer->update_data_buffer(buffer, 0, data.size() * sizeof(float), data.data());
            }
            const auto t1 = clock::now();
            renderer->clear(kin::Color::rgb(0, 0, 0));
            {
                const auto scope = renderer->gpu_scope("reads");
                if (mode == 0) {
                    renderer->draw_shader_geometry(vertices, {}, from_texture, {}, std::span<const kin::Texture>{&texture, 1});
                } else {
                    renderer->draw_shader_geometry(vertices, {}, from_buffer, {}, {}, std::span<const kin::DataBuffer>{&buffer, 1});
                }
            }
            renderer->present();
            for (const kin::GpuScopeTiming& t : renderer->take_gpu_scope_timings()) {
                gpu.push_back(t.ms);
            }
            if (f >= 5) {
                upload.push_back(ms(t0, t1));
            }
        }
        std::sort(upload.begin(), upload.end());
        std::sort(gpu.begin(), gpu.end());
        std::printf("per-object data from a %-12s: upload median %.3f ms, GPU reads median %.3f ms (%zu samples)\n",
                    mode == 0 ? "data texture" : "data buffer", upload[upload.size() / 2],
                    gpu.empty() ? 0.0 : gpu[gpu.size() / 2], gpu.size());
    }
    return 0;
}

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 60;
    const int quads = argc > 2 ? std::atoi(argv[2]) : 20000;
    kin::App app{};
    kin::Window& window = app.create_window({.title = "draw-bench", .width = 1280, .height = 720, .hidden = true});
    std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
    if (!backend || backend->name() != "SDL_GPU") {
        std::fprintf(stderr, "SDL_GPU backend unavailable\n");
        return 1;
    }
    if (argc > 3 && std::string_view{argv[3]} == "pipelines") {
        backend.reset();
        return pipeline_runs(window);
    }
    if (argc > 3 && std::string_view{argv[3]} == "data") {
        backend.reset();
        return data_reads(window, frames);
    }
    auto renderer = std::make_unique<kin::Renderer2D>(std::move(backend));
    renderer->set_gpu_timing_enabled(true);
    std::vector<kin::u8> texels(32u * 32u * 4u, 200);
    const kin::Texture texture = renderer->create_texture_from_rgba(texels.data(), {32, 32});
    std::vector<kin::SpriteInstance> sprites;
    for (int i = 0; i < quads; ++i) {
        const float x = static_cast<float>((i * 37) % 1260), y = static_cast<float>((i * 91) % 700);
        sprites.push_back({.dest = {x, y, 16.0f, 16.0f}, .source = {0.0f, 0.0f, 32.0f, 32.0f},
                           .tint = kin::Color::rgba(255, 255, 255, 200)});
    }
    using clock = std::chrono::steady_clock;
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

    // A shader's first use: making it, then its first frame (where a pipeline
    // made on first draw would show). Run with __GL_SHADER_DISK_CACHE=0 for a
    // cold driver cache.
    {
        renderer->present();
        const auto s0 = clock::now();
        const kin::ShaderHandle shader = renderer->builtin_shader(kin::BuiltinShader::Vignette);
        const auto s1 = clock::now();
        renderer->clear(kin::Color::rgb(0, 0, 0));
        renderer->draw_shader_surface({0.0f, 0.0f, 64.0f, 64.0f}, shader, {}, texture);
        renderer->present();
        const auto s2 = clock::now();
        renderer->clear(kin::Color::rgb(0, 0, 0));
        renderer->draw_shader_surface({0.0f, 0.0f, 64.0f, 64.0f}, shader, {}, texture);
        renderer->present();
        const auto s3 = clock::now();
        std::printf("shader: made in %.2f ms, first frame %.2f ms, second %.2f ms\n", ms(s0, s1), ms(s1, s2),
                    ms(s2, s3));
    }
    const char* names[3] = {"draw_texture", "fill_rect", "draw_sprites"};
    for (int mode = 0; mode < 3; ++mode) {
        std::vector<double> records;
        double present = 0, gpu = 0;
        int counted = 0;
        for (int f = 0; f < frames; ++f) {
            const auto t0 = clock::now();
            renderer->clear(kin::Color::rgb(0, 0, 0));
            if (mode == 0) {
                for (const kin::SpriteInstance& s : sprites) {
                    renderer->draw_texture(texture, s.source, s.dest, s.tint);
                }
            } else if (mode == 1) {
                for (const kin::SpriteInstance& s : sprites) {
                    renderer->fill_rect(s.dest, s.tint);
                }
            } else {
                renderer->draw_sprites(texture, sprites);
            }
            const auto t1 = clock::now();
            renderer->present();
            const auto t2 = clock::now();
            if (f >= 10) {
                records.push_back(ms(t0, t1));
                present += ms(t1, t2);
                gpu += renderer->backend_stats().last_gpu_frame_ms;
                ++counted;
            }
        }
        std::sort(records.begin(), records.end());
        std::printf("%d quads, %-12s: record min %.3f / median %.3f ms, present %.2f ms, GPU %.2f ms\n", quads,
                    names[mode], records.front(), records[records.size() / 2], present / counted, gpu / counted);
    }
}
