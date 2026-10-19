#include <kin/render3d/particles3d.hpp>

#include <kin/core/rng.hpp>
#include <kin/render3d/scene3d.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>

namespace kin {

namespace {

constexpr f64 two_pi = 6.283185307179586;
constexpr f32 pi = 3.14159265358979323846f;

// A hash of three integers to [0, 1): PCG3D (Jarzynski and Olano), on integers, so the same bits on every machine.
f32 hash01(u32 a, u32 b, u32 c) {
    u32 x = a * 1664525u + 1013904223u, y = b * 1664525u + 1013904223u, z = c * 1664525u + 1013904223u;
    x += y * z;
    y += z * x;
    z += x * y;
    x ^= x >> 16u;
    y ^= y >> 16u;
    z ^= z >> 16u;
    x += y * z;
    y += z * x;
    z += x * y;
    return static_cast<f32>(x >> 8u) * (1.0f / 16777216.0f);
}

// PCG3D on three integers, all three outputs (a weather particle needs ten numbers: three hashes make them).
struct Hash3 {
    u32 x, y, z;
    f32 fx() const { return static_cast<f32>(x >> 8u) * (1.0f / 16777216.0f); }
    f32 fy() const { return static_cast<f32>(y >> 8u) * (1.0f / 16777216.0f); }
    f32 fz() const { return static_cast<f32>(z >> 8u) * (1.0f / 16777216.0f); }
};

Hash3 hash3(u32 a, u32 b, u32 c) {
    u32 x = a * 1664525u + 1013904223u, y = b * 1664525u + 1013904223u, z = c * 1664525u + 1013904223u;
    x += y * z;
    y += z * x;
    z += x * y;
    x ^= x >> 16u;
    y ^= y >> 16u;
    z ^= z >> 16u;
    x += y * z;
    y += z * x;
    z += x * y;
    return {x, y, z};
}

f32 smooth(f32 x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// The wind's gusts: three sines, weights summing to 1 (so g is in [-1, 1]), their periods and lengths ratios of the
// settings'. The turbulence: two sines for each of three components. All in one table so that velocity and the closed-form
// displacement are the same sums.
struct GustTerm {
    f32 weight, period, length, phase;
};
constexpr GustTerm gust_terms[3] = {{0.6f, 1.0f, 1.0f, 0.0f}, {0.3f, 0.43f, 0.55f, 1.7f}, {0.1f, 0.19f, 0.27f, 4.1f}};
struct TurbulenceTerm {
    f32 frequency; // Hz
    Vec3f wave;    // in turns per `turbulence_length`
    f32 phase;
};
constexpr TurbulenceTerm turbulence_terms[3][2] = {
    {{0.31f, {0.9f, 0.3f, -0.4f}, 0.4f}, {0.83f, {-0.5f, 0.8f, 0.6f}, 2.2f}},
    {{0.47f, {0.2f, -0.9f, 0.5f}, 5.1f}, {0.67f, {0.7f, 0.4f, -0.8f}, 3.3f}},
    {{0.23f, {-0.6f, 0.5f, 0.7f}, 1.1f}, {1.09f, {0.4f, -0.7f, 0.2f}, 4.4f}},
};

Vec3f wind_unit(const Wind3D& w) {
    return length_squared(w.direction) > 0.0f ? normalize(w.direction) : Vec3f{};
}

} // namespace

// ---- Wind ----

namespace {
// The gust's factor along a unit direction `d` (the wind's, normalised once by the caller).
f32 gust_factor_along(const Wind3D& w, Vec3f d, Vec3f position, f64 time) {
    if (w.gust_strength == 0.0f || w.speed == 0.0f) {
        return 1.0f;
    }
    const f64 along = static_cast<f64>(dot(position, d));
    f64 g = 0.0;
    for (const GustTerm& term : gust_terms) {
        const f64 period = std::max(static_cast<f64>(w.gust_period) * term.period, 1e-6);
        const f64 length = std::max(static_cast<f64>(w.gust_length) * term.length, 1e-6);
        g += term.weight * std::sin(two_pi * (time / period - along / length) + term.phase);
    }
    return 1.0f + w.gust_strength * static_cast<f32>(g);
}
} // namespace

f32 Wind3D::gust_factor(Vec3f position, f64 time) const {
    return gust_factor_along(*this, wind_unit(*this), position, time);
}

Vec3f Wind3D::velocity(Vec3f position, f64 time) const {
    const Vec3f d = wind_unit(*this);
    Vec3f v = d * (speed * gust_factor_along(*this, d, position, time));
    if (turbulence != 0.0f) {
        const f64 length = std::max(static_cast<f64>(turbulence_length), 1e-6);
        f32 c[3];
        for (int k = 0; k < 3; ++k) {
            f64 sum = 0.0;
            for (const TurbulenceTerm& term : turbulence_terms[k]) {
                const f64 arg = two_pi * (static_cast<f64>(term.frequency) * time + static_cast<f64>(dot(position, term.wave)) / length) + term.phase;
                sum += std::sin(arg);
            }
            c[k] = static_cast<f32>(0.5 * sum) * turbulence;
        }
        v += Vec3f{c[0], c[1], c[2]};
    }
    return v;
}

Vec3f Wind3D::displacement(Vec3f position, f64 t0, f64 t1) const {
    const Vec3f d = wind_unit(*this);
    // Along the wind: the mean over the interval, and each gust sine's integral at this place.
    f64 along = static_cast<f64>(speed) * (t1 - t0);
    if (gust_strength != 0.0f && speed != 0.0f) {
        const f64 place = static_cast<f64>(dot(position, d));
        f64 g = 0.0;
        for (const GustTerm& term : gust_terms) {
            const f64 period = std::max(static_cast<f64>(gust_period) * term.period, 1e-6);
            const f64 length = std::max(static_cast<f64>(gust_length) * term.length, 1e-6);
            const f64 w = two_pi / period;
            const f64 theta = -two_pi * place / length + term.phase;
            g += term.weight * -(std::cos(w * t1 + theta) - std::cos(w * t0 + theta)) / w;
        }
        along += static_cast<f64>(speed) * static_cast<f64>(gust_strength) * g;
    }
    Vec3f out = d * static_cast<f32>(along);
    if (turbulence != 0.0f) {
        const f64 length = std::max(static_cast<f64>(turbulence_length), 1e-6);
        f32 c[3];
        for (int k = 0; k < 3; ++k) {
            f64 sum = 0.0;
            for (const TurbulenceTerm& term : turbulence_terms[k]) {
                const f64 w = two_pi * static_cast<f64>(term.frequency);
                const f64 theta = two_pi * static_cast<f64>(dot(position, term.wave)) / length + term.phase;
                sum += -(std::cos(w * t1 + theta) - std::cos(w * t0 + theta)) / w;
            }
            c[k] = static_cast<f32>(0.5 * sum * static_cast<f64>(turbulence));
        }
        out += Vec3f{c[0], c[1], c[2]};
    }
    return out;
}

// ---- Small shared functions ----

Vec3f particle_light_tint(const Lights3D& lights) {
    const Vec3f ground = lights.ground_ambient.value_or(lights.ambient);
    return (lights.ambient + ground) * 0.5f + lights.sun.color * 0.5f + lights.fill.color * 0.5f;
}

f32 soft_particle_fade(f32 gap, f32 distance) {
    if (distance <= 0.0f) {
        return 1.0f;
    }
    return smooth(gap / distance);
}

f32 particle_disc_alpha(f32 r, f32 softness, f32 ring) {
    // Inside the radius 1; across the outer `softness` of it, a smooth fall to 0 at the edge.
    const f32 s = std::clamp(softness, 0.0f, 1.0f);
    const f32 inner = 1.0f - s;
    f32 a = r <= inner ? 1.0f : 1.0f - smooth((r - inner) / std::max(s, 1e-4f));
    if (r > 1.0f) {
        a = 0.0f;
    }
    if (ring > 0.0f) {
        // A ring: nothing at the centre, up to full at 1 - ring of the radius, going out to the edge's fall.
        const f32 width = std::clamp(ring, 0.01f, 1.0f);
        a *= smooth((r - (1.0f - width)) / width);
    }
    return a;
}

Vec3f particle_streak(const ParticleSprite3D& sprite, const ParticleLook3D& look) {
    Vec3f s = sprite.velocity * look.stretch;
    const f32 len = length(s);
    if (len < look.stretch_min) {
        s = len > 1e-6f ? s * (look.stretch_min / len) : Vec3f{0.0f, look.stretch_min, 0.0f};
    }
    return s;
}

ParticleQuad3D particle_quad(const ParticleSprite3D& sprite, const ParticleLook3D& look, const Camera3D& camera) {
    static constexpr f32 cx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    static constexpr f32 cy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    ParticleQuad3D q;
    const f32 half = sprite.size * 0.5f;
    const Vec3f pos = sprite.position;
    const bool ortho = camera.projection == Projection3D::Orthographic;
    const Vec3f to_eye = ortho ? -camera.forward() : normalize(camera.position - pos);
    switch (look.facing) {
    case ParticleFacing3D::Camera: {
        const f32 c = std::cos(sprite.rotation), s = std::sin(sprite.rotation);
        const Vec3f right = camera.right() * c + camera.up() * s;
        const Vec3f up = camera.right() * -s + camera.up() * c;
        for (int k = 0; k < 4; ++k) {
            q.corner[k] = pos + right * (cx[k] * half) + up * (cy[k] * half);
        }
        break;
    }
    case ParticleFacing3D::Stretched: {
        const Vec3f streak = particle_streak(sprite, look);
        const f32 len = length(streak);
        const Vec3f dir = len > 1e-6f ? streak / len : Vec3f{0.0f, 1.0f, 0.0f};
        Vec3f side = cross(dir, to_eye);
        side = length_squared(side) > 1e-12f ? normalize(side) : camera.right();
        for (int k = 0; k < 4; ++k) {
            const f32 along = cy[k] * 0.5f + 0.5f; // 0: the tail, 1: the head
            q.corner[k] = pos - streak * (1.0f - along) + side * (cx[k] * half);
        }
        break;
    }
    case ParticleFacing3D::Axis: {
        const Vec3f up = length_squared(look.axis) > 0.0f ? normalize(look.axis) : Vec3f{0.0f, 1.0f, 0.0f};
        Vec3f right = cross(up, to_eye);
        right = length_squared(right) > 1e-12f ? normalize(right) : camera.right();
        for (int k = 0; k < 4; ++k) {
            q.corner[k] = pos + right * (cx[k] * half) + up * (cy[k] * half);
        }
        break;
    }
    case ParticleFacing3D::Flat: {
        const Vec3f n = length_squared(look.axis) > 0.0f ? normalize(look.axis) : Vec3f{0.0f, 1.0f, 0.0f};
        const Vec3f helper = std::abs(n.y) < 0.99f ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f};
        const Vec3f t = normalize(cross(n, helper));
        const Vec3f b = cross(n, t);
        const f32 c = std::cos(sprite.rotation), s = std::sin(sprite.rotation);
        const Vec3f right = t * c + b * s;
        const Vec3f up = t * -s + b * c;
        for (int k = 0; k < 4; ++k) {
            q.corner[k] = pos + right * (cx[k] * half) + up * (cy[k] * half);
        }
        break;
    }
    }
    return q;
}

// ---- The simulated system ----

namespace {

// What a random number is for.
enum Purpose : u32 {
    PLifetime = 1,
    PSpeed,
    PSize,
    PRotation,
    PSpin,
    PVelocityX,
    PVelocityY,
    PVelocityZ,
    PPosA,
    PPosB,
    PPosC,
    PDirA,
    PDirB,
    PFace,
    PRandom,
};

constexpr f32 lerp_f(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

// A direction in the cone of half-angle `degrees` round +Y, uniform over its solid angle.
Vec3f cone_direction(f32 degrees, f32 u, f32 v) {
    const f32 cos_max = std::cos(std::clamp(degrees, 0.0f, 180.0f) * pi / 180.0f);
    const f32 cos_t = 1.0f - u * (1.0f - cos_max);
    const f32 sin_t = std::sqrt(std::max(0.0f, 1.0f - cos_t * cos_t));
    const f32 phi = 2.0f * pi * v;
    return {sin_t * std::cos(phi), cos_t, sin_t * std::sin(phi)};
}

// ---- The wind at many particles ----
//
// Wind3D::velocity is nine sines in f64 for a place and a time, and a system of 20,000 particles takes it for every particle at
// every 1/120 s step. WindStep is the same field with all that does not depend on the place done once a step (the unit direction,
// the clock's share of each sine's phase), and the sines, in f32 turns, run 32 places at a time in loops the compiler makes
// vector code of. The polynomial is arithmetic only, so a sine is the same bits on every machine (libm's are not); it is within
// 1e-6 of the exact one, and the field within 1e-5 of Wind3D::velocity's near the origin (the sines' arguments are f32 turn counts, so the
// error grows with the distance from it: about 1e-3 of the turbulence at 10 km; tests/render3d_cpu_tests.cpp asserts 2e-3 over a 36 m box).

// sin(2 pi t) for t in turns: reduced to [-1/2, 1/2] by adding and taking away 1.5 * 2^23 (which rounds to a whole number), folded
// to [-1/4, 1/4] (sin(pi - x) = sin x), and an odd polynomial of degree 9 fitted to the sine there (error 3e-9: below f32's own).
inline f32 sin_turns(f32 t) {
    constexpr f32 magic = 12582912.0f;
    const f32 r = t - ((t + magic) - magic);
    const f32 a = std::fabs(r);
    const f32 u = std::copysign(std::min(a, 0.5f - a), r); // (no branches: a loop of these is vector code)
    const f32 u2 = u * u;
    return u * (6.283185160f + u2 * (-41.34165503f + u2 * (81.60100407f + u2 * (-76.54978238f + u2 * 39.53670662f))));
}

constexpr std::size_t wind_lanes = 32;
struct WindLanes { f32 x[wind_lanes], y[wind_lanes], z[wind_lanes]; }; // places in, the air's velocity there out

struct WindStep {
    Vec3f d{};                  // the wind's unit direction
    f32 speed = 0.0f, gust_strength = 0.0f, turbulence = 0.0f, influence = 1.0f;
    bool gusts = false, swirls = false;
    f32 gust_offset[3]{}, gust_slope[3]{};  // a gust sine's turns: offset + slope * (place along d)
    f32 swirl_offset[3][2]{};               // a turbulence sine's turns: offset + dot(place, swirl_wave)
    Vec3f swirl_wave[3][2]{};

    WindStep(const Wind3D& w, f64 time, f32 influence_) : d(wind_unit(w)), speed(w.speed), gust_strength(w.gust_strength),
                                                          turbulence(w.turbulence), influence(influence_) {
        const auto fraction = [](f64 x) { return static_cast<f32>(x - std::floor(x)); };
        gusts = w.gust_strength != 0.0f && w.speed != 0.0f;
        for (int k = 0; k < 3; ++k) {
            const f64 period = std::max(static_cast<f64>(w.gust_period) * gust_terms[k].period, 1e-6);
            const f64 length = std::max(static_cast<f64>(w.gust_length) * gust_terms[k].length, 1e-6);
            gust_offset[k] = fraction(time / period + static_cast<f64>(gust_terms[k].phase) / two_pi);
            gust_slope[k] = static_cast<f32>(-1.0 / length);
        }
        swirls = w.turbulence != 0.0f;
        const f32 inv_length = static_cast<f32>(1.0 / std::max(static_cast<f64>(w.turbulence_length), 1e-6));
        for (int k = 0; k < 3; ++k) {
            for (int j = 0; j < 2; ++j) {
                const TurbulenceTerm& term = turbulence_terms[k][j];
                swirl_offset[k][j] = fraction(static_cast<f64>(term.frequency) * time + static_cast<f64>(term.phase) / two_pi);
                swirl_wave[k][j] = term.wave * inv_length;
            }
        }
    }

    // The air's velocity (times the influence) at the `wind_lanes` places in `at`, put in their place. (One struct of arrays, and
    // locals for the rest, so that the compiler can see that no two of them alias.)
    void sample(WindLanes& at) const {
        const f32 dx = d.x, dy = d.y, dz = d.z, base_speed = speed, scale = influence; // (the stores to `at` could alias members)
        f32 factor[wind_lanes];
        if (gusts) {
            for (std::size_t i = 0; i < wind_lanes; ++i) {
                const f32 along = at.x[i] * dx + at.y[i] * dy + at.z[i] * dz;
                const f32 g = gust_terms[0].weight * sin_turns(gust_offset[0] + gust_slope[0] * along) +
                              gust_terms[1].weight * sin_turns(gust_offset[1] + gust_slope[1] * along) +
                              gust_terms[2].weight * sin_turns(gust_offset[2] + gust_slope[2] * along);
                factor[i] = 1.0f + gust_strength * g;
            }
        } else {
            for (std::size_t i = 0; i < wind_lanes; ++i) factor[i] = 1.0f;
        }
        f32 swirl[3][wind_lanes];
        for (int k = 0; k < 3; ++k) {
            const Vec3f w0 = swirl_wave[k][0], w1 = swirl_wave[k][1];
            const f32 o0 = swirl_offset[k][0], o1 = swirl_offset[k][1];
            if (swirls) {
                for (std::size_t i = 0; i < wind_lanes; ++i)
                    swirl[k][i] = 0.5f * (sin_turns(o0 + (at.x[i] * w0.x + at.y[i] * w0.y + at.z[i] * w0.z)) +
                                          sin_turns(o1 + (at.x[i] * w1.x + at.y[i] * w1.y + at.z[i] * w1.z))) * turbulence;
            } else {
                for (std::size_t i = 0; i < wind_lanes; ++i) swirl[k][i] = 0.0f;
            }
        }
        for (std::size_t i = 0; i < wind_lanes; ++i) {
            const f32 along = base_speed * factor[i];
            at.x[i] = (dx * along + swirl[0][i]) * scale;
            at.y[i] = (dy * along + swirl[1][i]) * scale;
            at.z[i] = (dz * along + swirl[2][i]) * scale;
        }
    }
};

// What a step needs of the desc (copied, so that the compiler knows that writing a particle changes none of it), and of the drag's
// relaxation that is the same for every particle: it depends on the drag and the step alone.
struct Forces {
    bool drag_on, ground;
    Vec3f gravity;
    f32 ground_height, bounce;
    f32 e = 0.0f, gain = 0.0f; // exp(-k h) and (1 - e) / k
    Vec3f gravity_over_k{};
    Forces(const ParticleDesc3D& d, f32 h) : drag_on(d.drag > 0.0f), ground(d.ground), gravity(d.gravity), ground_height(d.ground_height), bounce(d.bounce) {
        if (drag_on) {
            e = std::exp(-d.drag * h);
            gain = (1.0f - e) / d.drag;
            gravity_over_k = d.gravity / d.drag;
        }
    }
};

// Every particle of `particles` one step of `dt` seconds on, in the wind (null: none) as it blows at `time`. The air acts only through
// the drag, so without drag (or influence) it is not looked for; else the wind is found 32 places at a time.
void advance_all(std::span<Particle3D> particles, const ParticleDesc3D& desc, f64 time, f64 dt, const Wind3D* wind) {
    const f32 h = static_cast<f32>(dt);
    const Forces f(desc, h);
    const bool windy = wind && f.drag_on && desc.wind_influence != 0.0f;
    std::optional<WindStep> air;
    if (windy) air.emplace(*wind, time, desc.wind_influence);
    WindLanes lanes;
    for (std::size_t base = 0; base < particles.size(); base += wind_lanes) {
        const std::size_t n = std::min(wind_lanes, particles.size() - base);
        Particle3D* block = particles.data() + base;
        if (windy) {
            for (std::size_t i = 0; i < wind_lanes; ++i) { // the last block is filled up with its first place
                const Vec3f at = block[i < n ? i : 0].position;
                lanes.x[i] = at.x; lanes.y[i] = at.y; lanes.z[i] = at.z;
            }
            air->sample(lanes);
        }
        for (std::size_t i = 0; i < n; ++i) {
            Particle3D& p = block[i];
            if (f.drag_on) {
                // Velocity relaxes toward the terminal one (the air's, with gravity's pull balanced by the drag): exact for a step.
                const Vec3f wind_here = windy ? Vec3f{lanes.x[i], lanes.y[i], lanes.z[i]} : Vec3f{};
                const Vec3f terminal = wind_here + f.gravity_over_k;
                p.position += terminal * h + (p.velocity - terminal) * f.gain;
                p.velocity = terminal + (p.velocity - terminal) * f.e;
            } else {
                p.velocity += f.gravity * h;
                p.position += p.velocity * h;
            }
            if (f.ground && p.position.y < f.ground_height) {
                if (f.bounce > 0.0f && p.velocity.y < 0.0f) {
                    p.position.y = f.ground_height + (f.ground_height - p.position.y) * f.bounce;
                    p.velocity.y = -p.velocity.y * f.bounce;
                } else {
                    p.age = p.lifetime + 1.0f; // gone
                }
            }
            p.rotation += p.spin * h;
            p.age += h;
        }
    }
}

constexpr u64 fnv_offset = 1469598103934665603ull;
constexpr u64 fnv_prime = 1099511628211ull;

struct Hasher {
    u64 h = fnv_offset;
    void bytes(const void* data, std::size_t n) {
        const u8* p = static_cast<const u8*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ p[i]) * fnv_prime;
        }
    }
    template <typename T>
    void value(T v) {
        if constexpr (std::is_floating_point_v<T>) {
            if (v != v) {
                v = std::numeric_limits<T>::quiet_NaN();
            }
            if (v == T(0)) {
                v = T(0); // -0 is +0
            }
        }
        bytes(&v, sizeof(T));
    }
};

constexpr u32 save_magic = 0x4433504bu; // "KP3D"
constexpr u32 save_version = 1;

} // namespace

