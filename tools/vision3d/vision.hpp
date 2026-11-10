#pragma once
#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/graph_resources.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <stdexcept>

// Optional example-level scheduler, not an engine AI system. All calls stay on the render thread.
// Renderer2D/Renderer3D outlive it. Destruction drains at most max_pending small reads; live polling never waits.
namespace vision3d {
using namespace kin;
struct Request {
    u64 token = 0; // caller-owned identity/generation, returned unchanged even if the entity disappears
    Camera3D camera;
    VisibilityOptions3D query;
};
struct Result {
    u64 token = 0, frame = 0;
    Camera3D camera;
    u32 target_id = 0, visible_pixels = 0;
    bool valid = false; // failed read is unknown, not an occluded observation
};
struct Config {
    Vec2i size{64, 64};
    u32 views_per_batch = 4, max_pending = 3;
};
class Graph {
public:
    Graph(Renderer2D& renderer, Renderer3D& r3d, ComputeShaderHandle reduce, ShaderHandle pack, Config config = {})
        : _renderer(renderer), _r3d(r3d), _reduce(reduce), _pack(pack), _config(config) {
        if (config.size.x < 1 || config.size.y < 1 || config.size.x > 256 || config.size.y > 256 ||
            config.views_per_batch < 1 || config.views_per_batch > 64 || config.max_pending < 1 ||
            config.max_pending > 8 || !r3d.capabilities().visibility_pass || !reduce || !pack)
            throw std::runtime_error("unsupported visibility configuration/backend");
        _desc.resources = {
            {.name = "mask",
             .kind = GraphResourceKind::RenderAttachment,
             .format = TextureFormat::Rgba16Float,
             .size = config.size,
             .exported = true},
            {.name = "depth",
             .kind = GraphResourceKind::RenderAttachment,
             .format = TextureFormat::Depth32Float,
             .size = config.size},
            {.name = "counts", .kind = GraphResourceKind::Buffer, .bytes = config.views_per_batch * sizeof(u32)},
            {.name = "readback_bytes",
             .kind = GraphResourceKind::ColorTarget,
             .size = {4, static_cast<i32>(config.views_per_batch)},
             .exported = true,
             .hdr = renderer.hdr()}};
        _desc.passes = {{.name = "observer",
                         .writes = {0, 1},
                         .required_features = graph_feature::rendering3d | graph_feature::attachments},
                        {.name = "count",
                         .reads = {0},
                         .writes = {2},
                         .required_features = graph_feature::compute | graph_feature::buffers},
                        {.name = "pack",
                         .reads = {2},
                         .writes = {3},
                         .required_features = graph_feature::shaders | graph_feature::buffers}};
        prepare();
    }
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    ~Graph() {
        std::vector<u8> bytes;
        Vec2i size;
        for (const auto& batch : _pending)
            _renderer.take_read_rgba(batch.ticket, bytes, size, true);
    }
    // Must run before preparing the scene when the renderer's working space has changed.
    void prepare() {
        _desc.resources[3].hdr = _renderer.hdr();
        _plan = _compiler.compile(_desc, available_graph_features(_renderer));
        if (!_resources.prepare(_renderer, _desc, _plan))
            throw std::runtime_error("visibility graph allocation failed");
    }
    // Rotating selection bounds per-frame views and pending readbacks. Zero means no observation was scheduled.
    // Each graph uses the source's current camera, restored even on failure; geometry/poses/materials are untouched.
    u32 record(Scene3D& scene, const PreparedScene3D& prepared, std::span<const Request> requests, u64 frame,
               const std::function<void(u64, const Texture&)>& preview = {}) {
        if (!_staged.empty() || _pending.size() >= _config.max_pending || requests.empty())
            return 0;
        const Camera3D original = scene.camera;
        struct Restore {
            Scene3D& scene;
            Camera3D camera;
            ~Restore() { scene.camera = camera; }
        } restore{scene, original};
        const u32 count = static_cast<u32>(std::min<std::size_t>(requests.size(), _config.views_per_batch));
        try {
            for (u32 slot = 0; slot < count; ++slot) {
                const auto& request = requests[(_cursor + slot) % requests.size()];
                scene.camera = request.camera;
                const std::array<std::function<bool()>, 3> operations{
                    [&] {
                        return bool(_r3d.render_visibility({_resources.texture(1), _resources.texture(0)}, prepared,
                                                           request.query));
                    },
                    [&] {
                        ShaderParams params;
                        params.uniforms[0] = static_cast<f32>(slot);
                        const std::array sources{_resources.texture(0)};
                        const std::array buffers{_resources.buffer(2)};
                        return _renderer.dispatch_compute(
                            _reduce, {64, 1}, {.sources = sources, .output_buffers = buffers, .params = &params});
                    },
                    [&] {
                        auto target = _renderer.scoped_render_target(_resources.target(3));
                        _renderer.clear(colors::transparent);
                        const std::array buffers{_resources.buffer(2)};
                        _renderer.draw_shader_surface({0, 0, 4, static_cast<f32>(_config.views_per_batch)}, _pack, {},
                                                      {}, buffers);
                        return true;
                    }};
                if (!_resources.execute(_plan, operations))
                    throw std::runtime_error("visibility graph recording failed");
                _staged.push_back({.token = request.token,
                                   .frame = frame,
                                   .camera = request.camera,
                                   .target_id = request.query.target_id});
                if (preview)
                    preview(request.token, _resources.texture(0));
            }
        } catch (...) {
            _staged.clear();
            throw;
        }
        _cursor = (_cursor + count) % requests.size();
        return count;
    }
    // End-of-frame operation: records ONE small read after every main/observer consumer. Invalidates preparation.
    bool submit() {
        if (_staged.empty())
            return false;
        auto target = _renderer.scoped_render_target(_resources.target(3));
        const auto ticket = _renderer.request_read_rgba({0, 0, 4, static_cast<f32>(_staged.size())}, false);
        _pending.push_back({ticket, std::move(_staged)});
        _staged.clear();
        return ticket != 0;
    }
    std::vector<Result> poll() {
        std::vector<Result> results;
        for (auto it = _pending.begin(); it != _pending.end();) {
            std::vector<u8> bytes;
            Vec2i size;
            if (it->ticket != 0 && !_renderer.take_read_rgba(it->ticket, bytes, size, false)) {
                ++it;
                continue;
            }
            const bool valid =
                size == Vec2i{4, static_cast<i32>(it->results.size())} && bytes.size() == it->results.size() * 16;
            for (std::size_t row = 0; row < it->results.size(); ++row) {
                auto result = it->results[row];
                result.valid = valid;
                if (valid)
                    for (u32 b = 0; b < 4; ++b)
                        result.visible_pixels |= u32(bytes[row * 16 + b * 4 + 3]) << (b * 8);
                result.valid = valid && result.visible_pixels <= u32(_config.size.x * _config.size.y);
                results.push_back(result);
            }
            it = _pending.erase(it);
        }
        return results;
    }
    std::size_t pending() const { return _pending.size(); }
    const Texture& mask() const { return _resources.texture(0); }
    u64 compilations() const { return _compiler.stats().compilations; }

private:
    struct Batch {
        u64 ticket;
        std::vector<Result> results;
    };
    Renderer2D& _renderer;
    Renderer3D& _r3d;
    ComputeShaderHandle _reduce;
    ShaderHandle _pack;
    Config _config;
    ResourceGraphDesc _desc;
    CompiledResourceGraph _plan;
    ResourceGraphCompiler _compiler;
    GraphResources _resources;
    std::deque<Batch> _pending;
    std::vector<Result> _staged;
    std::size_t _cursor = 0;
};
} // namespace vision3d
