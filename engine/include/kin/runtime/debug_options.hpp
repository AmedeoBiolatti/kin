#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/render_command.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace kin {

struct RuntimeRenderDebugStats {
    u64 frames = 0;
    u64 static_commands = 0;
    u64 dynamic_commands = 0;
    u64 commands_before_cull = 0;
    u64 commands_after_cull = 0;
    u64 commands_culled = 0;
    u64 static_cache_rebuilds = 0;
    u64 static_cache_rebuilds_initial = 0;
    u64 static_cache_rebuilds_explicit = 0;
    u64 static_cache_rebuilds_clear = 0;
    u64 static_cache_rebuilds_other = 0;
    std::string static_cache_last_dirty_reason;
    u64 dynamic_queue_capacity = 0;
    u64 dynamic_queue_capacity_grows = 0;
    u64 ui_static_commands = 0;
    u64 ui_dynamic_commands = 0;
    u64 ui_static_command_capacity = 0;
    u64 ui_dynamic_command_capacity = 0;
    u64 ui_static_command_capacity_grows = 0;
    u64 ui_dynamic_command_capacity_grows = 0;
    u64 ui_static_record_capacity = 0;
    u64 ui_dynamic_record_capacity = 0;
    u64 ui_static_record_capacity_grows = 0;
    u64 ui_dynamic_record_capacity_grows = 0;
    bool static_cache_used = false;
    bool culling_used = false;
};

struct RuntimeDebugOptions {
    bool static_render_cache_enabled = true;
    bool camera_culling_enabled = true;
    bool pass_world_enabled = true;
    bool pass_effects_enabled = true;
    bool pass_ui_enabled = true;
    bool pass_debug_enabled = true;
    bool texture_batching_enabled = true;
    bool detailed_render_timings_enabled = false;
    bool render_culling_diagnostics_enabled = false;
    bool overdraw_view = false; // Renderer2D::set_overdraw_view
    RuntimeRenderDebugStats render_stats;
    std::function<void(std::string_view, f64)> record_timing;
    // Text rows for the debug overlay (e.g. per-system parallel execution
    // decisions). Installed only while something consumes them, like
    // record_timing; callers must handle the empty function.
    std::function<void(std::string_view, std::string)> record_status;

    u64 render_pass_mask() const {
        u64 mask = 0;
        if (pass_world_enabled) {
            mask |= render_pass_mask::world;
        }
        if (pass_effects_enabled) {
            mask |= render_pass_mask::effects;
        }
        if (pass_ui_enabled) {
            mask |= render_pass_mask::ui;
        }
        if (pass_debug_enabled) {
            mask |= render_pass_mask::debug;
        }
        return mask;
    }
};

} // namespace kin
