#pragma once
#include <kin/core/math3d.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace kin::gpu::point_tiles {
constexpr u32 tile_size = 64, max_tiles = 1024;
struct Masks {
    u32 columns = 0, rows = 0;
    std::array<u32, max_tiles> bits{};
    bool reset(u32 width, u32 height) {
        columns = width / tile_size + (width % tile_size != 0);
        rows = height / tile_size + (height % tile_size != 0);
        bits.fill(0);
        return columns && rows && columns <= max_tiles && rows <= max_tiles / columns;
    }
    // Each raster tile defines four homogeneous side planes. A contributing
    // sphere must reach all four. These are necessary conditions, also for
    // orthographic views and spheres crossing the eye plane; no depth is used.
    // Taking the rectangle of accepted rows/columns can only retain extra tiles.
    void add(const Mat4f& vp, Vec3f center, f32 radius, u32 light, u32 width, u32 height) {
        const u32 bit = 1u << light;
        const auto all = [&] { for (u32 i = 0; i < columns * rows; ++i) bits[i] |= bit; };
        if (!std::isfinite(radius) || radius < 0 || !std::isfinite(center.x) ||
            !std::isfinite(center.y) || !std::isfinite(center.z)) { all(); return; }
        for (float value : vp.m) if (!std::isfinite(value)) { all(); return; }
        // Inflate the sphere and plane tests for float shader/interpolation error.
        const double scale = std::max({1.0, std::abs(double(center.x)), std::abs(double(center.y)),
                                       std::abs(double(center.z)), double(radius)});
        const double r = double(radius) + scale * 1e-5;
        const auto reaches = [&](u32 axis, double ndc, bool positive) {
            double plane[4]{};
            for (u32 k = 0; k < 4; ++k) plane[k] = double(vp.m[4 * k + axis]) - ndc * double(vp.m[4 * k + 3]);
            const double distance = plane[0] * center.x + plane[1] * center.y + plane[2] * center.z + plane[3];
            const double reach = r * std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
            // Use uncombined matrix terms too: cancellation in a side plane
            // must not hide the rounding error of the GPU's separate clip x/w.
            const double at[]{double(center.x), double(center.y), double(center.z), 1};
            double magnitude = reach + 1;
            for (u32 k = 0; k < 4; ++k) magnitude +=
                (std::abs(double(vp.m[4 * k + axis])) + std::abs(ndc * double(vp.m[4 * k + 3]))) * std::abs(at[k]);
            const double error = magnitude * 1e-5;
            return (positive ? distance : -distance) + reach >= -error;
        };
        u32 first[2]{columns, rows}, last[2]{};
        const u32 sizes[]{width, height}, counts[]{columns, rows};
        for (u32 axis = 0; axis < 2; ++axis) {
            for (u32 tile = 0; tile < counts[axis]; ++tile) {
                // Two pixels of overlap protect raster and tile boundaries.
                const double lo = (double(tile) * tile_size - 2) / sizes[axis];
                const double hi = (std::min(double(tile + 1) * tile_size, double(sizes[axis])) + 2) / sizes[axis];
                const double low = axis == 0 ? 2 * lo - 1 : 1 - 2 * hi;
                const double high = axis == 0 ? 2 * hi - 1 : 1 - 2 * lo;
                if (reaches(axis, low, true) && reaches(axis, high, false)) {
                    first[axis] = std::min(first[axis], tile); last[axis] = tile;
                }
            }
            if (first[axis] == counts[axis]) return;
        }
        for (u32 y = first[1]; y <= last[1]; ++y)
            for (u32 x = first[0]; x <= last[0]; ++x) bits[y * columns + x] |= bit;
    }
};
} // namespace kin::gpu::point_tiles
