// Renders the reference scenes (docs/3d_plan.md, G0): each camera of each
// scene to a PNG, timed, with what was drawn, into a folder a later run (or
// Cycles' pictures) is compared with (tools/reference/run.py).
//
//   kin_reference <scene.json>... --out DIR [--cache DIR] [--frames N]
//                 [--warmup N] [--repeat N] [--width W --height H]
//
// A scene file names its models (in the cache, tools/reference/fetch.py),
// its cameras, lights and view; `procedural: "crowd"` builds Signal
// Breach's deck and 20,000 enemies in code. Times: `render()` on the CPU,
// and the GPU's frame with the view rendered --repeat times in it (divided
// by that: the 3D work then outweighs the rest of the frame and the
// timer's noise), median, p10 and p90 over --frames frames.

#include <kin/assets/image.hpp>
#include <kin/core/json_value.hpp>
#include <kin/core/jobs.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using kin::Mat4f;
using kin::Vec3f;

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

Vec3f vec3(const kin::JsonValue* v, Vec3f fallback = {}) {
    if (v == nullptr || v->items().size() < 3) {
        return fallback;
    }
    const auto& a = v->items();
    return {static_cast<float>(a[0].as_number()), static_cast<float>(a[1].as_number()), static_cast<float>(a[2].as_number())};
}

kin::Color colour(const kin::JsonValue* v, kin::Color fallback) {
    if (v == nullptr || v->items().size() < 3) {
        return fallback;
    }
    const auto& a = v->items();
    const auto c = [&](std::size_t i) { return static_cast<kin::u8>(std::clamp<double>(a[i].as_number(), 0, 255)); };
    return kin::Color::rgba(c(0), c(1), c(2), a.size() > 3 ? c(3) : 255);
}

struct Summary {
    double median = 0, p10 = 0, p90 = 0, min = 0;
};

Summary summarize(std::vector<double> v) {
    std::erase_if(v, [](double x) { return !(x > 0.0); });
    if (v.empty()) {
        return {};
    }
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))]; };
    return {at(0.5), at(0.1), at(0.9), v.front()};
}

std::string json_summary(const Summary& s) {
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, "{\"median\": %.4f, \"p10\": %.4f, \"p90\": %.4f, \"min\": %.4f}", s.median, s.p10, s.p90, s.min);
    return buffer;
}

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

// A model placed in the scene.
struct Placed {
    kin::Model model;
    Mat4f transform = Mat4f::identity();
};

// Signal Breach's deck (48 x 32 tiles) and `count` enemies over it, three
// meshes instanced, as kin_draw_bench's shadows mode has them.
struct Crowd {
    kin::Mesh tile;
    std::array<kin::Mesh, 3> enemy;
    std::vector<kin::MeshInstance> tiles;
    std::array<std::vector<kin::MeshInstance>, 3> enemies;

    void make(kin::Renderer3D& r3d, int count) {
        tile = r3d.create_mesh(kin::meshes::box({1.0f, 1.0f, 1.0f}));
        enemy = {r3d.create_mesh(kin::meshes::star_prism(5, 0.32f, 0.14f, 0.16f)), r3d.create_mesh(kin::meshes::cylinder(0.4f, 0.16f, 6)),
                 r3d.create_mesh(kin::meshes::torus(0.26f, 0.07f, 20, 8))};
        for (int z = 0; z < 32; ++z) {
            for (int x = 0; x < 48; ++x) {
                tiles.push_back({Mat4f::translation({x + 0.5f, -0.5f, z + 0.5f}), kin::Color::rgb(90, 96, 110)});
            }
        }
        kin::u32 seed = 7;
        const auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 16777216.0f;
        };
        for (int i = 0; i < count; ++i) {
            const Vec3f at{next() * 48.0f, 0.6f + next() * 2.0f, next() * 32.0f};
            enemies[static_cast<std::size_t>(i % 3)].push_back(
                {Mat4f::translation(at) * Mat4f::rotation_y(static_cast<float>(i) * 37.0f), kin::Color::rgb(255, 255, 255)});
        }
    }

    void draw(kin::Scene3D& scene) const {
        scene.draw_instanced(tile, tiles);
        for (std::size_t k = 0; k < 3; ++k) {
            scene.draw_instanced(enemy[k], enemies[k], {.color = kin::Color::rgb(200, 120, 80)});
        }
    }
};

