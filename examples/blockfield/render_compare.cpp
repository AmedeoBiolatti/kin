// Shared, bounded rendering workload for Kin and Godot. Geometry is generated once,
// then read a chunk at a time by each engine. No streaming, edits, physics or HUD.
#include "world.hpp"
#include "render_workload.hpp"
#include "grove.hpp"
#include <kin/core/json.hpp>
#include <kin/core/jobs.hpp>
#include <ctime>
#include <kin/render3d/renderer3d.hpp>
#include <kin/runtime/scene_app.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace {
using namespace kin;
using Clock = std::chrono::steady_clock;
constexpr u32 magic = 0x31434642; // BFC1, little endian
constexpr int warmup = 300;
// Optional Linux thread/process clocks distinguish CPU execution from waits inside render().
// The process clock includes driver/renderer helper threads; the thread clock covers the caller alone.
f64 cpu_clock(bool process) {
#ifdef __linux__
    timespec t{};
    if (clock_gettime(process ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_THREAD_CPUTIME_ID, &t) != 0) throw std::runtime_error("CPU clock unavailable");
    return f64(t.tv_sec) * 1000 + f64(t.tv_nsec) / 1000000;
#else
    return 0;
#endif
}
f64 monotonic_ms() {
    return std::chrono::duration<f64, std::milli>(Clock::now().time_since_epoch()).count();
}
struct Options { std::string snapshot, generate, metrics, dressing, generate_dressing; u32 foliage = 0, transparent = 0; bool preview = false, profile_cpu = false, rebuild_scene = false, visible = false; int workers = 0; int distance = 24, measured = 900, copies = 1; f32 fov = 110; bool triangle_stress = false; blockfield::compare::Features features; };
void put(std::ostream& out, u32 n) { for (int i = 0; i < 4; ++i) out.put(char(n >> (i * 8))); }
u32 get(std::istream& in) {
    u32 n = 0; for (int i = 0; i < 4; ++i) { const int c = in.get(); if (c == EOF) throw std::runtime_error("truncated snapshot"); n |= u32(c) << (8 * i); } return n;
}
void put_float(std::ostream& out, f32 n) { put(out, std::bit_cast<u32>(n)); }
f32 get_float(std::istream& in) { const f32 n = std::bit_cast<f32>(get(in)); if (!std::isfinite(n)) throw std::runtime_error("invalid snapshot float"); return n; }
void generate(const Options& o) {
    std::fstream out(o.generate, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create snapshot");
    put(out, magic); put(out, 1); put(out, 0); put(out, u32(o.distance));
    blockfield::World world(1);
    std::size_t bytes = 0; u32 chunks = 0; u64 triangles = 0;
    for (int ring = 0; ring <= o.distance; ++ring) for (int z = -ring; z <= ring; ++z) for (int x = -ring; x <= ring; ++x) {
        if (std::max(std::abs(x), std::abs(z)) != ring) continue;
        const auto mesh = blockfield::build_chunk(world, {x, z});
        if (bytes + blockfield::mesh_bytes(mesh) > 256 * 1048576ull) continue;
        put(out, u32(x)); put(out, u32(z)); put(out, u32(mesh.vertices.size())); put(out, u32(mesh.indices.size()));
        // Separate arrays allow Godot to decode packed vectors without a per-vertex script loop.
        for (const auto& v : mesh.vertices) { put_float(out, v.position.x); put_float(out, v.position.y); put_float(out, v.position.z); }
        for (const auto& v : mesh.vertices) { put_float(out, v.normal.x); put_float(out, v.normal.y); put_float(out, v.normal.z); }
        for (const auto& v : mesh.vertices) { put_float(out, v.uv.x); put_float(out, v.uv.y); }
        for (const auto& v : mesh.vertices) { put_float(out, v.color.r / 255.0f); put_float(out, v.color.g / 255.0f); put_float(out, v.color.b / 255.0f); put_float(out, v.color.a / 255.0f); }
        for (const auto index : mesh.indices) put(out, index);
        bytes += blockfield::mesh_bytes(mesh); triangles += mesh.triangle_count(); ++chunks;
    }
    out.seekp(8); put(out, chunks); out.flush(); if (!out) throw std::runtime_error("snapshot write failed");
    std::printf("snapshot: %u chunks, %llu triangles, %.2f MiB resident source meshes\n", chunks, (unsigned long long)triangles, double(bytes) / 1048576);
}
class CompareScene final : public Scene {
public:
    explicit CompareScene(Options o, JobSystem* jobs) : _options(std::move(o)), _jobs(jobs) {
        _csv.open(_options.metrics); if (!_csv) throw std::runtime_error("cannot open metrics");
        _csv << "frame,frame_interval_ms,scene_build_ms,cpu_submit_ms,kin_fence_ms,draws,triangles,lights,spots,shadow_draws,shadow_cascades,local_shadow_maps,local_shadow_draws,samples,taa_dispatches,ao_draws,ao_dispatches,contact_dispatches,feature_bytes,local_shadow_bytes,instances,foliage_requested,transparent_requested,route_frame,profile_mono_ms,scene_main_cpu_ms,render_main_cpu_ms,render_process_cpu_ms,spatial_groups,camera_instance_tests,bounds_instance_tests,local_instance_tests,camera_group_rejected,local_group_rejected,retained_instances,retained_gpu_bytes,retained_upload_bytes,retained_index_bytes,instance_upload_bytes,bounds_cache_bytes,bounds_scratch_bytes,bounds_cache_instances,bounds_cache_rebuilt,camera_range_accepted,local_candidates,local_group_accepted,local_plane_budget,local_radius_tests,local_bucket_bytes,local_task_bytes,local_slot_bytes,local_copy_bytes,upload_staging_bytes,upload_staging_allocations,upload_count,upload_batches,masked_depth_draws,masked_depth_instances\n";
    }
    std::string_view name() const override { return "Blockfield render comparison"; }
    void update(SceneContext& ctx) override { if (_options.preview && ctx.input.frame_pressed(Key::Escape)) ctx.app.quit(); }
    void render(SceneContext& ctx) override {
        if (!_renderer) load(ctx);
        const auto begin = Clock::now();
        const double mono = _options.profile_cpu ? monotonic_ms() : 0;
        const double build_cpu = _options.profile_cpu ? cpu_clock(false) : 0;
        const double interval = std::chrono::duration<double, std::milli>(begin - _previous).count(); _previous = begin;
        const f32 t = f32(_frame >= warmup ? _frame - warmup : 0) / 60;
        const f32 travel = std::fmod(t, 12.0f);
        if (_options.features.camera == "ground") {
            _route_frame = u32(std::max(_frame - warmup, 0)) % blockfield::grove::route_count;
            const auto& camera = _dressing.route[_route_frame];
            _scene.camera = Camera3D::look_at(camera.eye, camera.target, _options.fov);
            _scene.camera.z_far = 160;
            _scene.cull = true;
        } else if (_options.features.camera == "detail") {
            _scene.camera = Camera3D::look_at({32, 46, 58}, {8, 12, 0}, 60);
            _scene.camera.z_far = f32((_distance + 2) * 16);
        } else if (field_camera()) {
            _scene.camera.position = {32 * std::sin(t * 0.18f), 700, 32 * std::sin(t * 0.12f)};
            _scene.camera.rotation = Quatf::yaw_pitch(0, -90);
            _scene.camera.fov = 90; _scene.camera.z_far = 3000;
            _scene.cull = false;
        } else {
            _scene.camera.position = {32 * std::sin(t * 0.18f), 40 + 3 * std::sin(t * 0.12f), -(travel < 6 ? travel : 12 - travel) * 8};
            _scene.camera.rotation = Quatf::yaw_pitch(18 * std::sin(t * 0.21f), -28);
            _scene.camera.fov = _options.fov; _scene.camera.z_far = f32((_distance + 2) * 16);
        }
        // Geometry and materials are static on this route. Keep their commands as
        // the Godot scene does; only the camera and animated light rig change.
        if (_options.rebuild_scene || !_scene_ready) _scene.clear();
        else { _scene.lights.points.clear(); _scene.lights.spots.clear(); }
        if (_options.features.material != "unlit") blockfield::compare::configure(_scene, _options.features, t, _options.features.camera == "ground" ? 96.0f : f32((_distance + 2) * 16));
        else _scene.taa.enabled = _options.features.taa;
        if (_options.rebuild_scene || !_scene_ready) {
            for (const auto& chunk : _chunks) {
                const auto& material = _materials.values[chunk.material];
                if (_options.triangle_stress) _scene.draw_instanced(chunk.mesh, chunk.copies, material);
                else _scene.draw(chunk.mesh, chunk.transform, material);
            }
            if (_options.features.camera == "ground") _dressing.draw(_scene, _options.features.shadows || _options.features.local_shadows, _options.features.material == "unlit");
            _scene_ready = true;
        }
        const auto submitted = Clock::now();
        const double prepare_ms = std::chrono::duration<double, std::milli>(submitted - begin).count();
        const double render_cpu = _options.profile_cpu ? cpu_clock(false) : 0;
        const double process_cpu = _options.profile_cpu ? cpu_clock(true) : 0;
        if (!_renderer->render(_view, _scene)) throw std::runtime_error("comparison render failed");
        const double submit_ms = std::chrono::duration<double, std::milli>(Clock::now() - submitted).count();
        const double render_main_cpu = _options.profile_cpu ? cpu_clock(false) - render_cpu : 0;
        const double render_process_cpu = _options.profile_cpu ? cpu_clock(true) - process_cpu : 0;
        ctx.renderer.clear(Color::rgb(110, 160, 200));
        ctx.renderer.draw_texture(_view.texture(), {0, 0, 1280, 720});
        const auto backend_stats = ctx.renderer.backend_stats();
        if (_frame > warmup) {
            const auto stats = _renderer->stats();
            if (field_camera() && (stats.triangles != _triangles * u64(_options.copies) || stats.instances != _chunks.size() * _options.copies))
                throw std::runtime_error("triangle stress did not submit all requested geometry");
            _csv << _frame << ',' << interval << ',' << prepare_ms << ',' << submit_ms << ','
                 << backend_stats.last_gpu_frame_ms << ',' << stats.draws << ',' << stats.triangles << ','
                 << stats.lights << ',' << stats.spots << ',' << stats.shadow_draws << ',' << stats.shadow_cascades << ','
                 << stats.local_shadow_maps << ',' << stats.local_shadow_draws << ',' << stats.samples << ',' << stats.taa_dispatches << ','
                 << stats.ao_draws << ',' << stats.ao_dispatches << ',' << stats.contact_dispatches << ',' << stats.feature_bytes << ',' << stats.local_shadow_bytes << ',' << stats.instances << ',' << _dressing.foliage_count << ',' << _dressing.transparent_count << ',' << _route_frame << ',' << std::fixed << std::setprecision(6) << mono << ','
                 << (_options.profile_cpu ? render_cpu - build_cpu : 0) << ',' << render_main_cpu << ',' << render_process_cpu << ',' << stats.spatial_groups << ',' << stats.camera_instance_tests << ',' << stats.bounds_instance_tests << ',' << stats.local_instance_tests << ',' << stats.camera_group_rejected << ',' << stats.local_group_rejected << ',' << stats.retained_instances << ',' << stats.retained_gpu_bytes << ',' << stats.retained_upload_bytes << ',' << stats.retained_index_bytes << ',' << stats.instance_upload_bytes << ',' << stats.bounds_cache_bytes << ',' << stats.bounds_scratch_bytes << ',' << stats.bounds_cache_instances << ',' << stats.bounds_cache_rebuilt << ',' << stats.camera_range_accepted << ',' << stats.local_candidates << ',' << stats.local_group_accepted << ',' << stats.local_plane_budget << ',' << stats.local_radius_tests << ',' << stats.local_bucket_bytes << ',' << stats.local_task_bytes << ',' << stats.local_slot_bytes << ',' << stats.local_copy_bytes << ',' << backend_stats.upload_staging_bytes << ',' << backend_stats.upload_staging_allocations << ',' << backend_stats.texture_uploads << ',' << backend_stats.texture_upload_submits << ',' << stats.masked_depth_draws << ',' << stats.masked_depth_instances << '\n';
        }
        ++_frame;
        if (_options.visible && !_options.preview && _frame > warmup + _options.measured) ctx.app.quit();
    }
    void write_report(JsonWriter& json) const override {
        json.field("snapshot", _options.snapshot).field("resident_chunks", u64(_chunks.size())).field("resident_triangles", _triangles)
            .field("fov", f64(_options.features.camera == "detail" ? 60 : field_camera() ? 90 : _options.fov)).field("warmup_frames", _options.preview ? 0 : warmup).field("measured_frames", _options.measured)
            .field("material", _options.features.material).field("streaming", false);
        json.field("triangle_stress", _options.triangle_stress).field("copies", _options.copies)
            .field("scene_triangles", _triangles * u64(_options.copies) + u64(_dressing.foliage_count) * 6 + u64(_dressing.transparent_count) * 2)
            .field("mesh_instances", u64(_chunks.size()) * _options.copies + _dressing.foliage_count + _dressing.transparent_count)
            .field("camera_layout", _options.features.camera == "detail" ? "detail" : field_camera() ? "top_down_disjoint_tiles" : _options.features.camera == "ground" ? "ground_route" : "flight")
            .field("point_lights_requested", _options.features.points).field("spot_lights_requested", _options.features.spots)
            .field("render_width", 1280 * _options.features.resolution_scale).field("render_height", 720 * _options.features.resolution_scale)
            .field("resolution_scale", _options.features.resolution_scale).field("samples_requested", _options.features.samples).field("taa_requested", _options.features.taa)
            .field("environment", _options.features.environment).field("hdr", _options.features.hdr).field("shadows_requested", _options.features.shadows)
            .field("local_shadows_requested", _options.features.local_shadows).field("normal_map", _options.features.material == "pbr" && _options.features.normal_map)
            .field("ao_requested", _options.features.ao).field("contact_requested", _options.features.contact)
            .field("shadow_resolution", _options.features.shadow_resolution).field("procedural_texture_bytes", _materials.texture_bytes);
        json.field("dressing", _options.dressing).field("foliage_instances", _dressing.foliage_count)
            .field("transparent_instances", _dressing.transparent_count).field("camera_route_frame", _route_frame)
            .field("extra_texture_bytes", _options.features.camera == "ground" ? 81920 : 0)
            .field("extra_mesh_triangles", u64(_dressing.foliage_count) * 6 + u64(_dressing.transparent_count) * 2)
            .field("preview", _options.preview).field("renderer_workers", _jobs ? _jobs->worker_count() : 0).field("profile_cpu", _options.profile_cpu)
            .field("static_scene_commands", !_options.rebuild_scene).field("visible_window", _options.preview || _options.visible);
        if (_renderer) {
            const auto& st = _renderer->stats();
            json.field("spatial_groups", st.spatial_groups).field("camera_instance_tests", st.camera_instance_tests).field("camera_range_accepted", st.camera_range_accepted)
                .field("bounds_instance_tests", st.bounds_instance_tests).field("local_instance_tests", st.local_instance_tests)
                .field("camera_group_rejected", st.camera_group_rejected).field("local_group_rejected", st.local_group_rejected)
                .field("retained_instances", st.retained_instances).field("retained_gpu_bytes", st.retained_gpu_bytes)
                .field("retained_upload_bytes", st.retained_upload_bytes).field("retained_index_bytes", st.retained_index_bytes).field("instance_upload_bytes", st.instance_upload_bytes)
                .field("bounds_cache_bytes", st.bounds_cache_bytes).field("bounds_scratch_bytes", st.bounds_scratch_bytes)
                .field("bounds_cache_instances", st.bounds_cache_instances).field("bounds_cache_rebuilt", st.bounds_cache_rebuilt)
                .field("local_candidates", st.local_candidates).field("local_group_accepted", st.local_group_accepted)
                .field("local_plane_budget", st.local_plane_budget).field("local_radius_tests", st.local_radius_tests).field("local_bucket_bytes", st.local_bucket_bytes).field("local_task_bytes", st.local_task_bytes).field("local_slot_bytes", st.local_slot_bytes).field("local_copy_bytes", st.local_copy_bytes);
            json.field("lights_used", st.lights).field("spots_used", st.spots).field("samples_used", st.samples)
                .field("shadow_draws", st.shadow_draws).field("shadow_cascades", st.shadow_cascades)
                .field("local_shadow_maps", st.local_shadow_maps).field("local_shadow_draws", st.local_shadow_draws)
                .field("local_shadow_bytes", st.local_shadow_bytes).field("feature_bytes", st.feature_bytes).field("masked_depth_draws", st.masked_depth_draws).field("masked_depth_instances", st.masked_depth_instances)
                .field("taa_dispatches", st.taa_dispatches).field("ao_draws", st.ao_draws).field("ao_dispatches", st.ao_dispatches)
                .field("contact_dispatches", st.contact_dispatches);
        }
        if (_renderer) json.field("shared_mesh_allocated_bytes", _renderer->stats().shared_mesh_allocated_bytes)
            .field("unlit_draws", _renderer->stats().unlit_draws).field("unlit_textured_draws", _renderer->stats().unlit_textured_draws);
    }
private:
    bool field_camera() const { return _options.features.camera == "field" || (_options.features.camera == "auto" && _options.triangle_stress); }
    void load(SceneContext& ctx) {
        ctx.renderer.set_job_system(_jobs);
        _renderer = std::make_unique<Renderer3D>(ctx.renderer);
        if (!_renderer->available()) throw std::runtime_error("comparison requires GPU backend");
        const auto caps = _renderer->capabilities();
        if (_options.features.taa && !caps.temporal_aa) throw std::runtime_error("TAA unsupported on this device");
        if ((_options.features.ao || _options.features.contact) && !caps.ambient_occlusion) throw std::runtime_error("occlusion features unsupported");
        if (_options.features.local_shadows && !caps.local_shadows) throw std::runtime_error("local shadows unsupported");
        _view = _renderer->create_view({i32(1280 * _options.features.resolution_scale), i32(720 * _options.features.resolution_scale)}, {.samples = _options.features.samples, .hdr = _options.features.hdr});
        if (_view.samples() != _options.features.samples) throw std::runtime_error("requested MSAA unsupported");
        ctx.renderer.set_scale_mode(_view.texture(), ScaleMode::Linear);
        _materials.create(ctx.renderer, _options.features);
        if (_options.features.environment) _scene.lights.environment = blockfield::compare::make_environment(*_renderer);
        std::ifstream in(_options.snapshot, std::ios::binary);
        if (!in || get(in) != magic || get(in) != 1) throw std::runtime_error("invalid snapshot header");
        const u32 count = get(in); _distance = int(get(in));
        if (count > 4225 || _distance < 1 || _distance > 32) throw std::runtime_error("invalid snapshot bounds");
        u64 total_bytes = 0;
        for (u32 i = 0; i < count; ++i) {
            const i32 x = i32(get(in)), z = i32(get(in)); const u32 nv = get(in), ni = get(in);
            if (nv > 100000 || ni > 300000 || ni % 3) throw std::runtime_error("invalid chunk size");
            total_bytes += u64(nv) * 48 + u64(ni) * 4;
            if (total_bytes > 400 * 1048576ull) throw std::runtime_error("snapshot exceeds memory budget");
            MeshData mesh; mesh.vertices.resize(nv); mesh.indices.resize(ni);
            for (auto& v : mesh.vertices) v.position = {get_float(in), get_float(in), get_float(in)};
            for (auto& v : mesh.vertices) v.normal = {get_float(in), get_float(in), get_float(in)};
            for (auto& v : mesh.vertices) {
                v.uv = {get_float(in), get_float(in)};
                if (_options.features.material == "pbr") { v.uv.x *= 0.125f; v.uv.y *= 0.125f; }
            }
            for (auto& v : mesh.vertices) {
                const auto channel = [&] { return u8(std::clamp(std::lround(get_float(in) * 255), 0l, 255l)); };
                v.color = {channel(), channel(), channel(), channel()};
            }
            for (auto& n : mesh.indices) { n = get(in); if (n >= nv) throw std::runtime_error("invalid index"); }
            auto uploaded = _renderer->create_mesh(mesh); if (!uploaded.valid()) throw std::runtime_error("mesh upload failed");
            Chunk chunk{std::move(uploaded), Mat4f::translation({f32(x * 16), 0, f32(z * 16)}), {}, u32((std::abs(x) + std::abs(z)) % 3)};
            if (_options.triangle_stress) {
                const int grid = int(std::ceil(std::sqrt(f32(_options.copies))));
                const f32 scale = 1.0f / grid;
                const f32 spacing = f32((_distance * 2 + 1) * 16);
                for (int copy = 0; copy < _options.copies; ++copy) {
                    const f32 tx = (f32(copy % grid) - f32(grid - 1) * 0.5f) * spacing;
                    const f32 tz = (f32(copy / grid) - f32(grid - 1) * 0.5f) * spacing;
                    chunk.copies.push_back(Mat4f::trs({(x * 16 + tx) * scale, 0, (z * 16 + tz) * scale}, {}, {scale, scale, scale}));
                }
            }
            _chunks.push_back(std::move(chunk));
            _triangles += ni / 3;
            // Bound pending GPU upload allocations while loading a large scene.
            if (i % 16 == 15) ctx.renderer.present();
        }
        if (in.peek() != EOF) throw std::runtime_error("trailing snapshot data");
        if (_options.features.camera == "ground") _dressing.load(_options.dressing, _options.foliage, _options.transparent, *_renderer, ctx.renderer);
        if (_options.preview) _frame = warmup;
        _previous = Clock::now();
    }
    struct Chunk { Mesh mesh; Mat4f transform; std::vector<Mat4f> copies; u32 material = 0; };
    Options _options; JobSystem* _jobs = nullptr; std::ofstream _csv; std::vector<Chunk> _chunks;
    blockfield::compare::Materials _materials;
    blockfield::grove::Dressing _dressing; u32 _route_frame = 0;
    std::unique_ptr<Renderer3D> _renderer; View3D _view; Scene3D _scene;
    int _frame = 0, _distance = 0; u64 _triangles = 0; Clock::time_point _previous{}; bool _scene_ready = false;
};
}
int main(int argc, char** argv) {
    try {
        Options o; std::vector<char*> runtime{argv[0]};
        // Preset defaults first: explicit options override them regardless of argument order.
        for (int i = 1; i < argc; ++i) if (std::string_view(argv[i]) == "--preset") {
            if (++i >= argc || std::string_view(argv[i]) != "grove") throw std::runtime_error("preset must be grove");
            o.features.camera = "ground"; o.features.material = "pbr"; o.features.hdr = o.features.environment = true;
            o.features.shadows = o.features.local_shadows = true; o.features.points = 4; o.features.spots = 1;
            o.fov = 75; o.foliage = 10000; o.transparent = 512;
        }
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i]; const auto value = [&] { if (++i == argc) throw std::runtime_error("missing option value"); return std::string(argv[i]); };
            if (a == "--preset") { value(); }
            else if (a == "--dressing") o.dressing = value();
            else if (a == "--generate-dressing") o.generate_dressing = value();
            else if (a == "--foliage" || a == "--transparent") {
                const int count = std::stoi(value());
                if (count < 0 || count > int(a == "--foliage" ? blockfield::grove::foliage_limit : blockfield::grove::transparent_limit)) throw std::runtime_error("grove instance count out of range");
                (a == "--foliage" ? o.foliage : o.transparent) = u32(count);
            }
            else if (a == "--workers") { o.workers = std::stoi(value()); if (o.workers < 0 || o.workers > 4) throw std::runtime_error("workers must be 0..4"); }
            else if (a == "--profile-cpu") o.profile_cpu = true;
            else if (a == "--rebuild-scene") o.rebuild_scene = true;
            else if (a == "--visible") o.visible = true;
            else if (a == "--preview") o.preview = true;
            else if (a == "--generate") o.generate = value(); else if (a == "--snapshot") o.snapshot = value();
            else if (a == "--metrics") o.metrics = value(); else if (a == "--distance") o.distance = std::clamp(std::stoi(value()), 1, 32);
            else if (a == "--fov") { o.fov = std::stof(value()); if (!std::isfinite(o.fov) || o.fov < 45 || o.fov > 150) throw std::runtime_error("FOV must be 45..150"); }
            else if (a == "--measured") o.measured = std::clamp(std::stoi(value()), 1, 6000);
            else if (a == "--copies") { o.copies = std::stoi(value()); if (o.copies < 1 || o.copies > 100) throw std::runtime_error("copies must be 1..100"); o.triangle_stress = true; }
            else if (a == "--material") o.features.material = value();
            else if (a == "--camera") o.features.camera = value();
            else if (a == "--resolution-scale") o.features.resolution_scale = u32(std::stoi(value()));
            else if (a == "--samples") o.features.samples = u32(std::stoi(value()));
            else if (a == "--lights") o.features.points = u32(std::stoi(value()));
            else if (a == "--spots") o.features.spots = u32(std::stoi(value()));
            else if (a == "--shadow-resolution") o.features.shadow_resolution = u32(std::stoi(value()));
            else if (a == "--hdr" || a == "--taa" || a == "--shadows" || a == "--local-shadows" || a == "--normal-map" || a == "--ao" || a == "--contact" || a == "--environment") {
                const auto v = value(); if (v != "0" && v != "1") throw std::runtime_error("boolean options require 0 or 1");
                bool* target = a == "--hdr" ? &o.features.hdr : a == "--taa" ? &o.features.taa : a == "--shadows" ? &o.features.shadows :
                    a == "--local-shadows" ? &o.features.local_shadows : a == "--normal-map" ? &o.features.normal_map : a == "--ao" ? &o.features.ao : a == "--contact" ? &o.features.contact : &o.features.environment;
                *target = v == "1";
            }
            else runtime.push_back(argv[i]);
        }
