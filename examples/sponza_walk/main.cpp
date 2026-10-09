// Sponza Walk: fly a camera round Sponza on kin's experimental 3D renderer,
// with every renderer feature switchable live from a panel (docs/rendering_3d.md,
// "Sponza Walk"). Sponza is not in the repository: tools/reference/fetch.py
// puts it in the reference cache.
//
// What can be switched lives in one Settings struct (settings.hpp), applied to
// the Scene3D every frame. The panel, `--set key=value`, `--preset` and the
// printed settings all read one table of rows (SponzaScene::build_controls):
// a new feature (contact shadows, TAA) is a field and a row there.
#include "settings.hpp"

#include <kin/core/json.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/widgets.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace sponza_walk {
namespace {

namespace fs = std::filesystem;

constexpr int exit_skipped = 77; // CTest's SKIP_RETURN_CODE

struct Options {
    fs::path model;
    std::string screenshot;
    std::vector<std::string> sets; // key=value, in order (presets expand to these)
    std::string camera = "nave";
    bool eye_given = false;
    Vec3f eye{}, target{};
    float spin = 0.0f;            // degrees of yaw a frame (a scripted camera path)
    int width = 1280, height = 800;
    float sensitivity = 0.0f;     // 0: the default
    float ui_scale = 0.0f;        // 0: the display's
    bool hud = false;             // headless: draw the panel and stats too
    bool vsync = false;
    bool list_settings = false;
    bool print = false;           // headless: print the settings after the last frame
    bool skip_if_unavailable = false;
    bool help = false;
    int frames = 0;               // headless --frames
    int lod_levels = 4;           // levels of detail built for every mesh at load (1: none)
    bool lod_clusters = false;    // clusters (a hierarchy a mesh, cut by the error bound) instead of levels
};

// Named sets of `--set` values, for a run without a display to try a feature.
const std::map<std::string, std::string>& presets() {
    static const std::map<std::string, std::string> p{
        {"ao", "ao=on"},
        {"ao-half", "ao=on,ao_half=on"},
        {"contact", "contact=on"},
        {"lod", "lod=on"},
        {"clusters", "lod=on"}, // also builds clusters at load (as --lod-clusters)
        {"clusters-gpu", "lod=on,lod_gpu=on,lod_readback=on"}, // the same, cut in compute, with the counts read back
        {"taa", "taa=on"},
        {"taa-sharp", "taa=on,taa_sharpness=0.4"},
        {"taa-frozen", "taa=on,taa_freeze=on"},
        {"no-shadows", "shadows=off"},
        {"cascades1", "cascades=1"},
        {"cascades4", "cascades=4,shadow_distance=60"},
        {"sharp-shadows", "shadow_area=camera,shadow_extent=14,shadow_resolution=4096"},
        {"no-normals", "normal_maps=off"},
        {"no-textures", "textures=off"},
        {"no-pbr", "metal_rough_maps=off"},
        {"no-cutout", "cutout=off"},
        {"unlit", "unlit=on"},
        {"sun-off", "sun=off"},
        {"ambient-only", "sun=off,fill=off"},
        {"low-sun", "sun_elevation=10,sun_azimuth=20"},
        {"env", "environment=1,sun_intensity=0.6"},
        {"wireframe", "wireframe=overlay"},
        {"wire-only", "wireframe=only"},
        {"bloom", "bloom=0.8,bloom_threshold=0.9"},
        {"msaa1", "msaa=1"},
        {"msaa8", "msaa=8"},
        {"point-lights", "drop_light=1,light_r=14,light_g=9,light_b=4"},
    };
    return p;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> out;
    std::stringstream stream(text);
    for (std::string part; std::getline(stream, part, separator);) {
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

bool parse_vec3(const std::string& text, Vec3f& out) {
    const auto parts = split(text, ',');
    if (parts.size() != 3) return false;
    try {
        out = {std::stof(parts[0]), std::stof(parts[1]), std::stof(parts[2])};
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

fs::path cache_dir() {
    if (const char* env = std::getenv("KIN_REFERENCE_CACHE"); env && *env) return env;
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    return fs::path(home ? home : ".") / ".cache" / "kin" / "reference";
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 == argc) throw std::invalid_argument(arg + " needs a value");
            return argv[++i];
        };
        const auto number = [&](float& out, float lo, float hi) {
            const std::string text = value();
            try {
                out = std::stof(text);
            } catch (const std::exception&) {
                throw std::invalid_argument(arg + " needs a number");
            }
            if (!std::isfinite(out) || out < lo || out > hi) throw std::invalid_argument(arg + " is out of range");
        };
        const auto integer = [&](int& out, int lo, int hi) {
            float v = 0;
            number(v, float(lo), float(hi));
            out = static_cast<int>(v);
        };
        if (arg == "--model") o.model = value();
        else if (arg == "--screenshot") o.screenshot = value();
        else if (arg.starts_with("--screenshot=")) o.screenshot = arg.substr(13);
        else if (arg == "--set") o.sets.push_back(value());
        else if (arg == "--preset") {
            for (const std::string& name : split(value(), ',')) {
                const auto it = presets().find(name);
                if (it == presets().end()) throw std::invalid_argument("no preset named " + name + " (--help lists them)");
                for (const std::string& set : split(it->second, ',')) o.sets.push_back(set);
                if (name == "clusters" || name == "clusters-gpu") o.lod_clusters = true;
            }
        } else if (arg == "--camera") {
            o.camera = value();
            if (o.camera != "nave" && o.camera != "roof") throw std::invalid_argument("--camera is nave or roof");
        } else if (arg == "--eye") {
            if (!parse_vec3(value(), o.eye)) throw std::invalid_argument("--eye needs x,y,z");
            o.eye_given = true;
        } else if (arg == "--target") {
            if (!parse_vec3(value(), o.target)) throw std::invalid_argument("--target needs x,y,z");
        } else if (arg == "--spin") number(o.spin, -90, 90);
        else if (arg == "--width") integer(o.width, 320, 4096);
        else if (arg == "--height") integer(o.height, 200, 4096);
        else if (arg == "--sensitivity") number(o.sensitivity, 0.005f, 2);
        else if (arg == "--ui-scale") number(o.ui_scale, 0.5f, 4);
        else if (arg == "--lod-clusters") o.lod_clusters = true;
        else if (arg == "--hud") o.hud = true;
        else if (arg == "--vsync") o.vsync = true;
        else if (arg == "--list-settings") o.list_settings = true;
        else if (arg == "--print-settings") o.print = true;
        else if (arg == "--skip-if-unavailable") o.skip_if_unavailable = true;
        else if (arg == "--help") o.help = true;
    }
    return o;
}

// One line of the panel: the switch, its range, and how to read and write it
// as a number (a check is 0 or 1, a choice its index), so the panel, --set and
// the printed settings all share it.
struct Row {
    enum class Kind { Section, Check, Slider, Choice, Button, Note };
    Kind kind = Kind::Note;
    std::string key, label;
    std::function<float()> get;
    std::function<void(float)> set; // a Button runs on any value
    float lo = 0, hi = 1;
    bool integer = false;
    std::vector<std::string> choices;
    std::function<bool()> enabled;
    bool open = false; // Section
};

struct Hooks {
    std::function<void()> view_nave, view_roof, drop_light, clear_lights, screenshot, print, reset, reset_history;
};

struct Caps {
    std::vector<unsigned> samples{1, 2, 4, 8};
    std::vector<std::string> hdris; // file stems
    bool clusters = false;          // the meshes were built with clusters (--lod-clusters), not levels
    bool gpu_clusters = false;      // the device can cut them in compute
};

// The table of switches. Adding a feature is adding its row here.
class Controls {
public:
    Controls(Settings& s, Hooks h, const Caps& caps) : _s(s), _h(std::move(h)) { build(caps); }

    std::vector<Row> rows;

    Row* find(const std::string& key) {
        for (Row& r : rows) {
            if (r.key == key) return &r;
        }
        return nullptr;
    }
    // "on", "off", a number, or a choice's name.
    bool set(const std::string& key, const std::string& text, std::string& error) {
        Row* r = find(key);
        if (!r || r->kind == Row::Kind::Section || r->kind == Row::Kind::Note) {
            error = "no setting named " + key + " (--list-settings)";
            return false;
        }
        const std::string v = lower(text);
        float x = 0;
        if (r->kind == Row::Kind::Check) {
            if (v == "on" || v == "true" || v == "1" || v == "yes") x = 1;
            else if (v == "off" || v == "false" || v == "0" || v == "no") x = 0;
            else {
                error = key + " is on or off";
                return false;
            }
        } else if (r->kind == Row::Kind::Choice) {
            const auto it = std::find_if(r->choices.begin(), r->choices.end(), [&](const std::string& c) { return lower(c) == v; });
            if (it != r->choices.end()) {
                x = float(it - r->choices.begin());
            } else {
                try {
                    x = std::stof(v);
                } catch (const std::exception&) {
                    error = key + " has no choice " + text;
                    return false;
                }
                if (x < 0 || x >= float(r->choices.size())) {
                    error = key + " has no choice " + text + " here";
                    return false;
                }
            }
        } else {
            try {
                x = std::stof(v);
            } catch (const std::exception&) {
                error = key + " needs a number";
                return false;
            }
        }
        r->set(x);
        return true;
    }
    static std::string text(const Row& r) {
        char b[48];
        switch (r.kind) {
        case Row::Kind::Check: return r.get() > 0.5f ? "on" : "off";
        case Row::Kind::Choice: {
            const std::size_t i = static_cast<std::size_t>(std::clamp(int(r.get()), 0, int(r.choices.size()) - 1));
            return r.choices.empty() ? "" : r.choices[i];
        }
        case Row::Kind::Slider:
            std::snprintf(b, sizeof b, r.integer ? "%.0f" : (r.hi >= 100 ? "%.1f" : "%.2f"), r.get());
            return b;
        default: return "";
        }
    }

private:
    void add(Row r) { rows.push_back(std::move(r)); }
    void section(const std::string& label, bool open = false) {
        add({.kind = Row::Kind::Section, .key = "section:" + label, .label = label, .open = open});
    }
    void note(const std::string& text) { add({.kind = Row::Kind::Note, .key = "note:" + text, .label = text}); }
    void check(const std::string& key, const std::string& label, bool& v) {
        add({.kind = Row::Kind::Check, .key = key, .label = label, .get = [&v] { return v ? 1.0f : 0.0f; },
             .set = [&v](float x) { v = x > 0.5f; }});
    }
    void slider(const std::string& key, const std::string& label, float& v, float lo, float hi) {
        add({.kind = Row::Kind::Slider, .key = key, .label = label, .get = [&v] { return v; }, .set = [&v](float x) { v = x; },
             .lo = lo, .hi = hi});
    }
    void islider(const std::string& key, const std::string& label, int& v, int lo, int hi) {
        add({.kind = Row::Kind::Slider, .key = key, .label = label, .get = [&v] { return float(v); },
             .set = [&v](float x) { v = int(std::lround(x)); }, .lo = float(lo), .hi = float(hi), .integer = true});
    }
    void choice(const std::string& key, const std::string& label, int& v, std::vector<std::string> names) {
        add({.kind = Row::Kind::Choice, .key = key, .label = label, .get = [&v] { return float(v); },
             .set = [&v](float x) { v = int(std::lround(x)); }, .choices = std::move(names)});
    }
    void button(const std::string& key, const std::string& label, std::function<void()> f) {
        add({.kind = Row::Kind::Button, .key = key, .label = label, .set = [f = std::move(f)](float) { if (f) f(); }});
    }
    // The last row greyed unless `f`.
    void only_if(std::function<bool()> f) { rows.back().enabled = std::move(f); }
    // A slider over the colour or size of the newest point light.
    void light_slider(const std::string& key, const std::string& label, float PointLight::*field, int channel, float lo, float hi) {
        Settings* s = &_s;
        add({.kind = Row::Kind::Slider, .key = key, .label = label,
             .get = [s, field, channel] {
                 if (s->points.empty()) return 0.0f;
                 PointLight& l = s->points.back();
                 return field ? l.*field : (channel == 0 ? l.color.x : channel == 1 ? l.color.y : l.color.z);
             },
             .set = [s, field, channel](float x) {
                 if (s->points.empty()) return;
                 PointLight& l = s->points.back();
                 if (field) l.*field = x;
                 else (channel == 0 ? l.color.x : channel == 1 ? l.color.y : l.color.z) = x;
             },
             .lo = lo, .hi = hi, .enabled = [s] { return !s->points.empty(); }});
    }

    void build(const Caps& caps) {
        Settings& s = _s;
        MaterialSettings& m = s.materials;
        section("Camera");
        slider("move_speed", "Move speed (m/s)", s.move_speed, 0.2f, 40.0f);
        slider("sensitivity", "Mouse (deg/px)", s.sensitivity, 0.01f, 0.4f);
        slider("fov", "Field of view", s.fov, 30.0f, 110.0f);
        slider("near", "Near plane", s.near_plane, 0.02f, 1.0f);
        check("far_limit", "Limit the far plane", s.far_limit);
        slider("far", "Far plane", s.far_plane, 10.0f, 400.0f);
        only_if([&s] { return s.far_limit; });
        button("view_nave", "Jump to 'nave' (1)", _h.view_nave);
        button("view_roof", "Jump to 'roof' (2)", _h.view_roof);

        section("Sun", true);
        check("sun", "Sun", s.sun);
        slider("sun_intensity", "Intensity", s.sun_intensity, 0.0f, 4.0f);
        slider("sun_azimuth", "Azimuth", s.sun_azimuth, -180.0f, 180.0f);
        slider("sun_elevation", "Elevation", s.sun_elevation, -10.0f, 90.0f);
        check("sun_animate", "Animate the sun (L)", s.sun_animate);
        slider("sun_speed", "Animation (deg/s)", s.sun_speed, -90.0f, 90.0f);
        check("fill", "Fill light (no shadow)", s.fill);
        slider("fill_intensity", "Fill intensity", s.fill_intensity, 0.0f, 3.0f);

        section("Ambient");
        check("ambient", "Hemisphere ambient", s.ambient);
        slider("ambient_intensity", "Ambient intensity", s.ambient_intensity, 0.0f, 3.0f);
        check("ground", "Ground ambient (sky above, ground below)", s.ground);
        slider("ground_intensity", "Ground intensity", s.ground_intensity, 0.0f, 3.0f);
        std::vector<std::string> envs{"none"};
        envs.insert(envs.end(), caps.hdris.begin(), caps.hdris.end());
        if (caps.hdris.empty()) {
            note("No HDRI in the cache (tools/reference/fetch.py)");
        } else {
            choice("environment", "Environment light", s.environment, envs);
            note("An environment replaces the hemisphere ambient");
        }
        slider("environment_intensity", "Environment intensity", s.environment_intensity, 0.0f, 4.0f);
        slider("environment_rotation", "Environment rotation", s.environment_rotation, 0.0f, 360.0f);

        section("Point lights");
        button("drop_light", "Drop a light at the camera (K)", _h.drop_light);
        button("clear_lights", "Clear all lights", _h.clear_lights);
        check("light_markers", "Show light markers", s.light_markers);
        light_slider("light_r", "Newest: red", nullptr, 0, 0.0f, 40.0f);
        light_slider("light_g", "Newest: green", nullptr, 1, 0.0f, 40.0f);
        light_slider("light_b", "Newest: blue", nullptr, 2, 0.0f, 40.0f);
        light_slider("light_radius", "Newest: radius", &PointLight::radius, 0, 0.5f, 40.0f);
        light_slider("light_size", "Newest: size", &PointLight::size, 0, 0.01f, 2.0f);

        section("Shadows");
        check("shadows", "Sun shadows", s.shadows);
        choice("shadow_resolution", "Map size", s.shadow_resolution, {"512", "1024", "2048", "4096"});
        islider("cascades", "Cascades", s.shadow_cascades, 1, 4);
        slider("shadow_distance", "Cascade distance", s.shadow_distance, 5.0f, 200.0f);
        choice("shadow_area", "Area", s.shadow_area, {"auto", "camera"});
        slider("shadow_extent", "Camera area half-size", s.shadow_extent, 3.0f, 60.0f);

        section("Ambient occlusion");
        check("ao", "Occlusion (GTAO)", s.ao);
        slider("ao_radius", "Radius", s.ao_radius, 0.05f, 3.0f);
        slider("ao_intensity", "Intensity", s.ao_intensity, 0.0f, 3.0f);
        check("ao_half", "Half resolution", s.ao_half);

        section("Contact shadows");
        check("contact", "Contact shadows", s.contact);
        slider("contact_distance", "Distance", s.contact_distance, 0.05f, 3.0f);
        slider("contact_thickness", "Thickness", s.contact_thickness, 0.02f, 1.0f);
        slider("contact_intensity", "Intensity", s.contact_intensity, 0.0f, 1.0f);
        islider("contact_steps", "Steps", s.contact_steps, 4, 64);

        section("Levels of detail");
        check("lod", caps.clusters ? "Clusters (cut by error)" : "Levels of detail", s.lod);
        slider("lod_error", "Error (pixels)", s.lod_error, 0.1f, 16.0f);
        slider("lod_bias", "Bias (quality multiplier)", s.lod_bias, 0.25f, 8.0f);
        islider("lod_force", caps.clusters ? "Force cut depth (-1: by error, 0: leaves)" : "Force level (-1: by distance)", s.lod_force, -1,
                caps.clusters ? 20 : 7);
        if (caps.clusters) {
            check("lod_gpu", caps.gpu_clusters ? "Cut on the GPU (compute)" : "Cut on the GPU (this device cannot: the CPU cuts)", s.lod_gpu);
            check("lod_readback", "Read the GPU cut's counts back (waits for the GPU)", s.lod_readback);
        }
        note(caps.clusters ? "Mode: clusters, built at load (--lod-clusters); off and levels are the other modes"
                                   : "Mode: levels, built at load: --lod-levels N (4; 1 builds none); --lod-clusters builds clusters instead");

        section("Temporal anti-aliasing");
        check("taa", "TAA (replaces MSAA)", s.taa);
        slider("taa_blend", "History kept", s.taa_blend, 0.0f, 0.98f);
        slider("taa_sharpness", "Sharpen", s.taa_sharpness, 0.0f, 1.0f);
        slider("taa_jitter_scale", "Jitter scale (0: no AA)", s.taa_jitter_scale, 0.0f, 2.0f);
        islider("taa_jitter_samples", "Jitter samples", s.taa_jitter_samples, 1, 16);
        check("taa_freeze", "Freeze jitter (debug)", s.taa_freeze);
        button("taa_reset_history", "Reset history (a camera cut)", _h.reset_history);

        section("View");
        std::vector<std::string> samples;
        for (unsigned n : caps.samples) samples.push_back(std::to_string(n));
        choice("msaa", "MSAA samples", s.msaa, samples);
        slider("bg_r", "Background red", s.background[0], 0.0f, 255.0f);
        slider("bg_g", "Background green", s.background[1], 0.0f, 255.0f);
        slider("bg_b", "Background blue", s.background[2], 0.0f, 255.0f);
        check("cull", "Frustum culling", s.cull);
        choice("wireframe", "Wireframe", s.wireframe, {"off", "overlay", "only"});

        section("Post");
        check("hdr", "HDR view (exposed and tonemapped)", s.hdr);
        slider("exposure", "Exposure", s.exposure, 0.1f, 4.0f);
        slider("bloom", "Bloom (turns HDR on)", s.bloom, 0.0f, 2.0f);
        slider("bloom_threshold", "Bloom threshold", s.bloom_threshold, 0.0f, 4.0f);

        section("Materials");
        check("textures", "Base colour textures", m.textures);
        check("normal_maps", "Normal maps", m.normal_maps);
        slider("normal_scale", "Normal strength", m.normal_scale, 0.0f, 3.0f);
        check("metal_rough_maps", "Metallic-roughness maps", m.metal_rough_maps);
        check("roughness_override", "Override roughness", m.roughness_override);
        slider("roughness", "Roughness", m.roughness, 0.0f, 1.0f);
        check("metallic_override", "Override metallic", m.metallic_override);
        slider("metallic", "Metallic", m.metallic, 0.0f, 1.0f);
        check("occlusion_maps", "Occlusion maps", m.occlusion_maps);
        check("emissive", "Emissive", m.emissive);
        slider("emissive_scale", "Emissive strength", m.emissive_scale, 0.0f, 10.0f);
        check("cutout", "Alpha cut-out", m.cutout);
        check("blended", "Blended parts drawn", m.blended);
        check("double_sided", "Force double-sided", m.force_double_sided);
        check("casts_shadow", "Parts cast shadows", m.casts_shadow);
        check("receives_shadow", "Parts receive shadows", m.receives_shadow);
        check("unlit", "Force unlit", m.force_unlit);

        section("Debug", true);
        check("stats", "Stats (F3)", s.stats);
        check("gpu_timing", "GPU timing", s.gpu_timing);
        button("screenshot", "Screenshot (F12 or P)", _h.screenshot);
        button("print", "Print settings to the console (F2)", _h.print);
        button("reset", "Reset all", _h.reset);
        section("Keys");
        note("WASD move, Q/Ctrl down, E/Space up, Shift fast");
        note("Wheel: speed. Esc: free the mouse / quit");
        note("Tab: panel or mouse. 1/2: views. L: sun, K: light");
    }

    Settings& _s;
    Hooks _h;
};

std::string json_number(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", double(v));
    return b;
}
std::string json_vec(Vec3f v) { return "[" + json_number(v.x) + ", " + json_number(v.y) + ", " + json_number(v.z) + "]"; }

using Clock = std::chrono::steady_clock;

class SponzaScene final : public Scene {
public:
    SponzaScene(Options options, fs::path model_path) : _options(std::move(options)), _model_path(std::move(model_path)) {
        if (_options.sensitivity > 0) _s.sensitivity = _options.sensitivity;
    }
    ~SponzaScene() override { release_mouse(); }

    std::string_view name() const override { return "Sponza Walk"; }

    void update(SceneContext&) override {}

    void render(SceneContext& ctx) override {
        Renderer2D& renderer = ctx.renderer;
        if (!_ready && !init(ctx)) return;
        const bool headless = ctx.app.headless();

        // Frame time: the window's own clock; fixed for the scripted headless run.
        const auto now = Clock::now();
        float dt = headless ? 1.0f / 60.0f : std::chrono::duration<float>(now - _last).count();
        _last = now;
        dt = std::clamp(dt, 0.0001f, 0.1f);
        _frame_ms = _frame_ms == 0 ? dt * 1000 : _frame_ms + (dt * 1000 - _frame_ms) * 0.05f;

        Input& input = ctx.input;
        handle_keys(ctx, input, headless);
        if (_quit) {
            ctx.app.quit();
            return;
        }
        // Everything is drawn, and the mouse read, in the display's own pixels.
        const auto native = renderer.scoped_native_coordinates();
        const Vec2i out = renderer.output_size();
        // The panel follows the display's scale, but never takes more than
        // a share of a small window.
        float u = _options.ui_scale > 0 ? _options.ui_scale : headless ? 1.0f : std::clamp(ctx.window.display_scale(), 1.0f, 3.0f);
        if (_options.ui_scale <= 0) u = std::max(0.75f, std::min({u, float(out.x) / 900.0f, float(out.y) / 560.0f}));
        const bool panel_shown = !_captured && (!headless || _options.hud);
        const Rectf panel = panel_rect(out, u);

        // The mouse: a click on the picture (not the panel) captures it.
        _ui.begin(input, renderer, dt);
        const Vec2f pointer = _ui.pointer();
        const bool over_panel = panel_shown && pointer.x >= panel.x && pointer.x <= panel.x + panel.w && pointer.y >= panel.y &&
                                pointer.y <= panel.y + panel.h;
        bool skip_motion = false;
        if (!headless && !_captured && !over_panel && input.mouse_frame_pressed(MouseButton::Left)) {
            capture_mouse(ctx.window);
            skip_motion = true;
        }
        const float wheel = input.frame_mouse_wheel_y();
        if (wheel != 0 && !over_panel && (_captured || headless)) _s.move_speed = std::clamp(_s.move_speed * std::pow(1.15f, wheel), 0.2f, 40.0f);
        if (_captured && !skip_motion) {
            const Vec2f motion = input.frame_mouse_motion();
            _yaw = std::remainder(_yaw - motion.x * _s.sensitivity, 360.0f);
            _pitch = std::clamp(_pitch - motion.y * _s.sensitivity, -89.0f, 89.0f);
        }
        _yaw += _options.spin;
        move(input, dt);
        if (_s.sun_animate) _s.sun_azimuth = std::remainder(_s.sun_azimuth + _s.sun_speed * dt, 360.0f);

        // The view, made again when a setting that is part of it changes (not
        // while a slider is held, so dragging stays smooth).
        const ViewKey key = view_key(out);
        if (!_view.valid()) {
            _view = _r3d->create_view(key.size, view_options(key));
            _key = key;
        } else if (key != _key && !(input.mouse_held(MouseButton::Left) && key.size == _key.size)) {
            if (key.size != _key.size && key.samples == _key.samples && key.background == _key.background && key.hdr == _key.hdr &&
                key.exposure == _key.exposure && key.bloom == _key.bloom && key.threshold == _key.threshold) {
                _r3d->resize(_view, key.size);
            } else {
                _view = _r3d->create_view(key.size, view_options(key));
            }
            _key = key;
        }
        renderer.set_gpu_timing_enabled(_s.gpu_timing);
        _backend_stats = renderer.backend_stats();

        fill_scene();
        _r3d->render(_view, _scene);
        renderer.clear(Color::rgb(0, 0, 0));
        renderer.draw_texture(_view.texture(), {0.0f, 0.0f, float(out.x), float(out.y)});
        ++_frames;
        take_screenshots(ctx, out, !_options.hud, true);

        draw_ui(ctx, out, u, panel, panel_shown, dt);
        _ui.end();
        if (_options.hud) take_screenshots(ctx, out, true, false);
        if (headless && _options.print && _frames == std::max(1, _options.frames)) print_settings();
    }

private:
    // --- start-up ------------------------------------------------------------

    bool init(SceneContext& ctx) {
        _r3d = std::make_unique<Renderer3D>(ctx.renderer);
        std::printf("sponza_walk: window %dx%d units, %dx%d pixels, display scale %.2f, output %dx%d\n", ctx.window.size().x, ctx.window.size().y,
                    ctx.window.pixel_size().x, ctx.window.pixel_size().y, double(ctx.window.display_scale()), ctx.renderer.output_size().x,
                    ctx.renderer.output_size().y);
        if (!_r3d->available()) {
            fail(ctx, "this renderer cannot draw 3D: it needs SDL_GPU (KIN_RENDER_BACKEND=gpu, a Vulkan/Metal/D3D12 device)");
            return false;
        }
        _caps.clusters = _options.lod_clusters;
        _caps.gpu_clusters = _r3d->capabilities().gpu_driven_clusters;
        const auto sample_cap = _r3d->capabilities().max_samples;
        _caps.samples.clear();
        for (unsigned n : {1u, 2u, 4u, 8u}) {
            if (n <= sample_cap) _caps.samples.push_back(n);
        }
        std::error_code ec;
        for (const fs::directory_entry& e : fs::directory_iterator(cache_dir() / "hdri", ec)) {
            if (e.path().extension() == ".hdr") _hdri_files.push_back(e.path());
        }
        std::sort(_hdri_files.begin(), _hdri_files.end());
        for (const fs::path& p : _hdri_files) _caps.hdris.push_back(p.stem().string());
        _environments.resize(_hdri_files.size());
        _env_tried.assign(_hdri_files.size(), false);
        // 4 samples where the device has them, else the most.
        _default_msaa = 0;
        for (std::size_t i = 0; i < _caps.samples.size(); ++i) {
            if (_caps.samples[i] <= 4) _default_msaa = int(i);
        }
        _s.msaa = _default_msaa;

        build_controls();
        if (_options.list_settings) {
            for (const Row& r : _controls->rows) {
                if (r.kind == Row::Kind::Note || r.kind == Row::Kind::Section) continue;
                std::cout << r.key << (r.kind == Row::Kind::Button ? "  (action)" : "  = " + Controls::text(r));
                if (r.kind == Row::Kind::Choice) {
                    std::cout << "   [";
                    for (std::size_t i = 0; i < r.choices.size(); ++i) std::cout << (i ? ", " : "") << r.choices[i];
                    std::cout << ']';
                } else if (r.kind == Row::Kind::Slider) {
                    std::cout << "   [" << json_number(r.lo) << " .. " << json_number(r.hi) << ']';
                }
                std::cout << '\n';
            }
            _quit = true;
            _ready = true;
            return true;
        }

        std::string error;
        const auto start = Clock::now();
        MeshOptions mesh_options;
        mesh_options.levels = static_cast<u32>(_options.lod_levels);
        mesh_options.clusters = _options.lod_clusters;
        _model = _r3d->load_model(_model_path, mesh_options, &error);
        if (!_model) {
            fail(ctx, "could not load " + _model_path.string() + ": " + error);
            return false;
        }
        if (_options.lod_clusters) {
            std::size_t with_clusters = 0, clusters = 0;
            for (const ModelPart& part : _model.parts) {
                with_clusters += part.mesh.has_clusters() ? 1 : 0;
                clusters += part.mesh.cluster_count();
            }
            std::printf("sponza_walk: %s, %zu parts, %.1f s (including clusters: %zu meshes, %zu clusters)\n", _model_path.string().c_str(),
                        _model.parts.size(), std::chrono::duration<double>(Clock::now() - start).count(), with_clusters, clusters);
        } else {
            std::printf("sponza_walk: %s, %zu parts, %.1f s (including %d levels of detail a mesh where they fit)\n", _model_path.string().c_str(),
                        _model.parts.size(), std::chrono::duration<double>(Clock::now() - start).count(), _options.lod_levels);
        }
        _marker = _r3d->create_mesh(meshes::sphere(1.0f, 14, 8));
        _wire_material.color = Color::rgb(20, 255, 120);
        _wire_material.unlit = true;
        _wire_material.wireframe = true;
        _wire_material.casts_shadow = false;

        reference_view(_options.camera == "roof");
        if (_options.eye_given) look_at(_options.eye, _options.target);
        for (const std::string& set : _options.sets) {
            const auto eq = set.find('=');
            std::string why;
            if (eq == std::string::npos || !_controls->set(set.substr(0, eq), set.substr(eq + 1), why)) {
                fail(ctx, eq == std::string::npos ? "--set needs key=value, not " + set : why);
                return false;
            }
        }
        _last = Clock::now();
        _ready = true;
        return true;
    }

    void fail(SceneContext& ctx, const std::string& why) {
        std::cerr << "sponza_walk: " << why << '\n';
        if (ctx.report) ctx.report->fail(why);
        ctx.app.quit();
        _quit = true;
    }

    void build_controls() {
        Hooks h;
        h.view_nave = [this] { reference_view(false); };
        h.view_roof = [this] { reference_view(true); };
        h.drop_light = [this] { drop_light(); };
        h.clear_lights = [this] { _s.points.clear(); };
        h.screenshot = [this] { _shot_requested = true; };
        h.print = [this] { print_settings(); };
        h.reset = [this] { reset_all(); };
        h.reset_history = [this] { _view.reset_history(); };
        _controls = std::make_unique<Controls>(_s, std::move(h), _caps);
    }

    // --- camera --------------------------------------------------------------

    void look_at(Vec3f eye, Vec3f target) {
        const Vec3f f = normalize(target - eye);
        _pos = eye;
        _yaw = std::atan2(-f.x, -f.z) * 180.0f / pi;
        _pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f)) * 180.0f / pi;
        _velocity = {};
    }
    // The two cameras of tests/reference/scenes/sponza.json.
    void reference_view(bool roof) {
        if (roof) {
            look_at({2.0f, 1.6f, 0.0f}, {-6.0f, 9.0f, 0.0f});
            _s.fov = 70.0f;
        } else {
            look_at({-11.0f, 2.0f, -0.3f}, {10.0f, 4.0f, 0.3f});
            _s.fov = 65.0f;
        }
    }
    Camera3D camera() const {
        Camera3D c;
        c.position = _pos;
        c.rotation = Quatf::yaw_pitch(_yaw, _pitch);
        c.fov = _s.fov;
        c.z_near = _s.near_plane;
        if (_s.far_limit) c.z_far = _s.far_plane;
        return c;
    }
    // Free flight, eased so that starts and stops are smooth at any frame rate.
    void move(Input& input, float dt) {
        const Camera3D c = camera();
        Vec3f wish{};
        // With the panel up the mouse is free and the keys do not fly.
        if (_captured) {
            wish += c.forward() * (float(input.held(Key::W)) - float(input.held(Key::S)));
            wish += c.right() * (float(input.held(Key::D)) - float(input.held(Key::A)));
            wish.y += float(input.held(Key::E) || input.held(Key::Space)) -
                      float(input.held(Key::Q) || input.modifier_held(KeyModifiers::Ctrl));
        }
        const float fast = input.modifier_held(KeyModifiers::Shift) ? 3.0f : 1.0f;
        const float len = length(wish);
        const Vec3f target = len > 1e-4f ? wish * (_s.move_speed * fast / len) : Vec3f{};
        _velocity += (target - _velocity) * (1.0f - std::exp(-dt * 10.0f));
        _pos += _velocity * dt;
    }
    void drop_light() {
        PointLight l = _s.points.empty() ? PointLight{.position = {}, .color = {12.0f, 9.0f, 5.0f}, .radius = 10.0f, .size = 0.15f}
                                         : _s.points.back();
        l.position = _pos;
        if (_s.points.size() >= 16) _s.points.erase(_s.points.begin());
        _s.points.push_back(l);
    }
    void reset_all() {
        const Settings fresh;
        const float sensitivity = _options.sensitivity > 0 ? _options.sensitivity : fresh.sensitivity;
        _s = fresh;
        _s.sensitivity = sensitivity;
        _s.msaa = _default_msaa;
        reference_view(false);
    }