f32 ParticleSystem3D::random(u32 index, u32 purpose) const {
    // The seed's two halves and the number and purpose go through PCG3D: no stream to keep in step.
    return hash01(static_cast<u32>(_seed) ^ (static_cast<u32>(_seed >> 32) * 2654435761u), index, purpose);
}

void ParticleSystem3D::clear() {
    _particles.clear();
    _next_index = 0;
    _carry = 0.0;
    _accumulated = 0.0;
    _time = 0.0;
    _dropped = 0;
}

f32 ParticleSystem3D::size_over_life(const ParticleDesc3D& d, f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t < 0.5f ? lerp_f(d.size_start, d.size_mid, t * 2.0f) : lerp_f(d.size_mid, d.size_end, (t - 0.5f) * 2.0f);
}

f32 ParticleSystem3D::alpha_over_life(const ParticleDesc3D& d, f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    f32 a = lerp_f(d.alpha_start, d.alpha_end, t);
    if (d.fade_in > 0.0f) {
        a *= std::clamp(t / d.fade_in, 0.0f, 1.0f);
    }
    return a;
}

void ParticleSystem3D::advance(Particle3D& p, f64 dt, const Wind3D* wind) const {
    // A birth part-way through a step: one particle in the same code that steps the rest, so that it is carried as they are.
    advance_all({&p, 1}, desc, _time, dt, wind);
}

