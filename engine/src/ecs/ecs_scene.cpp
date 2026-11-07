#include <kin/ecs/ecs_scene.hpp>

#include <kin/renderer/renderer2d.hpp>

#include <kin/assets/content.hpp>
#include <kin/ecs/camera.hpp>
#include <kin/ecs/lighting.hpp>
#include <kin/platform/log.hpp>
#include <kin/platform/window.hpp>
#include <kin/runtime/debug_options.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

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

void EcsScene::add_asset_binder(std::string name, Assets2D::Binder binder) {
    if (_assets) {
        _assets->add_binder(name, binder);
    }
    const auto found = std::ranges::find(_asset_binders, name, &std::pair<std::string, Assets2D::Binder>::first);
    if (found != _asset_binders.end()) {
        found->second = std::move(binder);
    } else {
        _asset_binders.emplace_back(std::move(name), std::move(binder));
    }
}

std::unique_ptr<Assets2D> EcsScene::exchange_assets(std::unique_ptr<Assets2D> assets) {
    auto old = std::move(_assets);
    adopt_assets(std::move(assets));
    return old;
}

void EcsScene::adopt_assets(std::unique_ptr<Assets2D> assets) {
    if (assets) {
        for (const auto& [name, binder] : _asset_binders) {
            assets->add_binder(name, binder);
        }
        _render_config.asset_root = assets->root();
    }
    _assets = std::move(assets);
    _asset_frames = 0;
    mark_render_cache_dirty("preloaded assets");
}

std::unique_ptr<DevelopmentChange> EcsScene::prepare_asset_reload(const DevelopmentRevision& revision,
                                                                  const ScenePatchPreparer& prepare_scene,
                                                                  std::function<void()> on_finalize,
                                                                  std::string& error) {
    if (!_assets) {
        error = "the scene has no assets to reload";
        return {};
    }
    std::unique_ptr<Assets2D> candidate;
    const AssetPreviewCheck check = [&](EcsWorld& preview, std::string& failure) {
        candidate = prepare_reload_assets(*_assets, revision, preview, failure);
        return candidate != nullptr;
    };
    std::shared_ptr<DevelopmentChange> patch;
    if (prepare_scene) {
        patch = prepare_scene(check, error);
        if (!patch) {
            return {};
        }
    } else {
        EcsWorld preview;
        ecs().components().copy_schema_to(preview.components());
        if (!instantiate_scene(preview, scene_document_from_world(ecs()), error) || !check(preview, error)) {
            return {};
        }
    }
    if (!candidate) {
        error = "the scene patch did not check the reloaded assets";
        return {};
    }
    struct Swap {
        std::unique_ptr<Assets2D> next;
        std::unique_ptr<Assets2D> previous;
    };
    auto swap = std::make_shared<Swap>(Swap{.next = std::move(candidate), .previous = {}});
    return std::make_unique<DevelopmentChange>(DevelopmentChange{
        .activate =
            [this, swap, patch](std::string& failure) {
                if (patch && !patch->activate(failure)) {
                    return false;
                }
                swap->previous = exchange_assets(std::move(swap->next));
                _assets->bind(ecs());
                return true;
            },
        .rollback =
            [this, swap, patch] {
                if (swap->previous) {
                    swap->next = exchange_assets(std::move(swap->previous));
                }
                if (patch) {
                    patch->rollback();
                }
            },
        .finalize =
            [this, patch, on_finalize = std::move(on_finalize)] {
                if (patch && patch->finalize) {
                    patch->finalize();
                }
                if (on_finalize) {
                    on_finalize();
                }
                mark_render_cache_dirty("development reload");
            },
    });
}

void EcsScene::bind_assets(SceneContext& ctx) {
    if (!_assets) {
        std::filesystem::path root = _render_config.asset_root;
        if (root.empty()) {
            root = content_root();
        }
        _assets = std::make_unique<Assets2D>(ctx.renderer, root.empty() ? std::filesystem::path(".") : root);
        for (const auto& [name, binder] : _asset_binders) {
            _assets->add_binder(name, binder);
        }
    }
    if (_render_config.asset_check_frames > 0 && ++_asset_frames >= _render_config.asset_check_frames) {
        _asset_frames = 0;
        _assets->reload_changed();
    }
    if (_assets->bind(ecs())) {
        _static_render_cache.mark_dirty("assets");
    }
}

bool EcsScene::set_view_camera(const Camera2D* camera) {
    if (camera) {
        _view_camera = *camera;
    } else {
        _view_camera.reset();
    }
    return true;
}