    // --- input ---------------------------------------------------------------

    void handle_keys(SceneContext& ctx, Input& input, bool headless) {
        if (input.frame_pressed(Key::Escape)) {
            if (_captured) release_mouse();
            else if (!headless) _quit = true;
        }
        if (input.frame_pressed(Key::Tab) && !headless) {
            if (_captured) release_mouse();
            else capture_mouse(ctx.window);
        }
        if (input.frame_pressed(Key::Num1)) reference_view(false);
        if (input.frame_pressed(Key::Num2)) reference_view(true);
        if (input.frame_pressed(Key::L)) _s.sun_animate = !_s.sun_animate;
        if (input.frame_pressed(Key::K)) drop_light();
        if (input.frame_pressed(Key::F3)) _s.stats = !_s.stats;
        if (input.frame_pressed(Key::F2)) print_settings();
        if (input.frame_pressed(Key::F12) || input.frame_pressed(Key::P)) _shot_requested = true;
    }
    void capture_mouse(Window& window) {
        window.set_relative_mouse(true);
        _window = &window;
        _captured = true;
    }
    void release_mouse() {
        if (_window && _captured) _window->set_relative_mouse(false);
        _captured = false;
    }

    // --- the view and the scene ----------------------------------------------

    ViewKey view_key(Vec2i out) const {
        ViewKey k;
        const std::size_t i = std::min<std::size_t>(std::size_t(std::max(0, _s.msaa)), _caps.samples.size() - 1);
        k.samples = _caps.samples[i];
        k.background = background_colour(_s);
        k.bloom = _s.bloom;
        k.hdr = _s.hdr || _s.bloom > 0;
        k.exposure = _s.exposure;
        k.threshold = _s.bloom_threshold;
        k.size = {std::max(1, out.x), std::max(1, out.y)};
        return k;
    }
    static View3DOptions view_options(const ViewKey& k) {
        return {.samples = k.samples, .background = k.background, .hdr = k.hdr, .exposure = k.exposure, .bloom = k.bloom,
                .bloom_threshold = k.threshold};
    }

