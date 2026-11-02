#include <kin/runtime/scene_server.hpp>
#ifdef KIN_EXPERIMENTAL_3D
#include <kin/render3d/renderer3d.hpp>
#endif

#include "runtime_internal.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <kin/anim/anim_format.hpp>
#include <kin/anim/player.hpp>
#include <kin/core/json.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/system.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/render_view.hpp>
#include <cctype>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

namespace kin {
namespace {

constexpr i64 MAX_TICKS_PER_CALL = 1'000'000;

std::string_view server_mode_name(ServerMode mode) {
    return mode == ServerMode::Realtime ? "realtime" : "driven";
}

std::string_view server_transport_name(ServerTransport transport) {
    return transport == ServerTransport::Http ? "http" : "stdio";
}

void write_bindings_json(JsonWriter& json, const std::vector<InputBinding>& bindings) {
    json.begin_array();
    for (const InputBinding& binding : bindings) {
        json.value(binding_name(binding));
    }
    json.end_array();
}

void write_game_info_json(JsonWriter& json, const GameInfo& info) {
    json.begin_object();
    json.field("id", std::string_view{info.id});
    json.field("title", std::string_view{info.title});
    json.field("version", std::string_view{info.version});
    json.field("description", std::string_view{info.description});
    json.field("author", std::string_view{info.author});
    json.field("headless", info.headless_supported);
    json.key("window").begin_object();
    json.field("width", info.window.width);
    json.field("height", info.window.height);
    json.field("logical_width", info.window.logical_width);
    json.field("logical_height", info.window.logical_height);
    json.field("integer_scale", info.window.integer_scale);
    json.field("resizable", info.window.resizable);
    json.field("borderless", info.window.borderless);
    json.end_object();
    json.key("tags").begin_array();
    for (const std::string& tag : info.tags) {
        json.value(std::string_view{tag});
    }
    json.end_array();
    json.key("fields").begin_object();
    for (const GameInfoField& field : info.fields) {
        json.field(std::string_view{field.key}, std::string_view{field.value});
    }
    json.end_object();
    json.end_object();
}

std::string layer_kind_text(LayerKind kind) {
    return kind == LayerKind::Base ? "base" : "override";
}

std::string_view ecs_event_kind_name(EcsEventKind kind) {
    switch (kind) {
    case EcsEventKind::EntityCreated: return "entity_created";
    case EcsEventKind::EntityDestroyed: return "entity_destroyed";
    case EcsEventKind::EntityRenamed: return "entity_renamed";
    case EcsEventKind::EntityReparented: return "entity_reparented";
    case EcsEventKind::EntityEnabled: return "entity_enabled";
    case EcsEventKind::EntityDisabled: return "entity_disabled";
    case EcsEventKind::ComponentAdded: return "component_added";
    case EcsEventKind::ComponentRemoved: return "component_removed";
    case EcsEventKind::ComponentChanged: return "component_changed";
    case EcsEventKind::SystemAdded: return "system_added";
    case EcsEventKind::SystemRemoved: return "system_removed";
    case EcsEventKind::SystemEnabled: return "system_enabled";
    case EcsEventKind::SystemDisabled: return "system_disabled";
    case EcsEventKind::SystemError: return "system_error";
    }
    return "unknown";
}

std::string_view ecs_event_source_name(EcsEventSource source) {
    return source == EcsEventSource::FlecsObserver ? "flecs_observer" : "kin_api";
}

void write_string_map(JsonWriter& json, const Bindings& bindings) {
    json.begin_object();
    std::vector<std::string> keys;
    keys.reserve(bindings.size());
    for (const auto& [key, value] : bindings) {
        (void)value;
        keys.push_back(key);
    }
    std::ranges::sort(keys);
    for (const std::string& key : keys) {
        json.field(std::string_view{key}, std::string_view{bindings.at(key)});
    }
    json.end_object();
}

void write_animation_snapshot_json(JsonWriter& json, const AnimationPlayerSnapshot& snap) {
    json.field("state", std::string_view{snap.state});
    json.field("playing", snap.playing);
    json.key("bindings");
    write_string_map(json, snap.bindings);
    json.key("triggers").begin_array();
    for (const std::string& trigger : snap.triggers) {
        json.value(std::string_view{trigger});
    }
    json.end_array();
    json.key("layers").begin_array();
    for (const AnimationLayerSnapshot& layer : snap.layers) {
        json.begin_object();
        json.field("kind", layer_kind_text(layer.kind));
        json.field("animation", std::string_view{layer.animation});
        json.field("time", static_cast<f64>(layer.time));
        json.field("speed", static_cast<f64>(layer.speed));
        json.field("weight", static_cast<f64>(layer.weight));
        json.field("done", layer.done);
        json.end_object();
    }
    json.end_array();
}

struct EntityAnimationSnapshot {
    flecs::entity_t id = 0;
    std::string name;
    AnimationPlayerSnapshot snapshot;
};

std::vector<EntityAnimationSnapshot> animation_snapshots(flecs::world& world) {
    std::vector<EntityAnimationSnapshot> snapshots;
    if (!world.lookup("AnimationPlayer")) {
        return snapshots;
    }
    world.each([&](flecs::entity entity, AnimationPlayer& player) {
        const char* name = entity.name();
        snapshots.push_back({
            .id = entity.id(),
            .name = name ? std::string{name} : std::string{},
            .snapshot = snapshot(player),
        });
    });
    std::ranges::sort(snapshots, {}, &EntityAnimationSnapshot::id);
    return snapshots;
}

void write_world_animation_snapshots(JsonWriter& json, flecs::world& world) {
    json.key("animations").begin_array();
    for (const EntityAnimationSnapshot& entry : animation_snapshots(world)) {
        json.begin_object();
        json.field("entity", static_cast<u64>(entry.id));
        json.field("name", std::string_view{entry.name});
        write_animation_snapshot_json(json, entry.snapshot);
        json.end_object();
    }
    json.end_array();
}

void write_system_snapshot_json(JsonWriter& json, const SystemSnapshot& snapshot) {
    json.begin_object();
    json.field("id", std::string_view{snapshot.id});
    json.field("name", std::string_view{snapshot.name});
    json.field("enabled", snapshot.enabled);
    json.field("phase", system_phase_name(snapshot.phase));
    json.field("order", snapshot.order);
    json.field("batch_index", snapshot.batch_index);
    json.field("parallel_eligible", snapshot.parallel_eligible);
    json.field("last_execution_mode", system_execution_mode_name(snapshot.last_execution_mode));
    json.field("last_batch_execution_mode", system_execution_mode_name(snapshot.last_batch_execution_mode));
    json.field("last_execution_decision", system_execution_decision_name(snapshot.last_execution_decision));
    json.field("execution_decision_reason", std::string_view{snapshot.execution_decision_reason});
    json.field("worker_count", snapshot.worker_count);
    json.field("estimated_work_ms", snapshot.estimated_work_ms);
    json.field("matched_entities", snapshot.stats.matched_entities);
    json.field("runs", snapshot.stats.runs);
    json.field("last_duration_ms", snapshot.stats.last_duration_ms);
    json.field("average_duration_ms", snapshot.stats.average_duration_ms);
    json.field("max_duration_ms", snapshot.stats.max_duration_ms);
    json.key("command_stats").begin_object();
    json.field("queued", snapshot.command_stats.queued);
    json.field("flushed", snapshot.command_stats.flushed);
    json.field("discarded", snapshot.command_stats.discarded);
    json.field("failed", snapshot.command_stats.failed);
    json.end_object();
    json.field("last_error", std::string_view{snapshot.last_error});
    json.key("reads").begin_array();
    for (const std::string& read : snapshot.reads) {
        json.value(std::string_view{read});
    }
    json.end_array();
    json.key("writes").begin_array();
    for (const std::string& write : snapshot.writes) {
        json.value(std::string_view{write});
    }
    json.end_array();
    json.key("schedule_diagnostics").begin_array();
    for (const std::string& diagnostic : snapshot.schedule_diagnostics) {
        json.value(std::string_view{diagnostic});
    }
    json.end_array();
    json.key("parallel_diagnostics").begin_array();
    for (const SystemParallelDiagnostic& diagnostic : snapshot.parallel_diagnostics) {
        json.begin_object();
        json.field("system", std::string_view{diagnostic.system});
        json.field("code", std::string_view{diagnostic.code});
        json.field("message", std::string_view{diagnostic.message});
        json.end_object();
    }
    json.end_array();
    json.end_object();
}

void write_schedule_snapshot_json(JsonWriter& json, const SystemScheduleSnapshot& schedule) {
    json.begin_object();
    json.field("valid", schedule.valid);
    json.field("requested_execution_mode", system_execution_mode_name(schedule.requested_execution_mode));
    json.field("effective_execution_mode", system_execution_mode_name(schedule.effective_execution_mode));
    json.key("diagnostics").begin_array();
    for (const std::string& diagnostic : schedule.diagnostics) {
        json.value(std::string_view{diagnostic});
    }
    json.end_array();
    json.key("edges").begin_array();
    for (const SystemDependencyEdge& edge : schedule.edges) {
        json.begin_object();
        json.field("before", std::string_view{edge.before});
        json.field("after", std::string_view{edge.after});
        json.field("reason", std::string_view{edge.reason});
        json.end_object();
    }
    json.end_array();
    json.key("batches").begin_array();
    for (const SystemBatch& batch : schedule.batches) {
        json.begin_object();
        json.field("phase", system_phase_name(batch.phase));
        json.field("index", batch.index);
        json.field("parallel_eligible", batch.parallel_eligible);
        json.field("execution_mode", system_execution_mode_name(batch.execution_mode));
        json.field("execution_decision", system_execution_decision_name(batch.execution_decision));
        json.field("execution_decision_reason", std::string_view{batch.execution_decision_reason});
        json.field("worker_count", batch.worker_count);
        json.field("estimated_work_ms", batch.estimated_work_ms);
        json.field("last_duration_ms", batch.last_duration_ms);
        json.key("command_stats").begin_object();
        json.field("queued", batch.command_stats.queued);
        json.field("flushed", batch.command_stats.flushed);
        json.field("discarded", batch.command_stats.discarded);
        json.field("failed", batch.command_stats.failed);
        json.end_object();
        json.key("systems").begin_array();
        for (const SystemId& system : batch.systems) {
            json.value(std::string_view{system});
        }
        json.end_array();
        json.key("diagnostics").begin_array();
        for (const std::string& diagnostic : batch.diagnostics) {
            json.value(std::string_view{diagnostic});
        }
        json.end_array();
        json.key("parallel_diagnostics").begin_array();
        for (const SystemParallelDiagnostic& diagnostic : batch.parallel_diagnostics) {
            json.begin_object();
            json.field("system", std::string_view{diagnostic.system});
            json.field("code", std::string_view{diagnostic.code});
            json.field("message", std::string_view{diagnostic.message});
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    json.end_object();
}

void write_events_json(JsonWriter& json, const EcsEventRegistry& events) {
    const EcsDirtyState& dirty = events.dirty_state();
    json.begin_object();
    json.key("dirty").begin_object();
    json.field("world", dirty.world_dirty);
    json.field("hierarchy", dirty.hierarchy_dirty);
    json.field("components", dirty.components_dirty);
    json.field("systems", dirty.systems_dirty);
    json.field("render", dirty.render_dirty);
    json.field("last_event_sequence", dirty.last_event_sequence);
    json.end_object();
    json.key("items").begin_array();
    for (const EcsEvent& event : events.snapshot()) {
        json.begin_object();
        json.field("sequence", event.sequence);
        json.field("frame", event.frame);
        json.field("kind", ecs_event_kind_name(event.kind));
        json.field("source", ecs_event_source_name(event.source));
        json.field("entity", event.entity);
        json.field("component_id", event.component_id);
        json.field("component_name", std::string_view{event.component_name});
        json.field("field_name", std::string_view{event.field_name});
        json.field("system_id", std::string_view{event.system_id});
        json.field("message", std::string_view{event.message});
        json.end_object();
    }
    json.end_array();
    json.end_object();
}

void write_kin_world_snapshot_json(JsonWriter& json, EcsWorld& world, bool include_events) {
    json.begin_object();
    json.key("world");
    write_world_snapshot_json(json, world.snapshot());
    json.key("systems").begin_array();
    for (const SystemSnapshot& system : world.systems().snapshots()) {
        write_system_snapshot_json(json, system);
    }
    json.end_array();
    json.key("schedule");
    write_schedule_snapshot_json(json, world.systems().schedule_snapshot());
    if (include_events) {
        json.key("events");
        write_events_json(json, world.events());
    }
    json.end_object();
}

std::string diagnostic_severity_text(AnimationDiagnosticSeverity severity) {
    return severity == AnimationDiagnosticSeverity::Warning ? "warning" : "error";
}

void write_animation_diagnostics(JsonWriter& json, const std::vector<AnimationDiagnostic>& diagnostics) {
    json.begin_array();
    for (const AnimationDiagnostic& diagnostic : diagnostics) {
        json.begin_object();
        json.field("severity", diagnostic_severity_text(diagnostic.severity));
        json.field("path", std::string_view{diagnostic.path});
        json.field("message", std::string_view{diagnostic.message});
        json.end_object();
    }
    json.end_array();
}

// Builds a result object string for a handler. The lambda receives the writer
// already inside the top-level object scope is NOT assumed; callers open it.
ServerResponse ok_result(const std::string& json) {
    return {.ok = true, .error = {}, .result_json = json};
}

ServerResponse error_result(std::string message) {
    return {.ok = false, .error = std::move(message), .result_json = {}};
}

ServerResponse handle_game_info(const GameInfo* game, const WindowedAppConfig& window) {
    std::ostringstream out;
    JsonWriter json(out, false);
    if (game) {
        write_game_info_json(json, *game);
    } else {
        GameInfo fallback;
        fallback.title = std::string(window.title);
        fallback.window = {
            .width = window.width,
            .height = window.height,
            .logical_width = window.logical_width,
            .logical_height = window.logical_height,
            .integer_scale = window.integer_scale,
            .resizable = window.resizable,
            .borderless = window.borderless,
        };
        write_game_info_json(json, fallback);
    }
    return ok_result(out.str());
}

ServerResponse handle_scene_stack(ServerContext& ctx) {
    SceneManager& scenes = ctx.scenes();
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("depth", scenes.depth());
    json.key("scenes").begin_array();
    const i32 top = scenes.depth() - 1;
    for (i32 i = 0; i < scenes.depth(); ++i) {
        const Scene* scene = scenes.at(i);
        Scene* mutable_scene = scenes.at_mut(i);
        json.begin_object();
        json.field("index", i);
        json.field("name", scene->name());
        json.field("is_top", i == top);
        json.field("is_overlay", scene->is_overlay());
        json.field("updates_below", scene->updates_below());
        json.field("has_world", mutable_scene && mutable_scene->world() != nullptr);
        json.end_object();
    }
    json.end_array();
    json.end_object();
    return ok_result(out.str());
}

ServerResponse handle_scene_current(ServerContext& ctx) {
    const Scene* scene = ctx.scenes().top();
    if (!scene) {
        return error_result("no active scene");
    }
    Scene* mutable_scene = ctx.scenes().top_mut();
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("name", scene->name());
    json.field("has_world", mutable_scene && mutable_scene->world() != nullptr);
    json.key("state").begin_object();
    scene->write_report(json);
    json.end_object();
    json.end_object();
    return ok_result(out.str());
}

ServerResponse handle_scene_actions(ServerContext& ctx) {
    const std::vector<AvailableInputAction> actions =
        ctx.scenes().available_actions(ctx.app().input().map());
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.key("actions").begin_array();
    for (const AvailableInputAction& action : actions) {
        json.begin_object();
        json.field("name", std::string_view{action.name});
        json.field("label", std::string_view{action.label});
        json.field("description", std::string_view{action.description});
        json.key("bindings");
        write_bindings_json(json, action.bindings);
        json.end_object();
    }
    json.end_array();
    json.end_object();
    return ok_result(out.str());
}

ServerResponse handle_world_snapshot(ServerContext& ctx, const JsonValue& params) {
    SceneManager& scenes = ctx.scenes();
    i32 index = scenes.depth() - 1;
    if (const JsonValue* scene_index = params.find("scene")) {
        index = static_cast<i32>(scene_index->as_int(index));
    } else {
        // The topmost scene with a world: overlays without one (a transition,
        // a pause menu's) show the game's world under them.
        while (index > 0 && scenes.at_mut(index) && !scenes.at_mut(index)->world()) {
            --index;
        }
    }
    Scene* scene = scenes.at_mut(index);
    if (!scene) {
        return error_result("no scene at index " + std::to_string(index));
    }
    EcsWorld* world = scene->world();
    if (!world) {
        return error_result("scene has no ECS world");
    }
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("scene", index);
    if (params.bool_at("raw", true)) {
        const flecs::string serialized = world->raw().to_json();
        const char* world_json = serialized.c_str();
        json.key("world").raw(world_json ? std::string_view{world_json} : std::string_view{"null"});
    }
    if (params.bool_at("kin", true)) {
        json.key("kin");
        write_kin_world_snapshot_json(json, *world, params.bool_at("events", true));
    }
    if (params.bool_at("animations", true)) {
        write_world_animation_snapshots(json, world->raw());
    }
    json.end_object();
    return ok_result(out.str());
}

ServerResponse handle_animation_diagnostics(ServerContext& ctx) {
    const Scene* scene = ctx.scenes().top();
    if (!scene) {
        return error_result("no active scene");
    }
    const AnimationAssetLibrary* library = scene->animation_library();
    if (!library) {
        return error_result("scene has no animation library");
    }

    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.key("diagnostics");
    write_animation_diagnostics(json, library->diagnostics());
    json.end_object();
    return ok_result(out.str());
}

ServerResponse handle_sim_tick(ServerContext& ctx, const JsonValue& params) {
    i64 count = params.int_at("count", 1);
    if (count < 0) {
        return error_result("count must be >= 0");
    }
    count = std::min(count, MAX_TICKS_PER_CALL);
    for (i64 i = 0; i < count && !ctx.report().failed; ++i) {
        ctx.step();
    }
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("frame", ctx.frame());
    json.field("ticked", count);
    json.field("failed", ctx.report().failed);
    if (ctx.report().failed) {
        json.field("failure_reason", std::string_view{ctx.report().failure_reason});
    }
    json.end_object();
    return ok_result(out.str());
}

ServerResponse handle_input_action(ServerContext& ctx, const JsonValue& params) {
    const std::string name = params.string_at("name");
    if (name.empty()) {
        return error_result("input.action requires a 'name'");
    }
    std::string mode = params.string_at("mode", "press");
    if (mode != "press" && mode != "hold" && mode != "release") {
        return error_result("mode must be 'press', 'hold', or 'release'");
    }
    ctx.queue_action(name, mode);
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("queued", std::string_view{name});
    json.field("mode", std::string_view{mode});
    json.end_object();
    return ok_result(out.str());
}

// Confine a client-supplied output path to the server's working directory.
// Rejects absolute paths and any path that escapes the root via "..". Returns
// the normalized in-root path, or std::nullopt with a populated error. The path
// is untrusted (it arrives over stdio/HTTP, and the HTTP transport is reachable
// cross-origin), so this is the boundary that keeps a screenshot command from
// writing a PNG to an arbitrary filesystem location.
std::optional<std::filesystem::path> confine_to_working_dir(std::string_view requested, std::string& error) {
    namespace fs = std::filesystem;
    if (requested.empty()) {
        error = "path must not be empty";
        return std::nullopt;
    }
    const fs::path candidate{requested};
    // Reject anything that is not a pure relative path: absolute paths, drive or
    // UNC roots (has_root_name), and root-relative paths like "/etc/x" that
    // is_absolute() does not flag on Windows (has_root_directory).
    if (candidate.is_absolute() || candidate.has_root_name() || candidate.has_root_directory()) {
        error = "path must be relative to the working directory";
        return std::nullopt;
    }
    std::error_code ec;
    const fs::path root = fs::current_path(ec);
    if (ec) {
        error = "could not resolve the working directory";
        return std::nullopt;
    }
    const fs::path normalized = (root / candidate).lexically_normal();
    const fs::path relative = normalized.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") {
        error = "path escapes the working directory";
        return std::nullopt;
    }
    return normalized;
}

ServerResponse handle_view_screenshot(ServerContext& ctx, const JsonValue& params) {
    const std::string requested = params.string_at("path", "kin-screenshot.png");
    std::string path_error;
    const std::optional<std::filesystem::path> resolved = confine_to_working_dir(requested, path_error);
    if (!resolved) {
        KIN_LOG_WARN_F("server",
                       "screenshot command rejected",
                       (LogFields{{.name = "path", .value = requested},
                                  {.name = "error", .value = path_error}}));
        return error_result("invalid screenshot path: " + path_error);
    }
    const std::string path = resolved->string();
    ctx.render_frame();
    if (!ctx.renderer().save_png(path)) {
        KIN_LOG_WARN_F("server",
                       "screenshot command failed",
                       (LogFields{{.name = "path", .value = path}}));
        return error_result("failed to capture screenshot (backend cannot read pixels)");
    }
    const Vec2i size = ctx.renderer().output_size();
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("path", std::string_view{path});
    json.field("width", size.x);
    json.field("height", size.y);
    json.end_object();
    KIN_LOG_INFO_F("server",
                   "screenshot command completed",
                   (LogFields{
                       {.name = "path", .value = path},
                       {.name = "width", .value = std::to_string(size.x)},
                       {.name = "height", .value = std::to_string(size.y)},
                   }));
    return ok_result(out.str());
}

// view.scene {path, content?}: the top scene shows the scene file `path` (in
// the working folder) in place of its own, read as it reads its own (Kin
// Studio's scene view: see Scene::view_scene).
ServerResponse handle_view_scene(ServerContext& ctx, const JsonValue& params) {
    Scene* scene = ctx.scenes().top_mut();
    if (!scene) {
        return error_result("no active scene");
    }
    std::string path_error;
    const std::optional<std::filesystem::path> file = confine_to_working_dir(params.string_at("path"), path_error);
    if (!file) {
        return error_result("invalid scene path: " + path_error);
    }
    const std::filesystem::path content = params.string_at("content");
    std::string error;
    if (!scene->view_scene(*file, content, error)) {
        KIN_LOG_WARN_F("server", "view.scene failed", (LogFields{{.name = "path", .value = file->string()}, {.name = "error", .value = error}}));
        return error_result(error);
    }
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("path", std::string_view{file->string()});
    json.end_object();
    return ok_result(out.str());
}

// view.frame {path, width, height, x, y, zoom, rotation}: the top scene drawn
// at `width` x `height` through a camera centred on world point (x, y), `zoom`
// screen units a world unit, turned `rotation` degrees; its pixels written to
// `path` (in the working folder) as they are: rows of RGBA bytes, top first.
// The camera stays for later frames (screenshots too).
ServerResponse handle_view_frame(ServerContext& ctx, const JsonValue& params) {
    Scene* scene = ctx.scenes().top_mut();
    if (!scene) {
        return error_result("no active scene");
    }
    const i64 width = params.int_at("width", 0);
    const i64 height = params.int_at("height", 0);
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        return error_result("view.frame needs a width and height of 1 to 8192");
    }
    const f64 zoom = params.number_at("zoom", 1.0);
    if (!(zoom > 0.0)) {
        return error_result("view.frame needs a zoom above 0");
    }
    std::string path_error;
    const std::optional<std::filesystem::path> file = confine_to_working_dir(params.string_at("path", "kin-view.rgba"), path_error);
    if (!file) {
        return error_result("invalid frame path: " + path_error);
    }
    const Vec2i size{static_cast<i32>(width), static_cast<i32>(height)};
    if (ctx.window().size() != size) {
        ctx.window().set_size(size);
        ctx.window().sync(); // sized later on some systems (X11)
    }
    Camera2D camera;
    camera.set_viewport({static_cast<f32>(width), static_cast<f32>(height)});
    camera.zoom = static_cast<f32>(zoom);
    camera.rotation = static_cast<f32>(params.number_at("rotation", 0.0));
    camera.look_at({static_cast<f32>(params.number_at("x", 0.0)), static_cast<f32>(params.number_at("y", 0.0))});
    if (!scene->set_view_camera(&camera)) {
        return error_result("the scene has no world to draw through another camera");
    }
    ctx.render_frame();
    std::vector<u8> pixels;
    Vec2i read{};
    const Vec2i points = ctx.window().size();
    if (!ctx.renderer().read_rgba(Rectf{0.0f, 0.0f, static_cast<f32>(points.x), static_cast<f32>(points.y)}, pixels, read) ||
        pixels.size() != static_cast<std::size_t>(read.x) * static_cast<std::size_t>(read.y) * 4u) {
        return error_result("failed to read the frame back (the backend cannot read pixels)");
    }
    if (read != size) {
        return error_result("the frame came out " + std::to_string(read.x) + "x" + std::to_string(read.y) + ", not " +
                            std::to_string(size.x) + "x" + std::to_string(size.y) + " (the window could not be sized)");
    }
    {
        std::ofstream out(*file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        if (!out) {
            return error_result("failed to write the frame to " + file->string());
        }
    }
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("path", std::string_view{file->string()});
    json.field("width", read.x);
    json.field("height", read.y);
    json.end_object();
    return ok_result(out.str());
}

ServerResponse handle_sim_reset(ServerContext& ctx, const JsonValue& params) {
    if (!ctx.can_reset()) {
        KIN_LOG_WARN("server", "sim.reset rejected");
        return error_result("sim.reset is not supported (no scene factory provided)");
    }
    std::optional<u64> seed;
    if (const JsonValue* value = params.find("seed")) {
        seed = static_cast<u64>(value->as_int());
    }
    ctx.reset(seed);
    KIN_LOG_INFO_F("server",
                   "sim.reset completed",
                   (LogFields{
                       {.name = "reseeded", .value = seed ? "true" : "false"},
                       {.name = "seed", .value = seed ? std::to_string(*seed) : ""},
                   }));
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("frame", ctx.frame());
    json.field("scene_depth", ctx.scenes().depth());
    json.field("reseeded", seed.has_value());
    json.end_object();
    return ok_result(out.str());
}

ServerResponse handle_server_status(ServerContext& ctx) {
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("frame", ctx.frame());
    json.field("fixed_dt", static_cast<f64>(ctx.fixed_dt()));
    json.field("scene_depth", ctx.scenes().depth());
    json.field("failed", ctx.report().failed);
    json.end_object();
    return ok_result(out.str());
}

ServerResponse handle_frame_timing(ServerContext& ctx, const JsonValue& params) {
    const std::deque<ServerContext::FrameTiming>& timings = ctx.frame_timings();
    const auto total = [](const ServerContext::FrameTiming& t) { return t.update_ms + t.render_ms + t.present_ms; };
    ServerContext::FrameTiming sum{};
    ServerContext::FrameTiming max{};
    f64 sum_total = 0.0;
    f64 max_total = 0.0;
    for (const ServerContext::FrameTiming& t : timings) {
        sum.update_ms += t.update_ms;
        sum.render_ms += t.render_ms;
        sum_total += total(t);
        max.update_ms = std::max(max.update_ms, t.update_ms);
        max.render_ms = std::max(max.render_ms, t.render_ms);
        max_total = std::max(max_total, total(t));
    }
    const f64 n = timings.empty() ? 1.0 : static_cast<f64>(timings.size());
    const ServerContext::FrameTiming last = timings.empty() ? ServerContext::FrameTiming{} : timings.back();
    const auto write = [](JsonWriter& json, std::string_view name, f64 update, f64 render, f64 total) {
        json.key(name).begin_object();
        json.field("update_ms", update);
        json.field("render_ms", render);
        json.field("total_ms", total);
        json.end_object();
    };

    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("frames", static_cast<u64>(timings.size()));
    json.field("window", static_cast<u64>(ServerContext::frame_timing_window));
    write(json, "last", last.update_ms, last.render_ms, total(last));
    write(json, "mean", sum.update_ms / n, sum.render_ms / n, sum_total / n);
    write(json, "max", max.update_ms, max.render_ms, max_total);
    // Each frame after `since` (a frame number; -1: all kept), oldest first:
    // what a tool plots, asking again with the last it has.
    if (const JsonValue* since = params.find("since")) {
        const i64 after = static_cast<i64>(since->as_number());
        json.key("history").begin_array();
        for (const ServerContext::FrameTiming& t : timings) {
            if (t.frame > after) {
                json.begin_object();
                json.field("frame", t.frame);
                json.field("update_ms", t.update_ms);
                json.field("render_ms", t.render_ms);
                json.field("present_ms", t.present_ms);
                json.field("interval_ms", t.interval_ms);
                json.end_object();
            }
        }
        json.end_array();
    }
    json.end_object();
    if (const JsonValue* reset = params.find("reset"); reset && reset->as_bool()) {
        ctx.clear_frame_timings();
    }
    return ok_result(out.str());
}

ServerResponse handle_ui_snapshot(ServerContext& ctx, const JsonValue& params) {
    (void)ctx;
    (void)params;
    return error_result("ui.snapshot is unavailable after the legacy UI module was removed");
}

ServerResponse handle_input_mouse(ServerContext& ctx, const JsonValue& params) {
    const JsonValue* x = params.find("x");
    const JsonValue* y = params.find("y");
    const JsonValue* dx = params.find("dx");
    const JsonValue* dy = params.find("dy");
    if ((!x || !y) && !dx && !dy) {
        return error_result("input.mouse requires numeric 'x' and 'y', or a motion 'dx' / 'dy'");
    }
    // Motion in window pixels, as relative mouse mode reports it (mouse look).
    const Vec2f motion{dx ? static_cast<f32>(dx->as_number()) : 0.0f, dy ? static_cast<f32>(dy->as_number()) : 0.0f};
    if (dx || dy) {
        ctx.queue_mouse_motion(motion);
    }
    Vec2f pos = ctx.app().input().mouse_pos();
    if (x && y) {
        pos = {static_cast<f32>(x->as_number()), static_cast<f32>(y->as_number())};
        // Coordinates are logical UI space by default (matching ui.snapshot bounds);
        // convert to window space for injection. "window" passes through unchanged.
        if (params.string_at("space", "logical") != "window") {
            pos = ctx.renderer().logical_to_window(pos);
        }
        ctx.queue_mouse_move(pos);
    }

    if (const JsonValue* button = params.find("button")) {
        const std::string name = button->is_string() ? button->as_string() : "left";
        const std::string mode = params.string_at("mode", "press");
        if (mode != "press" && mode != "hold" && mode != "release") {
            return error_result("mode must be 'press', 'hold', or 'release'");
        }
        ctx.queue_mouse_button(name, mode);
    }
    if (const JsonValue* wheel = params.find("wheel")) {
        ctx.queue_mouse_wheel(static_cast<f32>(wheel->as_number()));
    }

    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("queued", "mouse");
    json.key("at").begin_object();
    json.field("x", static_cast<f64>(pos.x));
    json.field("y", static_cast<f64>(pos.y));
    json.end_object();
    if (dx || dy) {
        json.key("motion").begin_object();
        json.field("dx", static_cast<f64>(motion.x));
        json.field("dy", static_cast<f64>(motion.y));
        json.end_object();
    }
    json.end_object();
    return ok_result(out.str());
}

std::optional<Key> parse_key_name(std::string_view name) {
    const auto lower = [](std::string_view value) {
        std::string out{value};
        for (char& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    };
    const std::string target = lower(name);
    for (i32 code = static_cast<i32>(Key::Unknown) + 1; code <= static_cast<i32>(LastKey); ++code) {
        const Key key = static_cast<Key>(code);
        if (lower(key_name(key)) == target) {
            return key;
        }
    }
    return std::nullopt;
}

ServerResponse handle_input_key(ServerContext& ctx, const JsonValue& params) {
    const std::string name = params.string_at("name");
    const std::optional<Key> key = parse_key_name(name);
    if (!key) {
        return error_result("input.key requires a key 'name', e.g. \"Enter\", \"Left\" or \"A\"");
    }
    const std::string mode = params.string_at("mode", "press");
    if (mode != "press" && mode != "hold" && mode != "release") {
        return error_result("mode must be 'press', 'hold', or 'release'");
    }
    KeyModifiers modifiers = KeyModifiers::None;
    if (const JsonValue* list = params.find("modifiers")) {
        if (!list->is_array()) {
            return error_result("modifiers must be an array of \"shift\", \"ctrl\" or \"alt\"");
        }
        for (const JsonValue& item : list->items()) {
            const std::string value = item.is_string() ? item.as_string() : std::string{};
            if (value == "shift") {
                modifiers = modifiers | KeyModifiers::Shift;
            } else if (value == "ctrl") {
                modifiers = modifiers | KeyModifiers::Ctrl;
            } else if (value == "alt") {
                modifiers = modifiers | KeyModifiers::Alt;
            } else {
                return error_result("modifiers must be an array of \"shift\", \"ctrl\" or \"alt\"");
            }
        }
    }
    ctx.queue_key(*key, mode, modifiers);
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("queued", key_name(*key));
    json.field("mode", std::string_view{mode});
    json.end_object();
    return ok_result(out.str());
}

// A file dropped on the window, as from the desktop: Input::take_dropped_files
// has it at the next tick. At `x`, `y` (logical, or "space": "window"), or
// where the pointer is.
ServerResponse handle_input_drop(ServerContext& ctx, const JsonValue& params) {
    const JsonValue* path = params.find("path");
    if (!path || !path->is_string() || path->as_string().empty()) {
        return error_result("input.drop requires a string 'path'");
    }
    Vec2f pos = ctx.app().input().mouse_pos();
    const JsonValue* x = params.find("x");
    const JsonValue* y = params.find("y");
    if (x && y && x->is_number() && y->is_number()) {
        pos = {static_cast<f32>(x->as_number()), static_cast<f32>(y->as_number())};
        if (params.string_at("space", "logical") != "window") {
            pos = ctx.renderer().logical_to_window(pos);
        }
    }
    ctx.queue_drop(path->as_string(), pos);
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("queued", "drop");
    json.field("path", std::string_view{path->as_string()});
    json.end_object();
    return ok_result(out.str());
}

ServerResponse handle_input_text(ServerContext& ctx, const JsonValue& params) {
    const JsonValue* text = params.find("text");
    if (!text || !text->is_string()) {
        return error_result("input.text requires a string 'text'");
    }
    ctx.queue_text(text->as_string());
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("queued", "text");
    json.field("text", std::string_view{text->as_string()});
    json.end_object();
    return ok_result(out.str());
}

} // namespace

ServerContext::ServerContext(App& app,
                             Window& window,
                             Renderer2D& renderer,
                             SceneManager& scenes,
                             WindowedAppConfig window_config,
                             RngKey rng,
                             f32 fixed_dt,
                             bool render,
                             AssetServer* asset_server,
                             Renderer3D* renderer3d)
    : _app(app), _window(window), _renderer(renderer), _scenes(scenes),
      _window_config(std::move(window_config)), _rng(rng), _fixed_dt(fixed_dt), _render(render),
      _asset_server(asset_server), _renderer3d(renderer3d) {
}

namespace {

std::optional<MouseButton> parse_mouse_button(std::string_view name) {
    if (name == "left" || name.empty()) return MouseButton::Left;
    if (name == "middle") return MouseButton::Middle;
    if (name == "right") return MouseButton::Right;
    if (name == "x1") return MouseButton::X1;
    if (name == "x2") return MouseButton::X2;
    return std::nullopt;
}

} // namespace

void ServerContext::queue_action(std::string name, std::string mode) {
    _pending.push_back({PendingInput::Kind::Action, std::move(name), std::move(mode), {}, 0.0f});
}

void ServerContext::queue_mouse_move(Vec2f window_pos) {
    _pending.push_back({PendingInput::Kind::MouseMove, {}, {}, window_pos, 0.0f});
}

void ServerContext::queue_mouse_button(std::string button, std::string mode) {
    _pending.push_back({PendingInput::Kind::MouseButton, std::move(button), std::move(mode), {}, 0.0f});
}

void ServerContext::queue_mouse_motion(Vec2f delta) {
    _pending.push_back({PendingInput::Kind::MouseMotion, {}, {}, delta, 0.0f});
}

void ServerContext::queue_mouse_wheel(f32 delta) {
    _pending.push_back({PendingInput::Kind::Wheel, {}, {}, {}, delta});
}

void ServerContext::queue_text(std::string text) {
    _pending.push_back({PendingInput::Kind::Text, std::move(text), {}, {}, 0.0f});
}

void ServerContext::queue_key(Key key, std::string mode, KeyModifiers modifiers) {
    _pending.push_back({PendingInput::Kind::Key, {}, std::move(mode), {}, 0.0f, key, modifiers});
}

void ServerContext::queue_drop(std::string path, Vec2f window_pos) {
    _pending.push_back({PendingInput::Kind::Drop, std::move(path), {}, window_pos, 0.0f});
}

void ServerContext::step() {
    Input& input = _app.input();
    input.begin_frame();

    // Release last step's taps now, after begin_frame, so this step sees the
    // release edge: ui2 completes a click on it, as with a real mouse.
    for (const std::string& name : _tapped_actions) {
        input.set_action_held(name, false);
    }
    for (const MouseButton button : _tapped_buttons) {
        input.set_mouse_held(button, false);
    }
    for (const auto& [key, modifiers] : _tapped_keys) {
        input.set_key_released(key);
        input.set_modifier_held(modifiers, false);
    }
    _tapped_actions.clear();
    _tapped_buttons.clear();
    _tapped_keys.clear();

    for (const PendingInput& in : _pending) {
        switch (in.kind) {
        case PendingInput::Kind::Action:
            if (in.mode == "hold") {
                input.set_action_held(in.text, true);
            } else if (in.mode == "release") {
                input.set_action_held(in.text, false);
            } else {
                input.set_action_pressed(in.text);
                _tapped_actions.push_back(in.text);
            }
            break;
        case PendingInput::Kind::MouseMove:
            input.set_mouse_pos(in.pos, _window.id());
            break;
        case PendingInput::Kind::MouseButton:
            if (const std::optional<MouseButton> button = parse_mouse_button(in.text)) {
                if (in.mode == "hold") {
                    input.set_mouse_held(*button, true);
                } else if (in.mode == "release") {
                    input.set_mouse_held(*button, false);
                } else {
                    input.set_mouse_pressed(*button);
                    _tapped_buttons.push_back(*button);
                }
            }
            break;
        case PendingInput::Kind::Wheel:
            input.set_mouse_wheel_y(in.value);
            break;
        case PendingInput::Kind::MouseMotion:
            input.set_mouse_motion(in.pos);
            break;
        case PendingInput::Kind::Text:
            input.set_text_input(in.text);
            break;
        case PendingInput::Kind::Drop:
            input.add_dropped_file({.path = in.text, .pos = in.pos, .window = _window.id()});
            break;
        case PendingInput::Kind::Key:
            if (in.mode == "release") {
                input.set_key_released(in.key);
                input.set_modifier_held(in.modifiers, false);
            } else {
                input.set_modifier_held(in.modifiers, true);
                input.set_key_pressed(in.key);
                if (in.mode == "press") {
                    _tapped_keys.emplace_back(in.key, in.modifiers);
                }
            }
            break;
        }
    }
    _pending.clear();

    SceneContext scene_ctx = make_context();
    runtime_detail::pump_scene_assets(_asset_server, _render);
    using clock = std::chrono::steady_clock;
    const auto update_start = clock::now();
    _scenes.update(scene_ctx);
    const auto render_start = clock::now();
    auto present_start = render_start;
    if (_render) {
        _scenes.render(scene_ctx);
        present_start = clock::now();
        _renderer.present();
    }
    const auto end = clock::now();
    const auto ms = [](clock::duration d) { return std::chrono::duration<f64, std::milli>(d).count(); };
    record_frame_timing({
        .update_ms = ms(render_start - update_start),
        .render_ms = ms(present_start - render_start),
        .present_ms = ms(end - present_start),
    });
    ++_frame;
}

void ServerContext::record_frame_timing(FrameTiming timing) {
    timing.frame = _timed_frames++;
    _timings.push_back(timing);
    if (_timings.size() > frame_timing_window) {
        _timings.pop_front();
    }
}

SceneContext ServerContext::make_context() {
    return SceneContext{
        .app = _app,
        .window = _window,
        .renderer = _renderer,
        .input = _app.input(),
        .scenes = _scenes,
        .dt = _fixed_dt,
        .is_top = true,
        .rng = _rng,
        .report = &_report,
        .renderer3d = _renderer3d,
    };
}

void ServerContext::realize_scenes() {
    SceneContext scene_ctx = make_context();
    _scenes.flush_pending(scene_ctx);
}

void ServerContext::shutdown_scenes() {
    SceneContext scene_ctx = make_context();
    _scenes.shutdown(scene_ctx);
}

void ServerContext::render_frame() {
    // An extra render outside a step (a screenshot): the step already handled this
    // frame's input, so a scene that reads input in render() must not see it again.
    _app.input().consume_frame_edges();
    SceneContext scene_ctx = make_context();
    _scenes.render(scene_ctx);
}

void ServerContext::reset(std::optional<u64> seed) {
    _frame = 0;
    if (seed) {
        _rng = make_key(*seed);
    }
    _report = {};
    _pending.clear();
    _timings.clear();

    SceneContext scene_ctx = make_context();
    _scenes.clear();
    _scenes.flush_pending(scene_ctx);
    if (_reset_scenes) {
        _reset_scenes(_scenes);
    }
    _scenes.flush_pending(scene_ctx);
}

ServerResponse dispatch_server_command(ServerContext& ctx,
                                       const GameInfo* game,
                                       std::string_view method,
                                       const JsonValue& params) {
    if (method == "game.info") {
        return handle_game_info(game, ctx.window_config());
    }
    if (method == "scene.stack") {
        return handle_scene_stack(ctx);
    }
    if (method == "scene.current") {
        return handle_scene_current(ctx);
    }
    if (method == "scene.actions") {
        return handle_scene_actions(ctx);
    }
    if (method == "world.snapshot") {
        return handle_world_snapshot(ctx, params);
    }
    if (method == "animation.diagnostics") {
        return handle_animation_diagnostics(ctx);
    }
    if (method == "sim.tick") {
        return handle_sim_tick(ctx, params);
    }
    if (method == "input.action") {
        return handle_input_action(ctx, params);
    }
    if (method == "input.mouse") {
        return handle_input_mouse(ctx, params);
    }
    if (method == "input.text") {
        return handle_input_text(ctx, params);
    }
    if (method == "input.drop") {
        return handle_input_drop(ctx, params);
    }
    if (method == "input.key") {
        return handle_input_key(ctx, params);
    }
    if (method == "ui.snapshot") {
        return handle_ui_snapshot(ctx, params);
    }
    if (method == "view.screenshot") {
        return handle_view_screenshot(ctx, params);
    }
    if (method == "view.scene") {
        return handle_view_scene(ctx, params);
    }
    if (method == "view.frame") {
        return handle_view_frame(ctx, params);
    }
    if (method == "sim.reset") {
        return handle_sim_reset(ctx, params);
    }
    if (method == "server.status") {
        return handle_server_status(ctx);
    }
    if (method == "frame.timing") {
        return handle_frame_timing(ctx, params);
    }
    if (method == "ping") {
        return ok_result("{\"pong\":true}");
    }
    return error_result("unknown method: " + std::string(method));
}

namespace {

void write_envelope(std::ostream& out, const JsonValue* id, const ServerResponse& response) {
    std::ostringstream buffer;
    JsonWriter json(buffer, false);
    json.begin_object();
    json.key("id");
    if (id) {
        write_json(json, *id);
    } else {
        json.value_null();
    }
    if (response.ok) {
        json.key("result").raw(response.result_json);
    } else {
        json.key("error").begin_object();
        json.field("message", std::string_view{response.error});
        json.end_object();
    }
    json.end_object();
    out << buffer.str() << '\n';
    out.flush();
}

// Parses one request line and dispatches it. Returns false when the client asked
// the server to stop.
std::string_view strip_bom(std::string_view line) {
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.remove_prefix(3);
    }
    return line;
}

bool handle_line(ServerContext& ctx,
                 const GameInfo* game,
                 const std::string& line,
                 std::ostream& out) {
    const JsonParseResult parsed = parse_json(strip_bom(line));
    if (!parsed.ok()) {
        KIN_LOG_WARN_F("server",
                       "request rejected",
                       (LogFields{
                           {.name = "reason", .value = "invalid JSON"},
                           {.name = "error", .value = parsed.error},
                       }));
        write_envelope(out, nullptr, error_result("invalid JSON: " + parsed.error));
        return true;
    }

    const JsonValue& root = *parsed.value;
    const JsonValue* id = root.find("id");
    const std::string method = root.string_at("method");
    if (method.empty()) {
        KIN_LOG_WARN("server", "request rejected: missing method");
        write_envelope(out, id, error_result("request missing 'method'"));
        return true;
    }

    if (method == "server.shutdown") {
        KIN_LOG_INFO("server", "server shutdown requested");
        write_envelope(out, id, ok_result("{\"bye\":true}"));
        return false;
    }

    KIN_LOG_TRACE_F("server",
                    "server command",
                    (LogFields{{.name = "method", .value = method}}));
    static const JsonValue empty_params;
    const JsonValue* params = root.find("params");
    const ServerResponse response =
        dispatch_server_command(ctx, game, method, params ? *params : empty_params);
    if (!response.ok) {
        KIN_LOG_WARN_F("server",
                       "server command rejected",
                       (LogFields{
                           {.name = "method", .value = method},
                           {.name = "error", .value = response.error},
                       }));
    }
    write_envelope(out, id, response);
    return true;
}

int run_driven(ServerContext& ctx, const GameInfo* game, std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }
        if (!handle_line(ctx, game, line, out)) {
            break;
        }
    }
    KIN_LOG_INFO_F("server",
                   "stdio server stopped",
                   (LogFields{
                       {.name = "mode", .value = "driven"},
                       {.name = "failed", .value = ctx.report().failed ? "true" : "false"},
                   }));
    return ctx.report().failed ? 1 : 0;
}

int run_realtime(ServerContext& ctx, const GameInfo* game, std::istream& in, std::ostream& out) {
    // Shared state lives in a shared_ptr so that if the loop exits on a shutdown
    // command while the reader is still blocked in getline, detaching the reader
    // is safe: it keeps this alive until stdin reaches EOF.
    struct Shared {
        std::mutex mutex;
        std::deque<std::string> queue;
        bool reading = true;
    };
    auto shared = std::make_shared<Shared>();

    std::thread reader([shared, &in] {
        std::string line;
        while (std::getline(in, line)) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->queue.push_back(std::move(line));
            line.clear();
        }
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->reading = false;
    });