void ParticleSystem3D::spawn(u32 count, bool at_point, Vec3f point, const Wind3D* wind, f64 sub_step) {
    for (u32 n = 0; n < count; ++n) {
        if (_particles.size() >= desc.max_particles) {
            // Full: the births still to come are dropped (counted, and their numbers used) without being made one by one.
            _dropped += count - n;
            _next_index += count - n;
            return;
        }
        const u32 i = _next_index++;
        Particle3D p;
        p.index = i;
        p.random = random(i, PRandom);
        p.lifetime = std::max(lerp_f(desc.lifetime.min, desc.lifetime.max, random(i, PLifetime)), 1e-3f);
        p.size = lerp_f(desc.size.min, desc.size.max, random(i, PSize));
        p.rotation = lerp_f(desc.rotation.min, desc.rotation.max, random(i, PRotation)) * pi / 180.0f;
        p.spin = lerp_f(desc.spin.min, desc.spin.max, random(i, PSpin)) * pi / 180.0f;
        const f32 speed = lerp_f(desc.speed.min, desc.speed.max, random(i, PSpeed));
        const f32 a = random(i, PPosA), b = random(i, PPosB), c = random(i, PPosC);
        const f32 da = random(i, PDirA), db = random(i, PDirB);
        Vec3f local{};
        Vec3f dir{0.0f, 1.0f, 0.0f};
        switch (desc.shape) {
        case ParticleShape3D::Point:
            dir = cone_direction(desc.spread, da, db);
            break;
        case ParticleShape3D::Box: {
            const Vec3f e = desc.extent;
            if (desc.surface_only) {
                // A face, by area, then a point on it.
                const f32 ax = e.y * e.z, ay = e.x * e.z, az = e.x * e.y;
                const f32 total = std::max(ax + ay + az, 1e-12f);
                const f32 pick = random(i, PFace) * total;
                const f32 sign = a < 0.5f ? -1.0f : 1.0f;
                const f32 u = b * 2.0f - 1.0f, v = c * 2.0f - 1.0f;
                if (pick < ax) {
                    local = {sign * e.x, u * e.y, v * e.z};
                } else if (pick < ax + ay) {
                    local = {u * e.x, sign * e.y, v * e.z};
                } else {
                    local = {u * e.x, v * e.y, sign * e.z};
                }
            } else {
                local = {(a * 2.0f - 1.0f) * e.x, (b * 2.0f - 1.0f) * e.y, (c * 2.0f - 1.0f) * e.z};
            }
            dir = cone_direction(desc.spread, da, db);
            break;
        }
        case ParticleShape3D::Sphere: {
            const f32 z = 2.0f * a - 1.0f;
            const f32 phi = 2.0f * pi * b;
            const f32 rxy = std::sqrt(std::max(0.0f, 1.0f - z * z));
            dir = {rxy * std::cos(phi), z, rxy * std::sin(phi)};
            const f32 radius = desc.extent.x * (desc.surface_only ? 1.0f : std::cbrt(c));
            local = dir * radius;
            break;
        }
        case ParticleShape3D::Cone:
        case ParticleShape3D::Disc: {
            const f32 r = desc.extent.x * std::sqrt(a);
            const f32 phi = 2.0f * pi * b;
            local = {r * std::cos(phi), 0.0f, r * std::sin(phi)};
            dir = cone_direction(desc.shape == ParticleShape3D::Cone ? desc.cone_angle : desc.spread, da, db);
            break;
        }
        }
        const Vec3f extra{lerp_f(desc.velocity_min.x, desc.velocity_max.x, random(i, PVelocityX)),
                          lerp_f(desc.velocity_min.y, desc.velocity_max.y, random(i, PVelocityY)),
                          lerp_f(desc.velocity_min.z, desc.velocity_max.z, random(i, PVelocityZ))};
        p.position = at_point ? point + transform.apply_vector(local) : transform.apply(local);
        p.velocity = transform.apply_vector(dir * speed + extra);
        if (sub_step > 0.0) {
            // Born part-way through the step: carried on the rest of it, so that a stream is smooth and not clumped.
            const f64 u = (static_cast<f64>(n) + 0.5) / static_cast<f64>(count);
            advance(p, step * (1.0 - u), wind);
            if (p.age >= p.lifetime) {
                continue;
            }
        }
        _particles.push_back(p);
    }
}

