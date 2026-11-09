// What decals, auto exposure and morph targets cost (docs/rendering_3d.md, Decals / Views / Morph targets): GPU frame
// times of one lit scene (a floor, boxes, the sun's shadow, 4x MSAA HDR at 1920 x 1080) with each feature on and off,
// interleaved in rounds so that clocks and heat even out; and the CPU time of Renderer3D::set_morph_weights.
//
//     kin_render3d_surface_bench [frames per round, 60] [rounds, 6]
//
// Needs the GPU (KIN_RENDER_BACKEND is not read: SDL_GPU is asked for). Medians; a quiet machine matters.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <string>
#include <vector>

using namespace kin;
namespace {
constexpr Vec2i extent{1920, 1080};

f64 median(std::vector<f64> v) {
    if (v.empty())
        return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

struct Config {
    std::string name;
    bool exposure = false;
    int decals = 0;
    f32 decal_size = 2.0f;
    int pictures = 1; // decal i shows picture i % pictures
    std::vector<f64> gpu, cpu;
    std::vector<u32> draws;
};

Texture splat(Renderer2D& r, u8 tint = 40) {
    constexpr int side = 256;
    std::vector<u8> p(side * side * 4);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            const f32 u = (x + 0.5f) / side * 2 - 1, v = (y + 0.5f) / side * 2 - 1;
            const f32 a = std::clamp(1.0f - std::sqrt(u * u + v * v), 0.0f, 1.0f);
            u8* o = &p[(y * side + x) * 4];
            o[0] = tint, o[1] = 30, o[2] = 25, o[3] = static_cast<u8>(a * 255);
        }
    Texture t = r.create_texture_from_rgba(p.data(), side, side);
    r.set_scale_mode(t, ScaleMode::Mipmapped);
    return t;
}

void morphs(Renderer3D& r3d) {
    for (const int targets : {8, 52}) {
        MeshData3D data = meshes::sphere(1.0f, 128, 64);
        for (int t = 0; t < targets; ++t) {
            MorphTarget3D target{"t" + std::to_string(t), {}, {}};
            for (const auto& v : data.vertices) {
                target.positions.push_back(v.normal * (0.01f * f32(t % 5)));
                target.normals.push_back({0.01f, 0, 0});
            }
            data.morph_targets.push_back(std::move(target));
        }
        Mesh3D mesh = r3d.create_mesh(data).value_or({});
        for (const int active : {1, 5, targets}) {
            std::vector<f32> w(targets, 0.0f);
            std::vector<f64> times;
            for (int i = 0; i < 200; ++i) {
                for (int t = 0; t < active; ++t)
                    w[t] = 0.5f + 0.4f * std::sin(0.1f * i + t);
                const auto start = std::chrono::steady_clock::now();
                (void)r3d.set_morph_weights(mesh, w);
                times.push_back(
                    std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - start).count());
            }
            std::printf(
                "morph: %zu vertices, %2d targets, %2d changing: set_morph_weights %.0f us (%.1f ns a vertex)\n",
                data.vertices.size(), targets, active, median(times), median(times) * 1000.0 / data.vertices.size());
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 60, rounds = argc > 2 ? std::atoi(argv[2]) : 6;
    App app;
    auto& window = app.create_window({.title = "surface bench", .width = extent.x, .height = extent.y, .hidden = true});
    Renderer2D r{make_render_backend(window, false, true)};
    Renderer3D r3d{r};
    const auto caps = r3d.capabilities();
    if (!caps.decals || !caps.auto_exposure) {
        std::puts("surface bench: needs decals and auto exposure (a GPU 3D device)");
        return 77;
    }
    if (caps.software)
        std::puts("surface bench: a software device: the times mean nothing");
    r.set_gpu_timing_enabled(true);
    morphs(r3d);

    Mesh3D floor = r3d.create_mesh(meshes::box({40, 0.2f, 40})).value_or({});
    Mesh3D box = r3d.create_mesh(meshes::box({1, 1, 1})).value_or({});
    const std::array<Texture, 4> textures{splat(r, 40), splat(r, 90), splat(r, 140), splat(r, 190)};
    std::vector<Config> configs = {{"plain"},
                                   {"auto exposure", true},
                                   {"16 decals 2 m", false, 16},
                                   {"64 decals 2 m", false, 64},
                                   {"256 decals 2 m", false, 256},
                                   {"64 decals 8 m", false, 64, 8.0f},
                                   {"256 decals x4 pic", false, 256, 2.0f, 4}};
    const auto scene_for = [&](const Config& c) {
        Scene3D scene;
        scene.camera = Camera3D::look_at({0, 14, 18}, {0, 0, 0}, 60);
        scene.lights.sun = {.direction = {0.4f, 1.0f, 0.3f}, .color = {3, 3, 3}};
        scene.lights.shadow_area = {{-20, -1, -20}, {20, 4, 20}};
        (void)scene.draw(floor, Mat4f::translation({0, -0.1f, 0}), Material3D{.roughness = 0.8f});
        for (int i = 0; i < 64; ++i)
            (void)scene.draw(box, Mat4f::translation({f32(i % 8) * 4 - 14, 0.5f, f32(i / 8) * 4 - 14}),
                             Material3D{.color = Color::rgb(160, 120, 90)});
        for (int i = 0; i < c.decals; ++i) {
            const f32 a = 2.39996f * f32(i), r = 17.0f * std::sqrt((i + 0.5f) / std::max(c.decals, 1));
            (void)scene.draw_decal({.transform = Mat4f::trs({r * std::cos(a), 0, r * std::sin(a)},
                                                            Quatf::axis_angle({0, 1, 0}, f32(i) * 37.0f),
                                                            {c.decal_size, 1.0f, c.decal_size}),
                                    .texture = textures[std::size_t(i % c.pictures)]});
        }
        return scene;
    };
    std::vector<View3D> views;
    for (const Config& c : configs)
        views.push_back(r3d.create_view(extent, {.samples = 4, .hdr = true, .auto_exposure = {.enabled = c.exposure}})
                            .value_or({}));
    for (int round = 0; round < rounds; ++round) {
        for (std::size_t k = 0; k < configs.size(); ++k) {
            Config& c = configs[(k + round) % configs.size()]; // a different config first each round
            View3D& view = views[(k + round) % configs.size()];
            Scene3D scene = scene_for(c);
            u64 sampled = r.backend_stats().gpu_frames_sampled;
            for (int f = 0; f < frames + 10; ++f) {
                scene.time = f * (1.0f / 60);
                r.clear(colors::black);
                const auto start = std::chrono::steady_clock::now();
                (void)r3d.render(view, scene);
                const f64 cpu =
                    std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
                r.draw_texture(view.texture(), {0, 0, f32(extent.x), f32(extent.y)});
                r.present();
                const auto stats = r.backend_stats();
                if (f >= 10 && stats.gpu_frames_sampled != sampled) { // (the first frames of a config warm up)
                    c.gpu.push_back(stats.last_gpu_frame_ms);
                    c.cpu.push_back(cpu);
                    c.draws.push_back(r3d.debug_stats().decal_draws);
                }
                sampled = stats.gpu_frames_sampled;
            }
        }
    }
    const f64 base = median(configs[0].gpu);
    for (const Config& c : configs)
        std::printf("%-18s GPU %6.2f ms (%+.2f)   render() CPU %5.2f ms   decal draws %3u   (%zu frames)\n",
                    c.name.c_str(), median(c.gpu), median(c.gpu) - base, median(c.cpu),
                    c.draws.empty() ? 0u : c.draws.back(), c.gpu.size());
}
