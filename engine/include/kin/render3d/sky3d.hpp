#pragma once

// A sky, and fog that agrees with it, for kin's experimental 3D (docs/3d_plan.md, P3a). Plain data and pure
// functions, no GPU: Scene3D::sky and Scene3D::fog hold the settings, Renderer3D::render() draws them, and the
// functions here are the same arithmetic on the CPU (the tests compare the pictures with them, and TimeOfDay3D
// builds its lights from them).
//
//     scene.sky = {.enabled = true, .turbidity = 3.0f};
//     scene.fog = {.enabled = true, .density = 0.002f, .colour_from_sky = true};
//     kin::TimeOfDay3D day{.hours = 17.5f};          // render3d/timeofday3d.hpp
//     kin::apply(day, scene.lights, scene.sky, scene.fog);
//
// The sky is a single-scattering atmosphere (Rayleigh and Mie, an exponential atmosphere seen over a flat ground,
// eight steps along the ray) lit by Lights3D::sun.direction. Fog and the sky use the one model, so that far
// geometry melts into the sky at the horizon whatever the time of day. docs/rendering_3d.md has the model.

#include <array>
#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>

namespace kin {

enum class SkyMode3D : u8 {
    Procedural, // the atmosphere below
    Image,      // the panorama in Lights3D::environment, as it lights (intensity and rotation too)
};

struct Sky3D {
    // Off: the view shows its background colour where nothing is drawn, as before. On, the sky is drawn there, after
    // the opaque meshes and before the blended ones, in the view's HDR colour (bloom picks up the sun).
    bool enabled = false;
    SkyMode3D mode = SkyMode3D::Procedural;

    // --- Procedural ---
    // Haze: 2 is a very clear day, 3 to 4 an ordinary one, 8 and more a murky one (clamped to 1 to 20).
    f32 turbidity = 2.5f;
    // The sun's light above the atmosphere, in kin's light units (a white surface facing a clear noon sun shows about
    // this times the atmosphere's transmittance, 0.7 to 0.9 of it). The sky and the sun disc scale with it.
    // Lights3D::sun is what lights the meshes: set the two together (TimeOfDay3D's apply() does).
    f32 sun_intensity = 3.0f;
    // Times the light of the sky itself (not the sun disc, the moon or the stars).
    f32 intensity = 1.0f;
    // The ground below the horizon: its colour (a linear albedo, lit by the sun and sky as a horizontal surface is).
    // The sky's colour reaches it over the 9 degrees under the horizon, so there is no seam.
    Vec3f ground_color{0.20f, 0.18f, 0.16f};
    // The sun disc: its angular diameter in degrees (the real sun is 0.53; a little more reads better in a game's
    // field of view) and its radiance as a multiple of the sun's light (HDR: above 1 feeds bloom).
    f32 sun_size = 0.8f;
    f32 sun_disc = 30.0f;
    // A hash-based star field with its brightness (HDR), faded in as the sun sinks: none while the sun is higher
    // than 3 degrees under the horizon, all of it from 12 degrees under.
    bool stars = false;
    f32 star_brightness = 8.0f;
    // A moon: a disc of `moon_size` degrees toward `moon_direction`, of `moon_color` times `moon_light` (HDR), which
    // also scatters in the atmosphere (a faint glow round it) when above the horizon. TimeOfDay3D fills it in.
    bool moon = false;
    Vec3f moon_direction{0.3f, 0.7f, -0.6f};
    f32 moon_size = 1.4f;
    f32 moon_light = 0.0f;
    Vec3f moon_color{0.78f, 0.84f, 1.0f};
    // The night sky's own faint light (airglow), added to the sky.
    Vec3f night_color{0.0006f, 0.0009f, 0.0020f};