    // The HDRI chosen, loaded the first time it is.
    Environment3D environment() {
        if (_s.environment <= 0) return {};
        const std::size_t i = std::size_t(_s.environment - 1);
        if (i >= _hdri_files.size()) return {};
        if (!_env_tried[i]) {
            _env_tried[i] = true;
            std::string error;
            _environments[i] = _r3d->load_environment(_hdri_files[i], &error);
            if (!_environments[i]) std::cerr << "sponza_walk: " << _hdri_files[i].string() << ": " << error << '\n';
        }
        return _environments[i];
    }

    void fill_scene() {
        _scene.clear();
        _scene.camera = camera();
        apply(_s, _scene, environment(), _model.bounds, _pos);
        // The materials, made again only when a setting that changes them did.
        if (_materials.size() != _model.parts.size() || _material_key != _s.materials) {
            _material_key = _s.materials;
            _materials.clear();
            for (const ModelPart& part : _model.parts) _materials.push_back(adjust(part.material, _s.materials));
        }
        const auto wire = static_cast<WireMode>(_s.wireframe);
        for (std::size_t i = 0; i < _model.parts.size(); ++i) {
            const ModelPart& part = _model.parts[i];
            if (!_s.materials.blended && (part.material.blend == Blend3D::Alpha || part.material.blend == Blend3D::Additive)) continue;
            if (wire != WireMode::Only) _scene.draw(part.mesh, part.transform, _materials[i]);
            if (wire != WireMode::Off) _scene.draw(part.mesh, part.transform, _wire_material);
        }
        if (_s.light_markers) {
            for (const PointLight& l : _s.points) {
                const float peak = std::max({l.color.x, l.color.y, l.color.z, 1e-3f});
                Material3D marker;
                marker.color = Color::rgb(u8(255 * l.color.x / peak), u8(255 * l.color.y / peak), u8(255 * l.color.z / peak));
                marker.unlit = true;
                marker.casts_shadow = false;
                marker.receives_shadow = false;
                const float r = std::max(0.04f, l.size);
                _scene.draw(_marker, Mat4f::translation(l.position) * Mat4f::scaling({r, r, r}), marker);
            }
        }
    }