    using clock = std::chrono::steady_clock;
    const auto frame_duration =
        std::chrono::duration_cast<clock::duration>(std::chrono::duration<f32>(ctx.fixed_dt()));
    bool running = true;
    bool reader_finished = false;
    while (running) {
        const auto frame_start = clock::now();
        ctx.step();

        std::deque<std::string> drained;
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            drained.swap(shared->queue);
            reader_finished = !shared->reading;
        }
        for (const std::string& line : drained) {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
                continue;
            }
            if (!handle_line(ctx, game, line, out)) {
                running = false;
                break;
            }
        }
        if (reader_finished && drained.empty()) {
            KIN_LOG_INFO("server", "realtime server input EOF");
            running = false;
        }

        std::this_thread::sleep_until(frame_start + frame_duration);
    }

    // Join cleanly when the reader already hit EOF; otherwise (shutdown command)
    // detach — the shared_ptr copy in the reader keeps its state alive.
    if (reader.joinable()) {
        if (reader_finished) {
            reader.join();
        } else {
            reader.detach();
        }
    }
    KIN_LOG_INFO_F("server",
                   "stdio server stopped",
                   (LogFields{
                       {.name = "mode", .value = "realtime"},
                       {.name = "failed", .value = ctx.report().failed ? "true" : "false"},
                   }));
    return ctx.report().failed ? 1 : 0;
}

