#pragma once

// Summit's settings (docs/3d_plan.md, P4b): what a player changes and expects back next time, kept in the game's
// settings file by kin's SaveStore (`<save root>/summit/settings.json`; the root is the platform's user-data
// directory). Plain data, a JSON round trip, and a reader that never fails: a missing or damaged file, a wrong type or
// a number out of range gives the defaults (or the default of that one field), so a bad file cannot keep the game from
// starting.
//
//     summit::SummitSettings s = summit::load_settings(store);     // defaults when there is no file
//     s.master_volume = 0.5f;
//     summit::save_settings(store, s);

#include "input_map.hpp"

#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/save/save_store.hpp>
#include <string>

namespace summit {
using namespace kin;

// Which presentation effects are on. Each is cheap, opt-in and switchable; none touches the simulation.
struct EffectSettings {
    bool altitude_look = true;  // fog, haze, sun and sky change with altitude
    bool time_of_day = true;    // the clock advances over a run (see SummitSettings::time_speed)
    bool snow_line = true;      // snow falls and lies above a height band
    bool camp_lights = true;    // each camp's fire is a light at dusk and night
    bool fires = true;          // a fire's flames and smoke at each camp
    bool puffs = true;          // dust where it lands, breath in the cold
    bool wind_particles = true; // dust and flakes carried by the sim's wind
    bool storm = false;         // a heavier, darker, wetter look
    friend bool operator==(const EffectSettings&, const EffectSettings&) = default;
};

struct SummitSettings {
    std::string look = "afternoon";
    bool first_person = false;
    bool hud = true;
    // Linear volumes, 0 to 1: the whole mix, then the sound effects and the ambience (wind, fire) under it.
    f32 master_volume = 0.8f;
    f32 sfx_volume = 1.0f;
    f32 ambience_volume = 0.7f;
    // Game hours that pass a real minute while the time of day runs (0: frozen).
    f32 time_speed = 0.25f;
    EffectSettings effects{};
    // Key bindings, sensitivities and invert (InputBindings::to_text round trip).
    InputBindings bindings = InputBindings::defaults();

    friend bool operator==(const SummitSettings&, const SummitSettings&) = default;
};

// Writes the settings as one JSON object (a value inside a SaveStore settings payload, or the whole payload).
void write_settings(JsonWriter& json, const SummitSettings& settings);
// Reads one back over `out` (which holds the defaults or the current values): each field that is there and sane is
// taken, the others are left. False when `value` is not an object at all (nothing changed).
bool read_settings(const JsonValue& value, SummitSettings& out);

// The settings file: defaults for no file, a file that does not parse, or one of another game. `found` (optional) says
// whether a file was read. save_settings returns false when the file cannot be written.
SummitSettings load_settings(const SaveStore& store, bool* found = nullptr);
bool save_settings(SaveStore& store, const SummitSettings& settings);

// The store Summit keeps its settings in; `root` empty is the platform's user-data directory.
SaveStore make_settings_store(const std::filesystem::path& root = {});

} // namespace summit
