#include "external_assets.hpp"

#include <kin/assets/content.hpp>
#include <kin/render3d/renderer3d.hpp>

#include <algorithm>
#include <cstdio>

namespace summit {
using namespace kin;

ExternalAssets::ExternalAssets(Assets3D& assets, const SummitLevel& level, bool enabled) {
    if (!enabled || content_root().empty()) return;
    Texture atlas;
    unsigned loaded = 0;
    for (std::size_t i = 0; i < prop_names.size(); ++i) {
        const auto path = content_root() / "kenney-survival" / (std::string(prop_names[i]) + ".glb");
        if (!content_file_exists(path)) continue;
        std::string error;
        _models[i] = assets.load_model(std::filesystem::path("kenney-survival") / (std::string(prop_names[i]) + ".glb"), &error);
        if (!_models[i]) {
            std::fprintf(stderr, "summit: asset %s: %s\n", prop_names[i], error.c_str());
            continue;
        }
        const auto& expected = prop_geometry_data(static_cast<PropKind>(i));
        if (length(_models[i]->bounds.min - expected.source_min) > 0.0001f ||
            length(_models[i]->bounds.max - expected.source_max) > 0.0001f) {
            std::fprintf(stderr, "summit: %s no longer matches its baked collider; regenerate prop geometry\n", prop_names[i]);
            _models[i] = {};
            continue;
        }
        for (const ModelPart& part : _models[i]->parts) {
            if (!atlas) atlas = part.material.texture;
        }
        ++loaded;
    }
    std::printf("summit: external assets %u/%u models, shared atlas %s\n", loaded, unsigned(PropCount), atlas ? "loaded" : "missing");

    const auto scenery = scenery_props(level);
    _scenery_instances = u32(scenery.size());
    std::size_t mesh_batches = 0;
    // Immutable, spatially grouped records keep dense dressing out of the per-frame allocation/copy path. The renderer still
    // culls each cell and visible copy; all cells reuse the same small mesh/texture set.
    for (int kind = 0; kind < PropCount; ++kind) {
        if (!_models[kind]) continue;
        const Model& model = *_models[kind];
        const Mat4f normalise = model.placement({.pivot = ModelPivot3D::BottomCenter, .size = 1});
        std::vector<MeshInstance> instances;
        for (std::size_t i=0;i<scenery.size();++i) {
            const auto& p = scenery[i];
            if (p.kind != kind) continue;
            const Mat4f transform = Mat4f::trs(p.feet,Quatf::axis_angle({0,1,0},p.yaw),{p.span,p.span,p.span}) * normalise;
            Color tint = colors::white;
            if (kind == Grass || kind == LargeGrass) tint = Color::rgb(145,185,110);
            else if (kind == Tree || kind == TallTree) tint = Color::rgb(180,205,155);
            else if (kind == RockA || kind == RockB) tint = Color::rgb(210,201,180);
            instances.push_back({transform,tint,u32(1000000+i)});
        }
        if (instances.empty()) continue;
        ModelBatch3D batch{model, instances};
        _scenery_triangles += batch.triangle_count();
        _instance_record_bytes += batch.record_bytes();
        mesh_batches += batch.parts().size();
        _batches.push_back(std::move(batch));
    }
    std::printf("summit: scenery %u instances, %llu instance triangles, %llu CPU record bytes, %zu mesh batches\n",
                _scenery_instances,(unsigned long long)_scenery_triangles,(unsigned long long)_instance_record_bytes,mesh_batches);
    const auto cache = assets.stats();
    std::printf("summit: shared art cache %zu models, %zu textures, %llu texture reuses\n",
                cache.model_entries, cache.texture_entries, (unsigned long long)cache.texture_hits);
}

bool ExternalAssets::draw(Scene3D& scene, Kind kind, Vec3f feet, float span, float yaw) const {
    if (!_models[kind]) return false;
    const Model& model = *_models[kind];
    const Vec3f size = model.bounds.size();
    const float extent = std::max({size.x, size.y, size.z});
    if (extent < 1e-5f) return false;
    const Mat4f transform = model.placement({.position = feet, .rotation = Quatf::axis_angle({0, 1, 0}, yaw),
                                           .pivot = ModelPivot3D::BottomCenter, .size = span});
    scene.draw(model, transform);
    return true;
}

void ExternalAssets::draw_scenery(Scene3D& scene) const {
    for (const auto& batch : _batches) scene.draw_instanced(batch);
}

} // namespace summit
