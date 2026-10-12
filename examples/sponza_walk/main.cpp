// Sponza Walk: fly a camera round Sponza on kin's experimental 3D renderer,
// with every renderer feature switchable live from a panel (docs/rendering_3d.md,
// "Sponza Walk"). Sponza is not in the repository: tools/reference/fetch.py
// puts it in the reference cache.
//
// What can be switched lives in one Settings struct (settings.hpp), applied to
// the Scene3D every frame. The panel, `--set key=value`, `--preset` and the
// printed settings all read one table of rows (SponzaScene::build_controls):
// a new feature (contact shadows, TAA) is a field and a row there.
#include "../../tests/character_level.hpp"
#include "../../tests/climb_level.hpp"
#include "character.hpp"
#include "ik_rig.hpp"
#include "settings.hpp"

#include <kin/core/json.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/ecs/world.hpp>
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
#include <tuple>
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
        {"occlusion", "lod=on,lod_gpu=on,lod_readback=on,lod_occlusion=on"}, // the same, with clusters behind others not drawn (G3d)
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
        {"character", "character=on,char_clip=blend,char_count=5"}, // skinned entities with an animator (P1a part 2)
        {"character-all", "character=on,char_count=5,ao=on,contact=on,taa=on,cascades=4,shadow_distance=60"},
        {"ik", "character=on,char_count=1,char_clip=sway,ik=on,ik_look=on,ik_feet=on,ik_step=on"}, // a person with IK (P1b)
        {"ik-reach", "character=on,char_count=1,char_clip=sway,ik=on,ik_hand_x=0.8,ik_hand_y=0.5,ik_hand_z=0.2,ik_pole=3"},
        {"ik-tail", "character=on,char_count=1,char_clip=sway,ik=on,ik_tail=on,ik_tail_x=0.6,ik_tail_y=1.1,ik_tail_z=-0.2"},
        {"ik-walk", "character=on,char_count=1,char_clip=sway,ik=on,ik_feet=on,ik_lock=on,ik_toes=on,ik_limits=on,ik_walk=on,ik_step=off"},
        {"physics", "physics=on,phys_debug=on,phys_count=30"}, // Jolt boxes on a floor, thrown once when it goes on, with debug lines (P2b)
        {"controller", "ctrl=on,ctrl_demo=on,ctrl_debug=on"}, // a person walks the test level by a scripted route, camera behind (P2c)
        {"climb", "climb=on,climb_demo=on,climb_debug=on"}, // a person climbs a wall and pulls up over its ledge (P2d)
        {"ragdoll", "rag=on,rag_demo=on,rag_debug=on,rag_count=4"}, // people knocked down, lying, getting up, a ball thrown (P2e)
        {"rope", "rope=on,rope_demo=on"},                          // a person takes hold of a rope, climbs, swings, lets go (P2e)
        {"ik-all", "character=on,char_count=3,char_clip=sway,ik=on,ik_look=on,ik_feet=on,ao=on,contact=on,taa=on,cascades=4,shadow_distance=60"},
        // Sky, fog and time of day (P3a). The sky is seen through the nave's open roof (--camera roof looks up).
        {"sky", "sky=on,hdr=on,bloom=0.35"},
        {"dusk", "sky=on,tod=on,tod_hours=19.4,sky_lights=on,fog=on,fog_density=0.003,hdr=on,bloom=0.4,cascades=4,shadow_distance=60"},
        {"fog", "sky=on,tod=on,tod_hours=7.4,fog=on,fog_density=0.02,fog_height_density=0.08,fog_falloff=0.12,fog_base=4,hdr=on,bloom=0.3"},
        {"night", "sky=on,tod=on,tod_hours=1.5,sky_lights=on,fog=on,fog_density=0.004,hdr=on,bloom=0.3,exposure=1.5"},
        {"day-night", "sky=on,tod=on,tod_play=on,tod_speed=3600,sky_lights=on,fog=on,fog_density=0.003,hdr=on,bloom=0.35,cascades=4,shadow_distance=60"},
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
                if (name == "clusters" || name == "clusters-gpu" || name == "occlusion") o.lod_clusters = true;
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
    std::function<void()> view_nave, view_roof, drop_light, clear_lights, screenshot, print, reset, reset_history, throw_boxes, clear_boxes, respawn;
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
        check("shadow_cache", "Keep the map while nothing moves", s.shadow_cache);

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

        section("Sky and fog (P3a)");
        check("sky", "Sky behind the model", s.sky);
        slider("sky_turbidity", "Haze (turbidity)", s.sky_turbidity, 1.5f, 12.0f);
        slider("sky_sun_intensity", "Sun above the air", s.sky_sun_intensity, 0.5f, 8.0f);
        slider("sky_intensity", "Sky intensity", s.sky_intensity, 0.0f, 3.0f);
        slider("sky_sun_size", "Sun disc (degrees)", s.sky_sun_size, 0.1f, 8.0f);
        slider("sky_sun_disc", "Sun disc radiance (x light)", s.sky_sun_disc, 0.0f, 120.0f);
        check("sky_stars", "Stars at night", s.sky_stars);
        check("sky_moon", "Moon (the time of day's)", s.sky_moon);
        check("sky_lights", "Sky lights the scene (ambient and reflections)", s.sky_lights);
        check("fog", "Fog", s.fog);
        slider("fog_density", "Fog per metre", s.fog_density, 0.0f, 0.05f);
        slider("fog_height_density", "Height fog per metre", s.fog_height_density, 0.0f, 0.2f);
        slider("fog_falloff", "Height fog falls off (per m)", s.fog_falloff, 0.01f, 1.0f);
        slider("fog_base", "Height fog base (m)", s.fog_base, -5.0f, 20.0f);
        slider("fog_start", "Fog starts (m)", s.fog_start, 0.0f, 50.0f);
        slider("fog_max", "Fog's greatest opacity", s.fog_max, 0.0f, 1.0f);
        check("fog_from_sky", "Fog takes the sky's colour", s.fog_from_sky);
        slider("fog_sun_scatter", "Sun in the fog", s.fog_sun_scatter, 0.0f, 2.0f);
        slider("fog_anisotropy", "Sun in the fog: tightness", s.fog_anisotropy, 0.0f, 0.95f);
        check("tod", "Time of day drives the lights and sky", s.tod);
        slider("tod_hours", "Hour (solar)", s.tod_hours, 0.0f, 24.0f);
        slider("tod_day", "Day of the year", s.tod_day, 1.0f, 365.0f);
        slider("tod_latitude", "Latitude", s.tod_latitude, -80.0f, 80.0f);
        check("tod_play", "Play the day", s.tod_play);
        slider("tod_speed", "Game seconds a second", s.tod_speed, 0.0f, 20000.0f);
        note("A sky shows where the model has an opening (--camera roof looks up); presets: sky, dusk, fog, night, day-night");

        section("Levels of detail");
        check("lod", caps.clusters ? "Clusters (cut by error)" : "Levels of detail", s.lod);
        slider("lod_error", "Error (pixels)", s.lod_error, 0.1f, 16.0f);
        slider("lod_bias", "Bias (quality multiplier)", s.lod_bias, 0.25f, 8.0f);
        islider("lod_force", caps.clusters ? "Force cut depth (-1: by error, 0: leaves)" : "Force level (-1: by distance)", s.lod_force, -1,
                caps.clusters ? 20 : 7);
        if (caps.clusters) {
            check("lod_gpu", caps.gpu_clusters ? "Cut on the GPU (compute)" : "Cut on the GPU (this device cannot: the CPU cuts)", s.lod_gpu);
            check("lod_readback", "Read the GPU cut's counts back (waits for the GPU)", s.lod_readback);
            check("lod_occlusion", "Occlusion culling (needs the GPU cut; costs where little is hidden)", s.lod_occlusion);
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

        section("Character (skinned)");
        check("character", "Character (entities with Animator3D)", s.character);
        choice("char_clip", "Clip", s.char_clip, {"Sway", "Bend", "Wave", "Blend Sway-Wave"});
        slider("char_blend", "Blend parameter", s.char_blend, 0.0f, 1.0f);
        slider("char_speed", "Speed", s.char_speed, 0.0f, 3.0f);
        islider("char_count", "Count (a row)", s.char_count, 1, 64);
        slider("char_scale", "Size", s.char_scale, 0.5f, 3.0f);

        section("Character IK (the person rig)");
        check("ik", "IK on (the Person rig, 25 joints)", s.ik);
        slider("ik_hand_x", "Hand target x (to its left)", s.ik_hand_x, -1.2f, 1.2f);
        slider("ik_hand_y", "Hand target y (up)", s.ik_hand_y, -1.0f, 1.2f);
        slider("ik_hand_z", "Hand target z (forward)", s.ik_hand_z, -0.3f, 1.4f);
        choice("ik_pole", "Elbow points", s.ik_pole, {"as animated", "down", "back", "up", "out"});
        slider("ik_weight", "Hand weight", s.ik_weight, 0.0f, 1.0f);
        slider("ik_fade", "Weight fade (s)", s.ik_fade, 0.0f, 2.0f);
        slider("ik_stretch", "Arm may stretch to", s.ik_stretch, 1.0f, 1.6f);
        check("ik_marker", "Target marker", s.ik_marker);
        check("ik_look", "Head looks at the camera", s.ik_look);
        slider("ik_look_weight", "Look weight", s.ik_look_weight, 0.0f, 1.0f);
        slider("ik_look_smooth", "Look smoothing (s)", s.ik_look_smooth, 0.0f, 1.0f);
        check("ik_feet", "Foot placement", s.ik_feet);
        check("ik_step", "A step under the left foot", s.ik_step);
        slider("ik_step_height", "Step height (m)", s.ik_step_height, 0.0f, 0.4f);
        check("ik_limits", "Joint limits (knee hinge, hip twist 35, fold 150)", s.ik_limits);
        check("ik_lock", "Foot locking", s.ik_lock);
        check("ik_toes", "Toes on the ground", s.ik_toes);
        check("ik_walk", "The person drifts side to side", s.ik_walk);
        check("ik_tail", "Tail (FABRIK) to the green ball", s.ik_tail);
        slider("ik_tail_x", "Tail target x", s.ik_tail_x, -1.2f, 1.2f);
        slider("ik_tail_y", "Tail target y", s.ik_tail_y, 0.0f, 1.6f);
        slider("ik_tail_z", "Tail target z", s.ik_tail_z, -1.2f, 0.6f);

        section("Physics (Jolt: kin/physics3d)");
        check("physics", "Physics on (a floor at y = 0)", s.physics);
        check("phys_debug", "Debug lines (shapes, awake and asleep, contacts)", s.phys_debug);
        islider("phys_count", "Boxes a throw", s.phys_count, 1, 100);
        slider("phys_size", "Box size (m)", s.phys_size, 0.1f, 1.5f);
        slider("phys_speed", "Throw speed (m/s)", s.phys_speed, 0.0f, 30.0f);
        button("phys_throw", "Throw boxes (B)", _h.throw_boxes);
        button("phys_clear", "Remove the boxes", _h.clear_boxes);

        section("Character controller (kin/physics3d, P2c)");
        check("ctrl", "Controller on (a person and the test level in front of you)", s.ctrl);
        check("ctrl_debug", "Debug lines (capsule, level, contacts)", s.ctrl_debug);
        check("ctrl_demo", "Scripted walk (instead of the keys)", s.ctrl_demo);
        slider("ctrl_speed", "Walk speed (m/s)", s.ctrl_speed, 1.0f, 8.0f);
        slider("ctrl_jump", "Jump speed (m/s)", s.ctrl_jump, 1.0f, 10.0f);
        slider("ctrl_slope", "Steepest slope it stands on (deg)", s.ctrl_slope, 10.0f, 80.0f);
        slider("ctrl_step", "Highest step (m)", s.ctrl_step, 0.0f, 0.8f);
        slider("ctrl_distance", "Camera distance (m)", s.ctrl_distance, 1.0f, 10.0f);
        button("ctrl_respawn", "Back to the start", _h.respawn);

        section("Climb (kin/physics3d, P2d)");
        check("climb", "Climb on (a person and the climb level in front of you)", s.climb);
        check("climb_debug", "Debug lines (grips, anchors, hang place, what a hand sees)", s.climb_debug);
        check("climb_demo", "Scripted climb (instead of the keys)", s.climb_demo);
        slider("climb_reach", "Reach (m)", s.climb_reach, 0.4f, 1.4f);
        slider("climb_stamina", "Stamina (units)", s.climb_stamina, 10.0f, 400.0f);
        slider("climb_distance", "Camera distance (m)", s.climb_distance, 1.5f, 10.0f);
        button("climb_respawn", "Back to the start", _h.respawn);
        note("Mouse buttons (or Q, E): left, right hand. Aim with the view");
        note("WASD walk; hanging: along and up. Space jump / jump off, F pull up, X let go");

        section("Ragdoll and rope (kin/physics3d, P2e)");
        check("rag", "Ragdolls on (people in a row in front of you)", s.rag);
        check("rag_debug", "Debug lines (the bones' capsules, contacts)", s.rag_debug);
        check("rag_demo", "Scripted (knocked down, up again, a ball)", s.rag_demo);
        islider("rag_count", "People", s.rag_count, 1, 6);
        slider("rag_blend", "Blend into the ragdoll (s)", s.rag_blend, 0.0f, 1.0f);
        slider("rag_hit", "Push at the chest (N s)", s.rag_hit, 0.0f, 200.0f);
        note("G: knock down / get up. H: throw a ball along the view");
        check("rope", "Rope on (a swing, a person under it)", s.rope);
        check("rope_debug", "Debug lines (the segments)", s.rope_debug);
        check("rope_demo", "Scripted (take hold, climb, jump off)", s.rope_demo);
        slider("rope_length", "Rope length (m)", s.rope_length, 3.0f, 9.0f);
        slider("rope_stamina", "Stamina (units)", s.rope_stamina, 10.0f, 1000.0f);
        note("Q, E (or the mouse buttons): left, right hand. W, S climb; Space jumps off");

        section("Debug", true);
        check("stats", "Stats (F3)", s.stats);
        check("gpu_timing", "GPU timing", s.gpu_timing);
        button("screenshot", "Screenshot (F12 or P)", _h.screenshot);
        button("print", "Print settings to the console (F2)", _h.print);
        button("reset", "Reset all", _h.reset);
        section("Keys");
        note("WASD move, Q/Ctrl down, E/Space up, Shift fast");
        note("Wheel: speed. Esc: free the mouse / quit");
        note("Tab: panel or mouse. 1/2: views. L: sun, K: light, B: throw boxes");
        note("Controller on: WASD walk (camera relative), Space jump, Shift sprint, Ctrl crouch, R back to the start");
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
        _input = &input;
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
        if (_s.tod && _s.tod_play) {
            TimeOfDay3D clock{.hours = _s.tod_hours, .day_of_year = _s.tod_day, .speed = _s.tod_speed};
            kin::advance(clock, dt);
            _s.tod_hours = clock.hours;
            _s.tod_day = clock.day_of_year;
        }

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

        update_characters(dt);
        update_physics(dt);
        fill_scene();
        warm_up_features();
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
        _step_mesh = _r3d->create_mesh(meshes::box({1.0f, 1.0f, 1.0f}));
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
        h.throw_boxes = [this] { throw_boxes(); };
        h.respawn = [this] {
            respawn_controller();
            respawn_climb();
        };
        h.clear_boxes = [this] { clear_boxes(); };
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
        // With the controller or the climb on, the keys walk the person (drive_controller, drive_climb) and the camera follows it.
        if (_s.ctrl || _s.climb) return;
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
        if (input.frame_pressed(Key::B)) throw_boxes();
        if (input.frame_pressed(Key::R) && (_s.ctrl || _s.climb)) {
            respawn_controller();
            respawn_climb();
        }
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

    // Pipelines and targets are made on a feature's first use (tens of milliseconds, hundreds for temporal anti-aliasing on a cold driver
    // cache), which would show as a hitch the first time a switch on the panel is turned on. A viewer can afford to make them all once the
    // model is loaded (the engine does nothing of the kind by itself): every switch that takes a new pipeline or target on, warmed up for. Then,
    // whenever a switch that changes what a frame needs is turned (the shadow map's size, a view made again for MSAA or HDR), again for the scene as it is.
    void warm_up_features() {
        if (!_view.valid()) return;
        const auto key = std::make_tuple(_s.ao, _s.ao_half, _s.contact, _s.taa, _s.taa_sharpness > 0.0f, _s.shadows, _s.shadow_resolution,
                                         _s.shadow_cascades > 1, _s.lod, _s.lod_occlusion, _s.materials.blended, _s.materials.cutout, _s.wireframe,
                                         _key.samples, _key.hdr, _key.size.x, _key.size.y);
        if (!_warmed) {
            _warmed = true;
            _warmed_key = key;
            // Everything on, for one call, then the scene as the settings have it.
            _scene.ambient_occlusion.enabled = _scene.contact_shadows.enabled = _scene.taa.enabled = true;
            _scene.lights.shadow_cascades = max_shadow_cascades;
            if (_caps.clusters) _scene.lod.enabled = _scene.lod.occlusion.enabled = true;
            _r3d->warm_up(_view, _scene);
            fill_scene();
        } else if (key != _warmed_key) {
            _warmed_key = key;
            _r3d->warm_up(_view, _scene);
        }
    }

    // The characters: entities with a SkinnedMeshRenderer and an Animator3D (a state machine with one state, a blend space over
    // `speed` for the blend, a clip name else), a row in front of the camera where the switch was turned on or the count changed.
    void update_characters(float dt) {
        if (!_s.character || !_r3d->capabilities().skinning) {
            if (!_characters.empty()) {
                for (flecs::entity e : _characters) e.destruct();
                _characters.clear();
            }
            return;
        }
        if (!_character_model) _character_model = make_character(*_r3d);
        // The IK switch is the Person rig (18 joints, named limbs); the tube has no arms to reach with.
        const bool person = _s.ik;
        if (person && !_person_model) _person_model = make_person(*_r3d);
        if (person != _characters_are_people) {
            for (flecs::entity e : _characters) e.destruct();
            _characters.clear();
            _person_origin.clear();
            _characters_are_people = person;
        }
        const int count = std::clamp(_s.char_count, 1, 64);
        if (!_ecs) {
            _ecs = std::make_unique<::kin::EcsWorld>();
            _ecs_state = std::make_unique<WorldRenderState3D>(*_ecs);
        }
        if (int(_characters.size()) != count) {
            for (flecs::entity e : _characters) e.destruct();
            _characters.clear();
            _person_origin.clear();
            const Vec3f forward{-std::sin(_yaw * pi / 180.0f), 0.0f, -std::cos(_yaw * pi / 180.0f)};
            const Vec3f right{-forward.z, 0.0f, forward.x};
            auto machine = std::make_shared<AnimationStateMachine>();
            machine->initial = "Blend";
            machine->states = {{"Sway", "Sway"}, {"Bend", "Bend"}, {"Wave", "Wave"}, {"Blend", ""}};
            for (int i = 0; i < count; ++i) {
                Animator3D a;
                a.machine = machine;
                a.blend_spaces["Blend"] = {"blend", {{"Sway", 0.0f}, {"Wave", 1.0f}}};
                a.fade_seconds = 0.3f;
                const float offset = (float(i) - float(count - 1) * 0.5f) * 0.8f;
                const Vec3f at = _pos + forward * 3.0f + right * offset - Vec3f{0.0f, 1.5f, 0.0f};
                // A person faces the camera; its constraints go in the order the pose wants them (the feet lower the pelvis, the look
                // turns the spine, and the arm then reaches from where the shoulder has got to).
                const Quatf facing = Quatf::axis_angle({0, 1, 0}, _yaw);
                if (person) {
                    // The game's ground, in the world: the floor at the feet's height and the step, found in the person's own space as
                    // it was placed (the ground stays where it is when the person walks).
                    const GroundQuery ground = [this, at, facing](Vec3f from, Vec3f to) -> std::optional<GroundHit> {
                        const float sc = _s.char_scale;
                        const Mat4f m = Transform3D{.position = at, .rotation = facing, .scale = {sc, sc, sc}}.matrix();
                        const Mat4f inverse = m.inverse();
                        const bool step = _s.ik_step && _s.ik_step_height > 1e-4f;
                        const auto hit = floor_and_step(inverse.apply(from), inverse.apply(to), step, {0.0f, 0.0f, -0.4f}, {0.6f, _s.ik_step_height, 0.4f});
                        if (!hit) return std::nullopt;
                        return GroundHit{m.apply(hit->position), normalize(inverse.transposed().apply_vector(hit->normal))};
                    };
                    for (const IKConstraint& k : person_constraints(ground)) a.add_ik(k);
                    _person_origin.push_back({at, facing});
                }
                _characters.push_back(_ecs->raw().entity()
                                          .set(Transform3D{.position = at, .rotation = person ? facing : Quatf{}})
                                          .set(SkinnedMeshRenderer{.model = person ? _person_model : _character_model})
                                          .set(static_cast<const Animator3D&>(a)));
            }
        }
        for (std::size_t index = 0; index < _characters.size(); ++index) {
            flecs::entity e = _characters[index];
            Animator3D* a = e.get_mut<Animator3D>();
            a->speed = _s.char_speed;
            a->params.set_float("blend", _s.char_blend);
            Transform3D* t = e.get_mut<Transform3D>();
            t->scale = {_s.char_scale, _s.char_scale, _s.char_scale};
            // Changing the clip is a change of state: the animator crossfades over fade_seconds.
            static const char* states[] = {"Sway", "Bend", "Wave", "Blend"};
            a->state = states[std::clamp(_s.char_clip, 0, 3)];
            if (person) {
                if (index < _person_origin.size()) {
                    // The person as it was placed: its targets are in this space and stay where they are while it drifts.
                    const auto [origin, facing] = _person_origin[index];
                    const Mat4f base = Transform3D{.position = origin, .rotation = facing, .scale = t->scale}.matrix();
                    const PersonIk ik = person_ik(_s);
                    if (ik.walk) {
                        const Vec3f along = facing.rotate({1.0f, 0.0f, 0.0f});
                        t->position = origin + along * (person_walk_offset(_ik_time) * _s.char_scale);
                    } else {
                        t->position = origin;
                    }
                    drive_person(*a, ik, base, _pos);
                }
            }
        }
        _ik_time += dt;
        _ecs_state->animate(dt);
    }

    // Physics: the ECS world gets a World3D with a floor (made when the switch goes on) and boxes (entities with a
    // MeshRenderer, a Collider3D and a RigidBody3D) thrown from the camera; WorldPhysicsState3D steps it at 60 Hz.
    void ensure_physics() {
        if (!_ecs) {
            _ecs = std::make_unique<::kin::EcsWorld>();
            _ecs_state = std::make_unique<WorldRenderState3D>(*_ecs);
        }
        if (!_bodies) {
            World3DConfig cfg;
            cfg.single_threaded = true; // 100 boxes: not worth a pool
            cfg.persist_events = true;  // resting contacts for the debug lines
            _physics = std::make_unique<World3D>(cfg);
            _bodies = std::make_unique<WorldPhysicsState3D>(*_ecs, *_physics);
            Collider3D floor;
            floor.half_extents = {200.0f, 0.5f, 200.0f};
            _floor = _ecs->raw().entity().set(Transform3D{.position = {0.0f, -0.5f, 0.0f}}).set(floor);
        }
        if (!_box_mesh) _box_mesh = _r3d->create_mesh(meshes::box({1.0f, 1.0f, 1.0f}));
    }

    void throw_boxes() {
        if (!_r3d || !_s.physics) return;
        ensure_physics();
        const Camera3D c = camera();
        const float size = _s.phys_size;
        for (int i = 0; i < std::clamp(_s.phys_count, 1, 100); ++i) {
            // A small cloud ahead of the camera, going the way it looks.
            const float u = float(i % 5) - 2.0f, v = float(i / 5 % 4) - 1.5f;
            const Vec3f at = _pos + c.forward() * 1.5f + c.right() * (u * size * 1.3f) + Vec3f{0.0f, 0.2f + v * size * 1.3f, 0.0f};
            _boxes.push_back(_ecs->raw().entity()
                                 .set(Transform3D{.position = at, .rotation = Quatf::axis_angle({0.3f, 1.0f, 0.2f}, float(i) * 37.0f),
                                                  .scale = {size, size, size}})
                                 .set(MeshRenderer{.mesh = _box_mesh, .material = {.color = Color::rgb(210, 150, 60), .roughness = 0.6f}})
                                 .set(Collider3D{.kind = Collider3D::Kind::Box, .half_extents = {0.5f, 0.5f, 0.5f}})
                                 .set(RigidBody3D{.density = 400.0f, .friction = 0.5f, .restitution = 0.1f,
                                                  .linear_velocity = c.forward() * _s.phys_speed}));
        }
    }

    void clear_boxes() {
        for (flecs::entity e : _boxes) e.destruct();
        _boxes.clear();
    }

    void update_physics(float dt) {
        if (_s.climb && _s.ctrl) _s.ctrl = false; // (the two levels would stand on each other)
        if (!_s.ctrl && _ctrl_hero.is_valid()) remove_controller();
        if (!_s.climb && _climb_hero.is_valid()) remove_climb();
        if (!_s.rag && !_rag_people.empty()) remove_ragdolls();
        if (!_s.rope && _rope_hero.is_valid()) remove_rope();
        if (!_s.physics) {
            if (!_boxes.empty()) clear_boxes();
        }
        if (!_s.physics && !_s.ctrl && !_s.climb && !_s.rag && !_s.rope) {
            return;
        }
        ensure_physics();
        if (_s.rag) {
            ensure_ragdolls();
            drive_ragdolls(dt);
        }
        if (_s.rope) {
            ensure_rope();
            drive_rope(dt);
        }
        if (_s.physics && !_physics_started) { // the first volley when the switch goes on (so a headless run has something to show)
            _physics_started = true;
            throw_boxes();
        }
        if (_s.ctrl) {
            ensure_controller();
            drive_controller(dt);
        }
        if (_s.climb) {
            ensure_climb();
            drive_climb(dt);
        }
        if ((_s.ctrl && _ctrl_built) || (_s.climb && _climb_built)) {
            // The levels' platforms are a function of the step number: moved a step ahead before each fixed step, so the
            // steps are taken here (what advance() does, with that call in it).
            const float fixed = _bodies->config().fixed_dt;
            _ctrl_accumulator += dt;
            int taken = 0;
            while (_ctrl_accumulator >= fixed && taken < 8) {
                if (_s.ctrl && _ctrl_built) _ctrl_level.drive(*_physics, _ctrl_steps, fixed);
                if (_s.climb && _climb_built) _climb_level.drive(*_physics, _climb_steps, fixed);
                _bodies->step(fixed);
                ++_ctrl_steps;
                ++_climb_steps;
                ++taken;
                _ctrl_accumulator -= fixed;
            }
            if (taken == 8) _ctrl_accumulator = 0.0f;
        } else {
            _bodies->advance(dt);
        }
        if (_s.ctrl && _ctrl_hero.is_valid()) {
            follow_controller(dt);
            if (!_s.character) _ecs_state->animate(dt); // (update_characters does it when the characters are on)
        }
        if (_s.climb && _climb_hero.is_valid()) {
            follow_climb(dt);
            if (!_s.character) _ecs_state->animate(dt);
        }
        if (_s.rope && _rope_hero.is_valid()) {
            follow_rope(dt);
        }
        if ((_s.rag || _s.rope) && !_s.character && !(_s.ctrl && _ctrl_hero.is_valid()) && !(_s.climb && _climb_hero.is_valid())) {
            _ecs_state->animate(dt);
        }
    }

    // --- ragdolls and a rope (P2e) ------------------------------------------------------------------------------------------
    // People (entities with a Ragdoll3D, made from the skeleton at its rest pose) stand in a row in front of the camera on the
    // physics floor with the Sway clip playing; G starts their ragdolls (an impulse at the chest) and, the next time, gets them up
    // (each is put where its pelvis lies and the clip is blended back); H throws a ball along the view. A headless run cannot
    // press anything: `rag_demo` plays the same calls on a clock. The rope hangs from a beam, made 25 degrees out from the
    // vertical, drawn as a tube; the person under its end uses the climb's commands.
    void ensure_ragdolls() {
        if (!_r3d->capabilities().skinning || !_rag_people.empty()) return;
        if (!_person_model) _person_model = make_person(*_r3d);
        if (!_ball_mesh) _ball_mesh = _r3d->create_mesh(meshes::sphere(1.0f, 32, 16));
        const int n = std::clamp(_s.rag_count, 1, 6);
        const Vec3f origin{_pos.x, 0.0f, _pos.z - 3.5f};
        for (int i = 0; i < n; ++i) {
            Animator3D animator;
            animator.clip = "Sway";
            const Vec3f at = origin + Vec3f{2.4f * (float(i) - 0.5f * float(n - 1)), 0.0f, 0.0f};
            _rag_people.push_back(_ecs->raw().entity()
                                      .set(Transform3D{.position = at, .rotation = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 180.0f)})
                                      .set(SkinnedMeshRenderer{.model = _person_model})
                                      .set(static_cast<const Animator3D&>(animator))
                                      .set(Ragdoll3D{}));
        }
        _rag_time = 0.0f;
        _rag_down = false;
        _yaw = 0.0f;
        _pitch = -10.0f;
    }

    void remove_ragdolls() {
        for (flecs::entity e : _rag_people) {
            if (e.is_valid() && e.is_alive()) e.destruct();
        }
        _rag_people.clear();
        if (_rag_ball.valid() && _physics) _physics->destroy_body(_rag_ball);
        _rag_ball = {};
    }

    void rag_knock_down() {
        const float chest = _s.rag_hit;
        for (flecs::entity e : _rag_people) {
            const RagdollHandle3D r = _bodies->ragdoll_of(e);
            const RagdollDesc* d = _physics->ragdoll_desc(r);
            int bone = 0;
            if (d != nullptr) {
                const SkinnedMeshRenderer* sk = e.get<SkinnedMeshRenderer>();
                const i32 j = sk != nullptr ? sk->model->skeletons[0].find("chest") : -1;
                for (std::size_t b = 0; b < d->bones.size(); ++b) {
                    if (d->bones[b].joint == j) bone = static_cast<int>(b);
                }
            }
            _bodies->start_ragdoll(e, {.blend_seconds = _s.rag_blend, .impulse_bone = bone, .impulse = {chest * 0.6f, chest * 0.2f, -chest}});
        }
        _rag_down = true;
    }

    void rag_get_up() {
        for (flecs::entity e : _rag_people) {
            // Where it lies: put at the pelvis's ground point, facing by it, then the clip is blended in over a second.
            const RagdollPlace3D place = _physics->ragdoll_get_up_place(_bodies->ragdoll_of(e));
            if (place.found) {
                Transform3D* t = e.get_mut<Transform3D>();
                t->position = place.position;
                t->rotation = place.rotation;
            }
            _bodies->ragdoll_get_up(e, 1.0f);
        }
        _rag_down = false;
    }

    void throw_ball() {
        if (!_physics) return;
        if (_rag_ball.valid()) _physics->destroy_body(_rag_ball);
        const Camera3D c = camera();
        _rag_ball = _physics->create_body({.shape = Shape3D::sphere(0.2f), .motion = Body3DMotion::Dynamic, .position = c.position + c.forward() * 1.0f,
                                           .linear_velocity = c.forward() * 16.0f, .mass = 6.0f, .friction = 0.5f, .gravity_factor = 0.4f,
                                           .continuous = true});
    }

    void drive_ragdolls(float dt) {
        if (_rag_people.empty()) return;
        _rag_time += dt;
        if (_s.rag_demo) {
            // 12 s: standing, a ball at 1.5 s, knocked down at 3 s, up at 8 s.
            const float s = std::fmod(_rag_time, 12.0f);
            if (s < dt) {
                if (_rag_down) rag_get_up();
            }
            if (std::fabs(s - 1.5f) < dt * 0.5f) throw_ball_at_row();
            if (std::fabs(s - 3.0f) < dt * 0.5f && !_rag_down) rag_knock_down();
            if (std::fabs(s - 8.0f) < dt * 0.5f && _rag_down) rag_get_up();
        } else if (_input != nullptr && _captured) {
            if (_input->frame_pressed(Key::G)) {
                if (_rag_down) rag_get_up();
                else rag_knock_down();
            }
            if (_input->frame_pressed(Key::H)) throw_ball();
        }
        // A ball's first touch of a person starts that person's ragdoll, the momentum at the bone it hit.
        if (_rag_ball.valid() && _bodies) {
            for (const ContactEvent3D& e : _bodies->events()) {
                if (e.kind != ContactKind3D::Begin) continue;
                const Body3D other = e.a == _rag_ball ? e.b : (e.b == _rag_ball ? e.a : Body3D{});
                if (!other.valid()) continue;
                for (flecs::entity person : _rag_people) {
                    const std::vector<Body3D> bs = _physics->ragdoll_bodies(_bodies->ragdoll_of(person));
                    const auto found = std::find(bs.begin(), bs.end(), other);
                    if (found == bs.end() || _physics->ragdoll_state(_bodies->ragdoll_of(person)).mode != RagdollMode3D::Animated) continue;
                    _bodies->start_ragdoll(person, {.blend_seconds = 0.05f, .impulse_bone = static_cast<int>(found - bs.begin()),
                                                    .impulse = _physics->linear_velocity(_rag_ball) * 2.5f, .impulse_point = e.point,
                                                    .point_given = true});
                }
            }
        }
    }

    // The demo's ball: from the left of the row, along it, at chest height.
    void throw_ball_at_row() {
        if (!_physics || _rag_people.empty()) return;
        if (_rag_ball.valid()) _physics->destroy_body(_rag_ball);
        const Transform3D* first = _rag_people.front().get<Transform3D>();
        const Transform3D* last = _rag_people.back().get<Transform3D>();
        const float left = std::min(first->position.x, last->position.x) - 6.0f;
        const float z = first->position.z;
        _rag_ball = _physics->create_body({.shape = Shape3D::sphere(0.2f), .motion = Body3DMotion::Dynamic, .position = {left, 1.3f, z},
                                           .linear_velocity = {16.0f, 0.5f, 0.0f}, .mass = 6.0f, .friction = 0.5f, .gravity_factor = 0.3f,
                                           .continuous = true});
    }

    void ensure_rope() {
        if (!_r3d->capabilities().skinning || _rope_hero.is_valid()) return;
        if (!_person_model) _person_model = make_person(*_r3d);
        if (!_box_mesh) _box_mesh = _r3d->create_mesh(meshes::box({1.0f, 1.0f, 1.0f}));
        const Vec3f beam{_pos.x + 1.0f, 0.0f, _pos.z - 3.5f};
        const float a = 25.0f * pi / 180.0f;
        const float length = _s.rope_length;
        const float top = length * std::cos(a) + 2.5f; // the end 2.5 m up
        _rope_beam = Vec3f{beam.x, top, beam.z};
        Rope3D rope;
        rope.start = RopeAttach3D::at(_rope_beam);
        rope.end = RopeAttach3D::free();
        rope.length = length;
        rope.hang_direction = {std::sin(a), -std::cos(a), 0.0f};
        rope.linear_damping = 0.0f;
        rope.angular_damping = 0.1f;
        rope.render = true;
        rope.render_radius = 0.045f;
        _rope_entity = _ecs->raw().entity().set(Transform3D{}).set(rope);
        // The person under the end of it (a hand's width in front, the hands at the rope).
        const Vec3f end = _rope_beam + Vec3f{length * std::sin(a), -length * std::cos(a), 0.0f};
        Animator3D animator = locomotion_animator({.idle = "Sway", .walk = "Walk", .run = "Run", .fall = "Fall"});
        for (const char* side : {"L", "R"}) {
            IKConstraint hand;
            hand.name = std::string("hand_") + side;
            hand.kind = IKKind::TwoBone;
            hand.joints = {std::string("shoulder_") + side, std::string("elbow_") + side, std::string("hand_") + side};
            hand.weight = 0.0f;
            hand.fade_seconds = 0.12f;
            hand.pole = IKConstraint::Pole::ModelDirection;
            hand.pole_vector = {side[0] == 'L' ? 0.5f : -0.5f, -0.7f, -0.5f};
            hand.two_bone.max_stretch = 1.05f;
            animator.add_ik(hand);
        }
        CharacterController3D controller;
        controller.desc.traversal.enabled = true;
        controller.desc.traversal.resource.max = _s.rope_stamina;
        controller.input.forward = {0.0f, 0.0f, -1.0f};
        controller.facing = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 180.0f);
        CharacterAnimationLink3D link;
        link.hands = {"hand_L", "hand_R"};
        link.hand_clearance = 0.05f;
        _rope_start = end + Vec3f{0.0f, -1.5f, 0.45f};
        _rope_hero = _ecs->raw().entity()
                         .set(Transform3D{.position = _rope_start})
                         .set(controller)
                         .set(SkinnedMeshRenderer{.model = _person_model})
                         .set(static_cast<const Animator3D&>(animator))
                         .set(link);
        _rope_time = 0.0f;
        _yaw = 0.0f;
        _pitch = 0.0f;
    }

    void remove_rope() {
        if (_rope_hero.is_valid() && _rope_hero.is_alive()) _rope_hero.destruct();
        if (_rope_entity.is_valid() && _rope_entity.is_alive()) _rope_entity.destruct();
        _rope_hero = {};
        _rope_entity = {};
    }

    void drive_rope(float dt) {
        if (!_rope_hero.is_valid() || !_rope_hero.is_alive()) return;
        CharacterController3D* c = _rope_hero.get_mut<CharacterController3D>();
        _rope_time += dt;
        CharacterInput3D in;
        TraversalInput3D t;
        in.forward = camera().forward();
        t.reach = camera().forward();
        if (_s.rope_demo) {
            // 16 s: take hold at 0.3, climb from 4 to 8, hang, jump off at 13, and again (it is put back at the start).
            const float s = std::fmod(_rope_time, 16.0f);
            in.forward = {0.0f, 0.0f, -1.0f};
            t.reach = {0.0f, 0.5f, -1.0f};
            t.grab = {s > 0.3f && s < 13.2f, s > 0.3f && s < 13.2f};
            in.move = {0.0f, s > 4.0f && s < 8.0f ? 1.0f : 0.0f};
            t.jump_off = s > 13.0f && s < 13.1f;
            if (s < dt && _rope_time > 1.0f) {
                _rope_hero.get_mut<Transform3D>()->position = _rope_start; // a move by game code is a teleport
                _physics->set_traversal_resource(_bodies->character_of(_rope_hero), _s.rope_stamina);
            }
        } else if (_input != nullptr && _captured) {
            const Input& k = *_input;
            in.move = {float(k.held(Key::D)) - float(k.held(Key::A)), float(k.held(Key::W)) - float(k.held(Key::S))};
            in.jump = k.held(Key::Space);
            t.jump_off = k.held(Key::Space);
            t.grab = {k.mouse_held(MouseButton::Left) || k.held(Key::Q), k.mouse_held(MouseButton::Right) || k.held(Key::E)};
        }
        c->input = in;
        c->traversal = t;
        if (c->state.traversal_mode == TraversalMode3D::Walking) {
            const Vec3f fwd = normalize(Vec3f{in.forward.x, 0.0f, in.forward.z});
            const Vec3f right{-fwd.z, 0.0f, fwd.x};
            const Vec3f dir = right * -in.move.x + fwd * in.move.y;
            if (length(dir) > 0.1f) c->facing = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(dir.x, dir.z) * 180.0f / pi);
        }
    }

    void follow_rope(float) {
        const Transform3D* t = _rope_hero.get<Transform3D>();
        if (t == nullptr) return;
        const Vec3f target = t->position + Vec3f{0.0f, 1.3f, 0.0f};
        _pos = target - camera().forward() * 5.0f;
        _velocity = {};
    }

    // --- the character controller (P2c) ----------------------------------------------------------------------------------
    // The test level (tests/character_level.hpp) is built in front of the camera, along +X from there, and a person (entity with a
    // CharacterController3D, a locomotion Animator3D and the foot placer on the physics floor) is put at its start. WASD move it
    // relative to the camera (Space jumps, Shift sprints, Ctrl crouches); the camera follows behind. The keys are the only thing
    // a headless run cannot press: `ctrl_demo` plays a scripted walk through the same command struct instead.
    void ensure_controller() {
        if (!_r3d->capabilities().skinning) return;
        if (_ctrl_hero.is_valid() && _ctrl_hero.is_alive()) return;
        if (!_person_model) _person_model = make_person(*_r3d);
        if (!_ball_mesh) _ball_mesh = _r3d->create_mesh(meshes::sphere(1.0f, 32, 16));
        _ctrl_origin = {_pos.x - 1.0f, 0.0f, _pos.z};
        _ctrl_level = character_level::build(*_physics, true, _ctrl_origin);
        _physics->optimize_broad_phase();
        _ctrl_built = true;
        Animator3D animator = locomotion_animator({.idle = "Sway", .walk = "Walk", .run = "Run", .fall = "Fall"});
        IKConstraint feet = person_constraints(nullptr)[0];
        feet.fade_seconds = 0.12f;
        animator.add_ik(feet);
        CharacterController3D controller;
        controller.desc.height = 1.8f;
        controller.input.forward = {1.0f, 0.0f, 0.0f};
        _ctrl_hero = _ecs->raw().entity()
                         .set(Transform3D{.position = _ctrl_origin + Vec3f{1.0f, 0.0f, 0.0f}})
                         .set(controller)
                         .set(SkinnedMeshRenderer{.model = _person_model})
                         .set(static_cast<const Animator3D&>(animator))
                         .set(CharacterAnimationLink3D{});
        _ctrl_time = 0.0f;
        _ctrl_facing = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 90.0f);
        _yaw = -90.0f; // looking along +X, the way the level runs
    }

    void remove_controller() {
        if (_ctrl_hero.is_valid() && _ctrl_hero.is_alive()) _ctrl_hero.destruct();
        _ctrl_hero = {};
        if (_ctrl_built && _physics) {
            for (const character_level::Piece& p : _ctrl_level.pieces) _physics->destroy_body(p.body);
        }
        _ctrl_level = {};
        _ctrl_built = false;
    }

    void respawn_controller() {
        if (!_ctrl_hero.is_valid() || !_ctrl_hero.is_alive()) return;
        _ctrl_hero.get_mut<Transform3D>()->position = _ctrl_origin + Vec3f{1.0f, 0.0f, 0.0f}; // a move by game code is a teleport
        _ctrl_time = 0.0f;
    }

    // The command of this frame (held for the fixed steps that follow) and the descriptor's live numbers.
    void drive_controller(float dt) {
        if (!_ctrl_hero.is_valid() || !_ctrl_hero.is_alive()) return;
        CharacterController3D* c = _ctrl_hero.get_mut<CharacterController3D>();
        const Character3D handle = _bodies->character_of(_ctrl_hero);
        if (handle.valid()) {
            CharacterDesc3D d = c->desc;
            d.walk_speed = _s.ctrl_speed;
            d.sprint_speed = _s.ctrl_speed * 1.75f;
            d.jump_speed = _s.ctrl_jump;
            d.max_slope_degrees = _s.ctrl_slope;
            d.max_step_height = _s.ctrl_step;
            _physics->set_character_desc(handle, d);
        }
        _ctrl_time += dt;
        CharacterInput3D in;
        in.forward = camera().forward();
        if (_s.ctrl_demo) {
            // Along +X: walk, a jump now and then, a sprint, a crouch; over and over (the level is about 30 m of it).
            const float t = std::fmod(_ctrl_time, 14.0f);
            in.forward = {1.0f, 0.0f, 0.0f};
            in.move = {0.0f, t > 0.5f ? 1.0f : 0.0f};
            in.jump = std::fmod(t, 2.5f) < 0.1f && t > 1.0f;
            in.sprint = t > 7.0f && t < 9.0f;
            in.crouch = t > 11.0f && t < 12.0f;
        } else if (_input != nullptr && _captured) {
            const Input& k = *_input;
            in.move = {float(k.held(Key::D)) - float(k.held(Key::A)), float(k.held(Key::W)) - float(k.held(Key::S))};
            in.jump = k.held(Key::Space);
            in.sprint = k.modifier_held(KeyModifiers::Shift);
            in.crouch = k.modifier_held(KeyModifiers::Ctrl);
        }
        c->input = in;
        // It faces the way it goes (the person faces +Z); physics never turns it.
        const Vec3f fwd = normalize(Vec3f{in.forward.x, 0.0f, in.forward.z});
        const Vec3f right{-fwd.z, 0.0f, fwd.x};
        const Vec3f dir = right * -in.move.x + fwd * in.move.y;
        if (length(dir) > 0.1f) _ctrl_facing = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(dir.x, dir.z) * 180.0f / pi);
        c->facing = _ctrl_facing;
    }

    void follow_controller(float) {
        const Transform3D* t = _ctrl_hero.get<Transform3D>();
        if (t == nullptr) return;
        // The person has no crouch clip: while the capsule is crouched the model is squashed to its height.
        if (const CharacterController3D* c = _ctrl_hero.get<CharacterController3D>(); c != nullptr && c->state.height > 0.0f) {
            _ctrl_hero.get_mut<Transform3D>()->scale.y = c->state.height / c->desc.height;
        }
        const Vec3f target = t->position + Vec3f{0.0f, 1.3f, 0.0f};
        _pos = target - camera().forward() * _s.ctrl_distance;
        _velocity = {};
    }

    // --- grab and climb (P2d) -------------------------------------------------------------------------------------------------
    // The climb level (tests/climb_level.hpp) is built in front of the camera, along +X from there, with its walls facing +Z, and
    // a person (an entity with a CharacterController3D with traversal on, a locomotion Animator3D, the foot placer and a two-bone
    // arm each on the grips) at its start. The camera's forward is the way the hands reach; the mouse buttons (or Q and E) hold
    // with the left and the right hand. A headless run cannot press anything: `climb_demo` plays a scripted climb of the first
    // wall through the same command structs.
    void ensure_climb() {
        if (!_r3d->capabilities().skinning) return;
        if (_climb_hero.is_valid() && _climb_hero.is_alive()) return;
        if (!_person_model) _person_model = make_person(*_r3d);
        _climb_origin = {_pos.x - 1.0f, 0.0f, _pos.z - 4.0f};
        _climb_level = climb_level::build(*_physics, {}, _climb_origin);
        _climb_steps = 0;
        _climb_built = true;
        Animator3D animator = locomotion_animator({.idle = "Sway", .walk = "Walk", .run = "Run", .fall = "Fall"});
        IKConstraint feet = person_constraints(nullptr)[0];
        feet.fade_seconds = 0.12f;
        animator.add_ik(feet);
        for (const char* side : {"L", "R"}) {
            IKConstraint hand;
            hand.name = std::string("hand_") + side;
            hand.kind = IKKind::TwoBone;
            hand.joints = {std::string("shoulder_") + side, std::string("elbow_") + side, std::string("hand_") + side};
            hand.weight = 0.0f;
            hand.fade_seconds = 0.12f;
            hand.pole = IKConstraint::Pole::ModelDirection;
            hand.pole_vector = {side[0] == 'L' ? 0.5f : -0.5f, -0.7f, -0.5f};
            hand.two_bone.max_stretch = 1.05f;
            animator.add_ik(hand);
        }
        CharacterController3D controller;
        controller.desc = climb_level::climber_desc();
        controller.desc.traversal.reach = _s.climb_reach;
        controller.desc.traversal.resource.max = _s.climb_stamina;
        controller.input.forward = {0.0f, 0.0f, -1.0f};
        controller.facing = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 180.0f);
        CharacterAnimationLink3D link;
        link.hands = {"hand_L", "hand_R"};
        link.hand_clearance = 0.05f;
        _climb_hero = _ecs->raw().entity()
                          .set(Transform3D{.position = _climb_origin + Vec3f{0.0f, 0.0f, 0.7f}})
                          .set(controller)
                          .set(SkinnedMeshRenderer{.model = _person_model})
                          .set(static_cast<const Animator3D&>(animator))
                          .set(link);
        _climb_time = 0.0f;
        _yaw = 0.0f; // looking along -Z, at the wall
        _pitch = -8.0f;
    }

    void remove_climb() {
        if (_climb_hero.is_valid() && _climb_hero.is_alive()) _climb_hero.destruct();
        _climb_hero = {};
        if (_climb_built && _physics) {
            for (const climb_level::Piece& p : _climb_level.pieces) _physics->destroy_body(p.body);
        }
        _climb_level = {};
        _climb_built = false;
    }

    void respawn_climb() {
        if (!_climb_hero.is_valid() || !_climb_hero.is_alive()) return;
        _climb_hero.get_mut<Transform3D>()->position = _climb_origin + Vec3f{0.0f, 0.0f, 0.7f}; // a move by game code is a teleport
        _physics->set_traversal_resource(_bodies->character_of(_climb_hero), _s.climb_stamina);
        _climb_time = 0.0f;
    }

    // The commands of this frame (held for the fixed steps that follow) and the descriptor's live numbers.
    void drive_climb(float dt) {
        if (!_climb_hero.is_valid() || !_climb_hero.is_alive()) return;
        CharacterController3D* c = _climb_hero.get_mut<CharacterController3D>();
        const Character3D handle = _bodies->character_of(_climb_hero);
        if (handle.valid()) {
            CharacterDesc3D d = c->desc;
            d.traversal.reach = _s.climb_reach;
            d.traversal.resource.max = _s.climb_stamina;
            _physics->set_character_desc(handle, d);
        }
        _climb_time += dt;
        CharacterInput3D in;
        TraversalInput3D t;
        in.forward = camera().forward();
        t.reach = camera().forward();
        if (_s.climb_demo) {
            // The first wall: take hold with both hands, climb its 4 m, pull up over the ledge, walk off the back; over and over.
            const float s = std::fmod(_climb_time, 12.0f);
            in.forward = {0.0f, 0.0f, -1.0f};
            t.reach = {0.0f, 0.45f, -1.0f};
            t.grab = {s > 0.2f && s < 6.8f, s > 0.2f && s < 6.8f};
            in.move = {0.0f, s > 0.7f && s < 5.0f ? 1.0f : (s > 7.2f ? 1.0f : 0.0f)};
            t.pull_up = s > 5.4f && s < 5.5f;
            if (s < 0.05f && _climb_time > 1.0f) respawn_climb();
        } else if (_input != nullptr && _captured) {
            const Input& k = *_input;
            in.move = {float(k.held(Key::D)) - float(k.held(Key::A)), float(k.held(Key::W)) - float(k.held(Key::S))};
            in.jump = k.held(Key::Space);
            t.jump_off = k.held(Key::Space);
            t.grab = {k.mouse_held(MouseButton::Left) || k.held(Key::Q), k.mouse_held(MouseButton::Right) || k.held(Key::E)};
            t.pull_up = k.held(Key::F);
            t.drop = k.held(Key::X);
        }
        c->input = in;
        c->traversal = t;
        // While it walks it faces the way it goes (the person faces +Z); hanging, the system turns it to the surface.
        if (c->state.traversal_mode == TraversalMode3D::Walking) {
            const Vec3f fwd = normalize(Vec3f{in.forward.x, 0.0f, in.forward.z});
            const Vec3f right{-fwd.z, 0.0f, fwd.x};
            const Vec3f dir = right * -in.move.x + fwd * in.move.y;
            if (length(dir) > 0.1f) c->facing = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(dir.x, dir.z) * 180.0f / pi);
        }
    }

    void follow_climb(float) {
        const Transform3D* t = _climb_hero.get<Transform3D>();
        if (t == nullptr) return;
        const Vec3f target = t->position + Vec3f{0.0f, 1.3f, 0.0f};
        _pos = target - camera().forward() * _s.climb_distance;
        _velocity = {};
    }

    void draw_climb_level() {
        if (!_climb_built) return;
        using climb_level::Kind;
        const auto colour_of = [](Kind k) {
            switch (k) {
            case Kind::Floor: return Color::rgb(112, 120, 108);
            case Kind::Wall: return Color::rgb(186, 160, 130);
            case Kind::Lip: return Color::rgb(210, 190, 150);
            case Kind::Roof: return Color::rgb(130, 140, 160);
            case Kind::SmoothWall: return Color::rgb(150, 156, 170);
            case Kind::Hold: return Color::rgb(230, 130, 70);
            case Kind::Platform: return Color::rgb(80, 140, 220);
            case Kind::Pedestal: return Color::rgb(176, 170, 160);
            case Kind::Crate: return Color::rgb(220, 190, 90);
            case Kind::IceWall: return Color::rgb(150, 205, 235);
            case Kind::LowRoof: return Color::rgb(110, 110, 125);
            case Kind::Ledge: return Color::rgb(170, 120, 120);
            }
            return Color::rgb(200, 200, 200);
        };
        for (const climb_level::Piece& p : _climb_level.pieces) {
            if (p.kind == Kind::Floor) continue; // (Sponza has its own floor)
            const Vec3f centre = p.moving ? _physics->position(p.body) : p.centre;
            const Quatf rotation = p.moving ? _physics->rotation(p.body) : p.rotation;
            _scene.draw(_box_mesh, Mat4f::trs(centre, rotation, p.half * 2.0f), Material3D{.color = colour_of(p.kind), .roughness = 0.85f});
        }
    }

    // What the hands that are asked to hold and hold nothing could take (the best one with a longer normal).
    void climb_candidates_lines() {
        if (!_climb_hero.is_valid() || !_climb_built) return;
        const Character3D c = _bodies->character_of(_climb_hero);
        const CharacterController3D* controller = _climb_hero.get<CharacterController3D>();
        if (!c.valid() || controller == nullptr) return;
        const TraversalState3D t = _physics->traversal(c);
        const CharacterState3D s = _physics->character(c);
        const Vec3f facing = t.mode == TraversalMode3D::Walking ? normalize(Vec3f{controller->input.forward.x, 0.0f, controller->input.forward.z})
                                                                  : -normalize(t.surface_normal);
        const Vec3f right = normalize(cross(facing, s.up));
        for (u32 h = 0; h < t.limb_count; ++h) {
            if (!controller->traversal.grab[h] || t.grips[h].phase != GripPhase3D::Free) continue;
            GripProbeDesc3D probe;
            const Vec3f sh = controller->desc.traversal.limbs[h].shoulder;
            probe.origin = s.position + right * sh.x + s.up * sh.y + facing * sh.z;
            probe.direction = length(controller->traversal.reach) > 0.0f ? controller->traversal.reach : facing;
            probe.up = s.up;
            probe.forward = facing;
            probe.reach = controller->desc.traversal.reach;
            probe.clearance_radius = controller->desc.radius;
            probe.clearance_height = controller->desc.height;
            probe.ignore = s.body;
            _bodies->debug().candidates(_physics->probe_grips(probe));
        }
    }

    // The resource as a bar under the middle of the picture while the climb is on (not drawn by a headless run without --hud).
    void draw_climb_hud(Vec2i out, float u, Color glass, Color rim, Color ink, Color muted) {
        if (!_climb_hero.is_valid() || !_climb_hero.is_alive()) return;
        const Character3D c = _bodies->character_of(_climb_hero);
        if (!c.valid()) return;
        const TraversalState3D t = _physics->traversal(c);
        const float w = 260 * u, h = 22 * u;
        const Rectf r{(float(out.x) - w) * 0.5f, float(out.y) - 48 * u, w, h};
        ui2::Panel back{.bounds = r, .color = glass, .border = rim};
        ui2::run(_ui, back);
        const float f = std::clamp(t.resource.fraction(), 0.0f, 1.0f);
        const Color fill = t.resource.exhausted ? Color::rgb(220, 70, 60) : f < 0.3f ? Color::rgb(230, 170, 60) : Color::rgb(80, 200, 120);
        if (f > 0.0f) {
            ui2::Panel bar{.bounds = {r.x + 3 * u, r.y + 3 * u, (w - 6 * u) * f, h - 6 * u}, .color = fill, .border = fill};
            ui2::run(_ui, bar);
        }
        char label[96];
        const char* mode = t.mode == TraversalMode3D::Walking ? "walking" : t.mode == TraversalMode3D::Hanging ? "hanging" : "pulling up";
        std::snprintf(label, sizeof label, "%s   hands %u   stamina %.0f / %.0f%s", mode, t.held, t.resource.value, t.resource.max,
                      t.resource.exhausted ? "  EXHAUSTED" : "");
        const ui2::TextStyle st = text_style(u, t.resource.exhausted ? ink : muted, 11);
        put(label, r.x, r.y - 18 * u, 16 * u, st);
    }

    // The level's pieces as meshes (the bodies are drawn by the debug lines only when asked).
    void draw_controller_level() {
        if (!_ctrl_built) return;
        using character_level::Kind;
        const auto colour_of = [](Kind k) {
            switch (k) {
            case Kind::Floor: return Color::rgb(112, 120, 108);
            case Kind::Ramp: return Color::rgb(214, 150, 80);
            case Kind::Plateau: return Color::rgb(196, 176, 140);
            case Kind::Hump: return Color::rgb(120, 170, 120);
            case Kind::Step: return Color::rgb(176, 170, 160);
            case Kind::Ledge: return Color::rgb(170, 120, 120);
            case Kind::Wall: return Color::rgb(150, 160, 180);
            case Kind::Ceiling: return Color::rgb(130, 140, 160);
            case Kind::Box: return Color::rgb(220, 190, 90);
            case Kind::HeavyBox: return Color::rgb(90, 70, 60);
            case Kind::Platform: return Color::rgb(80, 140, 220);
            case Kind::Spinner: return Color::rgb(80, 200, 190);
            case Kind::Sensor: return Color::rgb(240, 220, 90);
            }
            return Color::rgb(200, 200, 200);
        };
        for (const character_level::Piece& p : _ctrl_level.pieces) {
            if (p.kind == Kind::Sensor || p.kind == Kind::Floor) continue; // (Sponza has its own floor; a trigger is not a thing)
            const Vec3f centre = p.moving ? _physics->position(p.body) : p.centre;
            const Quatf rotation = p.moving ? _physics->rotation(p.body) : p.rotation;
            const Material3D m{.color = colour_of(p.kind), .roughness = 0.85f};
            if (p.shape == Shape3DKind::Sphere) {
                _scene.draw(_ball_mesh, Mat4f::trs(centre, rotation, p.half), m);
            } else {
                _scene.draw(_box_mesh, Mat4f::trs(centre, rotation, p.half * 2.0f), m);
            }
        }
        // The level's platforms move by the physics step: drive them from its clock, one step ahead of the world's.
    }

    void fill_scene() {
        _scene.clear();
        _scene.camera = camera();
        apply(_s, _scene, environment(), _model.bounds, _pos);
        if (_ecs_state && (!_characters.empty() || !_boxes.empty() || _ctrl_hero.is_valid() || _climb_hero.is_valid() ||
                           !_rag_people.empty() || _rope_hero.is_valid())) {
            _ecs_state->propagate_transforms();
            _ecs_state->collect(_scene);
            if (_bodies) {
                _bodies->config().debug_draw = (_s.physics && _s.phys_debug) || (_s.ctrl && _s.ctrl_debug) || (_s.climb && _s.climb_debug) ||
                                               (_s.rag && _s.rag_debug) || (_s.rope && _s.rope_debug);
                if (_s.climb && _s.climb_debug) climb_candidates_lines();
                if (_s.ctrl && _s.ctrl_debug && _ctrl_hero.is_valid()) {
                    // What the character touches: a cross at each contact and its normal (green: it can stand on it).
                    for (const CharacterContact3D& k : _physics->character_contacts(_bodies->character_of(_ctrl_hero))) {
                        _bodies->debug().cross(k.point, k.sensor ? Color::rgb(255, 220, 60) : Color::rgb(255, 60, 60));
                        _bodies->debug().line(k.point, k.point + k.normal * 0.4f,
                                              k.walkable ? Color::rgb(60, 255, 120) : Color::rgb(255, 120, 0));
                    }
                }
                _bodies->collect_debug(*_r3d, _scene);
            }
            draw_controller_level();
            draw_climb_level();
            if (_s.rope && _bodies) _bodies->collect_ropes(*_r3d, _scene);
            if (_rag_ball.valid() && _physics && _ball_mesh) {
                _scene.draw(_ball_mesh, Mat4f::trs(_physics->position(_rag_ball), _physics->rotation(_rag_ball), {0.2f, 0.2f, 0.2f}),
                            Material3D{.color = Color::rgb(230, 80, 60), .roughness = 0.4f});
            }
            if (_s.ik && _characters_are_people) {
                // The hand's target (a small red ball), the tail's (a green one) and the step, in each person's own space as it was
                // placed (they stay put while the person walks).
                const PersonIk ik = person_ik(_s);
                for (std::size_t i = 0; i < _characters.size() && i < _person_origin.size(); ++i) {
                    const Transform3D* t = _characters[i].get<Transform3D>();
                    if (!t) continue;
                    const Mat4f m = Transform3D{.position = _person_origin[i].first, .rotation = _person_origin[i].second, .scale = t->scale}.matrix();
                    const float r = 0.04f * t->scale.x;
                    if (_s.ik_marker) {
                        Material3D red;
                        red.color = Color::rgb(255, 40, 30);
                        red.unlit = true;
                        red.casts_shadow = false;
                        _scene.draw(_marker, Mat4f::translation(m.apply(person_hand_target(ik))) * Mat4f::scaling({r, r, r}), red);
                        if (ik.tail) {
                            Material3D green;
                            green.color = Color::rgb(40, 220, 60);
                            green.unlit = true;
                            green.casts_shadow = false;
                            _scene.draw(_marker, Mat4f::translation(m.apply(person_tail_target(ik))) * Mat4f::scaling({r, r, r}), green);
                        }
                    }
                    if (_s.ik_step && _s.ik_step_height > 1e-4f) {
                        const float h = _s.ik_step_height;
                        Material3D stone;
                        stone.color = Color::rgb(120, 122, 130);
                        _scene.draw(_step_mesh, m * Mat4f::translation({0.3f, h * 0.5f, 0.0f}) * Mat4f::scaling({0.6f, h, 0.8f}), stone);
                    }
                }
            }
        }
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
            if (_options.lod_clusters) out << ",\n  \"lod_clusters\": true, \"lod_gpu\": " << (_s.lod_gpu ? "true" : "false") << ", \"lod_occlusion\": " << (_s.lod_occlusion ? "true" : "false");
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
        if (_s.climb && (!ctx.app.headless() || _options.hud)) draw_climb_hud(out, u, glass, rim, ink, muted);
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
        if (st.skinned_copies > 0) {
            std::snprintf(line, sizeof line, "skinned: %u copies in %u draws, %u joint matrices", st.skinned_copies, st.skinned_draws, st.skin_matrices);
            lines.push_back(line);
        }
        if (st.shadow_cascades > 0) {
            std::snprintf(line, sizeof line, "shadows: %u draws, %u casters, %u maps [%u %u %u %u]%s", st.shadow_draws, st.shadow_casters,
                          st.shadow_cascades, st.cascade_casters[0], st.cascade_casters[1], st.cascade_casters[2], st.cascade_casters[3],
                          st.shadow_cached ? "   KEPT from the render before" : "");
        } else {
            std::snprintf(line, sizeof line, "shadows: off");
        }
        lines.push_back(line);
        std::snprintf(line, sizeof line, "ao: %u draws, %u meshes, %u dispatches%s   lights %u", st.ao_draws, st.ao_instances, st.ao_dispatches,
                      st.ao_chain_built ? " (depth chain)" : "", st.lights);
        lines.push_back(line);
        if (st.feature_bytes > 0 || st.taa_main_velocity) {
            std::snprintf(line, sizeof line, "feature targets %.1f MiB%s%s", double(st.feature_bytes) / 1048576.0,
                          st.taa_main_velocity ? "   taa velocity from the main pass" : "", st.blur_merged ? "   one blur for ao and contact" : "");
            lines.push_back(line);
        }
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
            if (st.occlusion) {
                std::snprintf(line, sizeof line, "occlusion: %u pyramid levels, %u dispatches", st.occlusion_levels, st.occlusion_dispatches);
                lines.push_back(line);
                if (_s.lod_readback) {
                    std::snprintf(line, sizeof line, "  %u clusters in the first pass, %u occluded (%llu triangles), %u drawn for nothing", st.clusters_first_pass,
                                  st.clusters_occluded, static_cast<unsigned long long>(st.triangles_occluded), st.clusters_wasted);
                    lines.push_back(line);
                }
            } else if (_s.lod_occlusion) {
                lines.push_back("occlusion: asked for, not running (needs the GPU cut and clusters)");
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
    std::shared_ptr<const Model> _character_model;
    std::unique_ptr<::kin::EcsWorld> _ecs;
    std::unique_ptr<WorldRenderState3D> _ecs_state;
    std::vector<flecs::entity> _characters;
    std::unique_ptr<World3D> _physics;
    std::unique_ptr<WorldPhysicsState3D> _bodies;
    flecs::entity _floor;
    std::vector<flecs::entity> _boxes;
    Mesh _box_mesh;
    Mesh _ball_mesh;
    bool _physics_started = false;
    // The character controller section.
    flecs::entity _ctrl_hero;
    character_level::Level _ctrl_level;
    bool _ctrl_built = false;
    Vec3f _ctrl_origin{};
    Quatf _ctrl_facing{};
    float _ctrl_time = 0.0f, _ctrl_accumulator = 0.0f;
    int _ctrl_steps = 0;
    // The climb section.
    flecs::entity _climb_hero;
    climb_level::Level _climb_level;
    bool _climb_built = false;
    Vec3f _climb_origin{};
    float _climb_time = 0.0f;
    int _climb_steps = 0;
    // The ragdoll and rope section.
    std::vector<flecs::entity> _rag_people;
    Body3D _rag_ball{};
    bool _rag_down = false;
    float _rag_time = 0.0f;
    flecs::entity _rope_hero, _rope_entity;
    Vec3f _rope_beam{}, _rope_start{};
    float _rope_time = 0.0f;
    const Input* _input = nullptr;
    Model _model;
    Mesh _marker;
    Mesh _step_mesh;
    std::shared_ptr<const Model> _person_model;
    std::vector<std::pair<Vec3f, Quatf>> _person_origin; // where each person was placed (its IK targets and the step are in that space)
    float _ik_time = 0.0f;
    bool _characters_are_people = false;
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
    bool _warmed = false;
    std::tuple<bool, bool, bool, bool, bool, bool, int, bool, bool, bool, bool, bool, int, u32, bool, int, int> _warmed_key;
    bool _ready = false, _captured = false, _quit = false, _shot_requested = false, _shot_done = false;
    Clock::time_point _last = Clock::now();
};

void print_help() {
    std::cout << "sponza_walk: fly round Sponza on kin's experimental 3D renderer, every feature switchable.\n"
                 "  --model PATH          the glTF (else $KIN_REFERENCE_CACHE/sponza/Sponza.gltf, else ~/.cache/kin/reference/sponza/Sponza.gltf;\n"
                 "                        python3 tools/reference/fetch.py puts it there)\n"
                 "  --lod-levels N        levels of detail built for each mesh at load (default 4; 1: none); --preset lod turns them on\n"
                 "  --lod-clusters        clusters instead of levels (a hierarchy a mesh, cut by the error bound); --preset clusters is this and the setting on;\n"
                 "                        --preset clusters-gpu adds the cut in compute (the default where the device can) with its counts read back;\n"
                 "                        --preset occlusion adds occlusion culling (clusters behind others are not drawn) to that\n"
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
