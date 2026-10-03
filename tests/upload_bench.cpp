// Times texture uploads on the SDL_GPU backend: the request's case (many data
// textures created and updated in one frame).
#include <kin/core/jobs.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 40;
    kin::App app{};
    kin::Window& window = app.create_window({.title = "upload-bench", .width = 64, .height = 64, .hidden = true});
    std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
    if (!backend || backend->name() != "SDL_GPU") {
        std::fprintf(stderr, "SDL_GPU backend unavailable\n");
        return 1;
    }
    auto renderer = std::make_unique<kin::Renderer2D>(std::move(backend));
    // A second argument: workers for the renderer's job system (0: none).
    const int workers = argc > 2 ? std::atoi(argv[2]) : 0;
    std::unique_ptr<kin::JobSystem> jobs;
    if (workers > 0) {
        jobs = std::make_unique<kin::JobSystem>(kin::JobSystemConfig{.workers = workers});
        renderer->set_job_system(jobs.get());
    }
    // A third argument "big": 4 textures of 2048 x 2048 floats (16 MB) a frame
    // instead of 50 of 1024 x 64 (256 KB).
    const bool big = argc > 3 && std::string_view{argv[3]} == "big";
    const int count = big ? 4 : 50;
    const kin::Vec2i size = big ? kin::Vec2i{2048, 2048} : kin::Vec2i{1024, 64};
    std::vector<float> data(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y), 0.5f);
    using clock = std::chrono::steady_clock;
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    renderer->set_gpu_timing_enabled(true); // the GPU's own time, from fences
    std::vector<kin::Texture> keep;
    double t_create = 0, t_whole = 0, t_part = 0, t_present = 0, t_gpu = 0, gpu_frame = 0;
    double t_make_update = 0, t_make_write = 0;
    int counted = 0;
    for (int f = 0; f < frames; ++f) {
        const auto t0 = clock::now();
        keep.clear();
        for (int i = 0; i < count; ++i) keep.push_back(renderer->create_texture(size, kin::TextureFormat::R32Float, data.data()));
        const auto t1 = clock::now();
        for (int i = 0; i < count; ++i) renderer->update_texture(keep[i], {0, 0}, size, reinterpret_cast<const kin::u8*>(data.data()));
        const auto t2 = clock::now();
        for (int i = 0; i < count; ++i) renderer->update_texture(keep[i], {0, 0}, {size.x, size.y / 2}, reinterpret_cast<const kin::u8*>(data.data()));
        const auto t3 = clock::now();
        // Texels made this frame: into a buffer then uploaded, or written straight
        // into the upload memory.
        const auto make = [&](float* out, std::size_t n, int salt) {
            for (std::size_t k = 0; k < n; ++k) {
                out[k] = static_cast<float>((k + static_cast<std::size_t>(salt)) & 1023) * 0.001f;
            }
        };
        const auto g0 = clock::now();
        for (int i = 0; i < count; ++i) {
            make(data.data(), data.size(), i);
            renderer->update_texture(keep[i], {0, 0}, size, reinterpret_cast<const kin::u8*>(data.data()));
        }
        const auto g1 = clock::now();
        for (int i = 0; i < count; ++i) {
            renderer->write_texture(keep[i], {0, 0}, size, [&](std::span<kin::u8> texels) {
                make(reinterpret_cast<float*>(texels.data()), texels.size() / sizeof(float), i);
            });
        }
        const auto g2 = clock::now();
        renderer->present();
        const auto t4 = clock::now();
        std::vector<kin::u8> px; kin::Vec2i sz;
        renderer->read_rgba({0, 0, 1, 1}, px, sz);
        const auto t5 = clock::now();
        if (f >= 5) {
            t_create += ms(t0, t1); t_whole += ms(t1, t2); t_part += ms(t2, t3); t_present += ms(t3, t4); t_gpu += ms(t4, t5);
            gpu_frame += renderer->backend_stats().last_gpu_frame_ms;
            t_make_update += ms(g0, g1);
            t_make_write += ms(g1, g2);
            ++counted;
        }
    }
    renderer->set_job_system(nullptr);
    std::printf("made each frame: into a buffer then update_texture %.2f ms, write_texture %.2f ms\n",
                t_make_update / counted, t_make_write / counted);
    std::printf("%d x %dx%d floats, workers %d, per frame (mean of %d): create %.2f ms, whole update %.2f ms, half update %.2f ms, present %.2f ms, until GPU done %.2f ms, GPU frame %.2f ms\n",
                count, size.x, size.y, workers, counted, t_create / counted, t_whole / counted, t_part / counted, t_present / counted, t_gpu / counted, gpu_frame / counted);
}