struct HttpState {
    ServerContext* ctx;
    const GameInfo* game;
    bool stop = false;
};

void append_error_json(ecs_http_reply_t* reply, std::string_view message) {
    std::ostringstream buffer;
    JsonWriter json(buffer, false);
    json.begin_object();
    json.key("error").begin_object();
    json.field("message", message);
    json.end_object();
    json.end_object();
    ecs_strbuf_appendstr(&reply->body, buffer.str().c_str());
}

// flecs invokes this for each request while we call ecs_http_server_dequeue from
// the main loop, so it runs on the main thread: no locking around game state.
// The request path is the method; the request body (if any) is the params.
bool http_reply(const ecs_http_request_t* request, ecs_http_reply_t* reply, void* ctx) {
    auto* state = static_cast<HttpState*>(ctx);

    // Permissive CORS so the endpoints are reachable from a browser console.
    ecs_strbuf_appendstr(&reply->headers, "Access-Control-Allow-Origin: *\r\n");
    if (request->method == EcsHttpOptions) {
        reply->code = 200;
        return true;
    }

    std::string method = request->path ? request->path : "";
    while (!method.empty() && method.front() == '/') {
        method.erase(method.begin());
    }
    if (method.empty()) {
        reply->code = 400;
        KIN_LOG_WARN("server", "http request rejected: missing method");
        append_error_json(reply, "missing method in request path");
        return true;
    }

    JsonValue params;
    if (request->body && request->body[0] != '\0') {
        const JsonParseResult parsed = parse_json(strip_bom(request->body));
        if (!parsed.ok()) {
            reply->code = 400;
            KIN_LOG_WARN_F("server",
                           "http request rejected",
                           (LogFields{
                               {.name = "method", .value = method},
                               {.name = "reason", .value = "invalid JSON"},
                               {.name = "error", .value = parsed.error},
                           }));
            append_error_json(reply, "invalid JSON body: " + parsed.error);
            return true;
        }
        params = *parsed.value;
    }

    if (method == "server.shutdown") {
        state->stop = true;
        KIN_LOG_INFO("server", "http server shutdown requested");
        ecs_strbuf_appendstr(&reply->body, "{\"bye\":true}");
        return true;
    }

    KIN_LOG_TRACE_F("server",
                    "http server command",
                    (LogFields{{.name = "method", .value = method}}));
    const ServerResponse response =
        dispatch_server_command(*state->ctx, state->game, method, params);
    if (response.ok) {
        reply->code = 200;
        ecs_strbuf_appendstr(&reply->body, response.result_json.c_str());
    } else {
        reply->code = response.error.starts_with("unknown method") ? 404 : 400;
        KIN_LOG_WARN_F("server",
                       "http server command rejected",
                       (LogFields{
                           {.name = "method", .value = method},
                           {.name = "error", .value = response.error},
                       }));
        append_error_json(reply, response.error);
    }
    return true;
}

