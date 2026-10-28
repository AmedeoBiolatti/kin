#include <algorithm>
#include <chrono>
#include <kin/assets/bake_cache.hpp>
#include <kin/core/jobs.hpp>
#include <kin/platform/log.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <limits>
#include <string>

namespace kin {

namespace {

// Where 3D isn't available: a render target that shows the background.
class FallbackView final : public IViewBackend3D {
public:
    FallbackView(RenderTarget target, const ViewOptions3D& options) : target(std::move(target)), options(options) {}
    Texture texture() const override { return target.texture(); }
    Vec2i size() const override { return target.size(); }
    u32 samples() const override { return 1; }

    RenderTarget target;
    ViewOptions3D options;
};

Vec2i at_least_one(Vec2i size) {
    return {std::max(1, size.x), std::max(1, size.y)};
}

} // namespace

Renderer3D::Renderer3D(Renderer2D& renderer) : _renderer(&renderer), _backend(renderer.renderer_3d()) {}

RendererCapabilities3D Renderer3D::capabilities() const {
    return _backend ? _backend->capabilities() : RendererCapabilities3D{};
}

// A bake that passed the cache's checksum but is not a build of this kind (a format changed without its version): built
// again, and the new build replaces it.
void Renderer3D::warn_refused_bake(bool restored) {
    if (!restored && !_bake_warned) {
        _bake_warned = true;
        KIN_LOG_WARN("render", "3D: a baked mesh build was refused and is built again (later ones are not logged)");
    }
}

Result3D<Mesh3D> Renderer3D::create_mesh(const MeshData3D& data) {
    auto value = create_mesh_operation(data);
    return result3d(std::move(value), _errors);
}

Mesh3D Renderer3D::create_mesh_operation(const MeshData3D& data) {
    return create_mesh_operation(data, MeshOptions3D{});
}

Result3D<Mesh3D> Renderer3D::create_mesh(const MeshData3D& data, const MeshOptions3D& requested) {
    auto value = create_mesh_operation(data, requested);
    return result3d(std::move(value), _errors);
}

Mesh3D Renderer3D::create_mesh_operation(const MeshData3D& data, const MeshOptions3D& requested) {
    _errors.clear();
    const auto validation = data.validate();
    if (!validation) {
        const auto error = diagnostics3d_text(validation.diagnostics);
        fail3d(_errors, "Renderer3D::create_mesh", error);
        KIN_LOG_WARN_F("render", "3D: mesh not made", (LogFields{{.name = "reason", .value = error}}));
        return {};
    }
    return upload_validated_mesh(data, requested);
}

Mesh3D Renderer3D::upload_validated_mesh(const MeshData3D& data, const MeshOptions3D& requested) {
    _errors.clear();
    MeshOptions3D options = requested;
    if (data.skinned() && (options.clusters || options.levels > 1)) {
        // A skinned mesh moves with its joints: levels and clusters are built from the mesh at rest and would not
        // follow.
        KIN_LOG_WARN("render", "3D: a skinned mesh cannot have levels of detail or clusters: made without them");
        options.clusters = false;
        options.levels = 1;
    }
    const std::size_t vertices = data.vertices.size();
    const std::size_t indices = data.indices.empty() ? vertices : data.indices.size();
    if (vertices > std::numeric_limits<u32>::max() / sizeof(MeshVertex3D) ||
        indices > std::numeric_limits<u32>::max() / 4) {
        fail3d(_errors, "Renderer3D::create_mesh", "mesh too big for one buffer");
        KIN_LOG_WARN("render", "3D: mesh not made: too big for one buffer");
        return {};
    }
    if (!_backend) {
        fail3d(_errors, "Renderer3D::create_mesh", "3D is not available");
        return {};
    }
    MeshLodChain3D chain;
    std::shared_ptr<const MeshClusters3D> clusters;
    MeshData3D
        welded; // with clusters: what is uploaded (the hierarchy's vertices and its leaves' triangles), then let go
    if (options.clusters) {
        // A cluster hierarchy replaces the levels (which are then not built).
        JobSystem* jobs = _renderer->job_system();
        ClusterParallelFor3D parallel;
        if (jobs && jobs->worker_count() > 1) {
            parallel = [jobs](u32 count, const std::function<void(u32)>& body) {
                jobs->parallel_for(static_cast<i32>(count), [&](i32 i) { body(static_cast<u32>(i)); });
            };
        }
        // A bake of the same mesh and options is restored in place of the build: the same bytes. A mesh of one
        // cluster has nothing to build that costs more than reading a file.
        BakeCache* bake = _bake_cache && _bake_cache->enabled() && data.triangle_count() > options.cluster_triangles
                              ? _bake_cache
                              : nullptr;
        const u64 key = bake ? mesh_clusters_bake_key(data, options) : 0;
        MeshClusterBuild3D built;
        bool restored = false;
        if (bake) {
            if (std::optional<std::vector<u8>> baked = bake->read(key)) {
                restored = restore_mesh_clusters(*baked, built);
                warn_refused_bake(restored);
            }
        }
        if (!restored) {
            built = build_mesh_clusters(data, options, parallel);
            if (bake && built.leaf_count > 0) {
                bake->write(key, save_mesh_clusters(built));
            }
        }
        if (built.leaf_count == 0) {
            fail3d(_errors, "Renderer3D::create_mesh", "no triangles");
            KIN_LOG_WARN("render", "3D: mesh not made: no triangles");
            return {};
        }
        // The mesh keeps the hierarchy alone (the cut reads no vertex): the vertices go to the GPU and no further.
        welded.vertices = std::move(built.vertices);
        welded.indices = built.leaf_indices();
        clusters = std::make_shared<const MeshClusters3D>(std::move(static_cast<MeshClusters3D&>(built)));
    } else if (options.levels > 1) {
        // The same for levels, where the chain has work to do (build_mesh_levels returns level 0 alone at once
        // otherwise).
        const bool work = std::min(options.levels, max_mesh_levels) >= 2 &&
                          data.triangle_count() >= 2 * std::max<std::size_t>(options.min_triangles, 1);
        BakeCache* bake = _bake_cache && _bake_cache->enabled() && work ? _bake_cache : nullptr;
        const u64 key = bake ? mesh_levels_bake_key(data, options) : 0;
        bool restored = false;
        if (bake) {
            if (std::optional<std::vector<u8>> baked = bake->read(key)) {
                restored = restore_mesh_levels(*baked, data, chain);
                warn_refused_bake(restored);
            }
        }
        if (!restored) {
            chain = build_mesh_levels(data, options);
            if (bake) {
                bake->write(key, save_mesh_levels(chain));
            }
        }
    }
    std::shared_ptr<IMeshBackend3D> backend =
        _backend->create_mesh(clusters ? welded : data, chain.count() > 1 ? &chain : nullptr, clusters);
    welded = {};
    if (!backend) {
        fail3d(_errors, "Renderer3D::create_mesh", "device refused mesh buffers");
        return {};
    }
    std::shared_ptr<const std::vector<MeshLevelInfo3D>> levels;
    if (chain.count() > 1) {
        auto info = std::make_shared<std::vector<MeshLevelInfo3D>>();
        for (const MeshLodLevel3D& level : chain.levels) {
            info->push_back({level.triangles, level.error});
        }
        levels = std::move(info);
    }
    std::shared_ptr<const MeshClusterInfo3D> cluster_info;
    if (clusters) {
        cluster_info = std::make_shared<const MeshClusterInfo3D>(clusters->info());
    }
    return Mesh3D{std::move(backend), static_cast<u32>(vertices), static_cast<u32>(indices), data.bounds(),
                  std::move(levels),  std::move(cluster_info),    data.skin_joint_count()};
}

Result3D<View3D> Renderer3D::create_view(Vec2i size, const ViewOptions3D& options) {
    auto value = create_view_operation(size, options);
    return result3d(std::move(value), _errors);
}

View3D Renderer3D::create_view_operation(Vec2i size, const ViewOptions3D& options) {
    _errors.clear();
    size = at_least_one(size);
    if (_backend) {
        View3D result{_backend->create_view(size, options)};
        if (!result)
            fail3d(_errors, "Renderer3D::create_view", "device refused view targets");
        return result;
    }
    RenderTarget target = _renderer->create_render_target(size);
    if (!target.valid()) {
        fail3d(_errors, "Renderer3D::create_view", "fallback target creation failed");
        return {};
    }
    return View3D{std::make_shared<FallbackView>(std::move(target), options)};
}

Status3D Renderer3D::resize(View3D& view, Vec2i size) {
    auto value = resize_operation(view, size);
    return status3d(std::move(value), _errors);
}

bool Renderer3D::resize_operation(View3D& view, Vec2i size) {
    _errors.clear();
    size = at_least_one(size);
    if (!view.valid()) {
        return fail3d(_errors, "Renderer3D::resize", "invalid view");
    }
    if (view.size() == size) {
        return true;
    }
    if (auto* fallback = dynamic_cast<FallbackView*>(view.backend())) {
        RenderTarget target = _renderer->create_render_target(size);
        if (!target.valid()) {
            return fail3d(_errors, "Renderer3D::resize", "fallback target creation failed");
        }
        fallback->target = std::move(target);
        return true;
    }
    if (_backend && _backend->resize_view(*view.backend(), size))
        return true;
    return fail3d(_errors, "Renderer3D::resize", "device refused view resize");
}

Result3D<PickResult3D> Renderer3D::pick(View3D& view, Vec2i pixel) {
    auto value = pick_operation(view, pixel);
    return result3d(std::move(value), _errors);
}

PickResult3D Renderer3D::pick_operation(View3D& view, Vec2i pixel) {
    _errors.clear();
    if (!view.valid() || !_backend || dynamic_cast<FallbackView*>(view.backend())) {
        fail3d(_errors, "Renderer3D::pick", "picking requires a valid 3D view");
        return {};
    }
    auto picked = _backend->pick(*view.backend(), pixel);
    const bool success = picked.ok();
    _errors = std::move(picked.diagnostics);
    if (!success) {
        if (_errors.empty())
            fail3d(_errors, "Renderer3D::pick", "backend picking failed");
        return {};
    }
    return *picked.value;
}

Result3D<bool> Renderer3D::read_cluster_picks(View3D& view, std::vector<ClusterPick3D>& out) {
    auto value = read_cluster_picks_operation(view, out);
    return result3d(std::move(value), _errors);
}

bool Renderer3D::read_cluster_picks_operation(View3D& view, std::vector<ClusterPick3D>& out) {
    _errors.clear();
    out.clear();
    if (!view.valid() || !_backend || dynamic_cast<FallbackView*>(view.backend())) {
        return fail3d(_errors, "Renderer3D::read_cluster_picks", "operation requires a valid 3D view");
    }
    return _backend->read_cluster_picks(*view.backend(), out);
}

Result3D<bool> Renderer3D::read_cluster_draws(View3D& view, std::vector<ClusterDraw3D>& out) {
    auto value = read_cluster_draws_operation(view, out);
    return result3d(std::move(value), _errors);
}

bool Renderer3D::read_cluster_draws_operation(View3D& view, std::vector<ClusterDraw3D>& out) {
    _errors.clear();
    out.clear();
    if (!view.valid() || !_backend || dynamic_cast<FallbackView*>(view.backend())) {
        return fail3d(_errors, "Renderer3D::read_cluster_draws", "operation requires a valid 3D view");
    }
    return _backend->read_cluster_draws(*view.backend(), out);
}

Result3D<bool> Renderer3D::read_gi_scene(View3D& view, GiScene3D& out) {
    auto value = read_gi_scene_operation(view, out);
    return result3d(std::move(value), _errors);
}

bool Renderer3D::read_gi_scene_operation(View3D& view, GiScene3D& out) {
    _errors.clear();
    out = {};
    if (!view.valid() || !_backend || dynamic_cast<FallbackView*>(view.backend())) {
        return fail3d(_errors, "Renderer3D::read_gi_scene", "operation requires a valid 3D view");
    }
    return _backend->read_gi_scene(*view.backend(), out);
}

Status3D Renderer3D::warm_up(View3D& view, const Scene3D& scene) {
    auto value = warm_up_operation(view, scene);
    return status3d(std::move(value), _errors);
}

bool Renderer3D::warm_up_operation(View3D& view, const Scene3D& scene) {
    _errors.clear();
    if (!view.valid()) {
        return fail3d(_errors, "Renderer3D::warm_up", "operation requires a valid view");
    }
    if (!scene.diagnostics().empty()) {
        _errors = scene.diagnostics();
        return false;
    }
    if (const auto* overlay = scene.overlay_if_any(); overlay && !overlay->diagnostics().empty()) {
        _errors = overlay->diagnostics();
        return false;
    }
    if (dynamic_cast<FallbackView*>(view.backend()))
        return true;
    if (_backend && _backend->warm_up(*view.backend(), scene))
        return true;
    return fail3d(_errors, "Renderer3D::warm_up", "GPU warm-up failed");
}

Status3D Renderer3D::render(View3D& view, const Scene3D& scene) {
    auto value = render_operation(view, scene);
    return status3d(std::move(value), _errors);
}

Status3D Renderer3D::render_geometry(const GeometryTargets3D& targets, const Scene3D& scene) {
    _errors.clear();
    _stats = {};
    const auto fail = [&](const char* message) {
        fail3d(_errors, "Renderer3D::render_geometry", message);
        return status3d(false, _errors);
    };
    if (!_backend || !_backend->capabilities().geometry_pass)
        return fail("standalone geometry passes are unavailable on this backend");
    if (!targets.depth || !targets.normals || targets.depth.format() != TextureFormat::Depth32Float ||
        targets.normals.format() != TextureFormat::Rg16Float || targets.depth.size() != targets.normals.size() ||
        targets.depth.size().x <= 0 || targets.depth.size().y <= 0)
        return fail("requires matching D32F depth and RG16F normal render attachments");
    if (!scene.diagnostics().empty()) {
        _errors = scene.diagnostics();
        return status3d(false, _errors);
    }
    const auto start = std::chrono::steady_clock::now();
    const bool drawn = _backend->render_geometry(targets, scene, _stats);
    _stats.cpu_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!drawn)
        return fail("geometry inputs are incompatible with this renderer, or GPU recording failed");
    return status3d(true, _errors);
}