#ifndef __linux__
        if (o.profile_cpu) throw std::runtime_error("--profile-cpu requires Linux thread/process clocks");
#endif
        o.features.validate();
        if (!o.generate_dressing.empty()) { blockfield::grove::generate(o.generate_dressing); return 0; }
        if ((o.foliage || o.transparent) && o.features.camera != "ground") throw std::runtime_error("grove instances require --camera ground");
        if (o.features.camera == "ground" && (o.dressing.empty() || o.copies != 1)) throw std::runtime_error("ground route requires --dressing FILE and one terrain copy");
        if (!o.generate.empty()) { generate(o); return 0; }
        if (o.snapshot.empty() || o.metrics.empty()) throw std::runtime_error("use --generate FILE --distance N, or --snapshot FILE --metrics CSV [--fov N] [--measured N] [--copies 1..100] --report JSON");
        auto headless = parse_headless_options(int(runtime.size()), runtime.data());
        headless.enabled = !o.preview && !o.visible; headless.frames = o.visible && !o.preview ? 0 : warmup + o.measured + 1;
#ifdef _WIN32
        _putenv_s("KIN_LOG_FRAME_STATS", "1");
#else
        setenv("KIN_LOG_FRAME_STATS", "1", 1);
#endif
        // Owned outside the scene/app: worker tasks finish before either renderer or pool teardown.
        std::unique_ptr<JobSystem> jobs;
        if (o.workers) jobs = std::make_unique<JobSystem>(JobSystemConfig{.workers = o.workers, .max_background = 1});
        SceneManager scenes; scenes.push(std::make_unique<CompareScene>(o, jobs.get()));
        return run_scene_app({.window = {.title = "Blockfield render comparison", .width = 1280, .height = 720, .vsync = false, .max_fps = o.preview ? 60.0f : 0.0f, .high_pixel_density = false},
            .headless = headless, .render_headless = true}, scenes);
    } catch (const std::exception& e) { std::fprintf(stderr, "render compare: %s\n", e.what()); return 1; }
}