int run_http(ServerContext& ctx, const GameInfo* game, ServerMode mode, u16 port, std::ostream& log) {
    // The HTTP server needs the flecs OS API (threads/sockets) initialized even
    // for non-ECS games that never created a world.
    ecs_os_init();

    HttpState state{&ctx, game, false};
    ecs_http_server_desc_t desc{};
    desc.callback = http_reply;
    desc.ctx = &state;
    desc.port = port;
    desc.ipaddr = "127.0.0.1"; // this machine only: whoever reaches the server drives the game
    desc.cache_timeout = 0.0; // state changes every tick; never serve cached replies

    ecs_http_server_t* server = ecs_http_server_init(&desc);
    if (!server) {
        KIN_LOG_ERROR_F("server",
                        "http server initialization failed",
                        (LogFields{{.name = "port", .value = std::to_string(port)}}));
        log << "kin: failed to initialize HTTP server\n";
        ecs_os_fini();
        return 1;
    }
    if (ecs_http_server_start(server) != 0) {
        KIN_LOG_ERROR_F("server",
                        "http server start failed",
                        (LogFields{{.name = "port", .value = std::to_string(port)}}));
        log << "kin: failed to start HTTP server on port " << port << "\n";
        ecs_http_server_fini(server);
        ecs_os_fini();
        return 1;
    }
    KIN_LOG_INFO_F("server",
                   "http server started",
                   (LogFields{
                       {.name = "port", .value = std::to_string(port)},
                       {.name = "mode", .value = std::string{server_mode_name(mode)}},
                   }));
    log << "kin: HTTP server listening on port " << port
        << " (mode=" << server_mode_name(mode) << ")\n";
    log.flush();

    using clock = std::chrono::steady_clock;
    const auto frame_duration =
        std::chrono::duration_cast<clock::duration>(std::chrono::duration<f32>(ctx.fixed_dt()));
    while (!state.stop) {
        const auto frame_start = clock::now();
        if (mode == ServerMode::Realtime) {
            ctx.step();
        }
        ecs_http_server_dequeue(server, static_cast<ecs_ftime_t>(ctx.fixed_dt()));
        if (mode == ServerMode::Realtime) {
            std::this_thread::sleep_until(frame_start + frame_duration);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ecs_http_server_stop(server);
    ecs_http_server_fini(server);
    ecs_os_fini();
    KIN_LOG_INFO_F("server",
                   "http server stopped",
                   (LogFields{{.name = "failed", .value = ctx.report().failed ? "true" : "false"}}));
    return ctx.report().failed ? 1 : 0;
}

} // namespace

ServerMode parse_server_mode(std::string_view text) {
    return text == "realtime" ? ServerMode::Realtime : ServerMode::Driven;
}

ServerTransport parse_server_transport(std::string_view text) {
    return text == "http" ? ServerTransport::Http : ServerTransport::Stdio;
}

int run_scene_server(const ServerConfig& config, SceneManager& scenes) {
#ifndef KIN_EXPERIMENTAL_3D
    if (config.rendering_3d) {
        KIN_LOG_ERROR("server", "3D rendering requested in a 2D-only build");
        return 1;
    }
#endif
    std::istream& in = config.in ? *config.in : std::cin;
    std::ostream& out = config.out ? *config.out : std::cout;

    const f32 fixed_dt = config.window.fixed_dt > 0.0f ? config.window.fixed_dt : default_fixed_dt;
    KIN_LOG_INFO_F("server",
                   "scene server starting",
                   (LogFields{
                       {.name = "transport", .value = std::string{server_transport_name(config.transport)}},
                       {.name = "mode", .value = std::string{server_mode_name(config.mode)}},
                       {.name = "port", .value = std::to_string(config.port)},
                       {.name = "seed", .value = std::to_string(config.seed)},
                       {.name = "render", .value = config.render ? "true" : "false"},
                       {.name = "width", .value = std::to_string(config.window.width)},
                       {.name = "height", .value = std::to_string(config.window.height)},
                   }));
    App app{{
        .mode = AppMode::Headless,
        .fixed_dt = fixed_dt,
        .max_frame_time = config.window.max_frame_time,
        .max_steps = config.window.max_steps,
    }};

    Window& window = app.create_window({
        .title = config.window.title,
        .width = config.window.width,
        .height = config.window.height,
        .resizable = config.window.resizable,
        .maximized = config.window.maximized,
        .fullscreen = config.window.fullscreen,
        .hidden = true,
        .borderless = config.window.borderless,
    });

    // Software unless KIN_RENDER_BACKEND=gpu asks for the backend the game ships on.
    Renderer2D renderer{make_render_backend(window, false, false)};
    if (config.window.color_space != ColorSpace::Gamma || config.window.hdr) {
        renderer.set_color_space(config.window.color_space, config.window.hdr);
    }
    if (config.window.logical_width > 0 && config.window.logical_height > 0) {
        if (config.window.integer_scale) {
            renderer.set_integer_logical_size(config.window.logical_width, config.window.logical_height);
        } else {
            renderer.set_logical_size(config.window.logical_width, config.window.logical_height);
        }
    }
    if (config.window.input_map) {
        app.input().set_map(*config.window.input_map);
    }

    Renderer3D* scene_renderer3d = nullptr;
#ifdef KIN_EXPERIMENTAL_3D
    std::unique_ptr<Renderer3D> renderer3d;
    if (config.rendering_3d) {
        renderer3d = std::make_unique<Renderer3D>(renderer);
        scene_renderer3d = renderer3d.get();
    }
#endif
    ServerContext ctx{app,
                      window,
                      renderer,
                      scenes,
                      config.window,
                      make_key(config.seed),
                      fixed_dt,
                      config.render,
                      config.asset_server,
                      scene_renderer3d};
    ctx.set_reset_factory(config.reset_scenes);

    // Realize the initial scene stack (apply pending push + on_enter) without
    // advancing gameplay, so read endpoints work before the first tick.
    ctx.realize_scenes();

    int code = 0;
    if (config.transport == ServerTransport::Http) {
        code = run_http(ctx, config.game, config.mode, config.port, std::cerr);
    } else if (config.mode == ServerMode::Realtime) {
        code = run_realtime(ctx, config.game, in, out);
    } else {
        code = run_driven(ctx, config.game, in, out);
    }
    // The scene stack belongs to the caller and would otherwise be destroyed
    // after `renderer` — freeing GPU resources into a dead device.
    ctx.shutdown_scenes();
    return code;
}

struct LiveInspector::Shared {
    std::mutex mutex;
    std::condition_variable arrived; // a line, or the end of them
    std::deque<std::string> lines;
    bool reading = true;
};

// Answers on their way to the tool, in order.
struct LiveInspector::Outbox {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> answers;
    bool writing = false; // one is being written
    bool closing = false;
};

namespace {

// stdout's lock while an answer is written to std::cout: the game's own prints
// to it (through stdio, as std::cout's are) wait, not landing inside the answer.
class StdoutLock {
public:
    explicit StdoutLock(bool lock) : _locked(lock) {
        if (_locked) {
#if defined(_WIN32)
            _lock_file(stdout);
#else
            flockfile(stdout);
#endif
        }
    }
    ~StdoutLock() {
        if (_locked) {
#if defined(_WIN32)
            _unlock_file(stdout);
#else
            funlockfile(stdout);
#endif
        }
    }
    StdoutLock(const StdoutLock&) = delete;
    StdoutLock& operator=(const StdoutLock&) = delete;

private:
    bool _locked;
};

ServerResponse set_debug_breakpoints(const JsonValue& params) {
    ScriptDebugger::Breakpoints breakpoints;
    const JsonValue* files = params.find("files");
    if (files && !files->is_object()) {
        return error_result("files must be an object of line lists");
    }
    i64 count = 0;
    if (files) {
        for (const auto& [file, lines] : files->members()) {
            std::set<i32>& set = breakpoints[file];
            for (const JsonValue& line : lines.items()) {
                if (line.is_number() && line.as_int() > 0) {
                    set.insert(static_cast<i32>(line.as_int()));
                }
            }
            count += static_cast<i64>(set.size());
        }
    }
    script_debugger().set_breakpoints(std::move(breakpoints));
    return ok_result("{\"breakpoints\":" + std::to_string(count) + "}");
}

// The answer to what the reader does itself as it reads it (debug.breakpoints
// and debug.pause: the debugger's own, taken at once, so that a game's first
// update already stops), else nullopt.
std::optional<std::string> answered_on_reading(std::string_view line) {
    if (line.find("\"debug.") == std::string_view::npos) {
        return std::nullopt; // not parsed: most lines are not
    }
    const JsonParseResult parsed = parse_json(strip_bom(line));
    if (!parsed.ok()) {
        return std::nullopt;
    }
    const std::string method = parsed.value->string_at("method");
    if (method != "debug.breakpoints" && method != "debug.pause") {
        return std::nullopt;
    }
    ServerResponse response;
    if (method == "debug.pause") {
        script_debugger().pause();
        response = ok_result("{\"stopped\":false}");
    } else {
        static const JsonValue empty_params;
        const JsonValue* given = parsed.value->find("params");
        response = set_debug_breakpoints(given ? *given : empty_params);
    }
    std::ostringstream answer;
    write_envelope(answer, parsed.value->find("id"), response);
    return std::move(answer).str();
}

} // namespace

LiveInspector::LiveInspector(std::istream& in, std::ostream& out)
    : _shared(std::make_shared<Shared>()),
      _outbox(std::make_shared<Outbox>()),
      _out(out) {
    // The reader holds the shared state, so it can outlive this when it is
    // still blocked reading as the game ends (it is then left behind).
    _reader = std::thread([shared = _shared, outbox = _outbox, &in] {
        std::string line;
        while (std::getline(in, line)) {
            if (std::optional<std::string> answer = answered_on_reading(line)) {
                {
                    std::lock_guard<std::mutex> lock(outbox->mutex);
                    outbox->answers.push_back(std::move(*answer));
                }
                outbox->changed.notify_all();
                line.clear();
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->lines.push_back(std::move(line));
            }
            shared->arrived.notify_all();
            line.clear();
        }
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->reading = false;
        }
        shared->arrived.notify_all();
    });
    // The writer, too, holds what it shares: blocked by a tool that stopped
    // reading as the game ends, it is left behind.
    _writer = std::thread([outbox = _outbox, &out = _out] {
        const bool to_stdout = &out == &std::cout;
        std::unique_lock<std::mutex> lock(outbox->mutex);
        for (;;) {
            outbox->changed.wait(lock, [&] { return !outbox->answers.empty() || outbox->closing; });
            if (outbox->answers.empty()) {
                return; // closing, all written
            }
            const std::string answer = std::move(outbox->answers.front());
            outbox->answers.pop_front();
            outbox->writing = true;
            lock.unlock();
            {
                const StdoutLock stdout_lock{to_stdout};
                out << answer;
                out.flush();
            }
            lock.lock();
            outbox->writing = false;
            outbox->changed.notify_all();
        }
    });
    script_debugger().set_waiter([this](ScriptDebugger&) { return wait_stopped(); });
    KIN_LOG_INFO("server", "inspector listening on stdin");
}

