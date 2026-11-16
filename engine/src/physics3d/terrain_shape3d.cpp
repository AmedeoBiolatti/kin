#include <algorithm>
#include <cfloat>
#include <kin/physics3d/terrain_shape3d.hpp>
#include <vector>

namespace kin {

Shape3D terrain_shape3d(const Terrain3D& terrain) {
    if (!terrain.valid())
        return {};
    const TerrainDesc3D& d = terrain.desc();
    const std::span<const u16> heights = terrain.heights();
    i32 side = std::max({d.samples.x, d.samples.y, 4});
    side += side % 2;
    const f32 step = (d.max_height - d.min_height) / 65535.0f;
    std::vector<f32> samples(static_cast<std::size_t>(side) * static_cast<std::size_t>(side), FLT_MAX);
    for (i32 z = 0; z < d.samples.y; ++z)
        for (i32 x = 0; x < d.samples.x; ++x)
            samples[static_cast<std::size_t>(z) * static_cast<std::size_t>(side) + static_cast<std::size_t>(x)] =
                d.min_height +
                static_cast<f32>(heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(d.samples.x) +
                                         static_cast<std::size_t>(x)]) *
                    step;
    return Shape3D::height_field(samples, side, d.origin, {d.spacing, 1.0f, d.spacing}, {d.min_height, d.max_height});
}

bool update_terrain_collision(World3D& world, Body3D body, const Terrain3D& terrain, Terrain3D::Changed changed) {
    if (changed.empty())
        return true;
    if (!terrain.valid())
        return false;
    const TerrainDesc3D& d = terrain.desc();
    const std::span<const u16> heights = terrain.heights();
    // Out to whole 2 x 2 blocks (Jolt's), inside the padded square (samples past the terrain: holes).
    i32 side = std::max({d.samples.x, d.samples.y, 4});
    side += side % 2;
    const i32 x0 = std::max(0, changed.at.x) & ~1, z0 = std::max(0, changed.at.y) & ~1;
    i32 x1 = std::min(side, changed.at.x + changed.size.x), z1 = std::min(side, changed.at.y + changed.size.y);
    x1 = std::min(side, x1 + (x1 - x0) % 2);
    z1 = std::min(side, z1 + (z1 - z0) % 2);
    const f32 step = (d.max_height - d.min_height) / 65535.0f;
    std::vector<f32> part(static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(z1 - z0), FLT_MAX);
    for (i32 z = z0; z < std::min(z1, d.samples.y); ++z)
        for (i32 x = x0; x < std::min(x1, d.samples.x); ++x)
            part[static_cast<std::size_t>(z - z0) * static_cast<std::size_t>(x1 - x0) +
                 static_cast<std::size_t>(x - x0)] =
                d.min_height +
                static_cast<f32>(heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(d.samples.x) +
                                         static_cast<std::size_t>(x)]) *
                    step;
    return world.set_heights(body, {x0, z0}, {x1 - x0, z1 - z0}, part);
}

} // namespace kin
