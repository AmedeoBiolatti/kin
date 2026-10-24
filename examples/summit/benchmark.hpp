#pragma once

#include "game.hpp"
#include "view.hpp"

#include <chrono>
#include <fstream>
#include <kin/platform/app.hpp>
#include <kin/renderer/backend.hpp>

namespace summit {

// One pending frame and a streamed CSV: no retained event/percentile history in the game process.
class ExpeditionBenchmark {
public:
    using Clock = std::chrono::steady_clock;
    ExpeditionBenchmark(const std::string& path, Clock::time_point process_start, f64 warmup_seconds,
                        f64 frame_limit = 0);
    bool ready() const;
    void begin_frame(const AppFrameStats& app);
    void capture(const Game& game, i32 leg, const SummitView& view, const RenderDebugStats3D& stats, Vec2i size,
                 f64 build_ms, f64 submit_ms, f64 ui_ms, const RendererBackendStats& backend);
    void set_asset_setup_ms(f64 ms) { _asset_setup_ms = ms; }
    void finish(const Game& game, i32 samples);

private:
    struct Row {
        u64 frame = 0, step = 0;
        i32 leg = 0, phase = 0;
        Vec3f feet{};
        Vec2i size{};
        bool measured = false;
        f64 build_ms = 0, submit_ms = 0, ui_ms = 0, mono_ms = 0, gpu_ms = 0;
        RenderDebugStats3D stats{};
    } _pending;
    std::string _path;
    std::ofstream _csv;
    Clock::time_point _process_start, _first_frame{};
    f64 _warmup_seconds = 2, _asset_setup_ms = 0, _first_present_ms = 0, _frame_limit = 0;
    u64 _gpu_sample = 0;
    u64 _frames = 0, _scenery_triangles = 0, _record_bytes = 0, _static_triangles = 0;
    u32 _scenery = 0;
    bool _have_pending = false, _finished = false;
    bool _core_quality = false;
};

} // namespace summit
