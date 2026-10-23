#include "settings.hpp"

#include "environment.hpp"

#include <algorithm>
#include <cmath>

namespace summit {

namespace {

f32 clamped(const JsonValue& v, const char* key, f32 fallback, f32 lo, f32 hi) {
    const JsonValue* n = v.find(key);
    if (n == nullptr || !n->is_number())
        return fallback;
    const f64 x = n->as_number();
    if (!std::isfinite(x))
        return fallback;
    return f32(std::clamp(x, f64(lo), f64(hi)));
}

bool flag(const JsonValue& v, const char* key, bool fallback) {
    const JsonValue* n = v.find(key);
    return n != nullptr && n->is_bool() ? n->as_bool() : fallback;
}

} // namespace

void write_settings(JsonWriter& json, const SummitSettings& s) {
    json.begin_object();
    json.field("look", s.look);
    json.field("first_person", s.first_person);
    json.field("hud", s.hud);
    json.field("master_volume", f64(s.master_volume));
    json.field("sfx_volume", f64(s.sfx_volume));
    json.field("ambience_volume", f64(s.ambience_volume));
    json.field("time_speed", f64(s.time_speed));
    json.key("effects").begin_object();
    json.field("altitude_look", s.effects.altitude_look);
    json.field("time_of_day", s.effects.time_of_day);
    json.field("snow_line", s.effects.snow_line);
    json.field("camp_lights", s.effects.camp_lights);
    json.field("fires", s.effects.fires);
    json.field("puffs", s.effects.puffs);
    json.field("wind_particles", s.effects.wind_particles);
    json.field("storm", s.effects.storm);
    json.end_object();
    json.field("bindings", s.bindings.to_text());
    json.end_object();
}

bool read_settings(const JsonValue& v, SummitSettings& out) {
    if (!v.is_object())
        return false;
    SummitSettings s = out;
    if (const JsonValue* look = v.find("look");
        look != nullptr && look->is_string() && find_look(look->as_string()) != nullptr) {
        s.look = look->as_string();
    }
    s.first_person = flag(v, "first_person", s.first_person);
    s.hud = flag(v, "hud", s.hud);
    s.master_volume = clamped(v, "master_volume", s.master_volume, 0.0f, 1.0f);
    s.sfx_volume = clamped(v, "sfx_volume", s.sfx_volume, 0.0f, 1.0f);
    s.ambience_volume = clamped(v, "ambience_volume", s.ambience_volume, 0.0f, 1.0f);
    s.time_speed = clamped(v, "time_speed", s.time_speed, 0.0f, 12.0f);
    if (const JsonValue* e = v.find("effects"); e != nullptr && e->is_object()) {
        EffectSettings& f = s.effects;
        f.altitude_look = flag(*e, "altitude_look", f.altitude_look);
        f.time_of_day = flag(*e, "time_of_day", f.time_of_day);
        f.snow_line = flag(*e, "snow_line", f.snow_line);
        f.camp_lights = flag(*e, "camp_lights", f.camp_lights);
        f.fires = flag(*e, "fires", f.fires);
        f.puffs = flag(*e, "puffs", f.puffs);
        f.wind_particles = flag(*e, "wind_particles", f.wind_particles);
        f.storm = flag(*e, "storm", f.storm);
    }
    if (const JsonValue* b = v.find("bindings"); b != nullptr && b->is_string()) {
        // from_text changes nothing for text it does not understand: a damaged table keeps the defaults.
        InputBindings next = InputBindings::defaults();
        if (next.from_text(b->as_string())) {
            next.dead_zone = std::clamp(next.dead_zone, 0.0f, 0.9f);
            next.trigger_threshold = std::clamp(next.trigger_threshold, 0.05f, 0.95f);
            next.mouse_sensitivity =
                std::clamp(std::isfinite(next.mouse_sensitivity) ? next.mouse_sensitivity : 0.10f, 0.01f, 2.0f);
            next.stick_look_speed =
                std::clamp(std::isfinite(next.stick_look_speed) ? next.stick_look_speed : 150.0f, 20.0f, 720.0f);
            s.bindings = std::move(next);
        }
    }
    out = std::move(s);
    return true;
}

SaveStore make_settings_store(const std::filesystem::path& root) {
    SaveStoreConfig config;
    config.game_id = "summit";
    config.game_version = "0.1";
    config.root_override = root;
    return SaveStore(config);
}

SummitSettings load_settings(const SaveStore& store, bool* found) {
    SummitSettings s;
    if (found != nullptr)
        *found = false;
    const SaveLoadResult loaded = store.read_settings();
    if (!loaded.result.ok)
        return s;
    if (read_settings(loaded.payload, s) && found != nullptr)
        *found = true;
    return s;
}

bool save_settings(SaveStore& store, const SummitSettings& settings) {
    return store.write_settings([&](JsonWriter& json) { write_settings(json, settings); }).ok;
}

} // namespace summit