void ParticleSystem3D::burst(u32 count) {
    spawn(count, false, {}, nullptr, 0.0);
}

void ParticleSystem3D::burst_at(Vec3f position, u32 count) {
    spawn(count, true, position, nullptr, 0.0);
}

void ParticleSystem3D::update(f64 dt, const Wind3D* wind) {
    if (dt > 0.0) {
        _accumulated += dt;
    }
    // A hair under a step counts as one: the frame lengths 1/60 and 1/144 add up to whole steps only to rounding.
    while (_accumulated >= step - 1e-9) {
        _accumulated -= step;
        advance_all(_particles, desc, _time, step, wind);
        std::erase_if(_particles, [](const Particle3D& p) { return p.age >= p.lifetime; });
        if (emitting && desc.rate > 0.0f && rate_scale > 0.0f) {
            _carry += static_cast<f64>(desc.rate) * static_cast<f64>(rate_scale) * step;
            const f64 whole = std::floor(_carry);
            _carry -= whole;
            if (whole >= 1.0) {
                spawn(static_cast<u32>(whole), false, {}, wind, step);
            }
        }
        _time += step;
    }
}

void ParticleSystem3D::sprites(std::vector<ParticleSprite3D>& out) const {
    const f32 acc = static_cast<f32>(std::max(_accumulated, 0.0));
    const u32 frames = desc.look.atlas_frames > 0 ? desc.look.atlas_frames : std::max(1u, desc.look.atlas_columns * desc.look.atlas_rows);
    out.reserve(out.size() + _particles.size());
    for (const Particle3D& p : _particles) {
        const f32 t = std::clamp(p.age / p.lifetime, 0.0f, 1.0f);
        ParticleSprite3D s;
        s.position = p.position + p.velocity * acc;
        s.size = p.size * size_over_life(desc, t);
        s.rotation = p.rotation + p.spin * acc;
        const f32 shade = 1.0f - desc.colour_variation * p.random;
        s.colour = lerp(desc.colour_start, desc.colour_end, t) * shade;
        s.alpha = alpha_over_life(desc, t);
        s.velocity = p.velocity;
        if (desc.frames_per_life > 0.0f && frames > 1) {
            f32 f = t * desc.frames_per_life * static_cast<f32>(frames) + (desc.frame_random ? p.random * static_cast<f32>(frames) : 0.0f);
            f = std::fmod(f, static_cast<f32>(frames));
            s.frame = f;
        } else if (desc.frame_random && frames > 1) {
            s.frame = std::floor(p.random * static_cast<f32>(frames));
        }
        out.push_back(s);
    }
}

