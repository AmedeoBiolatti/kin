#include <kin/render3d/renderer3d.hpp>

#include <kin/core/jobs.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>

namespace kin {

namespace {

// Where 3D isn't available: a render target that shows the background.
class FallbackView final : public IView3DBackend {
public:
    FallbackView(RenderTarget target, const View3DOptions& options) : target(std::move(target)), options(options) {}
    Texture texture() const override { return target.texture(); }
    Vec2i size() const override { return target.size(); }
    u32 samples() const override { return 1; }

    RenderTarget target;
    View3DOptions options;
};

Vec2i at_least_one(Vec2i size) { return {std::max(1, size.x), std::max(1, size.y)}; }

} // namespace

Renderer3D::Renderer3D(Renderer2D& renderer) : _renderer(&renderer), _backend(renderer.renderer_3d()) {}

Renderer3DCapabilities Renderer3D::capabilities() const {
    return _backend ? _backend->capabilities() : Renderer3DCapabilities{};
}

Mesh Renderer3D::create_mesh(const MeshData& data) {
    return create_mesh(data, MeshOptions{});
}

Mesh Renderer3D::create_mesh(const MeshData& data, const MeshOptions& requested) {
    MeshOptions options = requested;
    if (data.skinned() && (options.clusters || options.levels > 1)) {
        // A skinned mesh moves with its joints: levels and clusters are built from the mesh at rest and would not follow.
        KIN_LOG_WARN("render", "3D: a skinned mesh cannot have levels of detail or clusters: made without them");
        options.clusters = false;
        options.levels = 1;
    }
    std::string error;
    if (!data.validate(&error)) {
        KIN_LOG_WARN_F("render", "3D: mesh not made", (LogFields{{.name = "reason", .value = error}}));
        return {};
    }
    const std::size_t vertices = data.vertices.size();
    const std::size_t indices = data.indices.empty() ? vertices : data.indices.size();
    if (vertices > std::numeric_limits<u32>::max() / sizeof(MeshVertex) || indices > std::numeric_limits<u32>::max() / 4) {
        KIN_LOG_WARN("render", "3D: mesh not made: too big for one buffer");
        return {};
    }
    if (!_backend) {
        return {};
    }
    MeshLodChain chain;
    std::shared_ptr<const MeshClusters> clusters;
    if (options.clusters) {
        // A cluster hierarchy replaces the levels (which are then not built).
        JobSystem* jobs = _renderer->job_system();
        ClusterParallelFor parallel;
        if (jobs && jobs->worker_count() > 1) {
            parallel = [jobs](u32 count, const std::function<void(u32)>& body) {
                jobs->parallel_for(static_cast<i32>(count), [&](i32 i) { body(static_cast<u32>(i)); });
            };
        }
        clusters = std::make_shared<const MeshClusters>(build_mesh_clusters(data, options, parallel));
        if (clusters->leaf_count == 0) {
            KIN_LOG_WARN("render", "3D: mesh not made: no triangles");
            return {};
        }
    } else if (options.levels > 1) {
        chain = build_mesh_levels(data, options);
    }
    std::shared_ptr<IMeshBackend> backend = _backend->create_mesh(data, chain.count() > 1 ? &chain : nullptr, clusters);
    if (!backend) {
        return {};
    }
    std::shared_ptr<const std::vector<MeshLevelInfo>> levels;
    if (chain.count() > 1) {
        auto info = std::make_shared<std::vector<MeshLevelInfo>>();
        for (const MeshLodLevel& level : chain.levels) {
            info->push_back({level.triangles, level.error});
        }
        levels = std::move(info);
    }
    std::shared_ptr<const MeshClusterInfo> cluster_info;
    if (clusters) {
        cluster_info = std::make_shared<const MeshClusterInfo>(clusters->info());
    }
    return Mesh{std::move(backend), static_cast<u32>(vertices), static_cast<u32>(indices), data.bounds(), std::move(levels), std::move(cluster_info),
                data.skin_joint_count()};
}

View3D Renderer3D::create_view(Vec2i size, const View3DOptions& options) {
    size = at_least_one(size);
    if (_backend) {
        return View3D{_backend->create_view(size, options)};
    }
    RenderTarget target = _renderer->create_render_target(size);
    if (!target.valid()) {
        return {};
    }
    return View3D{std::make_shared<FallbackView>(std::move(target), options)};
}

bool Renderer3D::resize(View3D& view, Vec2i size) {
    size = at_least_one(size);
    if (!view.valid()) {
        return false;
    }
    if (view.size() == size) {
        return true;
    }
    if (auto* fallback = dynamic_cast<FallbackView*>(view.backend())) {
        RenderTarget target = _renderer->create_render_target(size);
        if (!target.valid()) {
            return false;
        }
        fallback->target = std::move(target);
        return true;
    }
    return _backend && _backend->resize_view(*view.backend(), size);
}

PickResult Renderer3D::pick(View3D& view, Vec2i pixel) {
    if (!view.valid() || !_backend || dynamic_cast<FallbackView*>(view.backend())) {
        return {};
    }
    return _backend->pick(*view.backend(), pixel);
}

bool Renderer3D::read_cluster_picks(View3D& view, std::vector<ClusterPick>& out) {
    out.clear();
    if (!view.valid() || !_backend || dynamic_cast<FallbackView*>(view.backend())) {
        return false;
    }
    return _backend->read_cluster_picks(*view.backend(), out);
}

bool Renderer3D::read_cluster_draws(View3D& view, std::vector<ClusterDraw>& out) {
    out.clear();
    if (!view.valid() || !_backend || dynamic_cast<FallbackView*>(view.backend())) {
        return false;
    }
    return _backend->read_cluster_draws(*view.backend(), out);
}

bool Renderer3D::warm_up(View3D& view, const Scene3D& scene) {
    if (!view.valid() || !_backend || dynamic_cast<FallbackView*>(view.backend())) {
        return false;
    }
    return _backend->warm_up(*view.backend(), scene);
}

bool Renderer3D::render(View3D& view, const Scene3D& scene) {
    _stats = {};
    if (!view.valid()) {
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    bool drawn = false;
    if (auto* fallback = dynamic_cast<FallbackView*>(view.backend())) {
        if (!_warned) {
            KIN_LOG_WARN_F("render", "3D: not available, views show their background",
                           (LogFields{{.name = "backend", .value = std::string{_renderer->backend_name()}}}));
            _warned = true;
        }
        _renderer->push_render_target(fallback->target);
        _renderer->clear(fallback->options.background);
        _renderer->pop_render_target();
    } else if (_backend) {
        drawn = _backend->render(*view.backend(), scene, _stats);
    }
    _stats.cpu_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    return drawn;
}

} // namespace kin