LiveInspector::~LiveInspector() {
    script_debugger().set_waiter({});
    script_debugger().set_breakpoints({});
    if (_writer.joinable()) {
        // What is left goes out, unless the tool has stopped reading.
        std::unique_lock<std::mutex> lock(_outbox->mutex);
        _outbox->closing = true;
        _outbox->changed.notify_all();
        const bool written = _outbox->changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return _outbox->answers.empty() && !_outbox->writing;
        });
        lock.unlock();
        if (written) {
            _writer.join();
        } else {
            _writer.detach();
        }
    }
    if (!_reader.joinable()) {
        return;
    }
    bool reading = false;
    {
        std::lock_guard<std::mutex> lock(_shared->mutex);
        reading = _shared->reading;
    }
    if (reading) {
        _reader.detach();
    } else {
        _reader.join();
    }
}

void LiveInspector::poll(ServerContext& ctx, const GameInfo* game) {
    _ctx = &ctx;
    std::deque<std::string> lines;
    bool reading = true;
    {
        std::lock_guard<std::mutex> lock(_shared->mutex);
        lines.swap(_shared->lines);
        reading = _shared->reading;
    }
    for (const std::string& line : lines) {
        if (line.find_first_not_of(" \t\r\n") != std::string::npos) {
            answer(ctx, game, line);
        }
    }
    if (!reading && !_finished) {
        // Nobody is left to resume it.
        _finished = true;
        _paused = false;
        _steps = 0;
        KIN_LOG_INFO("server", "inspector input closed");
    }
}

