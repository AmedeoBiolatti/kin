// Times drawing many quads a frame on the SDL_GPU backend, three ways:
// draw_texture calls, fill_rect calls, and one draw_sprites batch.
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/shader_compiler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

// A costly full-screen effect at 1, 0.5 and 0.25 resolution: GPU time, and how
// far the picture strays from the full-resolution one (mean per channel, 0-255).
int scaled_effect(kin::Window& window, int frames) {
    std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
    auto renderer = std::make_unique<kin::Renderer2D>(std::move(backend));
    renderer->set_gpu_timing_enabled(true);
    std::ifstream file(std::filesystem::path{KIN_TEST_SHADER_DIR} / "bench_heavy.frag.spv", std::ios::binary);
    const std::vector<kin::u8> code{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    kin::ShaderDesc desc{};
    desc.spirv = {code.data(), static_cast<kin::u32>(code.size())};
    const kin::ShaderHandle shader = renderer->create_shader(desc);
    const kin::Rectf screen{0.0f, 0.0f, 1280.0f, 720.0f};
    std::vector<kin::u8> reference;
    for (const float resolution : {1.0f, 0.5f, 0.25f}) {
        std::vector<double> gpu, frame_gpu;
        for (int f = 0; f < frames; ++f) {
            renderer->clear(kin::Color::rgb(0, 0, 0));
            {
                const auto scope = renderer->gpu_scope("effect");
                renderer->draw_shader_surface_scaled(resolution, screen, shader, {});
            }
            renderer->present();
            // Wait for the GPU each frame: ahead of it, the timer fills and
            // frames go untimed, and leftovers of the last mode would count.
            std::vector<kin::u8> one;
            kin::Vec2i one_size{};
            renderer->read_rgba({0.0f, 0.0f, 1.0f, 1.0f}, one, one_size);
            const std::vector<kin::GpuScopeTiming> timings = renderer->take_gpu_scope_timings();
            if (f >= 5) {
                for (const kin::GpuScopeTiming& t : timings) {
                    gpu.push_back(t.ms);
                }
                frame_gpu.push_back(renderer->backend_stats().last_gpu_frame_ms);
            }
        }
        kin::RenderTarget target = renderer->create_render_target({1280, 720}, kin::ScaleMode::Nearest);
        std::vector<kin::u8> pixels;
        kin::Vec2i size{};
        {
            const auto bind = renderer->scoped_render_target(target);
            renderer->clear(kin::Color::rgb(0, 0, 0));
            renderer->draw_shader_surface_scaled(resolution, screen, shader, {});
            renderer->read_rgba(screen, pixels, size);
        }
        if (reference.empty()) {
            reference = pixels;
        }
        double error = 0.0;
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            error += std::abs(static_cast<int>(pixels[i]) - static_cast<int>(reference[i]));
        }
        std::sort(gpu.begin(), gpu.end());
        std::sort(frame_gpu.begin(), frame_gpu.end());
        std::printf("effect at %.2f resolution: GPU scope median %.3f ms, GPU frame median %.3f ms, mean error %.2f / 255\n",
                    resolution, gpu.empty() ? 0.0 : gpu[gpu.size() / 2], frame_gpu[frame_gpu.size() / 2],
                    error / static_cast<double>(pixels.size()));
    }
    return 0;
}