    // --- Image-based light from the sky (Lights3D::sky_lights_scene) ---
    // The environment is made again when a sky parameter (the sun's direction, its light, the haze, the colours) has
    // moved by more than `light_threshold` and `light_interval` renders have passed since the last time: a
    // time-of-day sweep re-filters a few times a second, not each frame.
    f32 light_threshold = 0.004f;
    u32 light_interval = 4;
};

// Distance and height fog of the main pass: opaque, cut-out, skinned, instanced and clustered meshes, blended and
// additive ones too (a particle pass shares kin_fog.inc.glsl). The optical depth along a view ray from the eye to a
// point `s` away is
//     tau = density * (s - start) + height_density * e^(-height_falloff * (y0 - base_height)) * (1 - e^(-k dy s')) / (k
//     dy)
// (y0: the eye's height, dy: the ray's vertical component, s' = s - start: the height term is the integral of the
// density that falls off with height, exact), the opacity 1 - e^-tau capped at `max_opacity`. Custom material
// shaders (Material3D::shader) are not fogged: they run their own code.
struct Fog3D {
    bool enabled = false;
    f32 density = 0.0f;        // per world unit of distance (0.002: visibility of about 2 km)
    f32 height_density = 0.0f; // per unit of distance, at `base_height`; thinner above and thicker below
    f32 height_falloff = 0.1f; // per unit of height: how fast the height fog thins going up (1 / its scale height)
    f32 base_height = 0.0f;    // world y where `height_density` is measured
    f32 start = 0.0f;          // nothing nearer than this is fogged
    f32 max_opacity = 1.0f;    // the most any pixel is fogged (below 1, the far sky and ground show through)
    Vec3f colour{0.62f, 0.68f, 0.76f}; // linear
    // Take the colour from the sky in the view's direction (the horizon colour for rays that look down), so far
    // geometry melts into the sky; `colour` is then unused. Needs the procedural sky's settings (it uses them whether
    // or not Scene3D::sky is enabled). With SkyMode3D::Image `colour` is used all the same (the panorama is not read
    // for the fog): set it to the panorama's horizon.
    bool colour_from_sky = false;
    // The sun brightening the fog it shines through, toward it: its light (Lights3D::sun.color) times this times a
    // Henyey-Greenstein lobe of `sun_anisotropy` (0 even, near 1 a tight glare), added to the fog's colour.
    f32 sun_scatter = 0.0f;
    f32 sun_anisotropy = 0.6f;
};

namespace sky_model {
// The atmosphere's constants, shared with kin_sky.inc.glsl (change them together). Optical depths at the zenith at sea
// level of the Rayleigh scattering for red, green and blue light; the Mie one is `mie_per_turbidity` times the haze.
constexpr Vec3f rayleigh_zenith{0.0464f, 0.1080f, 0.2650f};
// The ozone layer's absorption (the green mostly), which keeps the zenith blue at twilight.
constexpr Vec3f ozone_zenith{0.0065f, 0.0188f, 0.00085f};
constexpr f32 mie_per_turbidity = 0.0105f;
constexpr f32 mie_extinction = 1.1f;      // extinction over scattering
constexpr f32 mie_anisotropy = 0.8f;      // the Henyey-Greenstein g of the haze
constexpr f32 height_ratio = 8.0f / 1.2f; // Rayleigh's scale height over Mie's
constexpr u32 steps = 8;
constexpr f32 top = 0.9994f;       // the share of the atmosphere's density below its top (60 km over 8)
constexpr f32 brightness = 2.0f;   // multiple scattering and the ground's bounce, which single scattering leaves out
constexpr f32 twilight = 0.07f;    // sin(elevation) over which the sun's scattered light dies under the horizon
constexpr f32 ground_band = 0.15f; // sin(depression) over which the sky's colour becomes the ground's
// How many times the optical depth the sun's light meets (a path through the atmosphere, in zenith paths) at
// sin(elevation) `s` (Kasten and Young's fit, clamped at the horizon).
f32 air_mass(f32 sin_elevation);
} // namespace sky_model

// ---- The sky, on the CPU (what kin_sky.inc.glsl computes) ----

// The sun's light after the atmosphere, per colour: sun_intensity * transmittance, going to 0 as the sun sinks past the
// horizon. What Lights3D::sun.color should be for this sun.
Vec3f sky_sun_light(const Sky3D& sky, Vec3f sun_direction);
// The fraction of the sun's light that gets through to the ground, per colour (no horizon cut-off).
Vec3f sky_sun_transmittance(const Sky3D& sky, Vec3f sun_direction);
// The atmosphere's light along `direction` (rays that look below the horizon get the horizon's): sun and moon
// scattering and the night's own light, times `intensity`. Linear; no sun disc, moon disc, stars or ground.
Vec3f sky_radiance(const Sky3D& sky, Vec3f sun_direction, Vec3f direction);
// What the sky pass shows along `direction` without the discs and stars: sky_radiance above the horizon, going over
// the horizon to the ground's light below it.
Vec3f sky_color(const Sky3D& sky, Vec3f sun_direction, Vec3f direction);
// The ground's colour as it is lit: ground_color times the horizontal light of the sun and the sky.
Vec3f sky_ground_light(const Sky3D& sky, Vec3f sun_direction);
// The light a diffuse surface facing `normal` takes from sky_color's whole sphere, over pi (what a white one shows
// with no sun and no other light): a cosine-weighted sum over `samples_u` by `samples_v` directions. What
// Lights3D::sky_lights_scene lights a white sphere with, to within the environment's filtering.
Vec3f sky_irradiance(const Sky3D& sky, Vec3f sun_direction, Vec3f normal, u32 samples_u = 64, u32 samples_v = 32);

namespace sky_model {
// The sky's light as the GPU's fog reads it (Fog3D::colour_from_sky): from a table made once for each sky and sun, not
// the integral at every pixel. A ray's scattering splits by its phase functions, the only part that depends on the
// angle mu to the light: a light's share along a ray at sin(elevation) s is rayleigh_phase(mu) R(s) + mie_phase(mu)
// M(s). The rows hold R and M at `rows` elevations spaced evenly in coordinate(s), the log of the air mass's rounded
// sin (crowded where the colour changes fastest, the last degrees over the horizon); read linearly between them, within
// a fraction of a level of sky_radiance (at and under the horizon, the first row: the same). The renderer keeps one;
// the tests read it.
struct FogTable3D {
    static constexpr u32 rows = 48; // = KIN_FOG_ROWS in kin_fog.inc.glsl: change both
    // Per row: the sun's R and M, then the moon's (rgb and a w of 0: the GPU's vec4s), times the sky's brightness.
    std::array<f32, rows * 16> values{};
    // What they were made for: the sun's sin(elevation) and light, the haze's optical depth, the sky's intensity, the
    // moon's light and sin(elevation).
    std::array<f32, 6> made_for{};
    bool made = false;

