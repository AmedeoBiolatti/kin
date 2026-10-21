#include "render_workload.hpp"
#include "world.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <kin/core/jobs.hpp>
#include <kin/core/json.hpp>
#include <kin/physics3d/world3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/timeofday3d.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/widgets.hpp>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

namespace blockfield {
using Clock = std::chrono::steady_clock;
constexpr f32 dt = 1.0f / 60.0f;
constexpr int max_distance = 32;
constexpr f32 max_fov = 150;
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct Options {
    u64 seed = 1;
    int radius = 8, width = 1280, height = 720;
    f32 fov = 90;
    std::size_t mesh_budget = 64 * 1024 * 1024;
    bool benchmark = false, help = false;
    std::string world_file, metrics, quality = "basic";
};
struct ChunkResult {
    ChunkKey key;
    u64 revision = 0;
    MeshData3D mesh;
    double meshing_ms = 0;
};
struct Resident {
    Mesh3D mesh;
    Body3D body;
    std::size_t bytes = 0, triangles = 0;
};

class BlockScene final : public Scene {
public:
    explicit BlockScene(Options o)
        : _options(std::move(o)), _voxels(_options.seed), _jobs({.workers = 1, .max_background = 1}),
          _physics({.max_bodies = 512,
                    .max_body_pairs = 2048,
                    .max_contact_constraints = 1024,
                    .temp_memory_bytes = 4 * 1024 * 1024,
                    .single_threaded = true,
                    .contact_events = false}) {
        if (_options.quality != "basic") {
            _features.material = "pbr";
            _features.hdr = true;
            _features.shadows = true;
            _features.points = _options.quality == "high" ? 8 : 4;
            _features.spots = _options.quality == "high" ? 2 : 1;
            _features.samples = _options.quality == "high" ? 1 : 4;
            _features.taa = _features.ao = _features.contact = _features.local_shadows = _options.quality == "high";
            _features.validate();
        }
        if (!_options.world_file.empty()) {
            std::ifstream f(_options.world_file, std::ios::binary | std::ios::ate);
            if (f) {
                const auto n = f.tellg();
                if (n > 0 && n <= std::streamoff(20 + max_edits * 13)) {
                    std::vector<u8> data(std::size_t(n), 0);
                    f.seekg(0);
                    f.read(reinterpret_cast<char*>(data.data()), n);
                    _message = f && _voxels.restore(data) ? "Saved world loaded"
                                                          : "World file invalid / generated a fresh world";
                } else
                    _message = "World file invalid / generated a fresh world";
            }
        }
        CharacterDesc3D character;
        character.position = {0.5f, f32(_voxels.height(0, 0)), 0.5f};
        character.radius = 0.3f;
        character.height = 1.8f;
        character.walk_speed = 4.5f;
        character.sprint_speed = 7;
        character.jump_speed = 5.5f;
        character.max_step_height = 0.55f;
        character.coyote_seconds = 0.1f;
        character.jump_buffer_seconds = 0.1f;
        _character = _physics.create_character(character);
        _warmup = std::max(180, (2 * _options.radius + 1) * (2 * _options.radius + 1) + 60);
        // Daylight is fixed in this slice. Evaluate the atmosphere once; clear() preserves these scene settings.
        _scene.sky = {.enabled = true, .turbidity = 2.5f, .sun_intensity = 1.0f};
        _scene.lights.sky_lights_scene = true;
        apply({.hours = 14.0f, .day_of_year = 172, .latitude = 46}, _scene.lights, _scene.sky, _scene.fog);
        _scene.lights.shadow_resolution = 1024;
        _scene.lights.shadow_cascades = 2;
        _scene.lights.shadow_distance = 48;
        _scene.fog.enabled = true;
        _scene.fog.density = 0.004f;
        _scene.fog.colour_from_sky = true;
        _eye = _previous_eye = character.position + Vec3f{0, 1.65f, 0};
        if (_options.benchmark) {
            _fly = true;
            _physics.set_character_enabled(_character, false);
        }
        if (!_options.metrics.empty()) {
            _metrics.open(_options.metrics);
            if (!_metrics)
                throw std::runtime_error("cannot open metrics file");
            _metrics << "frame,step,simulation_ms,chunk_apply_ms,scene_build_ms,cpu_render_ms,gpu_ms,resident_chunks,"
                        "triangles,draws,resident_mesh_bytes,edits\n";
        }
    }
    ~BlockScene() override { _jobs.drain(); }
    void on_exit(SceneContext& ctx) override {
        ctx.window.set_relative_mouse(false);
        _window = nullptr;
        _captured = false;
    }
    std::string_view name() const override { return "Blockfield"; }
    void on_enter(SceneContext& ctx) override {
        ctx.renderer.set_gpu_timing_enabled(true);
        _started = Clock::now();
    }
    void update(SceneContext& ctx) override {
        const auto begin = Clock::now();
        _previous_eye = _eye;
        if (_options.benchmark) {
            // Fixed commands: warm-up, moving through chunk boundaries, repeated edits, then return to test
            // eviction/reuse.
            const f32 t = f32(_step > u64(_warmup) ? _step - _warmup : 0) * dt;
            const f32 travel = std::fmod(t, 12.0f);
            _eye = {32.0f * std::sin(t * 0.18f), 40 + 3 * std::sin(t * 0.12f),
                    -(travel < 6 ? travel : 12 - travel) * 8};
            _yaw = 18 * std::sin(t * 0.21f);
            _pitch = -28;
            if (_step > u64(_warmup) && _step % 30 == 0) {
                const int x = int(std::floor(_eye.x)), z = int(std::floor(_eye.z - 4));
                Cell c{x, _voxels.height(x, z) - 1, z};
                edit(c, _voxels.block(c) == Block::Air ? Block::Grass : Block::Air);
            }
        }
        if (_renderer)
            stream(ctx.app.headless());
        if (!_options.benchmark && (_captured || ctx.app.headless())) {
            const Vec2f motion = ctx.input.mouse_motion();
            _yaw -= motion.x * 0.1f;
            _pitch = std::clamp(_pitch - motion.y * 0.1f, -89.0f, 89.0f);
            const Vec3f forward = Quatf::yaw_pitch(_yaw, 0).rotate({0, 0, -1}), right{-forward.z, 0, forward.x};
            Vec2f move{f32(ctx.input.held(Key::D)) - f32(ctx.input.held(Key::A)),
                       f32(ctx.input.held(Key::W)) - f32(ctx.input.held(Key::S))};
            if (_fly) {
                Vec3f velocity = forward * move.y + right * move.x;
                velocity.y += f32(ctx.input.held(Key::Space)) - f32(ctx.input.held(Key::Q));
                if (length(velocity) > 1)
                    velocity = normalize(velocity);
                _eye += velocity * (ctx.input.held(Key::LeftShift) ? 18 : 9) * dt;
                _eye.y = std::clamp(_eye.y, 2.0f, 100.0f);
            } else if (collision_ready()) {
                _physics.set_character_enabled(_character, true);
                _physics.set_character_input(_character, {.move = move,
                                                          .forward = forward,
                                                          .jump = ctx.input.held(Key::Space),
                                                          .sprint = ctx.input.held(Key::LeftShift)});
            } else
                _physics.set_character_enabled(_character, false);
            _edit_cooldown = std::max(0.0f, _edit_cooldown - dt);
            if (_edit_cooldown == 0 && ctx.input.mouse_held(MouseButton::Left)) {
                if (auto hit = raycast(_voxels, _eye, look()))
                    edit(hit->cell, Block::Air);
                _edit_cooldown = 0.16f;
            }
            if (ctx.input.mouse_pressed(MouseButton::Right)) {
                if (auto hit = raycast(_voxels, _eye, look());
                    hit && hit->previous != hit->cell && !inside_player(hit->previous))
                    edit(hit->previous, _selected);
            }
        } else if (!_fly) {
            // Releasing the mouse must also stop the previous movement command.
            _physics.set_character_input(_character, {});
            _physics.set_character_enabled(_character, collision_ready());
        }
        const auto physics = Clock::now();
        _physics.step(dt);
        _physics.drain_character_events();
        _physics_ms = milliseconds(physics);
        if (!_fly) {
            _eye = _physics.character(_character).position + Vec3f{0, 1.65f, 0};
            if (_eye.y < -10)
                respawn();
        }
        _eye.x = std::clamp(_eye.x, -10000.0f, 10000.0f);
        _eye.z = std::clamp(_eye.z, -10000.0f, 10000.0f);
        ++_step;
        _simulation_ms = milliseconds(begin);
        if (_frames >= u64(_warmup))
            sample(_simulation_samples, _simulation_ms);
    }
    void render(SceneContext& ctx) override {
        if (!_renderer) {
            _renderer = std::make_unique<Renderer3D>(ctx.renderer);
            if (!_renderer->available()) {
                if (ctx.report)
                    ctx.report->fail("Blockfield needs the SDL_GPU 3D backend");
                ctx.app.quit();
                return;
            }
            texture(ctx.renderer);
            if (_options.quality != "basic") {
                const auto caps = _renderer->capabilities();
                if ((_features.taa && !caps.temporal_aa) || (_features.ao && !caps.ambient_occlusion) ||
                    (_features.local_shadows && !caps.local_shadows) || _features.samples > caps.max_samples_hdr)
                    throw std::runtime_error("requested quality preset is unsupported on this device");
                _materials.create(ctx.renderer, _features);
                _materials.values[0].normal_scale = 0.35f;
                _materials.values[0].emissive =
                    0; // Terrain reflects light; the benchmark's emissive stripes are its own stress case.
            }
            _target_box = _renderer->create_mesh(meshes::box({1, 1, 1})).value_or({});
        }
        if (!_options.benchmark)
            controls(ctx);
        const auto size = ctx.renderer.output_size();
        if (!_view.valid())
            _view = _renderer->create_view(size, {.samples = _features.samples, .hdr = _features.hdr}).value_or({});
        else if (size != _size)
            if (auto result = _renderer->resize(_view, size); !result)
                throw std::runtime_error(kin::diagnostics3d_text(result.diagnostics));
        _size = size;
        const auto prepare = Clock::now();
        _scene.clear();
        if (_options.quality != "basic") {
            const auto daylight = _scene.lights.sun;
            compare::configure(_scene, _features, f32(_step) * dt, 32);
            _scene.lights.sun = daylight;
            _scene.lights.shadow_cascades = 2;
            _scene.lights.shadow_distance = 48;
            const Vec3f centre{_eye.x, _eye.y - 60, _eye.z};
            for (auto& light : _scene.lights.points) {
                light.position += centre;
                light.color = light.color * 0.03f;
            }
            for (auto& light : _scene.lights.spots) {
                light.position += centre;
                light.color = light.color * 0.02f;
            }
        }
        _scene.camera.position =
            ctx.app.headless() ? _eye : lerp(_previous_eye, _eye, std::clamp(ctx.alpha, 0.0f, 1.0f));
        _scene.camera.rotation = Quatf::yaw_pitch(_yaw, _pitch);
        _scene.camera.fov = _options.fov;
        _scene.camera.z_far = f32((_options.radius + 2) * chunk_size);
        _scene.lights.shadow_area = {_eye - Vec3f{64, 64, 64}, _eye + Vec3f{64, 64, 64}};
        Material3D material{
            .texture = _texture, .roughness = 0.95f, .wrap_u = Wrap3D::Repeat, .wrap_v = Wrap3D::Repeat};
        if (_options.quality != "basic")
            material = _materials.values[0];
        for (const auto& [key, chunk] : _chunks)
            (void)_scene.draw(chunk.mesh, Mat4f::translation({f32(key.x * chunk_size), 0, f32(key.z * chunk_size)}),
                              material);
        const auto hit = raycast(_voxels, _eye, look());
        if (hit && !_options.benchmark) {
            Material3D outline{
                .color = Color::rgb(250, 220, 106), .unlit = true, .casts_shadow = false, .wireframe = true};
            (void)_scene.draw(
                _target_box,
                Mat4f::trs({hit->cell.x + 0.5f, hit->cell.y + 0.5f, hit->cell.z + 0.5f}, {}, {1.008f, 1.008f, 1.008f}),
                outline);
        }
        _scene_ms = milliseconds(prepare);
        const auto submit = Clock::now();
        if (!_renderer->render(_view, _scene)) {
            if (ctx.report)
                ctx.report->fail("3D render failed");
            ctx.app.quit();
            return;
        }
        _render_ms = milliseconds(submit);
        ctx.renderer.clear(Color::rgb(110, 160, 200));
        ctx.renderer.draw_texture(_view.texture(), {0, 0, f32(size.x), f32(size.y)});
        const auto gpu = ctx.renderer.backend_stats();
        if (gpu.gpu_frames_sampled != _gpu_count) {
            _gpu_count = gpu.gpu_frames_sampled;
            _gpu_ms = gpu.last_gpu_frame_ms;
            if (_frames >= u64(_warmup))
                sample(_gpu_samples, _gpu_ms);
        }
        if (_frames >= u64(_warmup)) {
            sample(_scene_samples, _scene_ms);
            sample(_render_samples, _render_ms);
        }
        hud(ctx, hit);
        if (_metrics) {
            _metrics << _frames << ',' << _step << ',' << _simulation_ms << ',' << _apply_ms << ',' << _scene_ms << ','
                     << _render_ms << ',';
            if (_gpu_count)
                _metrics << _gpu_ms;
            _metrics << ',' << _chunks.size() << ',' << _renderer->debug_stats().triangles << ','
                     << _renderer->debug_stats().draws << ',' << _mesh_bytes << ',' << _voxels.edits() << '\n';
        }
        ++_frames;
    }
    void write_report(JsonWriter& json) const override {
        json.field("seed", _voxels.seed())
            .field("world_hash", _voxels.hash())
            .field("frames_rendered", _frames)
            .field("steps", _step)
            .field("render_quality", _options.quality)
            .field("view_distance_chunks", _options.radius)
            .field("camera_fov_degrees", f64(_options.fov))
            .field("resident_chunks", u64(_chunks.size()))
            .field("resident_chunk_limit", (2 * _options.radius + 1) * (2 * _options.radius + 1))
            .field("resident_mesh_bytes", u64(_mesh_bytes))
            .field("mesh_budget_bytes", u64(_options.mesh_budget))
            .field("meshing_jobs_limit", 1)
            .field("mesher_workers", 1)
            .field("warmup_frames", _warmup)
            .field("edits", u64(_voxels.edits()))
            .field("chunks_built", _built)
            .field("chunks_evicted", _evicted)
            .field("blocks_changed", _changed)
            .field("selected_block", block_name(_selected))
            .field("yaw", f64(_yaw))
            .field("pitch", f64(_pitch))
            .field("fly", _fly)
            .field("collision_ready", collision_ready())
            .field("budget_reached", _budget_reached)
            .field("budget_blocked_chunks", u64(_budget_blocked.size()))
            .field("dirty_chunks", u64(_dirty.size()))
            .field("meshing_last_ms", _meshing_ms)
            .field("elapsed_seconds", milliseconds(_started) / 1000);
        json.key("camera").begin_array().value(f64(_eye.x)).value(f64(_eye.y)).value(f64(_eye.z)).end_array();
        if (const auto hit = raycast(_voxels, _eye, look())) {
            json.key("target")
                .begin_object()
                .field("block", block_name(hit->block))
                .field("distance", f64(hit->distance));
            json.key("cell").begin_array().value(hit->cell.x).value(hit->cell.y).value(hit->cell.z).end_array();
            json.key("placement")
                .begin_array()
                .value(hit->previous.x)
                .value(hit->previous.y)
                .value(hit->previous.z)
                .end_array();
            json.end_object();
        }
        for (const auto& [name, data] : std::array<std::pair<const char*, const std::vector<double>*>, 4>{
                 {{"cpu_update_ms", &_simulation_samples},
                  {"scene_build_ms", &_scene_samples},
                  {"cpu_render_submit_ms", &_render_samples},
                  {"gpu_frame_ms", &_gpu_samples}}}) {
            json.key(name).begin_object().field("samples", u64(data->size()));
            if (!data->empty()) {
                auto values = *data;
                std::sort(values.begin(), values.end());
                json.field("p50", values[values.size() / 2])
                    .field("p95", values[(values.size() - 1) * 95 / 100])
                    .field("p99", values[(values.size() - 1) * 99 / 100])
                    .field("max", values.back());
            }
            json.end_object();
        }
        if (_renderer) {
            const auto& stats = _renderer->debug_stats();
            json.field("draws_last", stats.draws)
                .field("triangles_last", stats.triangles)
                .field("lights_used", stats.lights)
                .field("spots_used", stats.spots)
                .field("samples_used", stats.samples)
                .field("taa_dispatches", stats.taa_dispatches)
                .field("ao_dispatches", stats.ao_dispatches)
                .field("contact_dispatches", stats.contact_dispatches)
                .field("local_shadow_maps", stats.local_shadow_maps)
                .field("local_shadow_bytes", stats.local_shadow_bytes)
                .field("feature_bytes", stats.feature_bytes)
                .field("shared_mesh_allocated_bytes", stats.shared_mesh_allocated_bytes)
                .field("shared_mesh_live_bytes", stats.shared_mesh_live_bytes)
                .field("shared_mesh_retired_bytes", stats.shared_mesh_retired_bytes)
                .field("shared_mesh_range_reuses", stats.shared_mesh_range_reuses)
                .field("shared_mesh_pages", stats.shared_mesh_pages);
        }
    }

private:
    static void sample(std::vector<double>& values, double value) {
        if (values.size() < 6000)
            values.push_back(value);
    }
    Vec3f look() const { return Quatf::yaw_pitch(_yaw, _pitch).rotate({0, 0, -1}); }
    ChunkKey centre() const { return chunk_of({int(std::floor(_eye.x)), 0, int(std::floor(_eye.z))}); }
    static int distance(ChunkKey a, ChunkKey b) { return std::max(std::abs(a.x - b.x), std::abs(a.z - b.z)); }
    bool collision_ready() const {
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 1; ++x) {
                const auto it = _chunks.find({centre().x + x, centre().z + z});
                if (it == _chunks.end() || !it->second.body.valid() || _dirty.contains(it->first))
                    return false;
            }
        return true;
    }
    bool inside_player(Cell c) const {
        const Vec3f feet = _eye - Vec3f{0, 1.65f, 0};
        return c.x < feet.x + 0.31f && c.x + 1 > feet.x - 0.31f && c.z < feet.z + 0.31f && c.z + 1 > feet.z - 0.31f &&
               c.y < feet.y + 1.8f && c.y + 1 > feet.y;
    }
    void edit(Cell c, Block b) {
        if (!_voxels.set(c, b))
            return;
        ++_changed;
        _budget_blocked.clear();
        const ChunkKey k = chunk_of(c);
        const auto dirty = [&](ChunkKey n) {
            if (_chunks.contains(n))
                _dirty.insert(n);
        };
        dirty(k);
        if (c.x - k.x * chunk_size == 0)
            dirty({k.x - 1, k.z});
        if (c.x - k.x * chunk_size == chunk_size - 1)
            dirty({k.x + 1, k.z});
        if (c.z - k.z * chunk_size == 0)
            dirty({k.x, k.z - 1});
        if (c.z - k.z * chunk_size == chunk_size - 1)
            dirty({k.x, k.z + 1});
    }
    void stream(bool deterministic) {
        const auto start = Clock::now();
        const ChunkKey camera = centre();
        // Drop the previous scene's CPU references before eviction. GPU resource retirement remains the engine's
        // responsibility.
        _scene.clear();
        _budget_reached = !_budget_blocked.empty();
        for (auto it = _budget_blocked.begin(); it != _budget_blocked.end();)
            if (distance(*it, camera) > _options.radius)
                it = _budget_blocked.erase(it);
            else
                ++it;
        for (auto it = _chunks.begin(); it != _chunks.end();) {
            if (distance(it->first, camera) > _options.radius) {
                if (it->second.body.valid())
                    _physics.destroy_body(it->second.body);
                _mesh_bytes -= it->second.bytes;
                _dirty.erase(it->first);
                it = _chunks.erase(it);
                ++_evicted;
                _budget_blocked.clear();
            } else {
                if (distance(it->first, camera) > 1 && it->second.body.valid()) {
                    _physics.destroy_body(it->second.body);
                    it->second.body = {};
                }
                ++it;
            }
        }
        if (_pending.valid() && (deterministic || _pending.ready())) {
            ChunkResult result = std::move(_pending.get());
            _pending = {};
            if (result.revision == _voxels.revision() && distance(result.key, camera) <= _options.radius) {
                const auto old = _chunks.find(result.key);
                const std::size_t previous = old == _chunks.end() ? 0 : old->second.bytes,
                                  bytes = mesh_bytes(result.mesh);
                // An edit near the player takes priority over distant scenery. Retire far meshes instead of leaving
                // stale collision.
                while (_mesh_bytes - previous + bytes > _options.mesh_budget && distance(result.key, camera) <= 1) {
                    auto far = _chunks.end();
                    for (auto candidate = _chunks.begin(); candidate != _chunks.end(); ++candidate)
                        if (candidate->first != result.key && distance(candidate->first, camera) > 1 &&
                            (far == _chunks.end() || distance(candidate->first, camera) > distance(far->first, camera)))
                            far = candidate;
                    if (far == _chunks.end())
                        break;
                    _mesh_bytes -= far->second.bytes;
                    _dirty.erase(far->first);
                    _chunks.erase(far);
                    ++_evicted;
                    _budget_blocked.clear();
                }
                if (_mesh_bytes - previous + bytes <= _options.mesh_budget) {
                    Resident& chunk = _chunks[result.key];
                    if (!chunk.mesh.valid() || _dirty.contains(result.key)) {
                        chunk.mesh = _renderer->create_mesh(result.mesh).value_or({});
                        if (!chunk.mesh.valid())
                            throw std::runtime_error("chunk mesh upload failed");
                        chunk.bytes = bytes;
                        chunk.triangles = result.mesh.triangle_count();
                        _mesh_bytes = _mesh_bytes - previous + bytes;
                    }
                    if (chunk.body.valid())
                        _physics.destroy_body(chunk.body);
                    chunk.body = {};
                    if (distance(result.key, camera) <= 1 && !result.mesh.indices.empty()) {
                        std::vector<Vec3f> vertices;
                        vertices.reserve(result.mesh.vertices.size());
                        for (const auto& vertex : result.mesh.vertices)
                            vertices.push_back(vertex.position);
                        chunk.body = _physics.create_body(
                            {.shape = Shape3D::triangle_mesh(vertices, result.mesh.indices),
                             .motion = Body3DMotion::Static,
                             .position = {f32(result.key.x * chunk_size), 0, f32(result.key.z * chunk_size)},
                             .friction = 0.8f});
                        if (!chunk.body.valid())
                            throw std::runtime_error("chunk collision creation failed");
                    }
                    _dirty.erase(result.key);
                    _meshing_ms = result.meshing_ms;
                    ++_built;
                } else {
                    _budget_reached = true;
                    _budget_blocked.insert(result.key);
                }
            }
        }
        if (!_pending.valid()) {
            bool queued = false;
            for (int ring = 0; ring <= _options.radius && !queued; ++ring)
                for (int z = -ring; z <= ring && !queued; ++z)
                    for (int x = -ring; x <= ring && !queued; ++x) {
                        if (std::max(std::abs(x), std::abs(z)) != ring)
                            continue;
                        const ChunkKey key{camera.x + x, camera.z + z};
                        if (_budget_blocked.contains(key))
                            continue;
                        const auto found = _chunks.find(key);
                        const bool missing = found == _chunks.end(), dirty = _dirty.contains(key);
                        if (missing || dirty || (ring <= 1 && !found->second.body.valid())) {
                            if (missing && _mesh_bytes >= _options.mesh_budget) {
                                _budget_reached = true;
                                continue;
                            }
                            _pending = _jobs.run([world = _voxels, key] {
                                const auto begin = Clock::now();
                                ChunkResult r{key, world.revision(), build_chunk(world, key)};
                                r.meshing_ms = milliseconds(begin);
                                return r;
                            });
                            queued = true;
                        }
                    }
        }
        _apply_ms = milliseconds(start);
    }
    void respawn() {
        _eye = {0.5f, f32(_voxels.height(0, 0)) + 1.65f, 0.5f};
        _previous_eye = _eye;
        _physics.teleport_character(_character, _eye - Vec3f{0, 1.65f, 0});
    }
    void controls(SceneContext& ctx) {
        if (ctx.input.frame_pressed(Key::Escape)) {
            _captured = !_captured;
            ctx.window.set_relative_mouse(_captured);
            _window = &ctx.window;
        } else if (!_captured && !ctx.app.headless() && ctx.input.mouse_frame_pressed(MouseButton::Left)) {
            _captured = true;
            ctx.window.set_relative_mouse(true);
            _window = &ctx.window;
        }
        if (ctx.input.frame_pressed(Key::F)) {
            _fly = !_fly;
            _physics.set_character_enabled(_character, false);
            if (!_fly) {
                Cell c{int(std::floor(_eye.x)), int(std::floor(_eye.y - 1.65f)), int(std::floor(_eye.z))};
                if (_voxels.block(c) != Block::Air || _voxels.block({c.x, c.y + 1, c.z}) != Block::Air)
                    _eye.y = f32(_voxels.height(c.x, c.z)) + 1.65f;
                _physics.teleport_character(_character, _eye - Vec3f{0, 1.65f, 0});
                _previous_eye = _eye;
            }
        }
        if (ctx.input.frame_pressed(Key::R))
            respawn();
        if (ctx.input.frame_pressed(Key::Equals)) {
            _options.radius = std::min(max_distance, _options.radius + 1);
            _budget_blocked.clear();
        }
        if (ctx.input.frame_pressed(Key::Minus)) {
            _options.radius = std::max(1, _options.radius - 1);
            _budget_blocked.clear();
        }
        if (ctx.input.frame_pressed(Key::PageUp))
            _options.fov = std::min(max_fov, _options.fov + 5);
        if (ctx.input.frame_pressed(Key::PageDown))
            _options.fov = std::max(45.0f, _options.fov - 5);
        const Key keys[] = {Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5, Key::Num6};
        for (int i = 0; i < 6; ++i)
            if (ctx.input.frame_pressed(keys[i]))
                _selected = Block(i + 1);
        if (ctx.input.frame_pressed(Key::F5)) {
            if (_options.world_file.empty())
                _message = "Choose --world PATH to enable saving";
            else {
                const auto data = _voxels.save();
                std::ofstream f(_options.world_file, std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
                f.close();
                _message = f ? "World saved" : "World save failed";
            }
        }
    }
    void texture(Renderer2D& r) {
        constexpr int n = 16;
        std::array<u8, n * n * 4> pixels{};
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const int at = (y * n + x) * 4;
                const u8 value = u8(208 + ((x * 37 + y * 61 + x * y * 17) % 43));
                pixels[at] = pixels[at + 1] = pixels[at + 2] = value;
                pixels[at + 3] = 255;
            }
        _texture = r.create_texture_from_rgba(pixels.data(), {n, n});
        r.set_scale_mode(_texture, ScaleMode::Mipmapped);
    }
    void hud(SceneContext& ctx, const std::optional<BlockHit>& hit) {
        const Vec2i size = ctx.renderer.output_size();
        const f32 u = std::min(f32(size.x) / 1280, f32(size.y) / 720);
        ctx.renderer.draw_line({f32(size.x) / 2 - 7, f32(size.y) / 2}, {f32(size.x) / 2 + 7, f32(size.y) / 2},
                               colors::white);
        ctx.renderer.draw_line({f32(size.x) / 2, f32(size.y) / 2 - 7}, {f32(size.x) / 2, f32(size.y) / 2 + 7},
                               colors::white);
        _ui.begin(ctx.input, ctx.renderer, 0);
        const auto text = [&](const std::string& value, f32 x, f32 y, f32 points = 15) {
            _ui.text(value, {x * u, y * u},
                     {.font = ui2::system_ui_font(std::max(8.0f, points * u)), .scale = 1, .color = colors::white});
        };
        ctx.renderer.draw_rect({12 * u, 12 * u, 650 * u, 102 * u}, Color::rgba(10, 17, 23, 190));
        char b[240];
        std::snprintf(b, sizeof b, "BLOCKFIELD / distance %d / FOV %.0f", _options.radius, double(_options.fov));
        if (_options.quality != "basic")
            std::snprintf(b, sizeof b, "BLOCKFIELD / %s lighting / distance %d / FOV %.0f", _options.quality.c_str(),
                          _options.radius, double(_options.fov));
        text(b, 24, 21, 20);
        std::snprintf(b, sizeof b, "Chunks %zu / %d   mesh %.1f / %.0f MiB   %llu edits   %s", _chunks.size(),
                      (2 * _options.radius + 1) * (2 * _options.radius + 1), double(_mesh_bytes) / 1048576,
                      double(_options.mesh_budget) / 1048576, (unsigned long long)_voxels.edits(),
                      _fly                ? "FLY"
                      : collision_ready() ? "WALK"
                                          : "Loading collision");
        text(b, 24, 54);
        std::snprintf(b, sizeof b, "Update %.2f ms   mesh %.2f ms   CPU render %.2f ms   GPU %s%.2f ms", _simulation_ms,
                      _meshing_ms, _render_ms, _gpu_count ? "" : "unavailable / ", _gpu_ms);
        text(b, 24, 82, 13);
        const f32 hot_y = f32(size.y) / u - 62;
        for (int i = 0; i < 6; ++i) {
            const f32 x = 310 + i * 110;
            ctx.renderer.draw_rect({x * u, hot_y * u, 100 * u, 36 * u}, i + 1 == int(_selected)
                                                                            ? Color::rgba(132, 106, 42, 235)
                                                                            : Color::rgba(10, 17, 23, 190));
            text(std::to_string(i + 1) + " " + block_name(Block(i + 1)), x + 8, hot_y + 10, 13);
        }
        text("WASD   Space jump/up   Q down   F fly   LMB/RMB mine/place   +/- distance   PgUp/PgDn FOV   R spawn   F5 "
             "save",
             18, f32(size.y) / u - 20, 12);
        if (hit)
            text(std::string("Target: ") + block_name(hit->block), 24, 128, 14);
        if (!_captured && !_options.benchmark && !ctx.app.headless())
            text("Click to play / Esc releases mouse", 430, 155, 19);
        if (_budget_reached)
            text("Mesh budget reached: lower distance or raise --mesh-budget-mib", 230, 190, 17);
        if (!_message.empty())
            text(_message, 24, 151, 13);
        _ui.end();
    }
    Options _options;
    World _voxels;
    JobSystem _jobs;
    World3D _physics;
    Character3D _character;
    Job<ChunkResult> _pending;
    std::map<ChunkKey, Resident> _chunks;
    std::set<ChunkKey> _dirty;
    std::set<ChunkKey> _budget_blocked;
    compare::Features _features;
    compare::Materials _materials;
    std::unique_ptr<Renderer3D> _renderer;
    View3D _view;
    Scene3D _scene;
    Mesh3D _target_box;
    Texture _texture;
    Vec2i _size{};
    Vec3f _eye{}, _previous_eye{};
    f32 _yaw = 0, _pitch = 0, _edit_cooldown = 0;
    Block _selected = Block::Grass;
    bool _captured = false, _fly = false, _budget_reached = false;
    Window* _window = nullptr;
    u64 _step = 0, _frames = 0, _gpu_count = 0, _built = 0, _evicted = 0, _changed = 0;
    std::size_t _mesh_bytes = 0;
    int _warmup = 180;
    double _simulation_ms = 0, _physics_ms = 0, _meshing_ms = 0, _apply_ms = 0, _scene_ms = 0, _render_ms = 0,
           _gpu_ms = 0;
    std::vector<double> _simulation_samples, _scene_samples, _render_samples, _gpu_samples;
    Clock::time_point _started = Clock::now();
    ui2::Context _ui;
    std::ofstream _metrics;
    std::string _message;
};

