#include <kin/ecs/ecs_scene.hpp>

#include <kin/platform/log.hpp>
#include <kin/runtime/debug_options.hpp>

#include <algorithm>
#include <chrono>
#include <string>

namespace kin {
namespace {

class DebugStepTimer {
public:
    DebugStepTimer(RuntimeDebugOptions* debug, std::string_view name)
        : _debug(debug),
          _name(name) {
        if (_debug && _debug->record_timing) {
            _start = std::chrono::steady_clock::now();
        }
    }

    ~DebugStepTimer() {
        if (!_debug || !_debug->record_timing) {
            return;
        }
        const auto end = std::chrono::steady_clock::now();
        const f64 ms = std::chrono::duration<f64, std::milli>(end - _start).count();
        _debug->record_timing(_name, ms);
    }

private:
    RuntimeDebugOptions* _debug = nullptr;
    std::string_view _name;
    std::chrono::steady_clock::time_point _start;
};

void count_static_rebuild_reason(RuntimeRenderDebugStats& stats, std::string_view reason) {
    if (reason == "initial") {
        ++stats.static_cache_rebuilds_initial;
    } else if (reason == "explicit") {
        ++stats.static_cache_rebuilds_explicit;
    } else if (reason == "clear") {
        ++stats.static_cache_rebuilds_clear;
    } else {
        ++stats.static_cache_rebuilds_other;
    }
}

} // namespace

WorldRenderState& EcsScene::render_state() {
    flecs::world& world = ecs().raw();
    if (!_render_state || !_render_state->matches(world)) {
        _render_state = std::make_unique<WorldRenderState>(world);
    }
    return *_render_state;
}

void EcsScene::render(SceneContext& ctx) {
    if (!_render_config.enabled) {
        return;
    }

    RuntimeDebugOptions* debug = ctx.debug_options;
    const bool culling_enabled = _render_config.culling_enabled && (!debug || debug->camera_culling_enabled);
    const bool static_cache_enabled = _render_config.static_cache_enabled && (!debug || debug->static_render_cache_enabled);
    const bool culling_diagnostics_enabled = debug && debug->render_culling_diagnostics_enabled;
    const u64 pass_mask = debug ? debug->render_pass_mask() : render_pass_mask::all;
    RuntimeDebugOptions* timing_debug = debug && debug->detailed_render_timings_enabled ? debug : nullptr;

    ctx.renderer.clear(_render_config.clear_color);

    Camera2D camera;
    const Camera2D* camera_ptr = nullptr;
    if (_render_config.camera_callback) {
        DebugStepTimer timer{timing_debug, "render.scene.camera"};
        camera = _render_config.camera_callback(ecs(), ctx);
        camera_ptr = &camera;
    }

    RenderProfile profile;
    RenderView view;
    {
        DebugStepTimer timer{timing_debug, "render.scene.view"};
        profile = _render_config.profile;
        profile.culling_enabled = culling_enabled;
        profile.cull_padding = _render_config.cull_padding;

        view = profile.make_view(camera_ptr);
        view.cull_padding = _render_config.cull_padding;
        view.culling_enabled = culling_enabled;
        if (camera_ptr) {
            view.cull_rect = camera_ptr->visible_rect(view.cull_padding);
        }
    }

    {
        DebugStepTimer timer{timing_debug, "render.scene.queue_reset"};
        _dynamic_render_queue.clear();
        _dynamic_render_queue.set_sort(profile.sort);
        _dynamic_render_queue.reserve(static_cache_enabled ? 128 : 512);
    }
    RuntimeRenderDebugStats frame_stats{
        .frames = debug ? debug->render_stats.frames + 1 : 1,
        .static_cache_used = static_cache_enabled,
        .culling_used = culling_enabled,
    };
    const RuntimeRenderDebugStats previous_stats = debug ? debug->render_stats : RuntimeRenderDebugStats{};
    frame_stats.dynamic_queue_capacity = static_cast<u64>(_dynamic_render_queue.capacity());
    frame_stats.dynamic_queue_capacity_grows = previous_stats.dynamic_queue_capacity_grows;
    if (frame_stats.dynamic_queue_capacity > previous_stats.dynamic_queue_capacity) {
        ++frame_stats.dynamic_queue_capacity_grows;
        if (debug) {
            KIN_LOG_DEBUG_F("render",
                            "dynamic render queue capacity grew",
                            (LogFields{{.name = "capacity", .value = std::to_string(frame_stats.dynamic_queue_capacity)},
                                       {.name = "previous_capacity", .value = std::to_string(previous_stats.dynamic_queue_capacity)}}));
        }
    }
    if (static_cache_enabled) {
        WorldRenderState& state = render_state();
        {
            DebugStepTimer timer{timing_debug, "render.scene.propagate_transforms"};
            state.propagate_transforms();
        }
        if (_static_render_cache.dirty()) {
            DebugStepTimer timer{timing_debug, "render.scene.static_rebuild"};
            const std::string dirty_reason{_static_render_cache.dirty_reason()};
            _static_render_cache.rebuild(ecs(), [&](EcsWorld& world, RenderQueue& queue) {
                (void)world;
                state.collect_static(queue, {.sort = true, .sort_mode = profile.sort});
            }, profile.sort);
            ++frame_stats.static_cache_rebuilds;
            frame_stats.static_cache_last_dirty_reason = dirty_reason;
            count_static_rebuild_reason(frame_stats, dirty_reason);
            KIN_LOG_DEBUG_F("render",
                            "scene static render cache rebuilt",
                            (LogFields{{.name = "reason", .value = dirty_reason},
                                       {.name = "count", .value = std::to_string(_static_render_cache.size())}}));
        }
        frame_stats.static_commands = static_cast<u64>(_static_render_cache.size());
        {
            DebugStepTimer timer{timing_debug, "render.scene.collect_dynamic"};
            state.collect_dynamic(_dynamic_render_queue, {.sort = true, .sort_mode = profile.sort, .view = &view});
        }
        frame_stats.commands_after_cull = static_cast<u64>(_dynamic_render_queue.size());
        if (culling_enabled && culling_diagnostics_enabled) {
            DebugStepTimer timer{timing_debug, "render.scene.count_unculled"};
            RenderQueue uncull_count{profile.sort};
            state.collect_dynamic(uncull_count, {.sort = false, .sort_mode = profile.sort});
            frame_stats.commands_before_cull = static_cast<u64>(uncull_count.size());
        } else {
            frame_stats.commands_before_cull = frame_stats.commands_after_cull;
        }
        frame_stats.dynamic_commands = frame_stats.commands_after_cull;
        frame_stats.commands_culled = frame_stats.commands_before_cull > frame_stats.commands_after_cull
            ? frame_stats.commands_before_cull - frame_stats.commands_after_cull
            : 0;
        if (culling_enabled && culling_diagnostics_enabled) {
            KIN_LOG_DEBUG_F("render",
                            "scene render culling summary",
                            (LogFields{{.name = "visible", .value = std::to_string(frame_stats.commands_after_cull)},
                                       {.name = "culled", .value = std::to_string(frame_stats.commands_culled)},
                                       {.name = "queued", .value = std::to_string(frame_stats.commands_before_cull)}}));
        }
        {
            DebugStepTimer timer{timing_debug, "render.scene.sort_dynamic"};
            _dynamic_render_queue.sort_commands();
        }
        {
            DebugStepTimer timer{timing_debug, "render.scene.flush_world"};
            _static_render_cache.flush_merged(ctx.renderer, _dynamic_render_queue, view, pass_mask);
        }
    } else {
        WorldRenderState& state = render_state();
        {
            DebugStepTimer timer{timing_debug, "render.scene.propagate_transforms"};
            state.propagate_transforms();
        }
        {
            DebugStepTimer timer{timing_debug, "render.scene.collect_world"};
            state.collect_all(_dynamic_render_queue, {}, {.sort = true, .sort_mode = profile.sort, .view = &view});
        }
        frame_stats.commands_after_cull = static_cast<u64>(_dynamic_render_queue.size());
        if (culling_enabled && culling_diagnostics_enabled) {
            DebugStepTimer timer{timing_debug, "render.scene.count_unculled"};
            RenderQueue uncull_count{profile.sort};
            state.collect_all(uncull_count, {}, {.sort = false, .sort_mode = profile.sort});
            frame_stats.commands_before_cull = static_cast<u64>(uncull_count.size());
        } else {
            frame_stats.commands_before_cull = frame_stats.commands_after_cull;
        }
        frame_stats.dynamic_commands = frame_stats.commands_after_cull;
        frame_stats.commands_culled = frame_stats.commands_before_cull > frame_stats.commands_after_cull
            ? frame_stats.commands_before_cull - frame_stats.commands_after_cull
            : 0;
        if (culling_enabled && culling_diagnostics_enabled) {
            KIN_LOG_DEBUG_F("render",
                            "scene render culling summary",
                            (LogFields{{.name = "visible", .value = std::to_string(frame_stats.commands_after_cull)},
                                       {.name = "culled", .value = std::to_string(frame_stats.commands_culled)},
                                       {.name = "queued", .value = std::to_string(frame_stats.commands_before_cull)}}));
        }
        {
            DebugStepTimer timer{timing_debug, "render.scene.flush_world"};
            _dynamic_render_queue.flush(ctx.renderer, view, pass_mask);
        }
    }
    if (debug) {
        frame_stats.static_cache_rebuilds += previous_stats.static_cache_rebuilds;
        frame_stats.static_cache_rebuilds_initial += previous_stats.static_cache_rebuilds_initial;
        frame_stats.static_cache_rebuilds_explicit += previous_stats.static_cache_rebuilds_explicit;
        frame_stats.static_cache_rebuilds_clear += previous_stats.static_cache_rebuilds_clear;
        frame_stats.static_cache_rebuilds_other += previous_stats.static_cache_rebuilds_other;
        if (frame_stats.static_cache_last_dirty_reason.empty()) {
            frame_stats.static_cache_last_dirty_reason = previous_stats.static_cache_last_dirty_reason;
        }
        debug->render_stats = frame_stats;
    }

}

} // namespace kin