void LiveInspector::flush() {
    std::unique_lock<std::mutex> lock(_outbox->mutex);
    _outbox->changed.wait(lock, [&] { return _outbox->answers.empty() && !_outbox->writing; });
}

void LiveInspector::send(const JsonValue* id, const ServerResponse& response) {
    std::ostringstream line;
    write_envelope(line, id, response);
    {
        std::lock_guard<std::mutex> lock(_outbox->mutex);
        _outbox->answers.push_back(std::move(line).str());
    }
    _outbox->changed.notify_all();
}

bool LiveInspector::admit_update() {
    if (_paused) {
        if (_steps <= 0) {
            return false;
        }
        --_steps;
    }
    ++_frame;
    return true;
}

void LiveInspector::answer(ServerContext& ctx, const GameInfo* game, std::string_view line) {
    const JsonParseResult parsed = parse_json(strip_bom(line));
    if (!parsed.ok()) {
        send(nullptr, error_result("invalid JSON: " + parsed.error));
        return;
    }
    const JsonValue& root = *parsed.value;
    const JsonValue* id = root.find("id");
    const std::string method = root.string_at("method");
    static const JsonValue empty_params;
    const JsonValue* given = root.find("params");
    const JsonValue& params = given ? *given : empty_params;
    const auto status = [&] { return this->status(false); };

    ServerResponse response;
    if (method.empty()) {
        response = error_result("request missing 'method'");
    } else if (method == "server.shutdown") {
        KIN_LOG_INFO("server", "inspector asked the game to quit");
        ctx.app().quit();
        response = ok_result("{\"bye\":true}");
    } else if (method == "server.status") {
        response = status();
    } else if (method == "sim.pause") {
        _paused = true;
        _steps = 0;
        response = status();
    } else if (method == "sim.resume") {
        _paused = false;
        _steps = 0;
        response = status();
    } else if (method == "sim.step") {
        const i64 count = params.int_at("count", 1);
        if (count < 0) {
            response = error_result("count must be >= 0");
        } else {
            _paused = true;
            _steps += std::min(count, MAX_TICKS_PER_CALL);
            response = status();
        }
    } else if (method.starts_with("debug.")) {
        std::optional<ScriptResume> resume;
        response = answer_debug(method, params, false, resume);
    } else if (method == "sim.tick" || method == "sim.reset" || method.starts_with("input.") ||
               method.starts_with("view.") || method == "ui.snapshot") {
        response = error_result(method + " drives a headless server (--server); this game runs in its window");
    } else {
        response = dispatch_server_command(ctx, game, method, params);
    }
    send(id, response);
}