    // --- screenshots ---------------------------------------------------------

    // A requested shot (F12) is of the picture alone, taken before the panel
    // is drawn. The headless run's is too, unless --hud puts the panel in it.
    void take_screenshots(SceneContext& ctx, Vec2i out, bool headless_now, bool before_panel) {
        if (_shot_requested && before_panel) {
            _shot_requested = false;
            for (int n = 1;; ++n) {
                char name[32];
                std::snprintf(name, sizeof name, "sponza_walk_%03d.png", n);
                if (!fs::exists(name)) {
                    const bool ok = ctx.renderer.save_png(name);
                    std::printf("sponza_walk: %s %s\n", ok ? "saved" : "could not save", name);
                    break;
                }
            }
        }
        if (headless_now && !_options.screenshot.empty() && !_shot_done && _frames >= std::max(1, _options.frames)) {
            _shot_done = true;
            const fs::path path{_options.screenshot};
            if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
            std::vector<u8> pixels;
            Vec2i size{};
            ctx.renderer.read_rgba({0.0f, 0.0f, float(out.x), float(out.y)}, pixels, size);
            // The picture must not be the background: a blank frame fails the run.
            std::size_t different = 0, total = std::size_t(size.x) * std::size_t(size.y);
            double luminance = 0;
            const Color bg = background_colour(_s);
            for (std::size_t p = 0; p + 3 < pixels.size() && p / 4 < total; p += 4) {
                luminance += 0.2126 * pixels[p] + 0.7152 * pixels[p + 1] + 0.0722 * pixels[p + 2];
                if (std::abs(int(pixels[p]) - bg.r) > 3 || std::abs(int(pixels[p + 1]) - bg.g) > 3 || std::abs(int(pixels[p + 2]) - bg.b) > 3)
                    ++different;
            }
            if (!ctx.renderer.save_png(_options.screenshot)) {
                fail(ctx, "could not save " + _options.screenshot);
                return;
            }
            std::printf("sponza_walk: %s (%dx%d), %.1f%% of the pixels differ from the background, mean luminance %.1f\n",
                        _options.screenshot.c_str(), size.x, size.y, total ? 100.0 * double(different) / double(total) : 0.0,
                        total ? luminance / double(total) : 0.0);
            if (total == 0 || different * 20 < total) fail(ctx, "the picture is blank");
            else if (!ctx.app.headless()) ctx.app.quit(); // a windowed --screenshot run ends with its picture
        }
    }