u64 ParticleSystem3D::state_hash() const {
    Hasher h;
    h.value(_seed);
    h.value(_next_index);
    h.value(_dropped);
    h.value(_carry);
    // The time given and not yet stepped, to a nanosecond: the same total time cut into frames differently leaves a few ulps.
    h.value(static_cast<i64>(std::llround(_accumulated * 1e9)));
    h.value(static_cast<i64>(std::llround(_time * 1e9)));
    h.value(static_cast<u64>(_particles.size()));
    for (const Particle3D& p : _particles) {
        for (const f32 v : {p.position.x, p.position.y, p.position.z, p.velocity.x, p.velocity.y, p.velocity.z, p.age, p.lifetime, p.size,
                            p.rotation, p.spin, p.random}) {
            h.value(v);
        }
        h.value(p.index);
    }
    return h.h;
}

static_assert(sizeof(Particle3D) == 52, "save() writes the particles as they lie");

std::vector<u8> ParticleSystem3D::save() const {
    std::vector<u8> out;
    const auto put = [&](const void* data, std::size_t n) {
        const u8* p = static_cast<const u8*>(data);
        out.insert(out.end(), p, p + n);
    };
    const u32 magic = save_magic, version = save_version, count = static_cast<u32>(_particles.size());
    put(&magic, 4);
    put(&version, 4);
    put(&_seed, sizeof(_seed));
    put(&_next_index, 4);
    put(&_dropped, 4);
    put(&_carry, 8);
    put(&_accumulated, 8);
    put(&_time, 8);
    put(&count, 4);
    if (count > 0) {
        put(_particles.data(), static_cast<std::size_t>(count) * sizeof(Particle3D));
    }
    return out;
}

