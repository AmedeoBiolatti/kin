#include "benchmark.hpp"
#include <kin/core/json.hpp>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <stdexcept>

namespace summit {

ExpeditionBenchmark::ExpeditionBenchmark(const std::string& path, Clock::time_point process_start, f64 warmup_seconds)
    : _path(path), _process_start(process_start), _warmup_seconds(warmup_seconds) {
    const auto directory = std::filesystem::path(path).parent_path();
    if (!directory.empty()) std::filesystem::create_directories(directory);
    _csv.open(path);
    if (!_csv) throw std::runtime_error("cannot open Summit benchmark CSV");
    _csv << "frame,step,leg,phase,measured,frame_interval_ms,scene_build_ms,cpu_submit_ms,draws,triangles,instances,culled,shadow_draws,shadow_casters,shadow_cascades,local_shadow_maps,local_shadow_draws,skinned_copies,skin_matrices,retained_gpu_bytes,instance_upload_bytes,spatial_groups,feet_x,feet_y,feet_z,width,height,profile_mono_ms,next_hit_max_steps,next_raw_frame_ms\n";
    _csv << std::fixed << std::setprecision(6);
}

bool ExpeditionBenchmark::ready() const {
    return _first_frame != Clock::time_point{} && std::chrono::duration<f64>(Clock::now()-_first_frame).count() >= _warmup_seconds;
}

void ExpeditionBenchmark::begin_frame(const AppFrameStats& app) {
    const auto now = Clock::now();
    if (_first_frame == Clock::time_point{}) _first_frame = now;
    if (!_have_pending) return;
    if (!_first_present_ms) _first_present_ms = std::chrono::duration<f64,std::milli>(now-_process_start).count();
    const Row& p = _pending;
    const auto& s = p.stats;
    // App's raw interval is start-to-start for the previous completed frame, including its present/GPU wait. Pair it with
    // the previous render's scene/camera metadata rather than pretending cpu_submit_ms is the complete frame cost.
    _csv << p.frame << ',' << p.step << ',' << p.leg << ',' << p.phase << ',' << p.measured << ',' << app.raw_frame_time*1000.0 << ','
         << p.build_ms << ',' << p.submit_ms << ',' << s.draws << ',' << s.triangles << ',' << s.instances << ',' << s.culled << ','
         << s.shadow_draws << ',' << s.shadow_casters << ',' << s.shadow_cascades << ',' << s.local_shadow_maps << ',' << s.local_shadow_draws << ','
         << s.skinned_copies << ',' << s.skin_matrices << ',' << s.retained_gpu_bytes << ',' << s.instance_upload_bytes << ',' << s.spatial_groups << ','
         << p.feet.x << ',' << p.feet.y << ',' << p.feet.z << ',' << p.size.x << ',' << p.size.y << ',' << p.mono_ms << ','
         << app.hit_max_steps << ',' << app.raw_frame_time*1000.0 << '\n';
    if (!_csv) throw std::runtime_error("cannot write Summit benchmark CSV");
    _have_pending = false;
}

void ExpeditionBenchmark::capture(const Game& game, i32 leg, const SummitView& view, const Render3DDebugStats& stats, Vec2i size,
                                  f64 build_ms, f64 submit_ms) {
    _pending = {};
    _pending.frame = ++_frames;
    _pending.step = game.step_count();
    _pending.leg = leg;
    _pending.phase = i32(game.player(0).expedition.phase);
    _pending.feet = game.feet(0);
    _pending.size = size;
    _pending.measured = ready() && _pending.step > 0;
    _pending.stats = stats;
    _pending.build_ms = build_ms;
    _pending.submit_ms = submit_ms;
    _pending.mono_ms = std::chrono::duration<f64,std::milli>(Clock::now().time_since_epoch()).count();
    _have_pending = true;
    _scenery = view.scenery_instances(); _scenery_triangles = view.scenery_triangles();
    _record_bytes = view.instance_record_bytes(); _static_triangles = view.static_triangles();
}

void ExpeditionBenchmark::finish(const Game& game, i32 samples) {
    if (_finished) return;
    _finished = true;
    _csv.flush();
    std::ofstream output(_path+".json");
    if (!_csv || !output) throw std::runtime_error("cannot finish Summit benchmark files");
    JsonWriter j(output);
    j.begin_object().field("scenery", game.level().expedition.dense_scenery ? "dense" : "compact")
        .field("seed",game.level().seed).field("completed",game.player(0).summit).field("steps",game.step_count())
        .field("final_hash",game.state_hash()).field("falls",game.player(0).falls).field("deaths",game.player(0).deaths)
        .field("sheltered",game.player(0).expedition.shelter_visited).field("samples",samples)
        .field("width",_pending.size.x).field("height",_pending.size.y).field("rendered_frames",_frames)
        .field("warmup_seconds",_warmup_seconds).field("asset_setup_ms",_asset_setup_ms).field("startup_to_first_completed_frame_ms",_first_present_ms)
        .field("scenery_instances",_scenery).field("scenery_instance_triangles",_scenery_triangles).field("instance_record_bytes",_record_bytes)
        .field("static_triangles",_static_triangles).field("prop_colliders",u64(game.props().size()))
        .field("clock","real-time 60 Hz simulation; uncapped native window; full start-to-start frame intervals")
        .field("loading","whole level preloaded; no asset streaming; startup excluded from measured traversal").end_object();
    std::printf("summit: benchmark %s complete=%d steps=%llu frames=%llu startup %.1f ms asset setup %.1f ms\n",_path.c_str(),
                int(game.player(0).summit),(unsigned long long)game.step_count(),(unsigned long long)_frames,_first_present_ms,_asset_setup_ms);
}

} // namespace summit
