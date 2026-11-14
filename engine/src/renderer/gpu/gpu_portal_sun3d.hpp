#pragma once

#include <array>
#include <kin/render3d/scene3d.hpp>

namespace kin::gpu {

constexpr u32 max_portal_sun_maps = 8;

// std140: shared by the beauty and voxel-lighting passes. Eight links fit in SDL_GPU's 4 KiB frame block.
struct PortalSunLight {
    f32 shadow_matrix[16];
    f32 plane[4], x[4], y[4]; // receiving opening's world-to-plane rows
    f32 direction[4];         // toward the transported sun; w: half width
    f32 aperture[4];          // half height, depth bias, normal offset (world), shadow layer (-1: no shadows)
};
static_assert(sizeof(PortalSunLight) == 144);

struct PortalSunPlan {
    std::array<PortalSunLight, max_portal_sun_maps> lights{};
    std::array<Mat4f, max_portal_sun_maps> source_projection{};
    std::array<Vec4f, max_portal_sun_maps> source_clip{};
    std::array<f32, max_portal_sun_maps> texel_world{};
    u32 count = 0;
    u32 shadow_count = 0;
};

// PortalLink3D describes a viewing ray. Sunlight takes the reverse physical path, from exit to entry. Each map
// combines source-side geometry transformed into receiving space and geometry in front of the receiving opening.
PortalSunPlan portal_sun_plan(const Scene3D& scene, u32 resolution);

} // namespace kin::gpu