bool ParticleSystem3D::restore(std::span<const u8> bytes) {
    constexpr std::size_t header = 4 + 4 + 8 + 4 + 4 + 8 + 8 + 8 + 4;
    if (bytes.size() < header) {
        return false;
    }
    std::size_t at = 0;
    const auto take = [&](void* out, std::size_t n) {
        std::memcpy(out, bytes.data() + at, n);
        at += n;
    };
    u32 magic = 0, version = 0, next = 0, dropped = 0, count = 0;
    u64 seed = 0;
    f64 carry = 0, accumulated = 0, time = 0;
    take(&magic, 4);
    take(&version, 4);
    take(&seed, 8);
    take(&next, 4);
    take(&dropped, 4);
    take(&carry, 8);
    take(&accumulated, 8);
    take(&time, 8);
    take(&count, 4);
    if (magic != save_magic || version != save_version || bytes.size() != header + static_cast<std::size_t>(count) * sizeof(Particle3D)) {
        return false;
    }
    _seed = seed;
    _next_index = next;
    _dropped = dropped;
    _carry = carry;
    _accumulated = accumulated;
    _time = time;
    _particles.resize(count);
    if (count > 0) {
        std::memcpy(_particles.data(), bytes.data() + at, static_cast<std::size_t>(count) * sizeof(Particle3D));
    }
    return true;
}

// ---- Weather ----

WeatherLayer3D weather_rain_layer() {
    WeatherLayer3D l;
    l.max_particles = 16000;
    l.box = {36.0f, 24.0f, 36.0f};
    l.above = 0.75f;
    l.fall_speed = 9.0f;
    l.fall_variation = 0.2f;
    l.wind_influence = 0.9f;
    l.size_min = 0.012f;
    l.size_max = 0.02f;
    l.colour = {0.7f, 0.76f, 0.85f};
    l.alpha = 0.4f;
    l.splash = 0.7f;
    l.splash_size = 0.3f;
    l.splash_time = 0.25f;
    l.look.facing = ParticleFacing3D::Stretched;
    l.look.stretch = 0.05f;
    l.look.stretch_min = 0.15f;
    l.look.lit = 0.35f;
    l.look.sort = false;
    return l;
}

WeatherLayer3D weather_snow_layer() {
    WeatherLayer3D l;
    l.max_particles = 9000;
    l.box = {30.0f, 20.0f, 30.0f};
    l.above = 0.7f;
    l.fall_speed = 1.2f;
    l.fall_variation = 0.35f;
    l.wind_influence = 0.85f;
    l.flutter = 0.35f;
    l.flutter_rate = 0.45f;
    l.size_min = 0.035f;
    l.size_max = 0.08f;
    l.colour = {0.95f, 0.97f, 1.0f};
    l.alpha = 0.85f;
    l.look.facing = ParticleFacing3D::Camera;
    l.look.softness = 0.8f;
    l.look.lit = 1.0f;
    l.look.sort = false;
    return l;
}

WeatherLayer3D weather_dust_layer() {
    WeatherLayer3D l;
    l.max_particles = 2500;
    l.box = {20.0f, 10.0f, 20.0f};
    l.above = 0.5f;
    l.fall_speed = 0.04f;
    l.fall_variation = 0.8f;
    l.wind_influence = 0.5f;
    l.flutter = 0.6f;
    l.flutter_rate = 0.12f;
    l.size_min = 0.015f;
    l.size_max = 0.04f;
    l.colour = {1.0f, 0.93f, 0.8f};
    l.alpha = 0.3f;
    l.look.lit = 1.0f;
    l.look.sort = false;
    return l;
}

WeatherLayer3D weather_mist_layer() {
    WeatherLayer3D l;
    l.max_particles = 160;
    l.box = {90.0f, 10.0f, 90.0f};
    l.above = 0.35f;
    l.fall_speed = 0.0f;
    l.fall_variation = 0.0f;
    l.wind_influence = 0.4f;
    l.flutter = 4.0f;
    l.flutter_rate = 0.03f;
    l.size_min = 7.0f;
    l.size_max = 14.0f;
    l.edge_fade = 0.3f;
    l.colour = {0.82f, 0.86f, 0.9f};
    l.alpha = 0.08f;
    l.look.soft = true;
    l.look.soft_distance = 3.0f;
    l.look.lit = 1.0f;
    l.look.sort = true;
    return l;
}

f32 Weather3D::wetness() const {
    return std::clamp(std::max({rain.intensity, 0.6f * snow.intensity, 0.5f * mist.intensity}), 0.0f, 1.0f);
}

f32 Weather3D::sun_disc_scale() const {
    return 1.0f - effects.sun_disc_dim * wetness();
}

void Weather3D::apply(Lights3D& lights, Fog3D& fog, const Fog3D& base_fog) const {
    const f32 w = wetness();
    if (w > 0.0f) {
        const f32 sun = 1.0f - effects.sun_dim * w;
        const f32 amb = 1.0f - effects.ambient_dim * w;
        lights.sun.color = lights.sun.color * sun;
        lights.fill.color = lights.fill.color * sun;
        lights.ambient = lights.ambient * amb;
        if (lights.ground_ambient) {
            lights.ground_ambient = *lights.ground_ambient * amb;
        }
    }
    fog = base_fog;
    if (w > 0.0f) {
        fog.enabled = true;
        fog.density += effects.fog_density * w;
        fog.height_density += effects.fog_height_density * w;
        fog.sun_scatter *= 1.0f - effects.sun_scatter_dim * w;
        const f32 grey = (fog.colour.x + fog.colour.y + fog.colour.z) / 3.0f;
        fog.colour = lerp(fog.colour, Vec3f{grey, grey, grey}, effects.fog_grey * w);
    }
}

