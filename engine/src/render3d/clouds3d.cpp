#include <algorithm>
#include <cmath>
#include <kin/render3d/sky3d.hpp>

// The clouds' noise and their layer on the CPU: the arithmetic of cloud_noise.comp.glsl and kin_clouds.inc.glsl's
// kin_cloud_span, written again (the tests compare the GPU's texels and pictures with it). Change them together.

namespace kin::cloud_model {

namespace {

struct U3 {
    u32 x, y, z;
};

// PCG3D (Jarzynski and Olano): integer bits, the same on every GPU and here.
U3 pcg3d(U3 v) {
    v.x = v.x * 1664525u + 1013904223u;
    v.y = v.y * 1664525u + 1013904223u;
    v.z = v.z * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x ^= v.x >> 16u;
    v.y ^= v.y >> 16u;
    v.z ^= v.z >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// The hash of a lattice cell wrapped to `period` cells (the noise tiles), for one `salt` (one noise of many).
U3 cell_hash(i32 x, i32 y, i32 z, i32 period, u32 salt) {
    const auto wrap = [period](i32 c) { return static_cast<u32>(((c % period) + period) % period); };
    return pcg3d({wrap(x) + salt * 0x9E3779B9u, wrap(y) + salt * 0x85EBCA6Bu, wrap(z) + salt * 0xC2B2AE35u});
}

f32 unit(u32 h) {
    return static_cast<f32>(h >> 8u) * (1.0f / 16777216.0f);
}

// Worley: 1 less the distance to the nearest of one point a cell, `cells` cells across the tile (p in 0 to 1).
f32 worley(f32 px, f32 py, f32 pz, i32 cells, u32 salt) {
    const f32 qx = px * static_cast<f32>(cells), qy = py * static_cast<f32>(cells), qz = pz * static_cast<f32>(cells);
    const f32 cx = std::floor(qx), cy = std::floor(qy), cz = std::floor(qz);
    const f32 fx = qx - cx, fy = qy - cy, fz = qz - cz;
    f32 nearest = 1e9f;
    for (i32 oz = -1; oz <= 1; ++oz) {
        for (i32 oy = -1; oy <= 1; ++oy) {
            for (i32 ox = -1; ox <= 1; ++ox) {
                const U3 h = cell_hash(static_cast<i32>(cx) + ox, static_cast<i32>(cy) + oy, static_cast<i32>(cz) + oz,
                                       cells, salt);
                const f32 dx = static_cast<f32>(ox) + unit(h.x) - fx;
                const f32 dy = static_cast<f32>(oy) + unit(h.y) - fy;
                const f32 dz = static_cast<f32>(oz) + unit(h.z) - fz;
                nearest = std::min(nearest, dx * dx + dy * dy + dz * dz);
            }
        }
    }
    return 1.0f - std::min(std::sqrt(nearest), 1.0f);
}

// Three octaves of Worley from `cells` across.
f32 worley_fbm(f32 px, f32 py, f32 pz, i32 cells, u32 salt) {
    return 0.625f * worley(px, py, pz, cells, salt) + 0.25f * worley(px, py, pz, cells * 2, salt + 1u) +
           0.125f * worley(px, py, pz, cells * 4, salt + 2u);
}

f32 gradient_dot(U3 h, f32 x, f32 y, f32 z) {
    // One of the twelve edge directions of a cube (Perlin's improved noise).
    switch (h.x % 12u) {
    case 0:
        return x + y;
    case 1:
        return -x + y;
    case 2:
        return x - y;
    case 3:
        return -x - y;
    case 4:
        return x + z;
    case 5:
        return -x + z;
    case 6:
        return x - z;
    case 7:
        return -x - z;
    case 8:
        return y + z;
    case 9:
        return -y + z;
    case 10:
        return y - z;
    default:
        return -y - z;
    }
}

f32 fade(f32 t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

f32 lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

// Perlin's gradient noise, about -1 to 1, tiling at `cells` cells.
f32 perlin(f32 px, f32 py, f32 pz, i32 cells, u32 salt) {
    const f32 qx = px * static_cast<f32>(cells), qy = py * static_cast<f32>(cells), qz = pz * static_cast<f32>(cells);
    const f32 cx = std::floor(qx), cy = std::floor(qy), cz = std::floor(qz);
    const f32 fx = qx - cx, fy = qy - cy, fz = qz - cz;
    const i32 ix = static_cast<i32>(cx), iy = static_cast<i32>(cy), iz = static_cast<i32>(cz);
    f32 corner[8];
    for (i32 k = 0; k < 8; ++k) {
        const i32 ox = k & 1, oy = (k >> 1) & 1, oz = k >> 2;
        corner[k] = gradient_dot(cell_hash(ix + ox, iy + oy, iz + oz, cells, salt), fx - static_cast<f32>(ox),
                                 fy - static_cast<f32>(oy), fz - static_cast<f32>(oz));
    }
    const f32 ux = fade(fx), uy = fade(fy), uz = fade(fz);
    const f32 x00 = lerp(corner[0], corner[1], ux), x10 = lerp(corner[2], corner[3], ux);
    const f32 x01 = lerp(corner[4], corner[5], ux), x11 = lerp(corner[6], corner[7], ux);
    return lerp(lerp(x00, x10, uy), lerp(x01, x11, uy), uz);
}

// `octaves` of Perlin from `cells` across, each half the last, in 0 to 1 (spread over most of it).
f32 perlin_fbm(f32 px, f32 py, f32 pz, i32 cells, i32 octaves, u32 salt) {
    f32 sum = 0.0f, amplitude = 1.0f, total = 0.0f;
    for (i32 k = 0; k < octaves; ++k) {
        sum += amplitude * perlin(px, py, pz, cells << k, salt + static_cast<u32>(k));
        total += amplitude;
        amplitude *= 0.5f;
    }
    return std::clamp(0.5f + 1.1f * sum / total, 0.0f, 1.0f);
}

} // namespace

std::array<f32, 4> noise_texel(Noise noise, u32 x, u32 y, u32 z) {
    const u32 size = noise == Noise::Shape ? shape_size : noise == Noise::Detail ? detail_size : weather_size;
    const f32 inv = 1.0f / static_cast<f32>(size);
    const f32 px = (static_cast<f32>(x) + 0.5f) * inv, py = (static_cast<f32>(y) + 0.5f) * inv;
    switch (noise) {
    case Noise::Shape: {
        const f32 pz = (static_cast<f32>(z) + 0.5f) * inv;
        const f32 low = worley_fbm(px, py, pz, 4, 0u);
        // Perlin-Worley (Schneider's, as Hillaire builds it): the Perlin noise raised to the Worley's floor, billowy.
        const f32 p = perlin_fbm(px, py, pz, 4, 3, 10u);
        return {low + p * (1.0f - low), low, worley_fbm(px, py, pz, 8, 20u), worley_fbm(px, py, pz, 16, 30u)};
    }
    case Noise::Detail: {
        const f32 pz = (static_cast<f32>(z) + 0.5f) * inv;
        return {worley_fbm(px, py, pz, 2, 40u), worley_fbm(px, py, pz, 4, 50u), worley_fbm(px, py, pz, 8, 60u), 1.0f};
    }
    case Noise::Weather:
    default: {
        // A plane through the middle of a tiling volume (off its lattice, where gradient noise is zero).
        const f32 pz = 0.37f;
        const f32 coverage = std::clamp(0.5f + 1.6f * (perlin_fbm(px, py, pz, 3, 5, 70u) - 0.5f), 0.0f, 1.0f);
        const f32 kind = std::clamp(0.5f + 1.6f * (perlin_fbm(px, py, pz, 2, 3, 80u) - 0.5f), 0.0f, 1.0f);
        return {coverage, kind, 0.0f, 1.0f};
    }
    }
}

std::array<f32, 2> layer_span(const Clouds3D& clouds, Vec3f eye, Vec3f direction) {
    // The height over the curved ground along the ray: h(t) = y0 + t dy + a t^2, a = (1 - dy^2) / 2R (the ray's
    // horizontal distance squared over twice the radius: the ground's drop there).
    const f32 dy = direction.y;
    const f32 a = std::max((1.0f - dy * dy) / (2.0f * std::max(clouds.planet_radius, 1.0f)), 1e-14f);
    const f32 y0 = eye.y;
    // The roots of h(t) = level, in order (none: false), in the stable form.
    const auto roots = [&](f32 level, f32& r1, f32& r2) {
        const f32 c = y0 - level;
        const f32 disc = dy * dy - 4.0f * a * c;
        if (disc < 0.0f)
            return false;
        const f32 q = -0.5f * (dy + std::copysign(std::sqrt(disc), dy));
        r1 = q / a;
        r2 = q != 0.0f ? c / q : r1;
        if (r1 > r2)
            std::swap(r1, r2);
        return true;
    };
    const f32 none_hi = -1.0f;
    const std::array<f32, 2> none{1.0f, none_hi};
    const f32 bottom = std::min(clouds.bottom, clouds.top), top = std::max(clouds.bottom, clouds.top);
    // Under the layer, nothing below the level: the sky has the (flat) ground there.
    if (y0 < bottom && dy <= 0.0f)
        return none;
    f32 s1 = 0, s2 = 0;
    if (!roots(top, s1, s2))
        return none; // never under the top: wholly above the layer
    f32 end = s2;
    // The ground, where the ray meets it first (an eye under it sees no clouds: none of its rays leave the ground).
    if (y0 < 0.0f)
        return none;
    f32 g1 = 0, g2 = 0;
    if (roots(0.0f, g1, g2) && g1 > 0.0f)
        end = std::min(end, g1);
    end = std::min(end, std::max(clouds.distance, 0.0f));
    f32 start = std::max(s1, 0.0f);
    f32 r1 = 0, r2 = 0;
    if (roots(bottom, r1, r2) && r2 > start) {
        // Under the base between r1 and r2: the part of [start, end] before r1, else after r2.
        if (r1 > start)
            end = std::min(end, r1);
        else
            start = r2;
    }
    if (!(start < end))
        return none;
    return {start, end};
}

f32 cover_share(const Clouds3D& clouds, Vec3f eye, f32 time, f32 radius) {
    if (!clouds.enabled || clouds.coverage <= 0.0f) {
        return 0.0f;
    }
    // The map's texel under each point of a 7 x 7 grid over the disc (the corners left out), as the shader reads it at
    // (p.xz + offset) / weather_scale, the offset being the wind's drift.
    const f32 inv = 1.0f / std::max(clouds.weather_scale, 1.0f);
    const f32 ox = -clouds.wind.x * time, oz = -clouds.wind.z * time;
    const f32 coverage = std::clamp(clouds.coverage, 0.0f, 1.0f);
    f32 sum = 0.0f;
    u32 count = 0;
    for (i32 j = -3; j <= 3; ++j) {
        for (i32 i = -3; i <= 3; ++i) {
            if (i * i + j * j > 10) {
                continue;
            }
            const f32 x = eye.x + radius * static_cast<f32>(i) / 6.0f + ox;
            const f32 z = eye.z + radius * static_cast<f32>(j) / 6.0f + oz;
            const auto wrap = [](f32 v) {
                const f32 t = v - std::floor(v);
                return std::min(static_cast<u32>(t * static_cast<f32>(weather_size)), weather_size - 1);
            };
            const auto w = noise_texel(Noise::Weather, wrap(x * inv), wrap(z * inv), 0);
            sum += std::clamp(w[0] + 2.0f * coverage - 1.0f, 0.0f, 1.0f);
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<f32>(count) : 0.0f;
}

} // namespace kin::cloud_model
