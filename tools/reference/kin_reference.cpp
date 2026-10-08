// Renders the reference scenes (docs/3d_plan.md, G0): each camera of each
// scene to a PNG, timed, with what was drawn, into a folder a later run (or
// Cycles' pictures) is compared with (tools/reference/run.py).
//
//   kin_reference <scene.json>... --out DIR [--cache DIR] [--frames N]
//                 [--warmup N] [--repeat N] [--width W --height H]
//                 [--cascades N] [--shadow-distance D]
//                 [--ao] [--ao-radius R] [--ao-intensity I] [--ao-half]
//                 [--contact-shadows] [--contact-distance D] [--contact-steps N]
//                 [--contact-thickness T]
//                 [--taa] [--taa-blend B] [--taa-sharpness S] [--taa-frames N]
//                 [--supersample N] [--samples N]
//
// A scene file names its models (in the cache, tools/reference/fetch.py),
// its cameras, lights and view; `procedural: "crowd"` builds Signal
// Breach's deck and 20,000 enemies in code. Times: `render()` on the CPU,
// and the GPU's frame with the view rendered --repeat times in it (divided
// by that: the 3D work then outweighs the rest of the frame and the
// timer's noise), median, p10 and p90 over --frames frames. A scene's
// `lights` may set `shadow_cascades` and `shadow_distance`; --cascades and
// --shadow-distance set them for every scene of a run instead (a label of
// its own, so the default pictures stay comparable). A scene file may have
// `ambient_occlusion: {enabled, radius, intensity, half_resolution}`; --ao turns
// it on for every scene (with --ao-radius, --ao-intensity and --ao-half to
// set the rest), likewise a label of its own. Likewise
// `contact_shadows: {enabled, distance, thickness, intensity, steps}` and
// --contact-shadows (with --contact-distance, --contact-steps, --contact-thickness).
// And `taa: {enabled, blend, sharpness, jitter_scale, jitter_samples}` and --taa (with
// --taa-blend, --taa-sharpness): temporal anti-aliasing, whose history needs frames to
// settle, so --taa-frames N makes the run at least N frames before each picture (the
// timing loop's own frames count; the default is 32). Every frame is a step of time:
// a scene with `"animated": true` (the procedural `motion`) is posed by the frame's
// time, which is 0 at the picture and negative before it (so every run's picture shows
// the same moment, and --repeat is 1 for it: the frames are the animation); a camera's
// `pan` is units a frame it has moved, arriving at its `eye` at the picture. --supersample N
// writes <camera>_ss.png too (--samples N sets the view's MSAA for a whole run): the same picture drawn at N x the size with 4x MSAA, with TAA
// off, and averaged down in linear light (the still picture to judge static parts by).

#include <kin/assets/image.hpp>
#include <kin/core/json_value.hpp>
#include <kin/core/jobs.hpp>
#include <kin/platform/app.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/taa.hpp>
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

// [a, b, c, d]
std::string json_list(const std::array<kin::u32, kin::max_shadow_cascades>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out += (i ? ", " : "") + std::to_string(values[i]);
    }
    return out + "]";
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

// A scene made for temporal anti-aliasing (`procedural: "motion"`): high-contrast thin things
// (a fence of posts and rails, a wire, poles at three depths), and four things moving in
// a way that is a pure function of time: a bar orbiting, a cross turning, a block going to
// and fro, a bright pole sliding in front of the fence. Time is in frames; 0 is the picture.
struct Motion {
    kin::Mesh floor, wall, post, rail, wire, pole, block, bar, cross_bar, ball;

    void make(kin::Renderer3D& r3d) {
        floor = r3d.create_mesh(kin::meshes::box({16.0f, 0.2f, 12.0f}));
        wall = r3d.create_mesh(kin::meshes::box({16.0f, 4.0f, 0.2f}));
        post = r3d.create_mesh(kin::meshes::box({0.035f, 1.2f, 0.035f}));
        rail = r3d.create_mesh(kin::meshes::box({4.0f, 0.03f, 0.03f}));
        wire = r3d.create_mesh(kin::meshes::box({6.0f, 0.012f, 0.012f}));
        pole = r3d.create_mesh(kin::meshes::box({0.02f, 2.4f, 0.02f}));
        block = r3d.create_mesh(kin::meshes::box({0.5f, 0.5f, 0.5f}));
        bar = r3d.create_mesh(kin::meshes::box({0.9f, 0.05f, 0.05f}));
        cross_bar = r3d.create_mesh(kin::meshes::box({1.0f, 0.03f, 0.03f}));
        ball = r3d.create_mesh(kin::meshes::sphere(0.18f, 24, 12));
    }