void EcsScene::render(SceneContext& ctx) {
    if (!_render_config.enabled) {
        return;
    }
    if (_render_config.assets) {
        bind_assets(ctx);
    }

    RuntimeDebugOptions* debug = ctx.debug_options;
    const bool culling_enabled = _render_config.culling_enabled && (!debug || debug->camera_culling_enabled);
    const bool static_cache_enabled = _render_config.static_cache_enabled && (!debug || debug->static_render_cache_enabled);
    const bool culling_diagnostics_enabled = debug && debug->render_culling_diagnostics_enabled;
    const u64 pass_mask = debug ? debug->render_pass_mask() : render_pass_mask::all;
    RuntimeDebugOptions* timing_debug = debug && debug->detailed_render_timings_enabled ? debug : nullptr;

    ctx.renderer.clear(_render_config.clear_color);

    if (_render_config.scene_lighting) {
        // The colour output a ColorGrade says, while there is one.
        if (std::optional<ColorOutput> output = scene_color_output(ecs())) {
            ctx.renderer.set_color_output(std::move(*output));
            _graded = true;
        } else if (std::exchange(_graded, false)) {
            ctx.renderer.set_color_output({});
        }
    }

    Vec2f screen = _render_config.view_size;
    if (screen.x <= 0.0f || screen.y <= 0.0f) {
        const Vec2i points = ctx.window.size();
        screen = {static_cast<f32>(points.x), static_cast<f32>(points.y)};
    }
    Camera2D camera;
    const Camera2D* camera_ptr = nullptr;
    if (_view_camera) {
        camera = *_view_camera;
        camera_ptr = &camera;
    } else if (_render_config.camera_callback) {
        DebugStepTimer timer{timing_debug, "render.scene.camera"};
        camera = _render_config.camera_callback(ecs(), ctx);
        camera_ptr = &camera;
    } else if (_render_config.scene_cameras) {
        DebugStepTimer timer{timing_debug, "render.scene.camera"};
        if (std::optional<Camera2D> found = scene_camera(ecs(), screen)) {
            camera = *found;
            camera_ptr = &camera;
        }
    }

    // Where Parallax layers are shifted from: the camera's centre (the screen's
    // without one); an editor's view draws them where they are placed.
    const std::optional<Vec2f> parallax_view =
        _view_camera ? std::nullopt : std::optional(camera_ptr ? camera_ptr->center() : Vec2f{screen.x * 0.5f, screen.y * 0.5f});

    // Lit: the world (and its effects) drawn first, then lit, then the UI layer and above.
    const auto* lighting = _render_config.scene_lighting ? scene_lighting(ecs()) : nullptr;
    const bool lit = lighting && !lighting->forward_sprites;
    constexpr u64 unlit_passes = render_pass_mask::ui | render_pass_mask::debug | render_pass_mask::editor_overlay;
    const auto light_world = [&] {
        DebugStepTimer timer{timing_debug, "render.scene.lighting"};
        const Rectf area = camera_ptr ? Rectf{0.0f, 0.0f, camera_ptr->viewport.x, camera_ptr->viewport.y}
                                      : Rectf{0.0f, 0.0f, screen.x, screen.y};
        apply_scene_lighting(ctx.renderer, _lighting, ecs(), camera_ptr ? camera_ptr->view_transform() : Affine2{}, area);
    };

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
        state.set_parallax_view(parallax_view);
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
            if (lit) {
                _static_render_cache.flush_merged(ctx.renderer, _dynamic_render_queue, view, pass_mask & ~unlit_passes);
                light_world();
                _static_render_cache.flush_merged(ctx.renderer, _dynamic_render_queue, view, pass_mask & unlit_passes);
            } else {
                _static_render_cache.flush_merged(ctx.renderer, _dynamic_render_queue, view, pass_mask);
            }
        }
    } else {
        WorldRenderState& state = render_state();
        state.set_parallax_view(parallax_view);
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
            if (lit) {
                _dynamic_render_queue.flush(ctx.renderer, view, pass_mask & ~unlit_passes);
                light_world();
                _dynamic_render_queue.flush(ctx.renderer, view, pass_mask & unlit_passes);
            } else {
                _dynamic_render_queue.flush(ctx.renderer, view, pass_mask);
            }
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

std::unique_ptr<Assets2D> prepare_reload_assets(const Assets2D& current, const DevelopmentRevision& revision,
                                                EcsWorld& preview, std::string& error) {
    auto candidate = current.for_snapshot(revision.content);
    candidate->bind(preview);
    if (auto problem = candidate->first_problem()) {
        error = std::move(*problem);
        return {};
    }
    return candidate;
}

} // namespace kin
