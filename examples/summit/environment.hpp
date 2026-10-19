#pragma once

// Summit's air (docs/3d_plan.md, P4b): the look of the sky and the fog over a run as pure functions of the player's
// altitude, the game's clock and the wind. No renderer in here: `evaluate_environment` returns numbers that the view
// puts into the scene, so the curves (fog against altitude, the snow line, the time of day) are checked on the CPU.
// Nothing here is read by the simulation.
//
//     const summit::EnvironmentState e = summit::evaluate_environment({.look = &look, .altitude = game.altitude(0),
//     ...}); scene.fog.density = e.fog_density;  // and so on (SummitView::fill does)
//
// A `Look` is the mood of one picture (time of day, haze, weather); the environment moves that mood along the climb:
// thinner, clearer, colder and snowier air higher up, and a clock that runs slowly (or not at all).

#include "settings.hpp"

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <string>
#include <vector>

namespace summit {
using namespace kin;

// A look: the time of day and the air at the start of a run (a mood, not a mechanic).
struct Look {
    const char* name;
    f32 hours;
    f32 turbidity;
    f32 fog_density;
    f32 fog_height_density;
    f32 rain;
    f32 snow;
    f32 mist;
    f32 exposure;
    f32 bloom;
    bool stars;
};

const std::vector<Look>& looks();
const Look* find_look(const std::string& name);

struct EnvironmentInput {
    const Look* look = nullptr; // null: the first look
    f32 altitude = 0.0f;        // feet above base camp (m)
    f32 height = 150.0f;        // base camp to the summit (m)
    f64 game_seconds = 0.0;     // the game's clock (steps / 60)
    f32 wind_speed = 3.0f;      // m/s (the sim's)
    EffectSettings effects{};
    f32 time_speed = 0.25f; // game hours a real minute of game time (0: frozen)
};

struct EnvironmentState {
    f32 altitude_fraction = 0.0f; // 0 at base camp, 1 at the summit
    f32 hours = 12.0f;            // solar time, 0 to 24
    f32 fog_density = 0.0f;
    f32 fog_height_density = 0.0f;
    f32 turbidity = 2.5f;
    f32 sun_intensity = 3.0f;
    Vec3f fog_tint{1.0f, 1.0f, 1.0f};                       // times the time-of-day fog colour: cooler with height
    f32 rain = 0.0f, snow = 0.0f, mist = 0.0f, dust = 0.0f; // weather layer intensities, 0 to 1
    f32 snow_line = 0.0f;                                   // the altitude (m) snow starts at
    f32 snow_share = 0.0f;                                  // 0 below the band, 1 above it, at this altitude
    f32 exposure = 1.0f;
    f32 gust_strength = 0.0f; // Wind3D::gust_strength
    f32 turbulence = 0.0f;    // Wind3D::turbulence (m/s)
};

EnvironmentState evaluate_environment(const EnvironmentInput& in);

// The snow line as a share of the mountain's height, and the width of the band it comes in over (also the rock's snow
// tint).
inline constexpr f32 snow_line_fraction = 0.55f;
inline constexpr f32 snow_band_fraction = 0.14f;
// 0 to 1 snow lying at `altitude` on a surface whose normal has `normal_up` as its +Y part: more on what faces up, none
// on a wall, none below the line.
f32 snow_cover(f32 altitude, f32 height, f32 normal_up);

// The time of day a clock gives: `start` plus `time_speed` hours a minute, wrapped to 0 to 24.
f32 clock_hours(f32 start_hours, f64 game_seconds, f32 time_speed);

// Light to put at a camp fire: how much of its full strength it has in `daylight` (0 night to 1 noon): a fire is a glow
// by day and a light at night.
f32 fire_light_strength(f32 daylight);

} // namespace summit