    void draw(kin::Scene3D& scene, int t) const {
        const float time = static_cast<float>(t);
        const kin::Material3D dark{.color = kin::Color::rgb(28, 30, 36), .roughness = 0.9f};
        const kin::Material3D bright{.color = kin::Color::rgb(245, 245, 240), .roughness = 0.6f};
        const kin::Material3D warm{.color = kin::Color::rgb(255, 150, 60), .roughness = 0.5f};
        const kin::Material3D unlit_white{.color = kin::Color::rgb(255, 255, 255), .unlit = true};
        scene.draw(floor, Mat4f::translation({0.0f, -0.1f, 0.0f}), {.color = kin::Color::rgb(70, 74, 82), .roughness = 0.9f});
        scene.draw(wall, Mat4f::translation({0.0f, 2.0f, -3.0f}), dark);
        // A fence: 60 posts every 6.5 cm, two rails (white against the dark wall).
        for (int i = 0; i < 60; ++i) {
            scene.draw(post, Mat4f::translation({-1.95f + 0.065f * static_cast<float>(i), 0.6f, -1.0f}), bright);
        }
        scene.draw(rail, Mat4f::translation({0.0f, 0.35f, -1.0f}), bright);
        scene.draw(rail, Mat4f::translation({0.0f, 1.0f, -1.0f}), bright);
        // A wire across, and poles at three depths.
        scene.draw(wire, Mat4f::translation({0.0f, 1.9f, -2.0f}) * Mat4f::rotation_z(4.0f), unlit_white);
        for (int i = 0; i < 3; ++i) {
            scene.draw(pole, Mat4f::translation({2.2f + 0.5f * static_cast<float>(i), 1.2f, -2.5f + 1.2f * static_cast<float>(i)}), bright);
        }
        // Moving: a bar orbiting on a circle at 3 degrees a frame, a cross turning at 4, a block going to and fro, a pole sliding.
        const float orbit = time * 3.0f * 3.14159265f / 180.0f;
        scene.draw(bar, Mat4f::translation({-2.6f + 0.6f * std::cos(orbit), 1.0f + 0.6f * std::sin(orbit), -0.4f}) * Mat4f::rotation_z(time * 3.0f + 90.0f), warm);
        const Mat4f turn = Mat4f::translation({-3.6f, 0.9f, -0.4f}) * Mat4f::rotation_y(time * 4.0f);
        scene.draw(cross_bar, turn, warm);
        scene.draw(cross_bar, turn * Mat4f::rotation_y(90.0f), warm);
        const float to_and_fro = 1.5f * std::sin(time * 0.05f);
        scene.draw(block, Mat4f::translation({3.5f + to_and_fro, 0.25f, -0.2f}) * Mat4f::rotation_y(time * 2.0f), warm);
        scene.draw(ball, Mat4f::translation({-0.8f + 1.2f * std::sin(time * 0.04f), 0.5f, 0.6f}), warm);
        scene.draw(pole, Mat4f::translation({0.6f * std::sin(time * 0.06f), 1.2f, 0.2f}), warm);
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

// The command line's settings that replace a scene's own: --ao, --contact-shadows and --taa turn the feature on for
// every scene, and each number, when above 0, takes the place of the scene's.
struct Overrides {
    bool ao = false, ao_half = false;
    double ao_radius = 0.0, ao_intensity = 0.0;
    bool contact = false;
    double contact_distance = 0.0, contact_thickness = 0.0;
    int contact_steps = 0;
    bool taa = false;
    double taa_blend = 0.0, taa_sharpness = 0.0;
};

// Each feature's settings: the scene file's (if it has any; then on unless it says not), then the command line's.
void merge_ambient_occlusion(kin::AmbientOcclusion3D& ao, const kin::JsonValue* doc, const Overrides& over) {
    if (doc) {
        ao.enabled = doc->bool_at("enabled", true);
        ao.radius = static_cast<float>(doc->number_at("radius", ao.radius));
        ao.intensity = static_cast<float>(doc->number_at("intensity", ao.intensity));
        ao.half_resolution = doc->bool_at("half_resolution", false);
    }
    if (over.ao) {
        ao.enabled = true;
        ao.half_resolution = ao.half_resolution || over.ao_half;
        if (over.ao_radius > 0.0) {
            ao.radius = static_cast<float>(over.ao_radius);
        }
        if (over.ao_intensity > 0.0) {
            ao.intensity = static_cast<float>(over.ao_intensity);
        }
    }
}

void merge_contact_shadows(kin::ContactShadows3D& cs, const kin::JsonValue* doc, const Overrides& over) {
    if (doc) {
        cs.enabled = doc->bool_at("enabled", true);
        cs.distance = static_cast<float>(doc->number_at("distance", cs.distance));
        cs.thickness = static_cast<float>(doc->number_at("thickness", cs.thickness));
        cs.intensity = static_cast<float>(doc->number_at("intensity", cs.intensity));
        cs.steps = static_cast<kin::u32>(doc->number_at("steps", cs.steps));
    }
    if (over.contact) {
        cs.enabled = true;
        if (over.contact_distance > 0.0) {
            cs.distance = static_cast<float>(over.contact_distance);
        }
        if (over.contact_thickness > 0.0) {
            cs.thickness = static_cast<float>(over.contact_thickness);
        }
        if (over.contact_steps > 0) {
            cs.steps = static_cast<kin::u32>(over.contact_steps);
        }
    }
}

void merge_taa(kin::TemporalAA3D& taa, const kin::JsonValue* doc, const Overrides& over) {
    if (doc) {
        taa.enabled = doc->bool_at("enabled", true);
        taa.blend = static_cast<float>(doc->number_at("blend", taa.blend));
        taa.sharpness = static_cast<float>(doc->number_at("sharpness", taa.sharpness));
        taa.jitter_scale = static_cast<float>(doc->number_at("jitter_scale", taa.jitter_scale));
        taa.jitter_samples = static_cast<kin::u32>(doc->number_at("jitter_samples", taa.jitter_samples));
    }
    if (over.taa) {
        taa.enabled = true;
        if (over.taa_blend > 0.0) {
            taa.blend = static_cast<float>(over.taa_blend);
        }
        if (over.taa_sharpness > 0.0) {
            taa.sharpness = static_cast<float>(over.taa_sharpness);
        }
    }
}

// The linear light of an 8-bit sRGB value, and back.
float decode_srgb(kin::u8 v) {
    return kin::srgb_to_linear(static_cast<float>(v) / 255.0f);
}
kin::u8 encode_srgb(float c) {
    return static_cast<kin::u8>(std::lround(kin::linear_to_srgb(std::clamp(c, 0.0f, 1.0f)) * 255.0f));
}

// A picture of `size` / `supersample` pixels a side made from `large`, each pixel the average, in linear light,
// of the supersample x supersample pixels it covers.
kin::Image supersample_average(const kin::Image& large, kin::Vec2i size, int supersample) {
    kin::Image small;
    small.size = size;
    small.rgba.assign(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 4, 0);
    for (int y = 0; y < size.y; ++y) {
        for (int x = 0; x < size.x; ++x) {
            float sum[3] = {0, 0, 0};
            for (int sy = 0; sy < supersample; ++sy) {
                for (int sx = 0; sx < supersample; ++sx) {
                    const std::size_t i = (static_cast<std::size_t>(y * supersample + sy) * static_cast<std::size_t>(large.size.x) +
                                           static_cast<std::size_t>(x * supersample + sx)) * 4;
                    for (int ch = 0; ch < 3; ++ch) {
                        sum[ch] += decode_srgb(large.rgba[i + static_cast<std::size_t>(ch)]);
                    }
                }
            }
            const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x)) * 4;
            for (int ch = 0; ch < 3; ++ch) {
                small.rgba[o + static_cast<std::size_t>(ch)] = encode_srgb(sum[ch] / static_cast<float>(supersample * supersample));
            }
            small.rgba[o + 3] = 255;
        }
    }
    return small;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // a line at a time: what was done shows if it stops
    std::vector<fs::path> scenes;
    fs::path out = "reference_out";
    const char* cache_env = std::getenv("KIN_REFERENCE_CACHE");
    const char* home = std::getenv("HOME");
    fs::path cache = cache_env ? fs::path{cache_env} : fs::path{home ? home : "."} / ".cache" / "kin" / "reference";
    int frames = 60, warmup = 10, repeat = 8, width = 1280, height = 720;
    bool gpu_timing = true;
    int cascades_override = 0; // 0: the scene's own
    double distance_override = 0.0;
    Overrides over;
    int taa_frames = 32, supersample = 1, samples_override = 0; // 0: the scene's
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
        } else if (a == "--cascades") {
            cascades_override = std::max(1, std::atoi(next().c_str()));
        } else if (a == "--shadow-distance") {
            distance_override = std::atof(next().c_str());
        } else if (a == "--ao") {
            over.ao = true;
        } else if (a == "--ao-half") {
            over.ao = over.ao_half = true;
        } else if (a == "--ao-radius") {
            over.ao = true;
            over.ao_radius = std::atof(next().c_str());
        } else if (a == "--ao-intensity") {
            over.ao = true;
            over.ao_intensity = std::atof(next().c_str());
        } else if (a == "--contact-shadows") {
            over.contact = true;
        } else if (a == "--contact-distance") {
            over.contact = true;
            over.contact_distance = std::atof(next().c_str());
        } else if (a == "--contact-thickness") {
            over.contact = true;
            over.contact_thickness = std::atof(next().c_str());
        } else if (a == "--contact-steps") {
            over.contact = true;
            over.contact_steps = std::atoi(next().c_str());
        } else if (a == "--taa") {
            over.taa = true;
        } else if (a == "--taa-blend") {
            over.taa = true;
            over.taa_blend = std::atof(next().c_str());
        } else if (a == "--taa-sharpness") {
            over.taa = true;
            over.taa_sharpness = std::atof(next().c_str());
        } else if (a == "--taa-frames") {
            taa_frames = std::max(1, std::atoi(next().c_str()));
        } else if (a == "--samples") {
            samples_override = std::clamp(std::atoi(next().c_str()), 1, 8);
        } else if (a == "--supersample") {
            supersample = std::clamp(std::atoi(next().c_str()), 1, 6);
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
        Motion motion;
        const bool is_motion = doc.string_at("procedural") == "motion";
        if (is_motion) {
            motion.make(r3d);
        }
        const bool animated = doc.bool_at("animated");
        const double load_ms = ms_since(load_start);
        const kin::Bounds3 bounds = is_crowd    ? kin::Bounds3{{0.0f, -1.0f, 0.0f}, {48.0f, 3.0f, 32.0f}}
                                    : is_motion ? kin::Bounds3{{-8.0f, -0.2f, -6.0f}, {8.0f, 4.0f, 6.0f}}
                                                : bounds_of(placed);

        // The view.
        const kin::JsonValue* v = doc.find("view");
        const kin::JsonValue none{};
        const kin::JsonValue& view_doc = v ? *v : none;
        const auto make_view = [&](kin::Vec2i size, kin::u32 samples) {
            return r3d.create_view(size, {.samples = samples,
                                          .background = colour(view_doc.find("background"), kin::Color::rgb(0, 0, 0)),
                                          .hdr = view_doc.bool_at("hdr"),
                                          .exposure = static_cast<float>(view_doc.number_at("exposure", 1.0)),
                                          .bloom = static_cast<float>(view_doc.number_at("bloom", 0.0))});
        };
        kin::View3D view = make_view({width, height}, static_cast<kin::u32>(samples_override > 0 ? samples_override : view_doc.number_at("samples", 4)));

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
        const auto fill_scene = [&](int time) {
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
                scene.lights.shadow_cascades = static_cast<kin::u32>(lights.number_at("shadow_cascades", 1));
                scene.lights.shadow_distance = static_cast<float>(lights.number_at("shadow_distance", scene.lights.shadow_distance));
                if (cascades_override > 0) {
                    scene.lights.shadow_cascades = static_cast<kin::u32>(cascades_override);
                }
                if (distance_override > 0.0) {
                    scene.lights.shadow_distance = static_cast<float>(distance_override);
                }
            }
            merge_ambient_occlusion(scene.ambient_occlusion, doc.find("ambient_occlusion"), over);
            merge_contact_shadows(scene.contact_shadows, doc.find("contact_shadows"), over);
            scene.taa = {};
            merge_taa(scene.taa, doc.find("taa"), over);
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
            if (is_motion) {
                motion.draw(scene, time);
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
            // The scene at `time` (frames; 0 is the picture, earlier is negative), the camera
            // having panned `pan` units a frame to reach `eye` at 0.
            const Vec3f pan = vec3(c.find("pan"));
            const auto pose = [&](int time) {
                fill_scene(time);
                const Vec3f moved = pan * static_cast<float>(time);
                scene.camera = kin::Camera3D::look_at(eye + moved, target + moved, fov);
                scene.camera.z_near = static_cast<float>(c.number_at("near", 0.05));
            };
            // A temporal history starts afresh at each camera, and runs long enough to settle.
            view.reset_history();
            pose(0); // for what the scene asks of temporal anti-aliasing
            const int run = warmup + frames + (scene.taa.enabled ? std::max(0, taa_frames - warmup - frames) : 0);
            const int each = animated ? 1 : repeat;

            // The first picture: pipelines and targets made, the GPU waited for.
            const auto first_start = std::chrono::steady_clock::now();
            pose(-run - 1);
            renderer.clear(kin::Color::rgb(0, 0, 0));
            r3d.render(view, scene);
            renderer.draw_texture(view.texture(), full);
            wait_for_gpu();
            const double first_ms = ms_since(first_start);
            renderer.present();

            std::vector<double> cpu, gpu;
            for (int f = 0; f < run; ++f) {
                pose(f - run);
                renderer.clear(kin::Color::rgb(0, 0, 0));
                for (int k = 0; k < each; ++k) {
                    r3d.render(view, scene);
                }
                renderer.draw_texture(view.texture(), full);
                renderer.present();
                if (f >= run - frames) {
                    cpu.push_back(r3d.stats().cpu_ms);
                    gpu.push_back(renderer.backend_stats().last_gpu_frame_ms / each);
                }
            }
            wait_for_gpu();

            // The picture.
            pose(0);
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
            // The same moment drawn large and averaged down, with temporal anti-aliasing off.
            if (supersample > 1) {
                const kin::Vec2i big_size{width * supersample, height * supersample};
                kin::View3D big = make_view(big_size, 4);
                kin::RenderTarget target = renderer.create_render_target(big_size);
                pose(0);
                scene.taa.enabled = false;
                kin::Image large;
                if (big && target.valid()) {
                    renderer.push_render_target(target);
                    renderer.clear(kin::Color::rgb(0, 0, 0));
                    r3d.render(big, scene);
                    const kin::Rectf big_rect{0.0f, 0.0f, static_cast<float>(big_size.x), static_cast<float>(big_size.y)};
                    renderer.draw_texture(big.texture(), big_rect);
                    renderer.read_rgba(big_rect, large.rgba, large.size);
                    renderer.pop_render_target();
                    renderer.present();
                }
                if (large.size.x == big_size.x && large.size.y == big_size.y) {
                    const kin::Image small = supersample_average(large, {width, height}, supersample);
                    kin::save_image(small, out / name / (camera_name + "_ss.png"));
                } else {
                    std::fprintf(stderr, "  %s: could not draw the supersampled picture\n", camera_name.c_str());
                }
            }
            const Summary cpu_s = summarize(cpu), gpu_s = summarize(gpu);
            std::printf("  %-14s render() %7.3f ms  GPU %7.3f ms  (p10-p90 %.3f-%.3f)  %u draws, %llu triangles, %u shadow casters (%u cascades)\n",
                        camera_name.c_str(), cpu_s.median, gpu_s.median, gpu_s.p10, gpu_s.p90, stats.draws,
                        static_cast<unsigned long long>(stats.triangles), stats.shadow_casters, stats.shadow_cascades);
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
                   << ", \"shadow_casters\": " << stats.shadow_casters << ", \"shadow_cascades\": " << stats.shadow_cascades
                   << ", \"cascade_casters\": " << json_list(stats.cascade_casters) << ", \"ao_draws\": " << stats.ao_draws
                   << ", \"ao_instances\": " << stats.ao_instances
                   << ", \"ao_dispatches\": " << stats.ao_dispatches << ", \"contact_dispatches\": " << stats.contact_dispatches
                   << ", \"taa_dispatches\": " << stats.taa_dispatches << ", \"lights\": " << stats.lights << "}";
        }
        report << "]}";
    }
    report << "\n ]\n}\n";
    std::ofstream(out / "results.json") << report.str();
    renderer.set_job_system(nullptr);
    return failures == 0 ? 0 : 1;
}