    // --- printing ------------------------------------------------------------

    void print_settings() {
        Settings defaults;
        defaults.sensitivity = _s.sensitivity;
        defaults.msaa = _default_msaa;
        Controls reference(defaults, {}, _caps);
        std::ostringstream out;
        out << "sponza_walk settings (changed from the defaults), as options:\n  sponza_walk";
        for (const Row& r : _controls->rows) {
            if (r.kind == Row::Kind::Section || r.kind == Row::Kind::Note || r.kind == Row::Kind::Button) continue;
            if (r.key.starts_with("light_") && r.key != "light_markers") continue; // per light: in the JSON
            const Row* d = reference.find(r.key);
            if (d && Controls::text(*d) == Controls::text(r)) continue;
            out << " --set " << r.key << '=' << Controls::text(r);
        }
        const Vec3f f = camera().forward();
        out << " --eye " << json_number(_pos.x) << ',' << json_number(_pos.y) << ',' << json_number(_pos.z) << " --target "
            << json_number(_pos.x + f.x) << ',' << json_number(_pos.y + f.y) << ',' << json_number(_pos.z + f.z) << '\n';
        out << "  (point lights are in the JSON below; --preset point-lights drops one at the camera)\n";
        // A scene file for tools/reference (tests/reference/scenes/*.json): the
        // lights, occlusion and view; materials are not part of a scene.
        const Lights3D& l = _scene.lights;
        const ViewKey& k = _key;
        out << "reference scene fragment:\n  \"view\": {\"samples\": " << k.samples << ", \"background\": [" << int(k.background.r) << ", "
            << int(k.background.g) << ", " << int(k.background.b) << "]";
        if (k.hdr) out << ", \"hdr\": true, \"exposure\": " << json_number(k.exposure);
        if (k.bloom > 0) out << ", \"bloom\": " << json_number(k.bloom) << ", \"bloom_threshold\": " << json_number(k.threshold);
        out << "},\n  \"lights\": {\n    \"sun\": {\"direction\": " << json_vec(l.sun.direction) << ", \"color\": " << json_vec(l.sun.color) << "},\n";
        if (_s.fill) out << "    \"fill\": {\"direction\": " << json_vec(l.fill.direction) << ", \"color\": " << json_vec(l.fill.color) << "},\n";
        out << "    \"ambient\": " << json_vec(l.ambient);
        if (l.ground_ambient) out << ",\n    \"ground_ambient\": " << json_vec(*l.ground_ambient);
        if (_s.environment > 0 && std::size_t(_s.environment) <= _hdri_files.size()) {
            out << ",\n    \"environment\": {\"file\": \"hdri/" << _hdri_files[std::size_t(_s.environment - 1)].filename().string()
                << "\", \"intensity\": " << json_number(l.environment_intensity) << ", \"rotation\": " << json_number(l.environment_rotation) << "}";
        }
        if (l.shadow_area.size().x > 0) {
            if (_s.shadow_area == int(ShadowArea::Auto)) out << ",\n    \"shadow_area\": \"auto\"";
            else out << ",\n    \"shadow_area\": [" << json_vec(l.shadow_area.min) << ", " << json_vec(l.shadow_area.max) << "]";
            out << ",\n    \"shadow_resolution\": " << l.shadow_resolution << ", \"shadow_cascades\": " << l.shadow_cascades
                << ", \"shadow_distance\": " << json_number(l.shadow_distance);
        }
        if (!l.points.empty()) {
            out << ",\n    \"points\": [";
            for (std::size_t i = 0; i < l.points.size(); ++i) {
                const PointLight& p = l.points[i];
                out << (i ? ",\n      " : "\n      ") << "{\"position\": " << json_vec(p.position) << ", \"color\": " << json_vec(p.color)
                    << ", \"radius\": " << json_number(p.radius) << "}";
            }
            out << "\n    ]";
        }
        out << "\n  }";
        if (_s.ao) {
            out << ",\n  \"ambient_occlusion\": {\"radius\": " << json_number(_s.ao_radius) << ", \"intensity\": " << json_number(_s.ao_intensity)
                << ", \"half_resolution\": " << (_s.ao_half ? "true" : "false") << "}";
        }
        if (_s.contact) {
            out << ",\n  \"contact_shadows\": {\"distance\": " << json_number(_s.contact_distance) << ", \"thickness\": "
                << json_number(_s.contact_thickness) << ", \"intensity\": " << json_number(_s.contact_intensity)
                << ", \"steps\": " << _s.contact_steps << "}";
        }
        if (_s.lod) {
            out << ",\n  \"lod\": {\"error_pixels\": " << json_number(_s.lod_error) << ", \"bias\": " << json_number(_s.lod_bias)
                << ", \"force_level\": " << _s.lod_force << "}";
            if (_options.lod_clusters) out << ",\n  \"lod_clusters\": true, \"lod_gpu\": " << (_s.lod_gpu ? "true" : "false");
        }
        if (_s.taa) {
            out << ",\n  \"taa\": {\"blend\": " << json_number(_s.taa_blend) << ", \"sharpness\": " << json_number(_s.taa_sharpness)
                << ", \"jitter_scale\": " << json_number(_s.taa_jitter_scale) << ", \"jitter_samples\": " << _s.taa_jitter_samples
                << ", \"freeze_jitter\": " << (_s.taa_freeze ? "true" : "false") << "}";
        }
        out << "\n  camera: {\"eye\": " << json_vec(_pos) << ", \"target\": " << json_vec(_pos + f) << ", \"fov\": " << json_number(_s.fov) << "}\n";
        std::cout << out.str() << std::flush;
    }