Result3D<PreparedScene3D> Renderer3D::prepare(const Scene3D& scene, const PrepareOptions3D& options) {
    _errors.clear();
    _stats = {};
    if (!_backend || !_backend->capabilities().shared_preparation) {
        fail3d(_errors, "Renderer3D::prepare", "shared preparation is unavailable on this backend");
        return {std::nullopt, _errors};
    }
    if (!scene.diagnostics().empty()) {
        _errors = scene.diagnostics();
        return {std::nullopt, _errors};
    }
    if (const auto* overlay = scene.overlay_if_any(); overlay && !overlay->diagnostics().empty()) {
        _errors = overlay->diagnostics();
        return {std::nullopt, _errors};
    }
    const auto start = std::chrono::steady_clock::now();
    auto prepared = _backend->prepare(scene, options, _stats);
    _stats.cpu_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!prepared) {
        fail3d(_errors, "Renderer3D::prepare", "incompatible scene inputs or preparation CPU budget exceeded");
        return {std::nullopt, _errors};
    }
    return result3d(PreparedScene3D{std::move(prepared)});
}
Status3D Renderer3D::render_prepared(View3D& view, const PreparedScene3D& prepared) {
    _errors.clear();
    _stats = {};
    const auto start = std::chrono::steady_clock::now();
    const bool ok = _backend && view && prepared.backend() &&
                    _backend->render_prepared(*view.backend(), *prepared.backend(), _stats);
    _stats.cpu_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!ok)
        fail3d(_errors, "Renderer3D::render",
               "invalid, edited, expired or foreign prepared submission, or GPU recording failed");
    return status3d(ok, _errors);
}
Status3D Renderer3D::render_geometry_prepared(const GeometryTargets3D& targets, const PreparedScene3D& prepared) {
    _errors.clear();
    _stats = {};
    const auto start = std::chrono::steady_clock::now();
    const bool ok = _backend && prepared.backend() && targets.depth && targets.normals &&
                    targets.depth.format() == TextureFormat::Depth32Float &&
                    targets.normals.format() == TextureFormat::Rg16Float &&
                    targets.depth.size() == targets.normals.size() &&
                    _backend->render_geometry_prepared(targets, *prepared.backend(), _stats);
    _stats.cpu_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!ok)
        fail3d(_errors, "Renderer3D::render_geometry",
               "invalid, edited, expired or foreign prepared submission/targets, or GPU recording failed");
    return status3d(ok, _errors);
}

bool Renderer3D::render_operation(View3D& view, const Scene3D& scene) {
    _errors.clear();
    _stats = {};
    if (!view.valid()) {
        return fail3d(_errors, "Renderer3D::render", "invalid view");
    }
    if (!scene.diagnostics().empty()) {
        _errors = scene.diagnostics();
        return false;
    }
    if (const auto* overlay = scene.overlay_if_any(); overlay && !overlay->diagnostics().empty()) {
        _errors = overlay->diagnostics();
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
        drawn = true;
    } else if (_backend) {
        drawn = _backend->render(*view.backend(), scene, _stats);
    }
    _stats.cpu_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!drawn)
        fail3d(_errors, "Renderer3D::render", _backend ? "device refused scene rendering" : "3D is not available");
    return drawn;
}

} // namespace kin
