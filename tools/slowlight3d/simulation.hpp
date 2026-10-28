#pragma once
// Example-owned classical transport model. Nothing in this file is an engine API.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace slowlight {
inline constexpr double step = 1.0 / 30.0;
inline constexpr std::size_t capacity = 121;         // four seconds, including both endpoints
inline constexpr double default_speed = 100.0 / 3.6; // metres / second
struct Point {
    double x = 0, y = 0, z = 0;
    Point operator+(Point b) const { return {x + b.x, y + b.y, z + b.z}; }
    Point operator-(Point b) const { return {x - b.x, y - b.y, z - b.z}; }
    Point operator*(double s) const { return {x * s, y * s, z * s}; }
};
inline double dot(Point a, Point b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline double length(Point a) {
    return std::sqrt(dot(a, a));
}
// Matches the compute/material shaders' std430 layout. Positions interpolate;
// power and radius/lifetime are held from the left sample until the next tick.
struct alignas(16) Sample {
    std::array<float, 4> light{};
    std::array<float, 4> occluder{};
};
static_assert(sizeof(Sample) == 32);
inline Point position(const std::array<float, 4>& p) {
    return {p[0], p[1], p[2]};
}
inline Sample demo(double t) {
    const auto tick = std::llround(t / step);
    return {{float(3 * std::sin(t * .6)), 4, float(2 * std::cos(t * .6)), tick % 90 < 24 ? 65.f : 0.f},
            {float(4 * std::sin(t * .9)), 2, float(2 * std::sin(t * .5)), tick % 180 < 150 ? 1.f : 0.f}};
}
class History {
public:
    std::array<Sample, capacity> samples{};
    std::size_t count = 0;
    std::int64_t tick = -1;
    double time() const { return double(tick) * step; }
    double oldest() const { return count ? -double(count - 1) * step : 0; } // relative to the newest tick
    template <class Generator> void seek(double seconds, Generator generate) {
        if (!std::isfinite(seconds) || seconds < 0 || seconds > 1e9)
            throw std::runtime_error("prototype time must be finite and in [0, 1e9] seconds");
        const auto next = std::int64_t(std::floor(seconds / step + 1e-8));
        if (next == tick)
            return;
        // Reconstruct at most capacity samples on a jump/rewind. No unbounded catch-up.
        if (next < tick || tick < 0 || next - tick >= std::int64_t(capacity)) {
            count = std::size_t(std::min<std::int64_t>(capacity, next + 1));
            for (std::size_t i = 0; i < count; ++i)
                samples[i] = generate(double(next - std::int64_t(count - 1 - i)) * step);
        } else {
            for (auto n = tick + 1; n <= next; ++n) {
                if (count == capacity)
                    std::move(samples.begin() + 1, samples.end(), samples.begin());
                else
                    ++count;
                samples[count - 1] = generate(double(n) * step);
            }
        }
        tick = next;
    }
    std::size_t interval(double relative) const {
        return std::min(count - 2, std::size_t(std::max(0.0, std::floor((relative - oldest()) / step))));
    }
    Sample at(double relative) const {
        if (count == 1)
            return samples[0];
        const auto i = interval(relative);
        const double f = std::clamp((relative - oldest()) / step - double(i), 0.0, 1.0);
        Sample value = samples[i];
        for (int c = 0; c < 3; ++c) {
            value.light[c] += float(f * (samples[i + 1].light[c] - value.light[c]));
            value.occluder[c] += float(f * (samples[i + 1].occluder[c] - value.occluder[c]));
        }
        if (relative >= 0) {
            value.light[3] = samples[count - 1].light[3];
            value.occluder[3] = samples[count - 1].occluder[3];
        }
        return value;
    }
    bool subluminal(double speed) const {
        if (!std::isfinite(speed) || speed <= 0 || count == 0)
            return false;
        for (std::size_t i = 1; i < count; ++i)
            if (length(position(samples[i].light) - position(samples[i - 1].light)) >= speed * step)
                return false;
        return true;
    }
};
inline double segment_distance_squared(Point a, Point velocity, double duration) {
    const double v2 = dot(velocity, velocity);
    const double t = v2 > 1e-20 ? std::clamp(-dot(a, velocity) / v2, 0.0, duration) : 0;
    const auto nearest = a + velocity * t;
    return dot(nearest, nearest);
}
struct Transport {
    double energy = 0, delay = 0;
    bool blocked = false, known = false;
};
// Static receiver; opaque sphere blocks only the incoming light leg. The eye leg
// contributes delay but is unoccluded. No scattering, bounces, Doppler or relativity.
inline Transport evaluate(const History& h, Point receiver, Point eye, double speed, bool instantaneous = false) {
    if (!h.count || !std::isfinite(speed) || speed <= 0)
        return {};
    double surface_time = instantaneous ? 0 : -length(receiver - eye) / speed;
    double emission_time = 0;
    Sample emitter = h.samples[h.count - 1];
    if (!instantaneous) {
        if (h.count < 2 || surface_time < h.oldest() ||
            h.oldest() + length(position(h.samples[0].light) - receiver) / speed > surface_time)
            return {}; // history does not contain the emission event: explicitly unknown
        double lo = h.oldest(), hi = surface_time;
        for (int n = 0; n < 22; ++n) {
            const double t = (lo + hi) * .5;
            if (t + length(position(h.at(t).light) - receiver) / speed < surface_time)
                lo = t;
            else
                hi = t;
        }
        emission_time = (lo + hi) * .5;
        emitter = h.at(emission_time);
    }
    const Point source = position(emitter.light), ray = receiver - source;
    const double distance = length(ray);
    bool blocked = false;
    if (instantaneous) {
        const auto sphere = h.samples[h.count - 1].occluder;
        blocked = sphere[3] > 0 &&
                  segment_distance_squared(source - position(sphere), ray, 1) < double(sphere[3]) * sphere[3];
    } else if (surface_time - emission_time > 1e-8) {
        const auto first = h.interval(emission_time), last = h.interval(surface_time);
        const auto photon_velocity = ray * (1 / (surface_time - emission_time));
        for (auto i = first; i <= last && !blocked; ++i) {
            const double begin = std::max(emission_time, h.oldest() + double(i) * step);
            const double end = std::min(surface_time, h.oldest() + double(i + 1) * step);
            const double radius = h.samples[i].occluder[3];
            if (end <= begin || radius <= 0)
                continue;
            const Point sphere_velocity =
                (position(h.samples[i + 1].occluder) - position(h.samples[i].occluder)) * (1 / step);
            const Point photon = source + photon_velocity * (begin - emission_time);
            const Point sphere = position(h.at(begin).occluder);
            blocked = segment_distance_squared(photon - sphere, photon_velocity - sphere_velocity, end - begin) <
                      radius * radius;
        }
    }
    const double energy =
        !blocked ? emitter.light[3] * std::max(0.0, -ray.y) / std::max(distance, .001) / (1 + distance * distance) : 0;
    return {energy, -emission_time, blocked, true};
}
} // namespace slowlight
