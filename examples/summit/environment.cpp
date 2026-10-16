#include "environment.hpp"

#include <algorithm>
#include <cmath>

namespace summit {

namespace {

f32 smoothstep(f32 a, f32 b, f32 x) {
    const f32 t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

} // namespace

const std::vector<Look>& looks() {
    //                  name         hours turb   fog      hfog     rain  snow  mist  exp   bloom stars
    static const std::vector<Look> all = {
        {"afternoon", 16.2f, 2.6f, 0.00045f, 0.0030f, 0.0f, 0.0f, 0.0f, 1.0f, 0.25f, false},
        {"morning", 7.3f, 3.0f, 0.00080f, 0.0100f, 0.0f, 0.0f, 0.15f, 1.0f, 0.30f, false},
        {"noon", 12.4f, 2.3f, 0.00030f, 0.0015f, 0.0f, 0.0f, 0.0f, 0.95f, 0.20f, false},
        {"dusk", 18.9f, 3.2f, 0.00090f, 0.0040f, 0.0f, 0.0f, 0.0f, 1.2f, 0.35f, false},
        {"night", 1.4f, 2.4f, 0.00060f, 0.0020f, 0.0f, 0.0f, 0.0f, 2.2f, 0.30f, true},
        {"snow", 14.0f, 3.5f, 0.00300f, 0.0100f, 0.0f, 0.85f, 0.2f, 1.0f, 0.20f, false},
        {"storm", 16.6f, 4.5f, 0.00450f, 0.0100f, 1.0f, 0.0f, 0.45f, 1.0f, 0.25f, false},
    };
    return all;
}

const Look* find_look(const std::string& name) {
    for (const Look& l : looks()) {
        if (name == l.name) return &l;
    }
    return nullptr;
}

f32 snow_cover(f32 altitude, f32 height, f32 normal_up) {
    const f32 line = snow_line_fraction * height;
    const f32 band = snow_band_fraction * height;
    const f32 high = smoothstep(line - 0.25f * band, line + band, altitude);
    // Flat ground holds it all, a steep face almost none, an overhang none.
    const f32 up = smoothstep(0.25f, 0.85f, normal_up);
    return std::clamp(high * (0.18f + 0.82f * up) * (normal_up > 0.0f ? 1.0f : 0.0f), 0.0f, 1.0f);
}

f32 clock_hours(f32 start_hours, f64 game_seconds, f32 time_speed) {
    const f64 h = f64(start_hours) + game_seconds / 60.0 * f64(std::max(0.0f, time_speed));
    f64 w = std::fmod(h, 24.0);
    if (w < 0.0) w += 24.0;
    return f32(w);
}

f32 fire_light_strength(f32 daylight) {
    // A glow by day (a tenth), full strength in the dark.
    return lerp(1.0f, 0.1f, smoothstep(0.0f, 0.6f, daylight));
}

EnvironmentState evaluate_environment(const EnvironmentInput& in) {
    const Look& look = in.look != nullptr ? *in.look : looks().front();
    const EffectSettings& fx = in.effects;
    EnvironmentState e;
    const f32 height = std::max(1.0f, in.height);
    e.altitude_fraction = std::clamp(in.altitude / height, 0.0f, 1.0f);
    const f32 t = fx.altitude_look ? smoothstep(0.0f, 1.0f, e.altitude_fraction) : 0.0f;
    e.hours = fx.time_of_day ? clock_hours(look.hours, in.game_seconds, in.time_speed) : look.hours;

    // Thinner air higher up: less haze, a clearer and bluer sky, a stronger sun, a colder fog; the mist that hugs the ground
    // stays low.
    e.fog_density = look.fog_density * lerp(1.0f, 0.45f, t);
    e.fog_height_density = look.fog_height_density * lerp(1.0f, 0.2f, t);
    e.turbidity = look.turbidity * lerp(1.0f, 0.68f, t);
    e.sun_intensity = 3.0f * lerp(1.0f, 1.18f, t);
    e.fog_tint = {lerp(1.0f, 0.86f, t), lerp(1.0f, 0.94f, t), lerp(1.0f, 1.08f, t)};
    e.exposure = look.exposure;

    e.rain = look.rain;
    e.snow = look.snow;
    e.mist = look.mist;
    e.snow_line = snow_line_fraction * height;
    e.snow_share = smoothstep(e.snow_line - 0.25f * snow_band_fraction * height, e.snow_line + snow_band_fraction * height, in.altitude);
    if (fx.snow_line) {
        e.snow = std::max(e.snow, 0.55f * e.snow_share);
        e.mist = std::min(1.0f, e.mist + 0.12f * e.snow_share);
    }
    // Wind carries dust low down and flakes up high: the dust layer is for the bare lower mountain.
    const f32 gusty = std::clamp((in.wind_speed - 2.0f) / 9.0f, 0.0f, 1.0f);
    e.gust_strength = fx.wind_particles ? 0.25f + 0.5f * gusty : 0.0f;
    e.turbulence = fx.wind_particles ? 0.3f + 1.4f * gusty : 0.0f;
    e.dust = fx.wind_particles ? 0.45f * gusty * (1.0f - 0.8f * e.snow_share) : 0.0f;
    if (fx.storm) {
        // Heavier air and a darker light; wet below the snow line, snow above it.
        e.fog_density *= 2.2f;
        e.fog_height_density *= 1.5f;
        e.turbidity = std::min(6.0f, e.turbidity + 1.2f);
        e.mist = std::min(1.0f, e.mist + 0.3f);
        e.rain = std::max(e.rain, 0.75f * (1.0f - e.snow_share));
        e.snow = std::max(e.snow, 0.9f * e.snow_share);
        e.gust_strength = std::min(1.0f, e.gust_strength + 0.25f);
        e.turbulence += 1.0f;
    }
    return e;
}

} // namespace summit