kin::Bounds3 bounds_of(const std::vector<Placed>& placed) {
    Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Placed& p : placed) {
        const kin::Bounds3& b = p.model.bounds;
        for (int i = 0; i < 8; ++i) {
            const Vec3f c = p.transform.apply({(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z});
            lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
            hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
        }
    }
    return placed.empty() ? kin::Bounds3{} : kin::Bounds3{lo, hi};
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // a line at a time: what was done shows if it stops
    std::vector<fs::path> scenes;
    fs::path out = "reference_out";
    const char* cache_env = std::getenv("KIN_REFERENCE_CACHE");
    const char* home = std::getenv("HOME");
    fs::path cache = cache_env ? fs::path{cache_env} : fs::path{home ? home : "."} / ".cache" / "kin" / "reference";
    int frames = 60, warmup = 10, repeat = 8, width = 1280, height = 720;
    bool gpu_timing = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--out") {
            out = next();
        } else if (a == "--cache") {
            cache = next();
        } else if (a == "--frames") {
            frames = std::max(1, std::atoi(next().c_str()));
        } else if (a == "--warmup") {
            warmup = std::max(0, std::atoi(next().c_str()));
        } else if (a == "--repeat") {
            repeat = std::max(1, std::atoi(next().c_str()));
        } else if (a == "--no-gpu-timing") {
            gpu_timing = false;
        } else if (a == "--width") {
            width = std::max(16, std::atoi(next().c_str()));
        } else if (a == "--height") {
            height = std::max(16, std::atoi(next().c_str()));
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        } else {
            scenes.emplace_back(a);
        }
    }
    if (scenes.empty()) {
        std::fprintf(stderr, "usage: kin_reference <scene.json>... --out DIR [--cache DIR] [--frames N] [--warmup N] [--repeat N]\n");
        return 2;
    }
    fs::create_directories(out);

    kin::App app{};
    kin::Window& window = app.create_window({.title = "kin-reference", .width = width, .height = height, .hidden = true});
    std::unique_ptr<kin::IRenderer2DBackend> backend = kin::make_render_backend(window, false, true);
    if (!backend || backend->name() != "SDL_GPU") {
        std::fprintf(stderr, "SDL_GPU is not available\n");
        return 1;
    }
    kin::Renderer2D renderer{std::move(backend)};
    renderer.set_gpu_timing_enabled(gpu_timing);
    kin::JobSystem jobs;
    renderer.set_job_system(&jobs);
    kin::Renderer3D r3d{renderer};
    const kin::Rectf full{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
    std::vector<kin::u8> pixel;
    kin::Vec2i pixel_size{};
    const auto wait_for_gpu = [&] { renderer.read_rgba({0.0f, 0.0f, 1.0f, 1.0f}, pixel, pixel_size); };

    std::ostringstream report;
    report << "{\n \"renderer\": " << quoted(std::string{renderer.backend_name()}) << ",\n \"vulkan_icd\": "
           << quoted(std::getenv("VK_ICD_FILENAMES") ? std::getenv("VK_ICD_FILENAMES") : "") << ",\n \"size\": [" << width << ", "
           << height << "],\n \"frames\": " << frames << ", \"repeat\": " << repeat << ",\n \"scenes\": [";
    bool first_scene = true;
    int failures = 0;
    for (const fs::path& file : scenes) {
        std::ifstream in(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        kin::JsonParseResult parsed = kin::parse_json(text);
        if (!parsed.ok()) {
            std::fprintf(stderr, "%s: %s\n", file.string().c_str(), parsed.error.c_str());
            ++failures;
            continue;
        }
        const kin::JsonValue& doc = *parsed.value;
        const std::string name = doc.string_at("name", file.stem().string());
        std::printf("%s\n", name.c_str());

        // Its models, loaded and timed.
        const auto load_start = std::chrono::steady_clock::now();
        std::vector<Placed> placed;
        bool missing = false;
        if (const kin::JsonValue* models = doc.find("models")) {
            for (const kin::JsonValue& m : models->items()) {
                const fs::path path = cache / m.string_at("file");
                if (!fs::exists(path)) {
                    std::printf("  %s: not in the cache (tools/reference/fetch.py)\n", path.string().c_str());
                    missing = true;
                    break;
                }
                std::string error;
                kin::Model model = r3d.load_model(path, &error);
                if (!model) {
                    std::fprintf(stderr, "  %s: %s\n", path.string().c_str(), error.c_str());
                    missing = true;
                    break;
                }
                const float scale = static_cast<float>(m.number_at("scale", 1.0));
                placed.push_back({std::move(model), Mat4f::translation(vec3(m.find("position"))) *
                                                        Mat4f::rotation_y(static_cast<float>(m.number_at("rotation_y", 0.0))) *
                                                        Mat4f::scaling({scale, scale, scale})});
            }
        }
        if (missing) {
            if (!doc.bool_at("optional")) {
                ++failures;
            }
            continue;
        }
        Crowd crowd;
        const bool is_crowd = doc.string_at("procedural") == "crowd";
        if (is_crowd) {
            crowd.make(r3d, static_cast<int>(doc.number_at("count", 20000)));
        }
        const double load_ms = ms_since(load_start);
        const kin::Bounds3 bounds = is_crowd ? kin::Bounds3{{0.0f, -1.0f, 0.0f}, {48.0f, 3.0f, 32.0f}} : bounds_of(placed);

        // The view.
        const kin::JsonValue* v = doc.find("view");
        const kin::JsonValue none{};
        const kin::JsonValue& view_doc = v ? *v : none;
        kin::View3D view = r3d.create_view({width, height}, {.samples = static_cast<kin::u32>(view_doc.number_at("samples", 4)),
                                                             .background = colour(view_doc.find("background"), kin::Color::rgb(0, 0, 0)),
                                                             .hdr = view_doc.bool_at("hdr"),
                                                             .exposure = static_cast<float>(view_doc.number_at("exposure", 1.0)),
                                                             .bloom = static_cast<float>(view_doc.number_at("bloom", 0.0))});

        // The lights, the same every frame; an environment (an .hdr in the cache) loaded once.
        kin::Environment3D environment;
        if (const kin::JsonValue* l = doc.find("lights")) {
            if (const kin::JsonValue* env = l->find("environment")) {
                std::string error;
                environment = r3d.load_environment(cache / env->string_at("file"), &error);
                if (!environment) {
                    std::fprintf(stderr, "  environment: %s\n", error.c_str());
                    ++failures;
                    continue;
                }
            }
        }

        kin::Scene3D scene;
        const auto fill_scene = [&] {
            scene.clear();
            const kin::JsonValue* l = doc.find("lights");
            const kin::JsonValue& lights = l ? *l : none;
            if (const kin::JsonValue* sun = lights.find("sun")) {
                scene.lights.sun = {.direction = vec3(sun->find("direction"), {0, 1, 0}), .color = vec3(sun->find("color"))};
            } else {
                scene.lights.sun.color = {};
            }
            if (const kin::JsonValue* fill = lights.find("fill")) {
                scene.lights.fill = {.direction = vec3(fill->find("direction"), {0, 1, 0}), .color = vec3(fill->find("color"))};
            }
            scene.lights.ambient = vec3(lights.find("ambient"));
            scene.lights.environment = environment;
            if (const kin::JsonValue* env = lights.find("environment")) {
                scene.lights.environment_intensity = static_cast<float>(env->number_at("intensity", 1.0));
                scene.lights.environment_rotation = static_cast<float>(env->number_at("rotation", 0.0));
            }
            if (const kin::JsonValue* ground = lights.find("ground_ambient")) {
                scene.lights.ground_ambient = vec3(ground);
            }
            if (const kin::JsonValue* area = lights.find("shadow_area")) {
                if (area->is_string() && area->as_string() == "auto") {
                    scene.lights.shadow_area = bounds;
                } else if (area->items().size() == 2) {
                    scene.lights.shadow_area = {vec3(&area->items()[0]), vec3(&area->items()[1])};
                }
                scene.lights.shadow_resolution = static_cast<kin::u32>(lights.number_at("shadow_resolution", 2048));
            }
            if (const kin::JsonValue* points = lights.find("points")) {
                for (const kin::JsonValue& p : points->items()) {
                    scene.add_light({.position = vec3(p.find("position")), .color = vec3(p.find("color")),
                                     .radius = static_cast<float>(p.number_at("radius", 1.0))});
                }
            }
            for (const Placed& p : placed) {
                scene.draw(p.model, p.transform);
            }
            if (is_crowd) {
                crowd.draw(scene);
            }
        };

        fs::create_directories(out / name);
        if (!first_scene) {
            report << ",";
        }
        first_scene = false;
        report << "\n  {\"name\": " << quoted(name) << ", \"load_ms\": " << load_ms << ", \"bounds\": [[" << bounds.min.x << ", "
               << bounds.min.y << ", " << bounds.min.z << "], [" << bounds.max.x << ", " << bounds.max.y << ", " << bounds.max.z
               << "]], \"cameras\": [";
        bool first_camera = true;
        const kin::JsonValue* cameras = doc.find("cameras");
        for (const kin::JsonValue& c : cameras ? cameras->items() : kin::JsonValue::Array{}) {
            const std::string camera_name = c.string_at("name", "camera");
            const float fov = static_cast<float>(c.number_at("fov", 50.0));
            Vec3f eye = vec3(c.find("eye")), target = vec3(c.find("target"));
            if (const kin::JsonValue* fit = c.find("fit")) {
                // Framed round the scene's bounds, from yaw (round +Y, from +Z) and pitch.
                target = bounds.center();
                const float radius = std::max(0.01f, bounds.radius());
                const float distance = radius / std::sin(fov * 3.14159265f / 360.0f) * static_cast<float>(fit->number_at("margin", 1.05));
                const float yaw = static_cast<float>(fit->number_at("yaw", 0.0)) * 3.14159265f / 180.0f;
                const float pitch = static_cast<float>(fit->number_at("pitch", 30.0)) * 3.14159265f / 180.0f;
                eye = target + Vec3f{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)} * distance;
            }
            fill_scene();
            scene.camera = kin::Camera3D::look_at(eye, target, fov);
            scene.camera.z_near = static_cast<float>(c.number_at("near", 0.05));

            // The first picture: pipelines and targets made, the GPU waited for.
            const auto first_start = std::chrono::steady_clock::now();
            renderer.clear(kin::Color::rgb(0, 0, 0));
            r3d.render(view, scene);
            renderer.draw_texture(view.texture(), full);
            wait_for_gpu();
            const double first_ms = ms_since(first_start);
            renderer.present();

            std::vector<double> cpu, gpu;
            for (int f = 0; f < warmup + frames; ++f) {
                fill_scene();
                renderer.clear(kin::Color::rgb(0, 0, 0));
                for (int k = 0; k < repeat; ++k) {
                    r3d.render(view, scene);
                }
                renderer.draw_texture(view.texture(), full);
                renderer.present();
                if (f >= warmup) {
                    cpu.push_back(r3d.stats().cpu_ms);
                    gpu.push_back(renderer.backend_stats().last_gpu_frame_ms / repeat);
                }
            }
            wait_for_gpu();

            // The picture.
            fill_scene();
            renderer.clear(kin::Color::rgb(0, 0, 0));
            r3d.render(view, scene);
            const kin::Render3DStats stats = r3d.stats();
            renderer.draw_texture(view.texture(), full);
            kin::Image image;
            if (!renderer.read_rgba(full, image.rgba, image.size)) {
                std::fprintf(stderr, "  %s: could not read the picture back\n", camera_name.c_str());
                ++failures;
            }
            renderer.present();
            for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
                image.rgba[i] = 255;
            }
            const fs::path png = out / name / (camera_name + ".png");
            kin::save_image(image, png);
            const Summary cpu_s = summarize(cpu), gpu_s = summarize(gpu);
            std::printf("  %-14s render() %7.3f ms  GPU %7.3f ms  (p10-p90 %.3f-%.3f)  %u draws, %llu triangles, %u shadow casters\n",
                        camera_name.c_str(), cpu_s.median, gpu_s.median, gpu_s.p10, gpu_s.p90, stats.draws,
                        static_cast<unsigned long long>(stats.triangles), stats.shadow_casters);
            if (!first_camera) {
                report << ",";
            }
            first_camera = false;
            report << "\n   {\"name\": " << quoted(camera_name) << ", \"png\": " << quoted(fs::relative(png, out).generic_string())
                   << ", \"eye\": [" << eye.x << ", " << eye.y << ", " << eye.z << "], \"target\": [" << target.x << ", " << target.y
                   << ", " << target.z << "], \"fov\": " << fov
                   << ", \"first_ms\": " << first_ms << ", \"cpu_ms\": " << json_summary(cpu_s) << ", \"gpu_ms\": " << json_summary(gpu_s)
                   << ", \"draws\": " << stats.draws << ", \"instances\": " << stats.instances << ", \"culled\": " << stats.culled
                   << ", \"triangles\": " << stats.triangles << ", \"shadow_draws\": " << stats.shadow_draws
                   << ", \"shadow_casters\": " << stats.shadow_casters << ", \"lights\": " << stats.lights << "}";
        }
        report << "]}";
    }
    report << "\n ]\n}\n";
    std::ofstream(out / "results.json") << report.str();
    renderer.set_job_system(nullptr);
    return failures == 0 ? 0 : 1;
}
