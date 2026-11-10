#pragma once
// Opt-in evaluation output. No file, counter collection or formatting in ordinary runs.
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <kin/renderer/backend.hpp>
#include <stdexcept>
#include <string_view>
namespace kin::detail {
class EvalTelemetry {
public:
    EvalTelemetry() {
        if (const auto* path = std::getenv("KIN_EVAL_FRAME_LOG"); path && *path) {
            _out.open(path, std::ios::binary);
            if (!_out)
                throw std::runtime_error("cannot open KIN_EVAL_FRAME_LOG");
            _out << std::setprecision(12);
        }
    }
    explicit operator bool() const { return _out.is_open(); }
    void record(std::string_view view, i64 frame, double interval, double work, double update, double render,
                double present, const RendererBackendStats& stats) {
        if (!_out.is_open())
            return;
        // View identifiers are generated/validated by the evaluation adapters.
        _out << "{\"schema\":\"kin.eval-frame/1\",\"view\":\"";
        for (char c : view) {
            if (c == '"' || c == '\\')
                _out << '\\';
            if (static_cast<unsigned char>(c) >= 32)
                _out << c;
        }
        _out << "\",\"frame\":" << frame << ",\"cpu\":{\"interval_ms\":" << interval << ",\"work_ms\":" << work
             << ",\"update_ms\":" << update << ",\"render_ms\":" << render << ",\"present_ms\":" << present
             << "},\"memory\":{\"owned_bytes\":" << stats.gpu_memory.payload_bytes()
             << ",\"pooled_texture_bytes\":" << stats.gpu_memory.pooled_texture_bytes
             << ",\"staging_bytes\":" << stats.upload_staging_bytes
             << ",\"allocations\":" << stats.gpu_memory.allocations << ",\"releases\":" << stats.gpu_memory.releases
             << "},\"copies\":{\"count\":" << stats.texture_copies << ",\"bytes\":" << stats.texture_copy_bytes
             << "},\"gpu\":{\"method\":\"fence_completion_estimate\",\"sample_serial\":" << stats.gpu_frames_sampled
             << ",\"latest_ms\":" << stats.last_gpu_frame_ms
             << ",\"source_frame\":null,\"percentiles_valid\":false}}\n";
        if (!_out)
            throw std::runtime_error("evaluation frame log write failed");
    }

private:
    std::ofstream _out;
};
} // namespace kin::detail