namespace {

f64 positive_mod(f64 x, f64 m) {
    return x - m * std::floor(x / m);
}

// What every particle of the weather shares at one clock: the wind's drift since time 0 and its velocity, at the world's origin
// (the gusts' and the turbulence's variation with place is left out of the drift: the volume is small beside a gust).
struct WeatherWind {
    Vec3f drift;
    Vec3f velocity;
};

WeatherWind weather_wind(const Weather3D& w) {
    return {w.wind.displacement({}, 0.0, w.time), w.wind.velocity({}, w.time)};
}

// A layer's volume for a camera at `eye`: its low corner, and its size.
struct WeatherBox {
    f64 lo_x, lo_y, lo_z, w, h, d;
};

WeatherBox weather_box(const WeatherLayer3D& layer, Vec3f eye) {
    const f64 w = static_cast<f64>(layer.box.x), h = static_cast<f64>(layer.box.y), d = static_cast<f64>(layer.box.z);
    return {static_cast<f64>(eye.x) - 0.5 * w, static_cast<f64>(eye.y) - (1.0 - static_cast<f64>(layer.above)) * h, static_cast<f64>(eye.z) - 0.5 * d,
            w, h, d};
}

// One particle's drop, and its splash, from its number and the clock. Both outputs are optional; each returns whether it is shown.
struct WeatherResult {
    bool drop = false;
    bool splash = false;
};

WeatherResult weather_one(const Weather3D& weather, const WeatherLayer3D& layer, u32 kind, u32 i, Vec3f eye, const WeatherBox& box,
                          const WeatherWind& wind, ParticleSprite3D* drop, ParticleSprite3D* splash, Vec3f* unwrapped,
                          const SphereTest* in_view = nullptr) {
    WeatherResult result;
    const f32 wanted = static_cast<f32>(layer.max_particles) * std::clamp(layer.intensity, 0.0f, 1.0f);
    const f32 share = std::clamp(wanted - static_cast<f32>(i), 0.0f, 1.0f);
    if (share <= 0.0f) {
        return result;
    }
    const u32 salt = weather.seed * 8u + kind;
    const Hash3 h1 = hash3(salt, i, 1), h2 = hash3(salt, i, 2);
    const f32 u = h1.fx(), v = h1.fy(), q = h1.fz();
    const f32 s = h2.fx(), k = h2.fy(), size_h = h2.fz();
    const f64 t = weather.time;
    const f32 fall = layer.fall_speed * (1.0f + layer.fall_variation * (2.0f * s - 1.0f));
    const f32 influence = layer.wind_influence * (0.8f + 0.2f * k);
    f64 x = static_cast<f64>(u) * box.w + static_cast<f64>(wind.drift.x * influence);
    f64 y = static_cast<f64>(v) * box.h + static_cast<f64>(wind.drift.y * influence) - static_cast<f64>(fall) * t;
    f64 z = static_cast<f64>(q) * box.d + static_cast<f64>(wind.drift.z * influence);
    const Hash3 h3 = hash3(salt, i, 3);
    if (layer.flutter != 0.0f) {
        const f32 p1 = h3.fx(), p2 = h3.fy();
        const f64 rate = static_cast<f64>(layer.flutter_rate) * (0.7 + 0.6 * static_cast<f64>(k));
        // The phase's whole turns are taken out in double (an hour in, a float would have no fraction left), the sine in float.
        const f64 a = rate * t + static_cast<f64>(p1), b = rate * 0.83 * t + static_cast<f64>(p2);
        x += static_cast<f64>(layer.flutter * std::sin(6.2831853f * static_cast<f32>(a - std::floor(a))));
        z += static_cast<f64>(layer.flutter * std::sin(6.2831853f * static_cast<f32>(b - std::floor(b))));
    }
    if (unwrapped) {
        *unwrapped = {static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)};
    }
    const Vec3f velocity = wind.velocity * influence + Vec3f{0.0f, -fall, 0.0f};
    const f32 width = lerp_f(layer.size_min, layer.size_max, size_h);
    const f64 wy = box.lo_y + positive_mod(y - box.lo_y, box.h);
    const f32 yf = static_cast<f32>(wy);
    const f32 edge = std::max(layer.edge_fade, 1e-3f);
    const f32 up = static_cast<f32>((wy - box.lo_y) / box.h);
    const f32 near_start = 0.4f, near_range = 1.6f; // nothing at the lens, whole from two metres: a flake at the lens is a blob across the picture
    if (weather.has_ground && yf < weather.ground_height) {
        // Under the ground: hidden; and for the splash, how long it has been since it met it.
        if (splash && layer.splash > 0.0f && fall > 1e-3f && h3.fz() < layer.splash) {
            const f32 age = (weather.ground_height - yf) / fall;
            if (age < layer.splash_time) {
                const f64 sx = x - static_cast<f64>(velocity.x) * static_cast<f64>(age);
                const f64 sz = z - static_cast<f64>(velocity.z) * static_cast<f64>(age);
                const f64 wx = box.lo_x + positive_mod(sx - box.lo_x, box.w);
                const f64 wz = box.lo_z + positive_mod(sz - box.lo_z, box.d);
                const f32 ex = 1.0f - std::abs(2.0f * static_cast<f32>(wx - static_cast<f64>(eye.x)) / layer.box.x);
                const f32 ez = 1.0f - std::abs(2.0f * static_cast<f32>(wz - static_cast<f64>(eye.z)) / layer.box.z);
                const Vec3f place{static_cast<f32>(wx), weather.ground_height + 0.004f, static_cast<f32>(wz)};
                const f32 life = age / layer.splash_time;
                const f32 near = smooth((length(place - eye) - near_start) / near_range);
                splash->position = place;
                splash->size = layer.splash_size * (0.25f + 0.75f * life);
                splash->rotation = 0.0f;
                splash->colour = layer.colour * 1.25f; // a ring catches the light: brighter than the drop
                splash->alpha = std::min(1.0f, layer.alpha * 2.5f) * (1.0f - life) * (1.0f - life) * smooth(ex / edge) * smooth(ez / edge) * near;
                splash->velocity = {};
                splash->frame = 0.0f;
                result.splash = splash->alpha > 0.0f;
            }
        }
        return result;
    }
    if (!drop) {
        return result;
    }
    const f64 wx = box.lo_x + positive_mod(x - box.lo_x, box.w);
    const f64 wz = box.lo_z + positive_mod(z - box.lo_z, box.d);
    if (in_view && *in_view) {
        // Out of view: not made. The reach is the quad's (its width, and a streak's length too).
        f32 reach = width * 0.75f;
        if (layer.look.facing == ParticleFacing3D::Stretched) {
            reach += std::max(length(velocity) * layer.look.stretch, layer.look.stretch_min);
        }
        if (!(*in_view)({static_cast<f32>(wx), yf, static_cast<f32>(wz)}, reach)) {
            return result;
        }
    }
    // Fading across the volume's sides and top (and its bottom, unless the ground is what ends it), so that nothing pops where
    // a particle wraps.
    const f32 ex = 1.0f - std::abs(2.0f * static_cast<f32>(wx - static_cast<f64>(eye.x)) / layer.box.x);
    const f32 ez = 1.0f - std::abs(2.0f * static_cast<f32>(wz - static_cast<f64>(eye.z)) / layer.box.z);
    f32 fade = smooth(ex / edge) * smooth(ez / edge) * smooth((1.0f - up) / edge);
    if (!(weather.has_ground && weather.ground_height >= static_cast<f32>(box.lo_y))) {
        fade *= smooth(up / edge);
    }
    const Vec3f place{static_cast<f32>(wx), yf, static_cast<f32>(wz)};
    // Nothing right at the lens.
    const f32 near = smooth((length(place - eye) - near_start) / near_range);
    drop->position = place;
    drop->size = width;
    drop->rotation = static_cast<f32>((h1.x ^ h3.y) >> 8u) * (6.2831853f / 16777216.0f);
    drop->colour = layer.colour;
    drop->alpha = layer.alpha * fade * near * share;
    drop->velocity = velocity;
    drop->frame = 0.0f;
    result.drop = drop->alpha > 0.0f;
    return result;
}

} // namespace