    // --- the panel -----------------------------------------------------------

    static Rectf panel_rect(Vec2i out, float u) { return {8 * u, 8 * u, 360 * u, std::max(100.0f, float(out.y) - 16 * u)}; }

    ui2::TextStyle text_style(float u, Color colour, float points = 12) const {
        return {.font = ui2::system_ui_font(points * u), .scale = 1.0f, .color = colour};
    }
    void put(const std::string& value, float x, float y, float h, const ui2::TextStyle& st) {
        const Vec2f size = ui2::measure_text(st.font, value, st.scale);
        _ui.text(value, {x, y + (h - size.y) * 0.5f}, st);
    }

    void draw_ui(SceneContext& ctx, Vec2i out, float u, Rectf panel, bool panel_shown, float dt) {
        (void)ctx;
        (void)dt;
        const Color ink = Color::rgb(226, 236, 240), muted = Color::rgb(150, 165, 175), teal = Color::rgb(120, 220, 200);
        const Color glass = Color::rgba(8, 11, 16, 214), rim = Color::rgb(60, 72, 88);
        const ui2::TextStyle label_style = text_style(u, ink), dim_style = text_style(u, muted, 11), head_style = text_style(u, teal, 15);

        if (_captured) {
            const float cx = out.x * 0.5f, cy = out.y * 0.5f;
            const Color cross = Color::rgba(235, 245, 245, 200);
            ctx.renderer.fill_rect({cx - 8 * u, cy - u * 0.5f, 5 * u, u}, cross);
            ctx.renderer.fill_rect({cx + 3 * u, cy - u * 0.5f, 5 * u, u}, cross);
            ctx.renderer.fill_rect({cx - u * 0.5f, cy - 8 * u, u, 5 * u}, cross);
            ctx.renderer.fill_rect({cx - u * 0.5f, cy + 3 * u, u, 5 * u}, cross);
        }
        if (_s.stats && (!ctx.app.headless() || _options.hud)) draw_stats(out, u, glass, rim, ink, muted);
        if (!panel_shown) {
            if (_captured) {
                const std::string hint = "Tab or Esc: panel";
                put(hint, 12 * u, out.y - 22 * u, 14 * u, dim_style);
            }
            return;
        }

        ui2::Panel background{.bounds = panel, .color = glass, .border = rim};
        ui2::run(_ui, background);
        const float pad = 8 * u, x = panel.x + pad, w = panel.w - 2 * pad;
        put("Sponza Walk", x, panel.y + 4 * u, 22 * u, head_style);
        const std::string hint = "wheel scrolls";
        put(hint, panel.x + panel.w - pad - ui2::measure_text(dim_style.font, hint, 1).x, panel.y + 4 * u, 22 * u, dim_style);
        const Rectf area{x, panel.y + 30 * u, w, panel.h - 38 * u};

        // The rows, those wholly inside the area run; the rest only count.
        const float row_h = 22 * u, section_h = 24 * u, note_h = 18 * u;
        const float label_w = 150 * u, value_w = 50 * u;
        float y = area.y - _scroll;
        bool open = true;
        const float top = y;
        for (Row& r : _controls->rows) {
            if (r.kind != Row::Kind::Section && !open) continue;
            const float h = r.kind == Row::Kind::Section ? section_h : r.kind == Row::Kind::Note ? note_h : row_h;
            const bool visible = y >= area.y - 0.5f && y + h <= area.y + area.h + 0.5f;
            const bool enabled = !r.enabled || r.enabled();
            const ui2::Id id = ui2::make_id(r.key);
            if (visible) {
                switch (r.kind) {
                case Row::Kind::Section: {
                    ui2::Button b{.id = id, .bounds = {x, y + 1, w, h - 2}, .label = std::string(r.open ? "v  " : ">  ") + r.label,
                                  .text_style = label_style};
                    ui2::run(_ui, b);
                    if (b.clicked) r.open = !r.open;
                    break;
                }
                case Row::Kind::Check: {
                    ui2::Toggle t{.id = id, .bounds = {x + 4 * u, y, w - 4 * u, h}, .label = r.label, .text_style = label_style,
                                  .value = r.get() > 0.5f, .enabled = enabled};
                    ui2::run(_ui, t);
                    if (t.changed) r.set(t.value ? 1.0f : 0.0f);
                    break;
                }
                case Row::Kind::Slider: {
                    put(r.label, x + 4 * u, y, h, enabled ? label_style : dim_style);
                    ui2::Slider sl{.id = id, .bounds = {x + label_w, y, w - label_w - value_w, h}, .value = r.get(), .min = r.lo,
                                   .max = r.hi, .step = r.integer ? 1.0f : 0.0f, .enabled = enabled};
                    ui2::run(_ui, sl);
                    if (sl.changed) r.set(sl.value);
                    put(Controls::text(r), x + w - value_w + 6 * u, y, h, enabled ? label_style : dim_style);
                    break;
                }
                case Row::Kind::Choice: {
                    put(r.label, x + 4 * u, y, h, label_style);
                    ui2::Button b{.id = id, .bounds = {x + label_w, y + 1, w - label_w, h - 2}, .label = "<  " + Controls::text(r) + "  >",
                                  .text_style = label_style, .enabled = enabled};
                    ui2::run(_ui, b);
                    if (b.clicked && !r.choices.empty()) {
                        const int n = int(r.choices.size()), step = _ui.modifier_held(KeyModifiers::Shift) ? n - 1 : 1;
                        r.set(float((int(r.get()) + step) % n));
                    }
                    break;
                }
                case Row::Kind::Button: {
                    ui2::Button b{.id = id, .bounds = {x, y + 1, w, h - 2}, .label = r.label, .text_style = label_style, .enabled = enabled};
                    ui2::run(_ui, b);
                    if (b.clicked) r.set(1.0f);
                    break;
                }
                case Row::Kind::Note:
                    put(r.label, x + 4 * u, y, h, dim_style);
                    break;
                }
            }
            y += h;
            if (r.kind == Row::Kind::Section) open = r.open;
        }
        // The wheel over the panel scrolls it; what it holds was counted above.
        _content = y - top;
        const float most = std::max(0.0f, _content - area.h);
        const Vec2f pointer = _ui.pointer();
        const float wheel = _ui.mouse_wheel_y();
        if (wheel != 0 && pointer.x >= panel.x && pointer.x <= panel.x + panel.w && pointer.y >= panel.y && pointer.y <= panel.y + panel.h)
            _scroll -= wheel * 3 * row_h;
        _scroll = std::clamp(_scroll, 0.0f, most);
        if (most > 0) {
            const float thumb = std::max(20 * u, area.h * area.h / _content);
            const float at = area.y + (area.h - thumb) * (_scroll / most);
            ctx.renderer.fill_rect({panel.x + panel.w - 4 * u, at, 2 * u, thumb}, Color::rgba(150, 165, 175, 160));
        }
    }