int run(int argc, char** argv) {
    Options options;
    std::vector<char*> runtime;
    runtime.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() -> std::string {
            if (++i >= argc)
                throw std::runtime_error(a + " needs a value");
            return argv[i];
        };
        if (a == "--seed")
            options.seed = std::stoull(value());
        else if (a == "--distance")
            options.radius = std::clamp(std::stoi(value()), 1, max_distance);
        else if (a == "--fov") {
            options.fov = std::stof(value());
            if (!std::isfinite(options.fov))
                throw std::runtime_error("--fov needs a finite angle");
            options.fov = std::clamp(options.fov, 45.0f, max_fov);
        } else if (a == "--width")
            options.width = std::clamp(std::stoi(value()), 320, 3840);
        else if (a == "--height")
            options.height = std::clamp(std::stoi(value()), 180, 2160);
        else if (a == "--mesh-budget-mib")
            options.mesh_budget = std::size_t(std::clamp(std::stoi(value()), 1, 256)) * 1048576;
        else if (a == "--quality")
            options.quality = value();
        else if (a == "--world")
            options.world_file = value();
        else if (a == "--metrics")
            options.metrics = value();
        else if (a == "--benchmark")
            options.benchmark = true;
        else if (a == "--help")
            options.help = true;
        else
            runtime.push_back(argv[i]);
    }
    if (options.quality != "basic" && options.quality != "pbr" && options.quality != "high")
        throw std::runtime_error("--quality must be basic, pbr or high");
    if (options.help) {
        std::puts(
            "Blockfield: a bounded Minecraft-style creative voxel sandbox on kin.\n"
            "  --seed N --distance N (1..32, default 8) --fov N (45..150 vertical degrees, default 90)\n"
            "  --mesh-budget-mib N (default 64, maximum 256)\n"
            "  --quality basic|pbr|high (default basic); pbr adds maps, lights, HDR/MSAA; high adds local shadows, "
            "TAA, AO/contact shadows\n"
            "  --width N --height N --world FILE (F5 saves edits; same seed loads them)\n"
            "  --benchmark --headless --frames N --report FILE --metrics FILE --screenshot=FILE\n"
            "  Benchmark renders EVERY frame: warm-up, flight through chunk boundaries and repeated terrain edits.\n"
            "  WASD move; mouse look; Space jump; Shift sprint; F fly; Space/Q fly up/down.\n"
            "  Left mouse mines, right mouse places; 1..6 choose blocks; +/- distance; PgUp/PgDn FOV; R respawn; Esc "
            "release/capture.\n"
            "  One meshing worker and one queued mesh; collision only in the nearest 3x3 chunks.\n"
            "  The mesh budget covers resident vertex/index data, not total process RAM or all driver allocations.");
        return 0;
    }
    auto headless = parse_headless_options(int(runtime.size()), runtime.data());
    headless.seed = options.seed;
    if (options.benchmark) {
        // The runtime otherwise turns the device timer off after scene rendering. Keep its delayed GPU samples alive.
#ifdef _WIN32
        _putenv_s("KIN_LOG_FRAME_STATS", "1");
#else
        setenv("KIN_LOG_FRAME_STATS", "1", 1);
#endif
    }
    if (options.benchmark && headless.frames == 0 && !headless.server) {
        headless.enabled = true;
        // Keep at least one complete flight cycle after loading larger views.
        headless.frames = std::max(1200, (2 * options.radius + 1) * (2 * options.radius + 1) + 60 + 900);
    }
    GameInfo game{.id = "blockfield",
                  .title = "Blockfield",
                  .version = "0.1",
                  .description = "A streamed, editable voxel world and continuous 3D benchmark",
                  .window = {.width = options.width, .height = options.height, .resizable = true},
                  .tags = {"example", "3d", "voxels"}};
    auto window = window_config(game);
    window.vsync = false;
    window.max_fps = headless.enabled ? 0 : 60;
    window.high_pixel_density = !headless.enabled;
    SceneManager scenes;
    const auto factory = [&](SceneManager& s) { s.push(std::make_unique<BlockScene>(options)); };
    factory(scenes);
    return run_scene_app(
        {.window = window, .headless = headless, .game = &game, .render_headless = true, .reset_scenes = factory},
        scenes);
}
} // namespace blockfield
int main(int argc, char** argv) {
    try {
        return blockfield::run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "blockfield: %s\n", error.what());
        return 1;
    }
}
