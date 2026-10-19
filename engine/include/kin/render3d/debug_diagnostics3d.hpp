#pragma once

#include <kin/render3d/debug_stats3d.hpp>
#include <kin/render3d/diagnostics3d.hpp>

namespace kin {

// Extra renderer implementation rows; explicitly opt in when profiling.
inline void record_render3d_debug_diagnostics(RuntimeDebugOptions* debug, const RenderDebugStats3D& s,
                                              Vec2i viewport = {}) {
    record_render3d_diagnostics(debug, s, viewport);
    if (!debug)
        return;
    if (!debug->record_status)
        return;
    const auto count = [](auto value) { return std::to_string(value); };
    const auto bytes = [&](u64 value) { return value < 1024 ? count(value) + " B" : count(value / 1024) + " KiB"; };
    debug->record_status("3D culling tests",
                         count(s.camera_instance_tests) + " instance tests, " + count(s.spatial_groups) + " groups");
    debug->record_status("3D dispatches", "AO " + count(s.ao_dispatches) + ", contact " + count(s.contact_dispatches) +
                                              ", TAA " + count(s.taa_dispatches));
    debug->record_status("3D uploads/frame", "instances " + bytes(s.instance_upload_bytes) + ", retained " +
                                                 bytes(s.retained_upload_bytes) + ", clusters " +
                                                 bytes(s.cluster_upload_bytes));
    // These are separate reported allocations, never advertised as total VRAM.
    debug->record_status("3D feature targets", bytes(s.feature_bytes));
    debug->record_status("3D local shadows", bytes(s.local_shadow_bytes));
    debug->record_status("3D shared mesh pages", bytes(s.shared_mesh_allocated_bytes) + " allocated, " +
                                                     bytes(s.shared_mesh_live_bytes) + " live");
}

} // namespace kin
