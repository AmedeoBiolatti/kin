#pragma once

#include <kin/core/types.hpp>

namespace kin {

// Explicit diagnostic overrides, separate from visual quality and budgets.
// Defaults preserve normal rendering and never request a blocking readback.
// Attach to Scene3D::debug; these are not authored/serialized scene settings.
struct RenderDebugOptions3D {
    i32 force_level = -1;          // -1: normal LOD; >=0: force a level/cluster depth
    bool gpu_culling = true;       // false: compare with the CPU cluster cut
    bool stats_readback = false;   // waits for GPU cluster counters: profiling only
    bool gpu_pass_markers = false; // labels for external GPU profilers, no readback
    bool freeze_jitter = false;    // hold the first TAA jitter sample
};

} // namespace kin