bool weather_particle(const Weather3D& weather, const WeatherLayer3D& layer, u32 kind, u32 i, Vec3f eye, ParticleSprite3D& out, Vec3f* unwrapped) {
    return weather_one(weather, layer, kind, i, eye, weather_box(layer, eye), weather_wind(weather), &out, nullptr, unwrapped).drop;
}

bool weather_splash(const Weather3D& weather, const WeatherLayer3D& layer, u32 kind, u32 i, Vec3f eye, ParticleSprite3D& out) {
    return weather_one(weather, layer, kind, i, eye, weather_box(layer, eye), weather_wind(weather), nullptr, &out, nullptr).splash;
}

void weather_sprites(const Weather3D& weather, const Camera3D& camera, std::vector<ParticleSprite3D>& sprites,
                     std::vector<WeatherBatch3D>& batches, const ParallelFor& parallel, const SphereTest& in_view) {
    const std::array<const WeatherLayer3D*, 4> layers{&weather.rain, &weather.snow, &weather.dust, &weather.mist};
    const WeatherWind wind = weather_wind(weather);
    constexpr u32 chunk = 2048;
    struct Piece {
        std::vector<ParticleSprite3D> drops, splashes;
    };
    std::vector<Piece> pieces;
    for (u32 kind = 0; kind < 4; ++kind) {
        const WeatherLayer3D& layer = *layers[kind];
        if (layer.intensity <= 0.0f || layer.max_particles == 0) {
            continue;
        }
        constexpr u32 most = 1u << 20; // a layer is at most a million particles (a bad number must not take a minute to make)
        const u32 count = std::min({static_cast<u32>(std::ceil(static_cast<f32>(layer.max_particles) * std::clamp(layer.intensity, 0.0f, 1.0f))),
                                    layer.max_particles, most});
        const WeatherBox box = weather_box(layer, camera.position);
        const bool want_splash = layer.splash > 0.0f && weather.has_ground;
        const u32 n = (count + chunk - 1) / chunk;
        pieces.resize(std::max<std::size_t>(pieces.size(), n));
        const auto make = [&](i32 k) {
            Piece& piece = pieces[static_cast<std::size_t>(k)];
            piece.drops.clear();
            piece.splashes.clear();
            ParticleSprite3D drop, splash;
            const u32 end = std::min(count, (static_cast<u32>(k) + 1) * chunk);
            for (u32 i = static_cast<u32>(k) * chunk; i < end; ++i) {
                const WeatherResult r = weather_one(weather, layer, kind, i, camera.position, box, wind, &drop, want_splash ? &splash : nullptr, nullptr,
                                                    in_view ? &in_view : nullptr);
                if (r.drop) {
                    piece.drops.push_back(drop);
                }
                if (r.splash) {
                    piece.splashes.push_back(splash);
                }
            }
        };
        if (parallel && n > 1) {
            parallel(static_cast<i32>(n), make);
        } else {
            for (u32 k = 0; k < n; ++k) {
                make(static_cast<i32>(k));
            }
        }
        WeatherBatch3D batch;
        batch.look = layer.look;
        batch.kind = static_cast<WeatherBatch3D::Kind>(kind);
        batch.first = static_cast<u32>(sprites.size());
        for (u32 k = 0; k < n; ++k) {
            sprites.insert(sprites.end(), pieces[k].drops.begin(), pieces[k].drops.end());
        }
        batch.count = static_cast<u32>(sprites.size()) - batch.first;
        if (batch.count > 0) {
            batches.push_back(batch);
        }
        WeatherBatch3D b;
        b.look.facing = ParticleFacing3D::Flat;
        b.look.axis = {0.0f, 1.0f, 0.0f};
        b.look.softness = 0.6f;
        b.look.ring = 0.45f;
        b.look.lit = 0.0f; // its own colour: a splash on a dark ground is a bright ring
        b.look.sort = false;
        b.kind = WeatherBatch3D::Kind::Splash;
        b.first = static_cast<u32>(sprites.size());
        for (u32 k = 0; k < n; ++k) {
            sprites.insert(sprites.end(), pieces[k].splashes.begin(), pieces[k].splashes.end());
        }
        b.count = static_cast<u32>(sprites.size()) - b.first;
        if (b.count > 0) {
            batches.push_back(b);
        }
    }
}

} // namespace kin
