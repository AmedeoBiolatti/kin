#pragma once

#include <kin/core/types.hpp>

namespace kin {

// What the view shows of Scene3D::global_illumination's GI scene instead of the picture
// (RenderDebugOptions3D::gi_view): each pixel traced through its voxels to the first one a surface is in, and that
// voxel's colour (shaded a little by which way it faces the camera), normal or emission; `Steps` the trace's cost, from
// blue (a few steps) to red (many). Off, or with no GI scene, the picture is drawn as ever.
enum class GiView3D : u8 { Off, Albedo, Normal, Emission, Steps };

// Explicit diagnostic overrides, separate from visual quality and budgets.
// Defaults preserve normal rendering and never request a blocking readback.
// Attach to Scene3D::debug; these are not authored/serialized scene settings.
struct RenderDebugOptions3D {
    i32 force_level = -1;          // -1: normal LOD; >=0: force a level/cluster depth
    bool gpu_culling = true;       // false: compare with the CPU cluster cut
    bool stats_readback = false;   // waits for GPU cluster counters: profiling only
    bool gpu_pass_markers = false; // labels for external GPU profilers, no readback
    bool freeze_jitter = false;    // hold the first TAA jitter sample
    GiView3D gi_view = GiView3D::Off;
};

} // namespace kin