ServerResponse LiveInspector::status(bool stopped) const {
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("frame", _frame);
    json.field("paused", _paused);
    json.field("stopped", stopped);
    if (_ctx) {
        json.field("fixed_dt", static_cast<f64>(_ctx->fixed_dt()));
        json.field("scene_depth", _ctx->scenes().depth());
    }
    json.end_object();
    return ok_result(out.str());
}

namespace {

void write_script_value(JsonWriter& json, const ScriptValue& value) {
    json.begin_object();
    json.field("name", value.name);
    json.field("value", value.value);
    json.field("kind", value.kind);
    if (!value.fields.empty()) {
        json.key("fields").begin_array();
        for (const ScriptValue& field : value.fields) {
            write_script_value(json, field);
        }
        json.end_array();
    }
    if (value.more) {
        json.field("more", true);
    }
    json.end_object();
}

std::string debug_state_json(const std::optional<ScriptStop>& stop) {
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    json.field("stopped", stop.has_value());
    if (stop) {
        json.field("serial", stop->serial);
        json.field("reason", stop->reason);
        json.key("frames").begin_array();
        for (const ScriptFrame& frame : stop->frames) {
            json.begin_object();
            json.field("function", frame.function);
            json.field("file", frame.file);
            json.field("line", frame.line);
            for (const auto& [name, values] : {std::pair{"locals", &frame.locals}, std::pair{"upvalues", &frame.upvalues}}) {
                json.key(name).begin_array();
                for (const ScriptValue& value : *values) {
                    write_script_value(json, value);
                }
                json.end_array();
            }
            json.end_object();
        }
        json.end_array();
    }
    json.end_object();
    return out.str();
}

} // namespace