    void draw_stats(Vec2i out, float u, Color glass, Color rim, Color ink, Color muted) {
        const Render3DStats& st = _r3d->stats();
        const RendererBackendStats be = _backend_stats;
        char line[160];
        std::vector<std::string> lines;
        std::snprintf(line, sizeof line, "%.0f fps   %.2f ms frame   cpu render %.2f ms", 1000.0 / std::max(0.01f, _frame_ms), _frame_ms, st.cpu_ms);
        lines.push_back(line);
        std::snprintf(line, sizeof line, "gpu %.2f ms%s   %s x%u   %dx%d", be.last_gpu_frame_ms, _s.gpu_timing ? "" : " (off)",
                      st.taa_dispatches > 0 ? "taa, samples" : "msaa", st.samples > 0 ? st.samples : _key.samples, out.x, out.y);
        lines.push_back(line);
        std::snprintf(line, sizeof line, "draws %u   meshes %u   culled %u   tris %.2fM", st.draws, st.instances, st.culled,
                      double(st.triangles) / 1e6);
        lines.push_back(line);
        if (st.shadow_cascades > 0) {
            std::snprintf(line, sizeof line, "shadows: %u draws, %u casters, %u maps [%u %u %u %u]", st.shadow_draws, st.shadow_casters,
                          st.shadow_cascades, st.cascade_casters[0], st.cascade_casters[1], st.cascade_casters[2], st.cascade_casters[3]);
        } else {
            std::snprintf(line, sizeof line, "shadows: off");
        }
        lines.push_back(line);
        std::snprintf(line, sizeof line, "ao: %u draws, %u meshes, %u dispatches   lights %u", st.ao_draws, st.ao_instances, st.ao_dispatches, st.lights);
        lines.push_back(line);
        if (_s.contact) {
            std::snprintf(line, sizeof line, "contact shadows: %u dispatches", st.contact_dispatches);
            lines.push_back(line);
        }
        if (_s.lod && _options.lod_clusters && st.cluster_gpu) {
            std::snprintf(line, sizeof line, "clusters (GPU cut): %u copies, %llu tested, %u dispatches, CPU %.2f ms", st.cluster_copies,
                          static_cast<unsigned long long>(st.clusters_tested), st.cluster_dispatches, st.cluster_ms);
            lines.push_back(line);
            if (_s.lod_readback) {
                std::snprintf(line, sizeof line, "  read back: %u clusters, %llu triangles drawn, %u dropped", st.clusters_drawn,
                              static_cast<unsigned long long>(st.triangles), st.cluster_overflow);
                lines.push_back(line);
            }
        } else if (_s.lod && _options.lod_clusters) {
            std::snprintf(line, sizeof line, "clusters: %u copies cut, %llu tested, %u drawn, %.1f KB indices, cut %.2f ms", st.cluster_copies,
                          static_cast<unsigned long long>(st.clusters_tested), st.clusters_drawn, static_cast<double>(st.cluster_upload_bytes) / 1024.0,
                          st.cluster_ms);
            lines.push_back(line);
        } else if (_s.lod) {
            std::snprintf(line, sizeof line, "lod: copies by level %u %u %u %u %u", st.lod_copies_by_level[0], st.lod_copies_by_level[1],
                          st.lod_copies_by_level[2], st.lod_copies_by_level[3], st.lod_copies_by_level[4]);
            lines.push_back(line);
        }
        if (_s.taa) {
            std::snprintf(line, sizeof line, st.taa_dispatches > 0 ? "taa: %u dispatches, jitter %+.2f %+.2f%s" : "taa: asked for, not available",
                          st.taa_dispatches, st.taa_jitter.x, st.taa_jitter.y, st.taa_reset ? ", history reset" : "");
            lines.push_back(line);
        }
        std::snprintf(line, sizeof line, "speed %.1f m/s   eye %.1f %.1f %.1f", _s.move_speed, _pos.x, _pos.y, _pos.z);
        lines.push_back(line);
        const ui2::TextStyle st_ink = text_style(u, ink, 11), st_dim = text_style(u, muted, 11);
        float widest = 0;
        for (const std::string& l : lines) widest = std::max(widest, ui2::measure_text(st_ink.font, l, 1).x);
        const float lh = 15 * u, w = widest + 16 * u, h = lh * float(lines.size()) + 10 * u;
        const Rectf r{float(out.x) - w - 8 * u, 8 * u, w, h};
        ui2::Panel p{.bounds = r, .color = glass, .border = rim};
        ui2::run(_ui, p);
        for (std::size_t i = 0; i < lines.size(); ++i) put(lines[i], r.x + 8 * u, r.y + 5 * u + lh * float(i), lh, i == 0 ? st_ink : st_dim);
    }