// A 512 x 512 light field (64 lights, a smooth falloff each) made each frame:
// on the CPU and uploaded, or by a compute shader. CPU time to make it, and GPU
// time of the dispatch.
int light_field(kin::Window& window, int frames) {
    using clock = std::chrono::steady_clock;
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
    auto renderer = std::make_unique<kin::Renderer2D>(std::move(backend));
    renderer->set_gpu_timing_enabled(true);
    std::ifstream file(std::filesystem::path{KIN_TEST_SHADER_DIR} / "bench_light_field.comp.spv", std::ios::binary);
    const std::vector<kin::u8> code{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    const kin::ComputeShaderHandle shader = renderer->create_compute_shader({code.data(), static_cast<kin::u32>(code.size())});
    constexpr int n = 512, count = 64;
    std::vector<float> lights;
    for (int i = 0; i < count; ++i) {
        lights.insert(lights.end(), {static_cast<float>((i * 97) % n), static_cast<float>((i * 61) % n), 40.0f + static_cast<float>(i % 7) * 10.0f, 1.0f});
    }
    const kin::DataBuffer light_buffer = renderer->create_data_buffer(lights.size() * sizeof(float), lights.data());
    const kin::Texture uploaded = renderer->create_texture({n, n}, kin::TextureFormat::R32Float, nullptr);
    const kin::Texture computed = renderer->create_storage_texture({n, n}, kin::TextureFormat::R32Float);
    kin::ShaderParams params;
    params.uniforms[0] = static_cast<float>(count);
    std::vector<float> field(static_cast<std::size_t>(n) * n);
    for (int mode = 0; mode < 2; ++mode) {
        std::vector<double> cpu, gpu;
        for (int f = 0; f < frames; ++f) {
            const auto t0 = clock::now();
            if (mode == 0) {
                for (int y = 0; y < n; ++y) {
                    for (int x = 0; x < n; ++x) {
                        float sum = 0.0f;
                        for (int i = 0; i < count; ++i) {
                            const float* l = &lights[static_cast<std::size_t>(i) * 4];
                            const float dx = (static_cast<float>(x) - l[0]) / l[2], dy = (static_cast<float>(y) - l[1]) / l[2];
                            const float g = std::max(0.0f, 1.0f - (dx * dx + dy * dy));
                            sum += g * g * l[3];
                        }
                        field[static_cast<std::size_t>(y) * n + static_cast<std::size_t>(x)] = sum;
                    }
                }
                renderer->update_texture(uploaded, {0, 0}, {n, n}, reinterpret_cast<const kin::u8*>(field.data()));
            } else {
                const auto scope = renderer->gpu_scope("field");
                renderer->dispatch_compute(shader, {n, n},
                                           {.buffers = std::span<const kin::DataBuffer>{&light_buffer, 1},
                                            .outputs = std::span<const kin::Texture>{&computed, 1},
                                            .params = &params});
            }
            const auto t1 = clock::now();
            renderer->present();
            std::vector<kin::u8> one;
            kin::Vec2i one_size{};
            renderer->read_rgba({0.0f, 0.0f, 1.0f, 1.0f}, one, one_size); // wait for the GPU
            for (const kin::GpuScopeTiming& t : renderer->take_gpu_scope_timings()) {
                gpu.push_back(t.ms);
            }
            cpu.push_back(ms(t0, t1));
        }
        std::sort(cpu.begin(), cpu.end());
        std::sort(gpu.begin(), gpu.end());
        std::printf("light field 512x512, 64 lights, %-26s CPU median %.3f ms, GPU median %.3f ms\n",
                    mode == 0 ? "on the CPU and uploaded:" : "by a compute shader:", cpu[cpu.size() / 2],
                    gpu.empty() ? 0.0 : gpu[gpu.size() / 2]);
    }
    return 0;
}

// Hot reload: compiling engine and bench shaders with glslc at runtime, and
// reloading one in place (KIN_SHADER_SOURCE_DIR: the source shaders).
int hot_reload(kin::Window& window) {
    using clock = std::chrono::steady_clock;
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
    auto renderer = std::make_unique<kin::Renderer2D>(std::move(backend));
    const std::filesystem::path engine{KIN_ENGINE_SHADER_SOURCE_DIR};
    for (const char* name : {"pp_vignette.frag.glsl", "pp_bloom_combine.frag.glsl", "transition_iris.frag.glsl"}) {
        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            const auto t0 = clock::now();
            const auto spirv = kin::compile_glsl(engine / name, kin::ShaderStage::Fragment);
            times.push_back(ms(t0, clock::now()));
            if (!spirv) {
                std::printf("%s did not compile\n", name);
                return 1;
            }
        }
        std::sort(times.begin(), times.end());
        std::printf("compile %-28s median %.1f ms\n", name, times[times.size() / 2]);
    }
    kin::ShaderFile file{*renderer, engine / "pp_vignette.frag.glsl"};
    const auto spirv = kin::compile_glsl(engine / "pp_bloom_combine.frag.glsl", kin::ShaderStage::Fragment);
    kin::ShaderDesc desc;
    desc.spirv = {spirv->data(), static_cast<kin::u32>(spirv->size())};
    std::vector<double> times;
    for (int i = 0; i < 10; ++i) {
        const auto t0 = clock::now();
        renderer->reload_shader(file.handle(), desc);
        times.push_back(ms(t0, clock::now()));
    }
    std::sort(times.begin(), times.end());
    std::printf("reload_shader (new shader + its pipeline) median %.2f ms\n", times[times.size() / 2]);
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
    if (argc > 3 && std::string_view{argv[3]} == "scaled") {
        backend.reset();
        return scaled_effect(window, frames);
    }
    if (argc > 3 && std::string_view{argv[3]} == "compute") {
        backend.reset();
        return light_field(window, frames);
    }
    if (argc > 3 && std::string_view{argv[3]} == "hotreload") {
        backend.reset();
        return hot_reload(window);
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
