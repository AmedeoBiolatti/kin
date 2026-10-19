#pragma once

#include <kin/core/types.hpp>

namespace kin {

// Last render's game-facing summary. Reading it never waits for the GPU.
// Counts cover the main scene and its overlay unless stated otherwise.
struct Render3DStats {
    u32 draws = 0;     // main-pass draw calls (including indirect calls)
    u32 instances = 0; // main-pass mesh copies submitted after CPU culling
    u32 culled = 0;    // mesh copies rejected by CPU view culling
    u64 triangles = 0; // main-pass triangles known to the CPU
    // GPU cluster cuts contribute only with explicit debug readback. False
    // means triangles is a partial count, not an estimate or a complete total.
    bool triangles_complete = true;
    f64 cpu_ms = 0.0; // CPU recording time; excludes present/GPU execution
    u32 samples = 0;  // effective MSAA samples (1 with temporal AA)
    u32 lights = 0, spots = 0; // point/spot lights selected for the view
    u32 shadow_draws = 0, shadow_casters = 0, shadow_cascades = 0; // sun maps drawn
    bool shadow_cached = false; // sun maps reused without drawing
    u32 skinned_copies = 0, skinned_draws = 0;
    u32 particles_drawn = 0, particles_dropped = 0;
    u32 pipelines_made = 0; // first-use work; warm_up() avoids these hitches
    // Reported feature-target/local-shadow allocations only, not total VRAM.
    u64 feature_bytes = 0, local_shadow_bytes = 0;
};

} // namespace kin
