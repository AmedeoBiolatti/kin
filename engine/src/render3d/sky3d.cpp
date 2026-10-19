#include <algorithm>
#include <cmath>
#include <kin/render3d/sky3d.hpp>
#include <kin/render3d/timeofday3d.hpp>
#include <limits>

// The sky and the fog on the CPU: the arithmetic of kin_sky.inc.glsl and kin_fog.inc.glsl, written again (the
// tests compare the GPU's pictures with it), then the time of day.

namespace kin {

namespace {

constexpr f32 pi = 3.14159265358979323846f;

f32 clamp01(f32 x) {
    return std::clamp(x, 0.0f, 1.0f);
}

f32 smoothstep(f32 a, f32 b, f32 x) {
    const f32 t = clamp01((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

Vec3f exp3(Vec3f v) {
    return {std::exp(v.x), std::exp(v.y), std::exp(v.z)};
}
Vec3f splat(f32 v) {
    return {v, v, v};
}

f32 mie_tau(const Sky3D& sky) {
    return sky_model::mie_per_turbidity * std::clamp(sky.turbidity, 1.0f, 20.0f);
}

f32 rayleigh_phase(f32 mu) {
    return 3.0f / (16.0f * pi) * (1.0f + mu * mu);
}

f32 mie_phase(f32 mu, f32 g) {
    const f32 g2 = g * g;
    return (1.0f - g2) / (4.0f * pi * std::pow(std::max(1.0f + g2 - 2.0f * g * mu, 1e-4f), 1.5f));
}

// kin_scatter: one light's scattering along the unit ray `v` (above the horizon), `l` toward the light, `e` its light.
Vec3f scatter(f32 tau_m, f32 g, Vec3f v, Vec3f l, f32 e) {
    using namespace sky_model;
    const f32 mv = air_mass(v.y);
    const f32 ms = air_mass(l.y);
    const f32 mu = dot(v, l);
    const f32 pr = rayleigh_phase(mu), pm = mie_phase(mu, g);
    Vec3f sum{};
    for (u32 i = 0; i < steps; ++i) {
        const f32 u = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(steps);
        const f32 dw = 2.0f * top * u / static_cast<f32>(steps);
        const f32 q = 1.0f - top * u * u;
        const f32 qm = std::pow(q, height_ratio);
        const Vec3f along = (rayleigh_zenith * (1.0f - q) + splat(mie_extinction * tau_m * (1.0f - qm))) * mv;
        const Vec3f toward = (rayleigh_zenith * q + splat(mie_extinction * tau_m * qm) + ozone_zenith) * ms;
        const Vec3f weight = rayleigh_zenith * pr + splat(tau_m * height_ratio * pm * qm / q);
        sum += weight * exp3(-(along + toward)) * dw;
    }
    return sum * (mv * pi * e);
}

Vec3f up_clamped(Vec3f v) {
    v.y = std::max(v.y, 0.0f);
    v.x += 1e-6f;
    return normalize(v);
}

} // namespace

namespace sky_model {
f32 air_mass(f32 sin_elevation) {
    const f32 s = std::max(sin_elevation, 0.0f);
    const f32 c =
        std::min(std::sqrt(s * s + 0.000625f), 1.0f); // rounded off over the last degree: the slope is 0 at the horizon
    const f32 zenith_degrees = std::acos(c) * 180.0f / pi;
    return 1.0f / (c + 0.50572f * std::pow(96.07995f - zenith_degrees, -1.6364f));
}
} // namespace sky_model

Vec3f sky_sun_transmittance(const Sky3D& sky, Vec3f sun_direction) {
    using namespace sky_model;
    const f32 m = air_mass(normalize(sun_direction).y);
    return exp3(-(rayleigh_zenith + splat(mie_extinction * mie_tau(sky)) + ozone_zenith) * m);
}

Vec3f sky_sun_light(const Sky3D& sky, Vec3f sun_direction) {
    const f32 cut = smoothstep(-0.01f, 0.04f, normalize(sun_direction).y);
    return sky_sun_transmittance(sky, sun_direction) * (sky.sun_intensity * cut);
}

Vec3f sky_radiance(const Sky3D& sky, Vec3f sun_direction, Vec3f direction) {
    using namespace sky_model;
    const Vec3f v = up_clamped(normalize(direction));
    const Vec3f sun = normalize(sun_direction);
    const f32 tau_m = mie_tau(sky);
    const f32 twilight_sun = sky.sun_intensity * std::exp(-std::max(-sun.y, 0.0f) / twilight);
    Vec3f light = scatter(tau_m, mie_anisotropy, v, sun, twilight_sun);
    if (sky.moon && sky.moon_light > 0.0f) {
        const Vec3f moon = normalize(sky.moon_direction);
        light +=
            scatter(tau_m, mie_anisotropy, v, moon, sky.moon_light * std::exp(-std::max(-moon.y, 0.0f) / twilight));
    }
    return light * (brightness * sky.intensity) + sky.night_color;
}

Vec3f sky_ground_light(const Sky3D& sky, Vec3f sun_direction) {
    const Vec3f sun = normalize(sun_direction);
    const Vec3f horizontal = sky_sun_light(sky, sun) * std::max(sun.y, 0.0f);
    return sky.ground_color * (horizontal + sky_radiance(sky, sun, {0.0f, 1.0f, 0.0f}));
}

Vec3f sky_color(const Sky3D& sky, Vec3f sun_direction, Vec3f direction) {
    const Vec3f v = normalize(direction);
    const Vec3f radiance = sky_radiance(sky, sun_direction, v);
    if (v.y >= 0.0f) {
        return radiance;
    }
    const f32 t = smoothstep(0.0f, sky_model::ground_band, -v.y);
    return radiance + (sky_ground_light(sky, sun_direction) - radiance) * t;
}

Vec3f sky_irradiance(const Sky3D& sky, Vec3f sun_direction, Vec3f normal, u32 samples_u, u32 samples_v) {
    // The cosine-weighted average of the sphere of directions about `normal`: stratified in (u, v) over the disc,
    // lifted to the hemisphere (Malley), so each sample has the same weight.
    const Vec3f n = normalize(normal);
    const Vec3f helper = std::abs(n.y) < 0.999f ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f};
    const Vec3f tx = normalize(cross(helper, n));
    const Vec3f ty = cross(n, tx);
    Vec3f sum{};
    for (u32 j = 0; j < samples_v; ++j) {
        for (u32 i = 0; i < samples_u; ++i) {
            const f32 r = std::sqrt((static_cast<f32>(j) + 0.5f) / static_cast<f32>(samples_v));
            const f32 phi = 2.0f * pi * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(samples_u);
            const Vec3f l =
                tx * (r * std::cos(phi)) + ty * (r * std::sin(phi)) + n * std::sqrt(std::max(1.0f - r * r, 0.0f));
            sum += sky_color(sky, sun_direction, l);
        }
    }
    return sum / static_cast<f32>(samples_u * samples_v);
}

namespace sky_model {

f32 FogTable3D::coordinate(f32 s) {
    // c is air_mass()'s: the rounding's 0.025 at the horizon, 1 at the zenith, its log over log 40 from 0 to 1.
    const f32 h = std::max(s, 0.0f);
    const f32 c = std::min(std::sqrt(h * h + 0.000625f), 1.0f);
    return std::clamp(std::log(c * 40.0f) * 0.2710850307f, 0.0f, 1.0f) * static_cast<f32>(rows - 1);
}

bool FogTable3D::update(const Sky3D& sky, Vec3f sun_direction) {
    // The sun and the moon as the GPU's atmosphere has them (fill_atmosphere): unit, up when there is no direction.
    const Vec3f sun = length_squared(sun_direction) > 0.0f ? normalize(sun_direction) : Vec3f{0.0f, 1.0f, 0.0f};
    const Vec3f moon =
        length_squared(sky.moon_direction) > 0.0f ? normalize(sky.moon_direction) : Vec3f{0.0f, 1.0f, 0.0f};
    const f32 moon_light = sky.moon ? std::max(sky.moon_light, 0.0f) : 0.0f;
    const f32 tau_m = mie_tau(sky);
    const std::array<f32, 6> key{sun.y, sky.sun_intensity, tau_m, sky.intensity, moon_light, moon.y};
    if (made && key == made_for) {
        return false;
    }
    made_for = key;
    made = true;
    values.fill(0.0f);
    // kin_scatter split by its phase functions: along a ray of air mass mv, a light's share is rayleigh_phase(mu) R +
    // mie_phase(mu) M, R and M the sums over the eight steps of the light that reaches each and gets to the eye. What
    // reaches a step from the light is the same for every row, what gets from it to the eye the same for both lights:
    // each worked out once.
    const f32 lights[2] = {sky.sun_intensity * std::exp(-std::max(-sun.y, 0.0f) / twilight),
                           moon_light * std::exp(-std::max(-moon.y, 0.0f) / twilight)};
    const f32 masses[2] = {air_mass(sun.y), air_mass(moon.y)};
    const u32 count = moon_light > 0.0f ? 2u : 1u;
    std::array<f32, steps> q, qm;
    std::array<Vec3f, steps> extinction;   // per unit of air mass from the eye to the step
    std::array<Vec3f, 2 * steps> arriving; // the light's share that reaches the step, times its width (dw)
    for (u32 i = 0; i < steps; ++i) {
        const f32 u = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(steps);
        const f32 dw = 2.0f * top * u / static_cast<f32>(steps);
        q[i] = 1.0f - top * u * u;
        qm[i] = std::pow(q[i], height_ratio);
        extinction[i] = rayleigh_zenith * (1.0f - q[i]) + splat(mie_extinction * tau_m * (1.0f - qm[i]));
        const Vec3f toward = rayleigh_zenith * q[i] + splat(mie_extinction * tau_m * qm[i]) + ozone_zenith;
        for (u32 l = 0; l < count; ++l) {
            arriving[l * steps + i] = exp3(-(toward * masses[l])) * dw;
        }
    }
    for (u32 row = 0; row < rows; ++row) {
        // The row's ray: c from 0.025 to 1 evenly in its log (coordinate()'s inverse), and the sin(elevation) of that
        // c.
        const f32 c = std::pow(0.025f, 1.0f - static_cast<f32>(row) / static_cast<f32>(rows - 1));
        const f32 mv = air_mass(std::sqrt(std::max(c * c - 0.000625f, 0.0f)));
        Vec3f r[2]{}, m[2]{};
        for (u32 i = 0; i < steps; ++i) {
            const Vec3f leaving = exp3(-(extinction[i] * mv));
            const f32 mie = tau_m * height_ratio * qm[i] / q[i];
            for (u32 l = 0; l < count; ++l) {
                const Vec3f seen = leaving * arriving[l * steps + i];
                r[l] += rayleigh_zenith * seen;
                m[l] += seen * mie;
            }
        }
        for (u32 l = 0; l < count; ++l) {
            const f32 scale = mv * pi * lights[l] * brightness * sky.intensity;
            const Vec3f rs = r[l] * scale, ms = m[l] * scale;
            const u32 at = row * 16 + l * 8;
            values[at] = rs.x;
            values[at + 1] = rs.y;
            values[at + 2] = rs.z;
            values[at + 4] = ms.x;
            values[at + 5] = ms.y;
            values[at + 6] = ms.z;
        }
    }
    return true;
}

Vec3f FogTable3D::radiance(const Sky3D& sky, Vec3f sun_direction, Vec3f direction) const {
    const Vec3f v = up_clamped(normalize(direction));
    const f32 x = coordinate(v.y);
    const u32 i = std::min(static_cast<u32>(x), rows - 2);
    const f32 w = x - static_cast<f32>(i);
    const auto at = [&](u32 k) {
        const u32 a = i * 16 + k, b = a + 16;
        return Vec3f{values[a] + (values[b] - values[a]) * w, values[a + 1] + (values[b + 1] - values[a + 1]) * w,
                     values[a + 2] + (values[b + 2] - values[a + 2]) * w};
    };
    const Vec3f sun = length_squared(sun_direction) > 0.0f ? normalize(sun_direction) : Vec3f{0.0f, 1.0f, 0.0f};
    const f32 mu = dot(v, sun);
    Vec3f light = at(0) * rayleigh_phase(mu) + at(4) * mie_phase(mu, mie_anisotropy);
    if (sky.moon && sky.moon_light > 0.0f) {
        const Vec3f moon =
            length_squared(sky.moon_direction) > 0.0f ? normalize(sky.moon_direction) : Vec3f{0.0f, 1.0f, 0.0f};
        const f32 mu_moon = dot(v, moon);
        light += at(8) * rayleigh_phase(mu_moon) + at(12) * mie_phase(mu_moon, mie_anisotropy);
    }
    return light + sky.night_color;
}

} // namespace sky_model

f32 fog_optical_depth(const Fog3D& fog, Vec3f eye, Vec3f direction, f32 distance) {
    const bool infinite = !(distance < std::numeric_limits<f32>::max());
    const f32 len = infinite ? std::numeric_limits<f32>::infinity() : std::max(distance - fog.start, 0.0f);
    f32 tau = infinite ? 0.0f : fog.density * len;
    if (fog.height_density > 0.0f && len > 0.0f) {
        const f32 k = fog.height_falloff;
        // From the start on, to infinity too: the sky's depth is the limit of the geometry's as `distance` grows.
        const f32 y0 = eye.y + direction.y * fog.start - fog.base_height;
        const f32 kd = k * direction.y;
        const f32 at_start = std::exp(std::clamp(-k * y0, -80.0f, 80.0f));
        f32 shape;
        if (infinite) {
            shape = kd > 1e-6f ? 1.0f / kd : 1e8f;
        } else {
            const f32 x = kd * len;
            shape =
                std::abs(x) < 1e-3f ? len * (1.0f - 0.5f * x) : (1.0f - std::exp(std::clamp(-x, -80.0f, 80.0f))) / kd;
        }
        tau += fog.height_density * at_start * shape;
    }
    return tau;
}

f32 fog_opacity(const Fog3D& fog, Vec3f eye, Vec3f direction, f32 distance) {
    return std::min(1.0f - std::exp(-fog_optical_depth(fog, eye, direction, distance)), fog.max_opacity);
}

Vec3f fog_color(const Fog3D& fog, const Sky3D& sky, Vec3f sun_direction, LinearRgb sun_light, Vec3f direction) {
    // An image sky's panorama is not read for the fog: the fixed colour stands in for it, as on the GPU.
    Vec3f c = fog.colour_from_sky && sky.mode == SkyMode3D::Procedural ? sky_radiance(sky, sun_direction, direction)
                                                                       : fog.colour;
    if (fog.sun_scatter > 0.0f) {
        const f32 g = fog.sun_anisotropy;
        const f32 mu = dot(normalize(direction), normalize(sun_direction));
        c += Vec3f{sun_light.r, sun_light.g, sun_light.b} *
             (fog.sun_scatter * (1.0f - g * g) / std::pow(std::max(1.0f + g * g - 2.0f * g * mu, 1e-4f), 1.5f));
    }
    return c;
}

Vec3f fog_apply(const Fog3D& fog, const Sky3D& sky, Vec3f sun_direction, LinearRgb sun_light, Vec3f eye,
                Vec3f direction, f32 distance, Vec3f color) {
    const f32 opacity = fog_opacity(fog, eye, direction, distance);
    if (opacity <= 0.0f) {
        return color;
    }
    const Vec3f fc = fog_color(fog, sky, sun_direction, sun_light, direction);
    return color + (fc - color) * opacity;
}

// ---- Time of day ----

namespace {

struct Spherical {
    f32 sin_elevation;
    f32 azimuth; // degrees from north through east
};

f32 declination_of(const TimeOfDay3D& t) {
    // Cooper's: zero at the equinoxes (day 81 and 264), the tilt at the June solstice (day 172).
    return t.axial_tilt * std::sin(2.0f * pi * (284.0f + t.day_of_year) / 365.0f);
}

// Direction of a body of declination `dec` (degrees) and hour angle `hour_angle` (degrees; 0 on the meridian,
// negative before it) seen from `latitude`: north is -z, east +x, up +y.
Vec3f body_direction(f32 latitude, f32 dec, f32 hour_angle, f32* azimuth_degrees, f32* elevation_degrees) {
    const f32 d2r = pi / 180.0f;
    const f32 phi = latitude * d2r, del = dec * d2r, h = hour_angle * d2r;
    const f32 sin_alt = std::sin(phi) * std::sin(del) + std::cos(phi) * std::cos(del) * std::cos(h);
    const f32 alt = std::asin(std::clamp(sin_alt, -1.0f, 1.0f));
    // East and north components of the direction (standard hour-angle to horizon conversion).
    const f32 east = -std::cos(del) * std::sin(h);
    const f32 north = std::sin(del) * std::cos(phi) - std::cos(del) * std::cos(h) * std::sin(phi);
    const f32 up = sin_alt;
    if (azimuth_degrees) {
        f32 a = std::atan2(east, north) / d2r;
        if (a < 0.0f) {
            a += 360.0f;
        }
        *azimuth_degrees = a;
    }
    if (elevation_degrees) {
        *elevation_degrees = alt / d2r;
    }
    return normalize(Vec3f{east, up, -north});
}

} // namespace

SkyState3D evaluate(const TimeOfDay3D& time, const Sky3D& base) {
    SkyState3D s;
    s.declination = declination_of(time);
    const f32 hour_angle = 15.0f * (time.hours - 12.0f);
    s.sun_direction = body_direction(time.latitude, s.declination, hour_angle, &s.sun_azimuth, &s.sun_elevation);
    // The moon: as far round the sky from the sun as its phase says (the hour angle shifts by a full turn a month), and
    // its declination swinging from the sun's (new) to the opposite (full), as the ecliptic does.
    const f32 phase = time.moon_phase - std::floor(time.moon_phase);
    const f32 moon_dec = s.declination * std::cos(2.0f * pi * phase);
    s.moon_direction = body_direction(time.latitude, moon_dec, hour_angle + 360.0f * phase, nullptr, &s.moon_elevation);
    s.moon_illumination = 0.5f * (1.0f - std::cos(2.0f * pi * phase));

    Sky3D sky = base;
    sky.moon = false;
    const Vec3f sun_color = sky_sun_light(sky, s.sun_direction);
    s.sun_color = {sun_color.x, sun_color.y, sun_color.z};
    // Moonlight: a full moon is a tenth of a noon sun's light by kin's artistic scale (the real ratio, 1 in 400000,
    // would be black), going out as the moon sets and with the phase.
    const f32 moon_up = smoothstep(-0.01f, 0.05f, s.moon_direction.y);
    constexpr f32 full_moon_light = 0.1f;
    s.moon_light = full_moon_light * s.moon_illumination * moon_up;
    s.moon_color = LinearRgb{0.78f, 0.84f, 1.0f} * s.moon_light;
    sky.moon = s.moon_light > 0.0f;
    sky.moon_direction = s.moon_direction;
    sky.moon_light = s.moon_light;
    const Vec3f ambient = sky_irradiance(sky, s.sun_direction, {0.0f, 1.0f, 0.0f});
    s.ambient = {ambient.x, ambient.y, ambient.z};
    const Vec3f ground_ambient = sky_irradiance(sky, s.sun_direction, {0.0f, -1.0f, 0.0f});
    s.ground_ambient = {ground_ambient.x, ground_ambient.y, ground_ambient.z};
    // The fog's colour: the sky at the horizon, away from the sun.
    Vec3f away{-s.sun_direction.x, 0.0f, -s.sun_direction.z};
    if (length_squared(away) < 1e-6f) {
        away = {0.0f, 0.0f, 1.0f};
    }
    s.fog_color = sky_radiance(sky, s.sun_direction, normalize(away));
    const f32 visibility = 24000.0f / std::clamp(base.turbidity, 1.0f, 20.0f);
    s.fog_density = 3.912f / visibility;
    s.daylight = smoothstep(-0.01f, 0.5f, s.sun_direction.y);
    return s;
}

SkyState3D apply(const TimeOfDay3D& time, Lights3D& lights, Sky3D& sky, Fog3D& fog) {
    const SkyState3D s = evaluate(time, sky);
    lights.sun.direction = s.sun_direction;
    lights.sun.color = s.sun_color;
    // The moon is the fill light; with TimeOfDay3D::shadows it casts a shadow of its own, faded in with its height and
    // light.
    const bool moon_up = s.moon_color.r + s.moon_color.g + s.moon_color.b > 0.0f;
    lights.fill.direction = s.moon_direction;
    lights.fill.color = s.moon_color;
    lights.ambient = s.ambient;
    lights.ground_ambient = s.ground_ambient;
    if (time.shadows) {
        const auto smooth = [](f32 a, f32 b, f32 x) {
            const f32 t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        lights.sun_shadow_strength = smooth(-2.0f, 1.0f, s.sun_elevation);
        const f32 strength = smooth(0.0f, 8.0f, s.moon_elevation) * smooth(0.004f, 0.02f, s.moon_light);
        lights.fill_shadow.strength = strength;
        lights.fill_shadow.enabled = strength > 0.0f;
    }
    sky.moon = moon_up;
    sky.moon_direction = s.moon_direction;
    sky.moon_light = s.moon_light;
    fog.colour = s.fog_color;
    return s;
}

SunTimes3D sun_times(const TimeOfDay3D& time) {
    SunTimes3D out;
    const f32 d2r = pi / 180.0f;
    const f32 dec = declination_of(time) * d2r;
    const f32 phi = time.latitude * d2r;
    const f32 cos_h = (std::sin(-0.833f * d2r) - std::sin(phi) * std::sin(dec)) / (std::cos(phi) * std::cos(dec));
    if (cos_h <= -1.0f) {
        out.polar_day = true;
        out.sunrise = 0.0f;
        out.sunset = 24.0f;
    } else if (cos_h >= 1.0f) {
        out.polar_night = true;
    } else {
        const f32 h = std::acos(cos_h) / d2r / 15.0f; // hours from noon
        out.sunrise = 12.0f - h;
        out.sunset = 12.0f + h;
    }
    return out;
}

void advance(TimeOfDay3D& time, f32 dt) {
    time.hours += dt * time.speed / 3600.0f;
    while (time.hours >= 24.0f) {
        time.hours -= 24.0f;
        time.day_of_year += 1.0f;
        if (time.day_of_year > 365.0f) {
            time.day_of_year -= 365.0f;
        }
    }
    while (time.hours < 0.0f) {
        time.hours += 24.0f;
        time.day_of_year -= 1.0f;
        if (time.day_of_year < 1.0f) {
            time.day_of_year += 365.0f;
        }
    }
}

} // namespace kin
