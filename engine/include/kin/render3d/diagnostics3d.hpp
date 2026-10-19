#pragma once

#include <kin/render3d/stats3d.hpp>
#include <kin/runtime/debug_options.hpp>

#include <string>

namespace kin {

// Publish the last completed 3D collection/render to the existing runtime
// overlay (F1), capture (F2) and profiling sinks. Call once per view after
// Renderer3D::render(). No retained state, GPU reads, asset ownership or work
// when no sink consumes diagnostics. The runtime already records gpu.frame;
// render3d.cpu is CPU recording time, not GPU execution or complete frame time.
inline void record_render3d_diagnostics(RuntimeDebugOptions* debug, const Render3DStats& s,
                                        Vec2i viewport = {}) {
    if (!debug) return;
    if (debug->record_timing) debug->record_timing("render3d.cpu", s.cpu_ms);
    if (!debug->record_status) return;
    const auto count = [](auto value) { return std::to_string(value); };
    debug->record_status("3D viewport", count(viewport.x)+" x "+count(viewport.y)+", MSAA "+count(s.samples));
    debug->record_status("3D main", count(s.draws)+" draws, "+count(s.instances)+" instances, "+count(s.triangles)+
                         (s.triangles_complete ? " triangles" : " triangles (partial; GPU cut not read back)"));
    debug->record_status("3D visibility", count(s.culled)+" copies culled");
    debug->record_status("3D shadows", count(s.shadow_draws)+" draws, "+count(s.shadow_casters)+" casters, "+count(s.shadow_cascades)+" cascades"+(s.shadow_cached?", cached":""));
    debug->record_status("3D lights", count(s.lights)+" point, "+count(s.spots)+" spot");
    debug->record_status("3D particles", count(s.particles_drawn)+" drawn, "+count(s.particles_dropped)+" dropped");
    debug->record_status("3D pipelines/frame", count(s.pipelines_made));
}

} // namespace kin
