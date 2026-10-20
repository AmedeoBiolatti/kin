#pragma once

// Time of day for kin's experimental 3D (docs/3d_plan.md, P3a): where the sun and the moon are for a time of day, a
// day of the year and a latitude, and the lights, sky and fog that go with it. Pure functions, deterministic, no GPU.
//
//     kin::TimeOfDay3D day{.hours = 18.5f, .day_of_year = 200, .latitude = 48.0f};
//     kin::apply(day, scene.lights, scene.sky, scene.fog);          // sun, moonlight, ambient, sky, fog colour
//     ...a game clock:   kin::advance(day, dt);                     // `speed` game seconds a second
//
// The sun's position is the textbook one (Cooper's declination, the hour angle, no equation of time: `hours` is solar
// time). World axes: +y up, north is -z, east +x, so the sun at noon in the northern hemisphere is toward +z.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/render3d/sky3d.hpp>

namespace kin {

struct TimeOfDay3D {
    f32 hours = 12.0f;        // solar time, 0 to 24
    f32 day_of_year = 172.0f; // 1 to 365 (81: the March equinox, 172: the June solstice, 264: the September equinox)
    f32 latitude = 45.0f;     // degrees, north positive
    f32 axial_tilt = 23.44f;  // the planet's, degrees (0: the sun stays over the equator all year)
    f32 moon_phase = 0.5f;    // 0 new, 0.25 first quarter, 0.5 full
    // A game clock (advance(), and WorldRenderState3D::advance_time_of_day): game seconds that pass each real second
    // (60: a game hour a real minute; 0: it stands still).
    f32 speed = 0.0f;
    // With it (P3b), apply() also drives the shadows of the two lights: the sun's strength fades to nothing between 1
    // degree above the horizon and 2 below it, and the moon casts (Lights3D::fill_shadow, its cascades and resolution
    // as the game set them) once it is above the horizon and bright enough, its strength fading in over its first 8
    // degrees and with its light. Off, apply() leaves them as they are (the sun casts while it shines, the moon never).
    bool shadows = false;
    // advance()'s own: the clock summed exactly (f64; `hours` is f32, and a frame at speed 1 is below its resolution
    // in the evening) and the `hours` it last wrote, so that it notices when the game sets `hours` itself and starts
    // again from that. Not authored, not for setting: leave them alone.
    f64 clock = -1.0;
    f32 clock_hours = -1.0f;
};

// What a time of day makes of the sky.
struct SkyState3D {
    Vec3f sun_direction{0.0f, 1.0f, 0.0f}; // toward the sun, length 1 (below the horizon at night)
    f32 sun_elevation = 90.0f;             // degrees above the horizon
    f32 sun_azimuth = 180.0f;              // degrees from north (-z) through east (+x)
    f32 declination = 0.0f;                // the sun's, degrees
    LinearRgb sun_color{};                 // its light on the ground (Lights3D::sun.color), 0 below the horizon
    Vec3f moon_direction{0.0f, -1.0f, 0.0f};
    f32 moon_elevation = -90.0f;
    f32 moon_illumination = 0.0f; // 0 new, 1 full
    f32 moon_light = 0.0f;        // its strength (Sky3D::moon_light): 0.1 at full moon high up, 0 below the horizon
    LinearRgb moon_color{};       // its light on the ground (the fill light): a bluish white times moon_light
    LinearRgb ambient{};          // the sky's light from above (Lights3D::ambient), over pi
    LinearRgb ground_ambient{};   // and the ground's from below
    LinearRgb fog_color{};        // the sky's colour at the horizon away from the sun
    f32 fog_density = 0.0f;       // a suggestion for Fog3D::density: a visibility of 24 km over the haze
    f32 daylight = 1.0f;          // 0 at night to 1 by day (the sun's light on the ground, as a fraction of its noon)
};

// The sun and the moon, and what they and the sky make of the light, for `time`, with `sky`'s haze and sun intensity.
SkyState3D evaluate(const TimeOfDay3D& time, const Sky3D& sky = {});
// Puts it into the scene's settings: lights.sun (direction and colour), lights.fill (the moon's light; its shadow when
// TimeOfDay3D::shadows asks), the ambient and ground ambient, sky.moon_direction and moon_light and `moon` (on while
// the moon is up), and fog.colour. Leaves what is a look (the sky's haze and intensity, the fog's density, the stars)
// as it is. Returns the state.
SkyState3D apply(const TimeOfDay3D& time, Lights3D& lights, Sky3D& sky, Fog3D& fog);
// The solar hour (0 to 24) the sun's upper limb rises and sets, with 0.833 degrees of refraction and size; nothing
// (a polar day or night: -1 and -1 for night, 0 and 24 for day) when it does not.
struct SunTimes3D {
    f32 sunrise = -1.0f;
    f32 sunset = -1.0f;
    bool polar_day = false;
    bool polar_night = false;
};
SunTimes3D sun_times(const TimeOfDay3D& time);
// Steps the clock by `dt` real seconds: `speed` game seconds each, hours wrapping at 24 into the next day (365 days a
// year). Exact over any number of frames (an f64 sum behind the f32 `hours`: an hour of frames at 60 fps is an hour,
// not 0.82 of one); a NaN or infinite `dt` or `speed` changes nothing.
void advance(TimeOfDay3D& time, f32 dt);

} // namespace kin