ServerResponse LiveInspector::answer_debug(std::string_view method, const JsonValue& params, bool stopped,
                                           std::optional<ScriptResume>& resume) {
    ScriptDebugger& debugger = script_debugger();
    if (method == "debug.breakpoints") {
        return set_debug_breakpoints(params);
    }
    if (method == "debug.state") {
        return ok_result(debug_state_json(debugger.stop()));
    }
    if (method == "debug.pause") {
        if (!stopped) {
            debugger.pause();
        }
        return ok_result(debug_state_json(debugger.stop()));
    }
    if (method == "debug.continue" || method == "debug.step") {
        if (!stopped) {
            return error_result("no script is stopped");
        }
        if (method == "debug.continue") {
            resume = ScriptResume::Continue;
        } else {
            const std::string how = params.string_at("how", "over");
            if (how == "into") {
                resume = ScriptResume::Into;
            } else if (how == "over") {
                resume = ScriptResume::Over;
            } else if (how == "out") {
                resume = ScriptResume::Out;
            } else {
                return error_result("how must be into, over or out");
            }
        }
        return ok_result("{\"stopped\":false}");
    }
    return error_result("unknown method: " + std::string(method));
}

ScriptResume LiveInspector::wait_stopped() {
    // What waits for the game to go on: answered once it does, in order.
    std::deque<std::string> later;
    const auto go_on = [&](ScriptResume how) {
        std::lock_guard<std::mutex> lock(_shared->mutex);
        _shared->lines.insert(_shared->lines.begin(), std::make_move_iterator(later.begin()),
                              std::make_move_iterator(later.end()));
        return how;
    };
    KIN_LOG_INFO("server", "a script stopped for the inspector");
    for (;;) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(_shared->mutex);
            _shared->arrived.wait(lock, [&] { return !_shared->lines.empty() || !_shared->reading; });
            if (_shared->lines.empty()) {
                // The tool is gone: nobody is left to say go on.
                lock.unlock();
                script_debugger().set_breakpoints({});
                return go_on(ScriptResume::Continue);
            }
            line = std::move(_shared->lines.front());
            _shared->lines.pop_front();
        }
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }
        const JsonParseResult parsed = parse_json(strip_bom(line));
        if (!parsed.ok()) {
            send(nullptr, error_result("invalid JSON: " + parsed.error));
            continue;
        }
        const JsonValue& root = *parsed.value;
        const JsonValue* id = root.find("id");
        const std::string method = root.string_at("method");
        static const JsonValue empty_params;
        const JsonValue* given = root.find("params");
        const JsonValue& params = given ? *given : empty_params;
        if (method.starts_with("debug.")) {
            std::optional<ScriptResume> resume;
            send(id, answer_debug(method, params, true, resume));
            if (resume) {
                return go_on(*resume);
            }
        } else if (method == "server.status") {
            send(id, status(true));
        } else if (method == "ping") {
            send(id, ok_result("{\"pong\":true}"));
        } else if (method == "server.shutdown") {
            KIN_LOG_INFO("server", "inspector asked the game to quit");
            if (_ctx) {
                _ctx->app().quit();
            }
            send(id, ok_result("{\"bye\":true}"));
            script_debugger().set_breakpoints({});
            return go_on(ScriptResume::Continue);
        } else {
            later.push_back(std::move(line));
        }
    }
}

} // namespace kin