    Options _options;
    fs::path _model_path;
    Settings _s;
    Caps _caps;
    int _default_msaa = 2;
    std::unique_ptr<Controls> _controls;
    std::unique_ptr<Renderer3D> _r3d;
    View3D _view;
    ViewKey _key;
    Scene3D _scene;
    Model _model;
    Mesh _marker;
    Material3D _wire_material;
    std::vector<Material3D> _materials;
    MaterialSettings _material_key;
    std::vector<fs::path> _hdri_files;
    std::vector<Environment3D> _environments;
    std::vector<bool> _env_tried;
    ui2::Context _ui;
    RendererBackendStats _backend_stats{};
    Window* _window = nullptr;
    Vec3f _pos{}, _velocity{};
    float _yaw = 0, _pitch = 0;
    float _frame_ms = 0, _scroll = 0, _content = 0;
    int _frames = 0;
    bool _ready = false, _captured = false, _quit = false, _shot_requested = false, _shot_done = false;
    Clock::time_point _last = Clock::now();
};

void print_help() {
    std::cout << "sponza_walk: fly round Sponza on kin's experimental 3D renderer, every feature switchable.\n"
                 "  --model PATH          the glTF (else $KIN_REFERENCE_CACHE/sponza/Sponza.gltf, else ~/.cache/kin/reference/sponza/Sponza.gltf;\n"
                 "                        python3 tools/reference/fetch.py puts it there)\n"
                 "  --lod-levels N        levels of detail built for each mesh at load (default 4; 1: none); --preset lod turns them on\n"
                 "  --lod-clusters        clusters instead of levels (a hierarchy a mesh, cut by the error bound); --preset clusters is this and the setting on;\n"
                 "                        --preset clusters-gpu adds the cut in compute (the default where the device can) with its counts read back\n"
                 "  --set KEY=VALUE       a setting at start (on/off, a number or a choice's name); --list-settings lists them\n"
                 "  --preset NAME[,NAME]  a named set of --set values:";
    for (const auto& [name, sets] : presets()) std::cout << ' ' << name;
    std::cout << "\n  --camera nave|roof    the start camera (else --eye X,Y,Z --target X,Y,Z); --spin DEG turns it that much a frame\n"
                 "  --width N --height N  window size (1280x800)       --ui-scale S   --sensitivity DEG   --vsync\n"
                 "  --headless --frames N --screenshot PATH   no window: N frames from the start camera, then a PNG of the picture\n"
                 "                        (KIN_RENDER_BACKEND=gpu); --hud puts the panel and stats in the headless frame\n"
                 "  --print-settings      headless: print the settings (as options and a reference scene fragment) after the last frame\n"
                 "  --skip-if-unavailable exit 77 (CTest: skipped) when Sponza or a 3D renderer is missing\n"
                 "Keys: WASD move, Q/Ctrl down, E/Space up, Shift fast, wheel speed, 1/2 views, Tab or Esc mouse and panel,\n"
                 "      L sun, K light, F2 print settings, F3 stats, F12 or P screenshot.\n";
}

int run(int argc, char** argv) {
    try {
        Options options = parse(argc, argv);
        if (options.help) {
            print_help();
            return 0;
        }
        // --screenshot is handled here (the picture without the panel), so the
        // runtime's own end-of-run capture must not see it.
        std::vector<char*> kept;
        for (int i = 0; i < argc; ++i) {
            const std::string_view a = argv[i];
            if (a == "--screenshot" || a == "--set" || a == "--preset" || a == "--camera" || a == "--eye" || a == "--target" ||
                a == "--spin" || a == "--model" || a == "--lod-levels") {
                ++i;
                continue;
            }
            if (a.starts_with("--screenshot=")) continue;
            kept.push_back(argv[i]);
        }
        auto headless = parse_headless_options(int(kept.size()), kept.data());
        options.frames = headless.frames;
        if (!options.screenshot.empty() && headless.frames == 0) {
            // A headless run lasts its frames; a window's ends itself with the picture.
            options.frames = headless.enabled ? 8 : 30;
            if (headless.enabled) headless.frames = options.frames;
        }

        fs::path model = options.model;
        if (model.empty()) model = cache_dir() / "sponza" / "Sponza.gltf";
        if (!fs::exists(model)) {
            std::cerr << "sponza_walk: Sponza is not at " << model.string() << ".\n"
                      << "  It is not in the repository (Crytek's licence allows using it, not passing it on). Fetch it with\n"
                      << "    python3 tools/reference/fetch.py\n"
                      << "  which downloads it (and the HDRIs) into $KIN_REFERENCE_CACHE or ~/.cache/kin/reference,\n"
                      << "  or pass --model PATH to a Sponza.gltf.\n";
            return options.skip_if_unavailable ? exit_skipped : 1;
        }

        InputMap input;
        GameInfo game{.id = "sponza_walk", .title = "Sponza Walk", .version = "0.1",
                      .description = "Fly round Sponza with every 3D renderer feature switchable",
                      .window = {.width = options.width, .height = options.height, .resizable = true},
                      .tags = {"example", "3d"}, .input_map = input};
        auto window = window_config(game);
        window.vsync = options.vsync;
        window.max_fps = 0.0f;
        const bool headless_picture = headless.enabled || !options.screenshot.empty();
        // A window draws at the display's own pixels (the panel scales by display_scale()); a headless picture is as big as asked.
        window.high_pixel_density = !headless.enabled;
        SceneManager scenes;
        const auto build = [&](SceneManager& manager) { manager.push(std::make_unique<SponzaScene>(options, model)); };
        build(scenes);
        const int code = run_scene_app({.window = window, .headless = headless, .game = &game, .render_headless = headless_picture,
                                        .reset_scenes = build},
                                       scenes);
        return code;
    } catch (const std::exception& e) {
        std::cerr << "sponza_walk: " << e.what() << '\n';
        return 1;
    }
}

} // namespace
} // namespace sponza_walk

int main(int argc, char** argv) { return sponza_walk::run(argc, argv); }