    // The row coordinate of a ray at sin(elevation) `s`: 0 at the horizon (and under it), rows - 1 at the zenith.
    static f32 coordinate(f32 s);
    // Makes the rows for this sky and sun unless they are made for them already (the sky is still most frames; the
    // sun's and the moon's azimuths do not change them): true if it did.
    bool update(const Sky3D& sky, Vec3f sun_direction);
    // The sky's light along `direction` read from the rows (made for `sky` and `sun_direction`): what the GPU's fog
    // takes for sky_radiance.
    Vec3f radiance(const Sky3D& sky, Vec3f sun_direction, Vec3f direction) const;
};
} // namespace sky_model

// ---- Fog, on the CPU ----

// The optical depth from `eye` along the unit `direction` for `distance` (infinity: to the end of the ray: the height
// fog's integral from `start` to the sky, no distance term).
f32 fog_optical_depth(const Fog3D& fog, Vec3f eye, Vec3f direction, f32 distance);
// 1 - e^-tau, at most max_opacity.
f32 fog_opacity(const Fog3D& fog, Vec3f eye, Vec3f direction, f32 distance);
// The fog's colour along `direction` (unit): the fixed one or the sky's, and the sun's in-scattering.
Vec3f fog_color(const Fog3D& fog, const Sky3D& sky, Vec3f sun_direction, LinearRgb sun_light, Vec3f direction);
// `color` (a surface's) as seen through the fog at `distance`.
Vec3f fog_apply(const Fog3D& fog, const Sky3D& sky, Vec3f sun_direction, LinearRgb sun_light, Vec3f eye,
                Vec3f direction, f32 distance, Vec3f color);

} // namespace kin
